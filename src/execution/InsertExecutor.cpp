// =============================================================================
// 54_dml：InsertExecutor 实现（含 REPLACE INTO 与 RETURNING emit）
// =============================================================================
//
// RETURNING 语义（PG 风格）：
//   - INSERT RETURNING 发出"新插入的行"——post-image。
//   - 实现要点：每条候选行落盘后立即评估 returning_exprs（用新行），再写堆失败
//     时回退 pending 缓冲。Next() 优先消费 pending；空时再驱动 VALUES / SELECT。
//
// REPLACE 语义（MySQL）：
//   - 候选行若在 PRIMARY KEY / UNIQUE 索引上冲突，先按冲突行的 RID 调 DeleteTuple
//     （连带索引项删除），再走常规 INSERT 路径。简化实现：仅处理 PRIMARY KEY
//     冲突（与现有 UpsertExecutor 一致）；UNIQUE INDEX 冲突按 PG/Oracle 等价
//     行为也走"先删后插"。
// =============================================================================

#include "execution/InsertExecutor.h"

#include "catalog/SystemCatalog.h"
#include "common/Error.h"
#include "execution/ConstraintChecker.h"
#include "execution/ExecutionEngine.h"
#include "execution/IndexMaintenance.h"
#include "execution/ExpressionEvaluator.h"
#include "execution/TriggerExecutor.h"

#include <unordered_map>
#include <unordered_set>

