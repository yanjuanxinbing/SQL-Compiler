// =============================================================================
// 54_dml：MergeExecutor 实现（SQL:2003 MERGE INTO）
// =============================================================================
//
// V1 简化语义：
//   - source_child 拉出每一行；为每行扫描 target 表，按 on_condition 判定 MATCHED。
//   - 命中第一条 target 行 → 走 WHEN MATCHED UPDATE SET（要求 has_matched_update）。
//   - 未命中任何 target 行 → 走 WHEN NOT MATCHED INSERT (cols) VALUES (exprs)。
//   - 每个 source 行最多影响一次 target 表；影响完毕立即 Next 返回 true（emit 一行
//     "OK"，列数为 0）。全部 source 行处理完返回 false。
//
// 与 InsertExecutor / UpdateExecutor 完全共享约束 / 索引 / WAL / FK 路径。
// =============================================================================

#include "execution/MergeExecutor.h"

#include "catalog/SystemCatalog.h"
#include "common/Error.h"
#include "execution/ConstraintChecker.h"
#include "execution/ExpressionEvaluator.h"
#include "execution/IndexMaintenance.h"

#include <unordered_map>

namespace sqlcompiler {

namespace {

Value CoerceToColumnType(const Value& v, const std::string& col_type) {
    std::string up;
    for (char c : col_type) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (up == "INT" || up == "INTEGER" || up == "BIGINT" ||
        up == "SMALLINT" || up == "TINYINT" || up == "BOOLEAN" || up == "BOOL") {
        if (v.IsNull()) return v;
        if (v.GetType() == ValueType::INTEGER) return v;
        if (v.GetType() == ValueType::FLOAT) return Value::MakeInt(static_cast<int32_t>(v.AsFloat()));
        if (v.GetType() == ValueType::VARCHAR) {
            try { return Value::MakeInt(static_cast<int32_t>(std::stoi(v.AsVarchar()))); } catch (...) { return Value::MakeInt(0); }
        }
    } else if (up == "FLOAT" || up == "DOUBLE" || up == "REAL") {
        if (v.IsNull()) return v;
        if (v.GetType() == ValueType::FLOAT) return v;
        if (v.GetType() == ValueType::INTEGER) return Value::MakeFloat(static_cast<double>(v.AsInt()));
        if (v.GetType() == ValueType::VARCHAR) {
            try { return Value::MakeFloat(std::stod(v.AsVarchar())); } catch (...) { return Value::MakeFloat(0.0); }
        }
    } else if (up == "DECIMAL" || up == "NUMERIC") {
        if (v.IsNull()) return v;
        if (v.GetType() == ValueType::VARCHAR) return v;
        if (v.GetType() == ValueType::INTEGER) {
            return Value::MakeVarchar(std::to_string(v.AsInt()));
        }
        if (v.GetType() == ValueType::FLOAT) {
            return Value::MakeVarchar(FormatDecimal(v.AsFloat()));
        }
        return v;
    }
    return v;
}

}  // namespace

MergeExecutor::MergeExecutor(ExecutionContext* context,
                              std::string target_table,
                              std::string target_alias,
                              std::string source_alias,
                              ExprPtr on_condition,
                              std::unordered_map<std::string, size_t> target_column_index_map,
                              std::unordered_map<std::string, size_t> combined_column_index_map,
                              ExecutorPtr source_child,
                              bool has_matched_update,
                              std::vector<std::pair<std::string, ExprPtr>> matched_assignments,
                              bool has_not_matched_insert,
                              std::vector<std::string> not_matched_columns,
                              std::vector<ExprPtr> not_matched_values)
    : Executor(context),
      target_table_(std::move(target_table)),
      target_alias_(std::move(target_alias)),
      source_alias_(std::move(source_alias)),
      on_condition_(std::move(on_condition)),
      target_column_index_map_(std::move(target_column_index_map)),
      combined_column_index_map_(std::move(combined_column_index_map)),
      source_child_(std::move(source_child)),
      has_matched_update_(has_matched_update),
      matched_assignments_(std::move(matched_assignments)),
      has_not_matched_insert_(has_not_matched_insert),
      not_matched_columns_(std::move(not_matched_columns)),
      not_matched_values_(std::move(not_matched_values)) {
}

void MergeExecutor::Init() {
    target_heap_ = context_->GetCatalog()->GetTableHeap(target_table_);
    const TableInfo* info = context_->GetCatalog()->GetTable(target_table_);
    if (info) {
        target_column_types_ = BuildColumnTypes(*info);
        target_column_count_ = info->columns.size();
    }
    if (source_child_) source_child_->Init();
    affected_rows_ = 0;

    // Item #2 (perf)：探测 on_condition 形态。若是简单等值谓词
    // `target.col = source.col`（target 是限定到 target 的列，source 是限定到
    // source_alias 的列），则在 Init 时把 target 表按等值列建一张 hash 表，
    // 跑 source 时按列做 O(1) 哈希探测，把每次 source 行的 O(T) SeqScan
    // 降到 O(1) 探测。Hash 探测命中后仍需 on_condition 的其它复合项
    // （AND 形式）评估；这里用复合谓词求值评估残余条件。
    use_target_hash_ = false;
    target_hash_.clear();
    if (on_condition_ && on_condition_->GetType() == NodeType::BINARY_EXPR) {
        auto b = std::static_pointer_cast<BinaryExpr>(on_condition_);
        if (b->op == BinaryOperator::EQUAL &&
            b->left && b->left->GetType() == NodeType::COLUMN_REF_EXPR &&
            b->right && b->right->GetType() == NodeType::COLUMN_REF_EXPR) {
            auto cr_l = std::static_pointer_cast<ColumnRefExpr>(b->left);
            auto cr_r = std::static_pointer_cast<ColumnRefExpr>(b->right);
            // target 的列：以 target_table_ / target_alias_ / 无限定名为准；
            // source 的列：以 source_alias_ / 无限定名为准。
            bool left_is_target = cr_l->table_name.empty() ||
                                  cr_l->table_name == target_table_ ||
                                  (!target_alias_.empty() && cr_l->table_name == target_alias_);
            bool right_is_source = cr_r->table_name.empty() ||
                                   (!source_alias_.empty() && cr_r->table_name == source_alias_);
            if (left_is_target && right_is_source) {
                eq_target_col_ = cr_l->column_name;
                eq_source_col_ = cr_r->column_name;
            } else if (!cr_l->table_name.empty() &&
                       (!source_alias_.empty() && cr_l->table_name == source_alias_) &&
                       (cr_r->table_name.empty() ||
                        cr_r->table_name == target_table_ ||
                        (!target_alias_.empty() && cr_r->table_name == target_alias_))) {
                eq_target_col_ = cr_r->column_name;
                eq_source_col_ = cr_l->column_name;
            }
            if (!eq_target_col_.empty() && !eq_source_col_.empty()) {
                // 找 target 表上该列的下标。
                size_t target_col_idx = static_cast<size_t>(-1);
                if (info) {
                    for (size_t i = 0; i < info->columns.size(); ++i) {
                        if (info->columns[i].name == eq_target_col_) {
                            target_col_idx = i;
                            break;
                        }
                    }
                }
                if (target_col_idx != static_cast<size_t>(-1)) {
                    // 预扫描 target 建 hash
                    auto it = target_heap_->Begin();
                    while (it.HasNext()) {
                        Tuple cand = it.Next(target_column_types_);
                        if (!cand.GetRid().IsValid()) continue;
                        if (cand.ColumnCount() <= target_col_idx) continue;
                        std::string k = cand.GetValue(target_col_idx).ToString();
                        target_hash_[k].push_back(cand);
                    }
                    use_target_hash_ = true;
                }
            }
        }
    }
}

bool MergeExecutor::Next(Tuple* tuple) {
    if (executed_) return false;
    executed_ = true;
    if (!source_child_ || !target_heap_) {
        if (tuple) *tuple = Tuple({Value::MakeInt(0)});
        return false;
    }
    ExpressionEvaluator eval(combined_column_index_map_, context_, nullptr);
    Tuple source_row;
    while (source_child_->Next(&source_row)) {
        // Item #2 (perf)：如果启用了 target hash 索引，按 eq_source_col_ 的
        // 值做 O(1) 哈希探测；命中后只对桶内候选 target 行做 on_condition
        // 完整评估。否则走原来的 O(T) SeqScan。
        bool matched = false;
        RID matched_rid;
        Tuple matched_target_tuple;
        std::vector<Tuple> candidates;  // 本轮候选 target 行
        if (use_target_hash_) {
            // 找 source_row 上 eq_source_col_ 列的下标。source_row 只含
            // source 表的列；combined_cmap 中"<source_alias>.<col>"指向
            // target 列数 + i。需要把它映射回 source 表内的列下标 i。
            size_t source_col_idx = static_cast<size_t>(-1);
            std::string qk_source = source_alias_ + "." + eq_source_col_;
            auto it_cmap = combined_column_index_map_.find(qk_source);
            if (it_cmap == combined_column_index_map_.end()) {
                it_cmap = combined_column_index_map_.find(eq_source_col_);
            }
            if (it_cmap != combined_column_index_map_.end() &&
                it_cmap->second >= target_column_count_) {
                source_col_idx = it_cmap->second - target_column_count_;
            }
            if (source_col_idx < source_row.ColumnCount()) {
                std::string sk = source_row.GetValue(source_col_idx).ToString();
                auto it_h = target_hash_.find(sk);
                if (it_h != target_hash_.end()) candidates = it_h->second;
            }
        } else {
            auto it = target_heap_->Begin();
            while (it.HasNext()) {
                Tuple cand = it.Next(target_column_types_);
                if (!cand.GetRid().IsValid()) continue;
                candidates.push_back(cand);
            }
        }
        for (const auto& cand : candidates) {
            std::vector<Value> joined;
            joined.reserve(cand.ColumnCount() + source_row.ColumnCount());
            for (size_t i = 0; i < cand.ColumnCount(); ++i) {
                joined.push_back(cand.GetValue(i));
            }
            for (size_t i = 0; i < source_row.ColumnCount(); ++i) {
                joined.push_back(source_row.GetValue(i));
            }
            Tuple joined_t(std::move(joined));
            bool ok = true;
            if (on_condition_) {
                Value v = eval.Evaluate(on_condition_, joined_t);
                ok = !v.IsNull() && v.AsInt() != 0;
            }
            if (ok) {
                matched = true;
                matched_rid = cand.GetRid();
                matched_target_tuple = cand;
                break;
            }
        }
        if (matched) {
            if (!has_matched_update_) {
                // 没声明 MATCHED UPDATE 但有命中：跳过该 source 行。
                continue;
            }
            // 评估 SET 右侧（在 joined tuple 上求值）。
            std::vector<Value> new_values = matched_target_tuple.GetValues();
            for (const auto& kv : matched_assignments_) {
                auto it = target_column_index_map_.find(kv.first);
                if (it == target_column_index_map_.end()) {
                    throw CompilerException(ErrorStage::SEMANTIC,
                        "MERGE: unknown target column '" + kv.first + "'");
                }
                size_t idx = it->second;
                if (idx >= new_values.size()) continue;
                // 重新构造 joined tuple（target + source）以便 evaluate
                std::vector<Value> joined;
                joined.reserve(matched_target_tuple.ColumnCount() + source_row.ColumnCount());
                for (size_t i = 0; i < matched_target_tuple.ColumnCount(); ++i) {
                    joined.push_back(matched_target_tuple.GetValue(i));
                }
                for (size_t i = 0; i < source_row.ColumnCount(); ++i) {
                    joined.push_back(source_row.GetValue(i));
                }
                Tuple joined_t(std::move(joined));
                new_values[idx] = eval.Evaluate(kv.second, joined_t);
            }
            Tuple new_t(std::move(new_values));
            // 约束 / FK / 索引 / WAL —— 复用 UpdateExecutor 路径。
            {
                const TableInfo* info = context_->GetCatalog()->GetTable(target_table_);
                if (info) {
                    std::vector<Value> row_snapshot;
                    row_snapshot.reserve(new_t.ColumnCount());
                    for (size_t i = 0; i < new_t.ColumnCount(); ++i) {
                        row_snapshot.push_back(new_t.GetValue(i));
                    }
                    ValidateRowConstraints(context_->GetCatalog(), *info, target_heap_,
                                           row_snapshot, &matched_rid, context_);
                    CheckUniqueIndexes(context_->GetCatalog(), *info, row_snapshot, &matched_rid);
                    EnforceChildForeignKeys(context_->GetCatalog(), target_table_, row_snapshot);
                }
            }
            const TableInfo* info = context_->GetCatalog()->GetTable(target_table_);
            Transaction* txn = context_->GetTransaction();
            if (info != nullptr) {
                DeleteFromIndexes(context_->GetCatalog(), *info,
                                  matched_target_tuple.GetValues(), matched_rid, txn);
            }
            target_heap_->SetActiveTransaction(txn);
            // 同 UpdateExecutor：行增长时 UpdateTuple 会 delete+insert，RID
            // 会变；后续 InsertIntoIndexes 必须用新 RID，否则索引键指向
            // 旧 slot（已被墓碑化），下次 UPDATE 走 exclude_rid 校验失败。
            RID new_rid = matched_rid;
            bool ok = target_heap_->UpdateTuple(matched_rid, new_t,
                                                target_column_types_, &new_rid);
            target_heap_->SetActiveTransaction(nullptr);
            if (ok) {
                if (info != nullptr) {
                    InsertIntoIndexes(context_->GetCatalog(), *info,
                                      new_t.GetValues(), new_rid, txn);
                }
                ++affected_rows_;
            } else if (info != nullptr) {
                InsertIntoIndexes(context_->GetCatalog(), *info,
                                  matched_target_tuple.GetValues(), matched_rid, txn);
                throw CompilerException(ErrorStage::SEMANTIC,
                    "MERGE: heap write failed during MATCHED UPDATE");
            }
            continue;
        }
        // 未命中：走 NOT MATCHED INSERT。
        if (!has_not_matched_insert_) {
            // 没声明 INSERT 分支：跳过。
            continue;
        }
        const TableInfo* info = context_->GetCatalog()->GetTable(target_table_);
        if (!info) {
            throw CompilerException(ErrorStage::SEMANTIC,
                "MERGE: target table not found: " + target_table_);
        }
        std::vector<Value> row_values(info->columns.size());
        // 按 not_matched_columns 写到目标表对应列下；source 列引用走 combined cmap。
        for (size_t i = 0; i < not_matched_columns_.size(); ++i) {
            const auto& cn = not_matched_columns_[i];
            size_t target_idx = info->columns.size();
            bool found = false;
            for (size_t k = 0; k < info->columns.size(); ++k) {
                if (info->columns[k].name == cn) {
                    target_idx = k;
                    found = true;
                    break;
                }
            }
            if (!found) {
                throw CompilerException(ErrorStage::SEMANTIC,
                    "MERGE: unknown target column '" + cn + "'");
            }
            Value v = eval.Evaluate(not_matched_values_[i], source_row);
            row_values[target_idx] = CoerceToColumnType(v, info->columns[target_idx].data_type);
        }
        // 复用 InsertExecutor 风格的约束 + 写堆 + 索引同步。
        Tuple t(std::move(row_values));
        RID rid;
        {
            const TableInfo* info2 = context_->GetCatalog()->GetTable(target_table_);
            if (info2) {
                std::vector<Value> row_snapshot;
                row_snapshot.reserve(t.ColumnCount());
                for (size_t i = 0; i < t.ColumnCount(); ++i) {
                    row_snapshot.push_back(t.GetValue(i));
                }
                ValidateRowConstraints(context_->GetCatalog(), *info2, target_heap_,
                                       row_snapshot, nullptr, context_);
                CheckUniqueIndexes(context_->GetCatalog(), *info2, row_snapshot, nullptr);
                EnforceChildForeignKeys(context_->GetCatalog(), target_table_, row_snapshot);
            }
        }
        Transaction* txn = context_->GetTransaction();
        target_heap_->SetActiveTransaction(txn);
        if (!target_heap_->InsertTuple(t, &rid, target_column_types_)) {
            target_heap_->SetActiveTransaction(nullptr);
            throw CompilerException(ErrorStage::SEMANTIC,
                "MERGE: heap write failed during NOT MATCHED INSERT");
        }
        target_heap_->SetActiveTransaction(nullptr);
        if (info != nullptr) {
            InsertIntoIndexes(context_->GetCatalog(), *info, t.GetValues(), rid, txn);
        }
        ++affected_rows_;
    }
    if (tuple) *tuple = Tuple({Value::MakeInt(affected_rows_)});
    return false;
}

}  // namespace sqlcompiler
