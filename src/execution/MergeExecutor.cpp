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
        // 在 target 表上做一次完整 SeqScan，按 on_condition 寻找第一条命中。
        bool matched = false;
        RID matched_rid;
        Tuple matched_target_tuple;
        {
            auto it = target_heap_->Begin();
            while (it.HasNext()) {
                Tuple cand = it.Next(target_column_types_);
                if (!cand.GetRid().IsValid()) continue;
                // 拼成 "target + source" 的 joined tuple 以便 on_condition 评估。
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
            bool ok = target_heap_->UpdateTuple(matched_rid, new_t, target_column_types_);
            target_heap_->SetActiveTransaction(nullptr);
            if (ok) {
                if (info != nullptr) {
                    InsertIntoIndexes(context_->GetCatalog(), *info,
                                      new_t.GetValues(), matched_rid, txn);
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