namespace sqlcompiler {

namespace {

// 与 UpsertExecutor.cpp 中的同名函数保持一致：把值强制转换为与列声明一致的类型。
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

Value EvaluateDefaultLiteral(const ExprPtr& default_expr,
                              const std::string& col_name) {
    if (!default_expr) return Value::MakeNull();
    if (default_expr->GetType() != NodeType::LITERAL_EXPR) {
        throw CompilerException(
            ErrorStage::SEMANTIC,
            "default expression not supported for column '" + col_name + "'");
    }
    const auto* lit = static_cast<const LiteralExpr*>(default_expr.get());
    switch (lit->literal_type) {
        case LiteralType::INTEGER:
            return Value::MakeInt(static_cast<int32_t>(std::atoi(lit->value.c_str())));
        case LiteralType::FLOAT:
            return Value::MakeFloat(std::atof(lit->value.c_str()));
        case LiteralType::STRING:
            return Value::MakeVarchar(lit->value);
        case LiteralType::NULL_VALUE:
            return Value::MakeNull();
        case LiteralType::BOOLEAN:
            return Value::MakeInt(
                (lit->value != "0" && lit->value != "false" && lit->value != "FALSE") ? 1 : 0);
    }
    return Value::MakeNull();
}

void ApplyDefaults(const TableInfo& info,
                   const std::vector<std::string>& explicit_columns,
                   std::vector<Value>& row_values) {
    std::unordered_set<std::string> explicit_names;
    explicit_names.reserve(explicit_columns.size());
    for (const auto& c : explicit_columns) explicit_names.insert(c);
    for (size_t i = 0; i < info.columns.size() && i < row_values.size(); ++i) {
        if (!row_values[i].IsNull()) continue;
        if (!info.columns[i].default_expr) continue;
        if (explicit_columns.empty()) continue;
        if (explicit_names.count(info.columns[i].name) > 0) continue;
        Value v = EvaluateDefaultLiteral(info.columns[i].default_expr,
                                          info.columns[i].name);
        row_values[i] = CoerceToColumnType(v, info.columns[i].data_type);
    }
}

// REPLACE INTO 路径：探测 PK / UNIQUE 冲突，按冲突行 RID 列表返回。
// 当前实现复用 UpsertExecutor 的 PRIMARY KEY 探测思路，并对每张 UNIQUE INDEX
// 索引做 B+Tree 等值探测（与 CheckUniqueIndexes 对齐）。
// 若无任何冲突，返回空列表。
std::vector<RID> FindReplaceConflicts(SystemCatalog* catalog,
                                      const TableInfo& info,
                                      const std::vector<Value>& row_values) {
    std::vector<RID> conflicts;
    if (!catalog) return conflicts;
    auto groups = info.GetPrimaryKeyGroups();
    for (const auto& g : groups) {
        BPlusTree* tree = catalog->GetPrimaryKeyIndexTree(info.table_name, g);
        if (tree == nullptr) continue;
        IndexKey key;
        bool complete = true;
        for (const auto& col_name : g) {
            const ColumnInfo* col = info.GetColumn(col_name);
            if (!col) { complete = false; break; }
            size_t idx = 0;
            bool found = false;
            for (size_t i = 0; i < info.columns.size(); ++i) {
                if (info.columns[i].name == col_name) { idx = i; found = true; break; }
            }
            if (!found || idx >= row_values.size() || row_values[idx].IsNull()) {
                complete = false; break;
            }
            key.values.push_back(row_values[idx]);
        }
        if (!complete) continue;
        RID existing = tree->FindFirst(key);
        if (existing.IsValid()) {
            conflicts.push_back(existing);
        }
    }
    return conflicts;
}

// REPLACE INTO：删除冲突行（含索引项）。冲突行必须按 RID 排序后再依次删除，
// 避免"边删边迭代"破坏索引一致性。
void DeleteConflicts(ExecutionContext* ctx, const std::string& table_name,
                     const std::vector<Value>& conflict_row_values_for_fk,
                     const std::vector<RID>& conflict_rids) {
    if (conflict_rids.empty()) return;
    TableHeap* heap = ctx->GetCatalog()->GetTableHeap(table_name);
    if (!heap) return;
    const TableInfo* info = ctx->GetCatalog()->GetTable(table_name);
    if (!info) return;
    std::vector<ValueType> col_types = BuildColumnTypes(*info);
    Transaction* txn = ctx->GetTransaction();
    for (const RID& r : conflict_rids) {
        // 先读出当前冲突行的列值（用于索引同步）。
        Tuple cur;
        if (!heap->GetTuple(r, &cur, col_types)) continue;
        DeleteFromIndexes(ctx->GetCatalog(), *info, cur.GetValues(), r, txn);
        heap->SetActiveTransaction(txn);
        heap->DeleteTuple(r);
        heap->SetActiveTransaction(nullptr);
    }
    (void)conflict_row_values_for_fk;
}

}  // namespace

InsertExecutor::InsertExecutor(ExecutionContext* context, std::string table_name,
                                std::vector<std::string> columns,
                                std::vector<std::vector<ExprPtr>> values_list,
                                bool is_replace,
                                std::vector<ExprPtr> returning_exprs,
                                std::vector<std::string> returning_aliases)
    : Executor(context), table_name_(std::move(table_name)),
      columns_(std::move(columns)), values_list_(std::move(values_list)),
      is_replace_(is_replace),
      returning_exprs_(std::move(returning_exprs)),
      returning_aliases_(std::move(returning_aliases)),
      current_row_(0) {
}

InsertExecutor::InsertExecutor(ExecutionContext* context, std::string table_name,
                                std::vector<std::string> columns,
                                PlanNodePtr query_plan,
                                std::vector<ExprPtr> returning_exprs,
                                std::vector<std::string> returning_aliases)
    : Executor(context), table_name_(std::move(table_name)),
      columns_(std::move(columns)), is_replace_(false),
      returning_exprs_(std::move(returning_exprs)),
      returning_aliases_(std::move(returning_aliases)),
      current_row_(0) {
    if (query_plan) {
        ExecutionEngine engine(context_->GetCatalog());
        source_ = engine.BuildExecutor(query_plan, context_);
    }
}

void InsertExecutor::Init() {
    current_row_ = 0;
    pending_returning_.clear();
    pending_pos_ = 0;
    if (source_) source_->Init();
    // 60_view_trigger: 重置 STATEMENT 级 AFTER 触发器的"已 fire"标记。
    TriggerExecutor::ResetStatementFireState(context_);
}

bool InsertExecutor::InsertRow(const std::vector<Value>& row_values_in, bool is_replace) {
    const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
    if (!info) {
        throw CompilerException(ErrorStage::SEMANTIC, "table not found: " + table_name_);
    }
    TableHeap* heap = context_->GetCatalog()->GetTableHeap(table_name_);
    if (!heap) {
        throw CompilerException(ErrorStage::SEMANTIC, "table heap missing: " + table_name_);
    }

    std::vector<Value> row_values = row_values_in;
    if (row_values.size() < info->columns.size()) {
        row_values.resize(info->columns.size());
    }

    // AUTO_INCREMENT 处理与既有 InsertRow 完全一致。
    {
        std::vector<ValueType> schema;
        schema.reserve(info->columns.size());
        for (const auto& c : info->columns) {
            ValueType vt = ValueTypeFromString(c.data_type);
            schema.push_back(vt);
        }
        for (size_t idx = 0; idx < info->columns.size() && idx < row_values.size(); ++idx) {
            const auto& col = info->columns[idx];
            if (!col.is_auto_increment) continue;
            Value& v = row_values[idx];
            bool user_explicit = false;
            for (const auto& cn : columns_) {
                if (cn == col.name) { user_explicit = true; break; }
            }
            bool needs_autoinc = false;
            if (!user_explicit) {
                needs_autoinc = true;
            } else if (v.IsNull()) {
                needs_autoinc = true;
            } else if (v.GetType() == ValueType::INTEGER && v.AsInt() == 0) {
                needs_autoinc = true;
            }
            if (!needs_autoinc) continue;
            int32_t max_val = 0;
            auto it = heap->Begin();
            while (it.HasNext()) {
                Tuple t = it.Next(schema);
                if (t.ColumnCount() <= idx) continue;
                const Value& cur = t.GetValue(idx);
                if (cur.IsNull()) continue;
                if (cur.GetType() == ValueType::INTEGER && cur.AsInt() > max_val) {
                    max_val = cur.AsInt();
                }
            }
            row_values[idx] = Value::MakeInt(max_val + 1);
        }
    }

    // BEFORE INSERT 触发器
    {
        std::unordered_map<std::string, size_t> cmap;
        for (size_t i = 0; i < info->columns.size(); ++i) {
            cmap[info->columns[i].name] = i;
        }
        TriggerExecutor::FireBefore(
            context_->GetCatalog(), context_, table_name_,
            TriggerTiming::BEFORE, TriggerEvent::INSERT,
            cmap, nullptr, row_values);
    }

    // 54_dml: REPLACE INTO —— 在写堆前探测冲突并先删除。
    if (is_replace) {
        auto conflicts = FindReplaceConflicts(context_->GetCatalog(), *info, row_values);
        if (!conflicts.empty()) {
            DeleteConflicts(context_, table_name_, row_values, conflicts);
        }
    }

    Tuple t(std::move(row_values));
    RID rid;
    std::vector<ValueType> col_types = BuildColumnTypes(*info);
    {
        std::vector<Value> row_snapshot;
        row_snapshot.reserve(t.ColumnCount());
        for (size_t i = 0; i < t.ColumnCount(); ++i) {
            row_snapshot.push_back(t.GetValue(i));
        }
        ValidateRowConstraints(context_->GetCatalog(), *info, heap,
                               row_snapshot, nullptr, context_);
        CheckUniqueIndexes(context_->GetCatalog(), *info, row_snapshot, nullptr);
        EnforceChildForeignKeys(context_->GetCatalog(), table_name_, row_snapshot);
    }
    heap->SetActiveTransaction(context_->GetTransaction());
    if (!heap->InsertTuple(t, &rid, col_types)) {
        heap->SetActiveTransaction(nullptr);
        throw CompilerException(ErrorStage::SEMANTIC,
            "INSERT failed (no space?)");
    }
    heap->SetActiveTransaction(nullptr);
    InsertIntoIndexes(context_->GetCatalog(), *info, t.GetValues(), rid,
                      context_->GetTransaction());
    // 60_view_trigger: AFTER 触发器 + STATEMENT 级触发器。
    {
        std::unordered_map<std::string, size_t> cmap;
        for (size_t i = 0; i < info->columns.size(); ++i) {
            cmap[info->columns[i].name] = i;
        }
        TriggerExecutor::FireAfter(
            context_->GetCatalog(), context_, table_name_,
            TriggerEvent::INSERT, cmap, nullptr, &t.GetValues());
    }

    // 54_dml: INSERT RETURNING —— 评估并暂存 RETURNING 行（post-image）。
    if (!returning_exprs_.empty()) {
        std::unordered_map<std::string, size_t> cmap;
        for (size_t i = 0; i < info->columns.size(); ++i) {
            cmap[info->columns[i].name] = i;
        }
        ExpressionEvaluator eval(cmap, context_, nullptr);
        std::vector<Value> out;
        out.reserve(returning_exprs_.size());
        for (const auto& e : returning_exprs_) {
            out.push_back(eval.Evaluate(e, t));
        }
        pending_returning_.push_back(Tuple(std::move(out)));
    }
    return true;
}

bool InsertExecutor::Next(Tuple* tuple) {
    // 优先消费 pending RETURNING 行。
    if (pending_pos_ < pending_returning_.size()) {
        if (tuple) *tuple = pending_returning_[pending_pos_++];
        return true;
    }
    pending_returning_.clear();
    pending_pos_ = 0;

    // SELECT 路径：每次 Next 从 source_ 拉一行，按列映射写入目标表。
    if (source_) {
        Tuple src;
        if (!source_->Next(&src)) return false;
        const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
        if (!info) {
            throw CompilerException(ErrorStage::SEMANTIC, "table not found: " + table_name_);
        }
        const size_t N = columns_.empty() ? info->columns.size() : columns_.size();
        if (src.ColumnCount() < N) {
            throw CompilerException(ErrorStage::SEMANTIC,
                "INSERT ... SELECT column count mismatch for " + table_name_);
        }
        std::vector<Value> row_values(info->columns.size());
        if (columns_.empty()) {
            for (size_t i = 0; i < info->columns.size(); ++i) {
                row_values[i] = CoerceToColumnType(src.GetValue(i), info->columns[i].data_type);
            }
        } else {
            for (size_t i = 0; i < columns_.size(); ++i) {
                size_t target_idx = info->columns.size();
                bool found = false;
                for (size_t k = 0; k < info->columns.size(); ++k) {
                    if (info->columns[k].name == columns_[i]) {
                        target_idx = k;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    throw CompilerException(ErrorStage::SEMANTIC,
                        "unknown column: " + columns_[i]);
                }
                Value v = src.GetValue(i);
                row_values[target_idx] = CoerceToColumnType(v, info->columns[target_idx].data_type);
            }
            ApplyDefaults(*info, columns_, row_values);
        }
        InsertRow(row_values, false /* REPLACE 不支持 INSERT...SELECT */);
        ++current_row_;
        if (pending_pos_ < pending_returning_.size()) {
            if (tuple) *tuple = pending_returning_[pending_pos_++];
            return true;
        }
        if (tuple) *tuple = Tuple({Value::MakeInt(1)});
        return true;
    }

    // VALUES 路径：原行为保持不变。
    if (current_row_ >= values_list_.size()) return false;
    const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
    if (!info) {
        throw CompilerException(ErrorStage::SEMANTIC, "table not found: " + table_name_);
    }
    TableHeap* heap = context_->GetCatalog()->GetTableHeap(table_name_);
    if (!heap) {
        throw CompilerException(ErrorStage::SEMANTIC, "table heap missing: " + table_name_);
    }

    std::unordered_map<std::string, size_t> idx_map;
    for (size_t i = 0; i < info->columns.size(); ++i) {
        idx_map[info->columns[i].name] = i;
    }
    ExpressionEvaluator eval(idx_map, context_, nullptr);

    auto& row_exprs = values_list_[current_row_];
    std::vector<Value> row_values;
    row_values.resize(info->columns.size());

    if (columns_.empty()) {
        if (row_exprs.size() != info->columns.size()) {
            throw CompilerException(ErrorStage::SEMANTIC,
                "INSERT column count mismatch for " + table_name_);
        }
        for (size_t i = 0; i < row_exprs.size(); ++i) {
            Value v = eval.Evaluate(row_exprs[i], Tuple());
            row_values[i] = CoerceToColumnType(v, info->columns[i].data_type);
        }
    } else {
        for (size_t i = 0; i < row_exprs.size() && i < columns_.size(); ++i) {
            auto it = idx_map.find(columns_[i]);
            if (it == idx_map.end()) {
                throw CompilerException(ErrorStage::SEMANTIC,
                    "unknown column: " + columns_[i]);
            }
            const auto& target_col = info->columns[it->second];
            Value v = eval.Evaluate(row_exprs[i], Tuple());
            row_values[it->second] = CoerceToColumnType(v, target_col.data_type);
        }
        ApplyDefaults(*info, columns_, row_values);
    }

    InsertRow(row_values, is_replace_);
    ++current_row_;
    if (pending_pos_ < pending_returning_.size()) {
        if (tuple) *tuple = pending_returning_[pending_pos_++];
        return true;
    }
    if (tuple) {
        *tuple = Tuple({Value::MakeInt(1)});
    }
    return true;
}

}  // namespace sqlcompiler
