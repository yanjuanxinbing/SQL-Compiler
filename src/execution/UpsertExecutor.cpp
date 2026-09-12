// =============================================================================
// 43_upsert：ON DUPLICATE KEY UPDATE 执行器实现
// =============================================================================
// 范围与限制详见 include/execution/UpsertExecutor.h 文件头注释。
// =============================================================================

#include "execution/UpsertExecutor.h"

#include "common/Error.h"
#include "execution/ConstraintChecker.h"
#include "execution/ExpressionEvaluator.h"
#include "execution/IndexMaintenance.h"

#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sqlcompiler {

namespace {

// 与 InsertExecutor.cpp 私有命名空间里的同名函数保持一致：把值强转到列声明类型。
// 这里不复用 InsertExecutor 的匿名命名空间实现（不可跨 TU 访问）。
Value CoerceToColumnType(const Value& v, const std::string& col_type) {
    std::string up;
    for (char c : col_type) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (up == "INT" || up == "INTEGER" || up == "BIGINT") {
        if (v.IsNull()) return v;
        if (v.GetType() == ValueType::INTEGER) return v;
        if (v.GetType() == ValueType::FLOAT) return Value::MakeInt(static_cast<int32_t>(v.AsFloat()));
        if (v.GetType() == ValueType::VARCHAR) {
            try { return Value::MakeInt(static_cast<int32_t>(std::stoi(v.AsVarchar()))); } catch (...) { return Value::MakeInt(0); }
        }
    } else if (up == "FLOAT" || up == "DOUBLE" || up == "DECIMAL") {
        if (v.IsNull()) return v;
        if (v.GetType() == ValueType::FLOAT) return v;
        if (v.GetType() == ValueType::INTEGER) return Value::MakeFloat(static_cast<double>(v.AsInt()));
        if (v.GetType() == ValueType::VARCHAR) {
            try { return Value::MakeFloat(std::stod(v.AsVarchar())); } catch (...) { return Value::MakeFloat(0.0); }
        }
    }
    return v;
}

// 评估列上的 DEFAULT 表达式：仅支持字面量。详见 InsertExecutor.cpp 同名实现。
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

// 与 InsertExecutor::ApplyDefaults 同语义：仅在显式未列出该列、且列上有 DEFAULT 时填充。
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

}  // namespace

UpsertExecutor::UpsertExecutor(ExecutionContext* context, std::string table_name,
                                std::vector<std::string> columns,
                                std::vector<std::vector<ExprPtr>> values_list,
                                std::vector<std::pair<std::string, ExprPtr>> upsert_assignments,
                                std::vector<ExprPtr> returning_exprs,
                                std::vector<std::string> returning_aliases)
    : Executor(context), table_name_(std::move(table_name)),
      columns_(std::move(columns)),
      values_list_(std::move(values_list)),
      upsert_assignments_(std::move(upsert_assignments)),
      returning_exprs_(std::move(returning_exprs)),
      returning_aliases_(std::move(returning_aliases)) {
}

void UpsertExecutor::Init() {
    executed_ = false;
    affected_rows_ = 0;
    pending_returning_.clear();
    pending_pos_ = 0;
    const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
    if (info) {
        column_types_.clear();
        column_types_.reserve(info->columns.size());
        for (const auto& c : info->columns) {
            column_types_.push_back(ValueTypeFromString(c.data_type));
        }
        column_index_map_.clear();
        column_index_map_.reserve(info->columns.size());
        for (size_t i = 0; i < info->columns.size(); ++i) {
            column_index_map_[info->columns[i].name] = i;
        }
    }
}

void UpsertExecutor::PrepareCandidateRow(std::vector<Value>& row_values) {
    const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
    if (!info) {
        throw CompilerException(ErrorStage::SEMANTIC, "table not found: " + table_name_);
    }
    if (row_values.size() < info->columns.size()) {
        row_values.resize(info->columns.size());
    }
    // AUTO_INCREMENT：第一列为 PRIMARY KEY 且未赋值时按当前行数 + 1 自动编号。
    if (!info->columns.empty() && info->columns[0].is_primary_key) {
        size_t idx = 0;
        bool need_autoinc = row_values[idx].IsNull();
        if (!need_autoinc && row_values[idx].GetType() == ValueType::INTEGER &&
            !columns_.empty()) {
            for (const auto& c : columns_) {
                if (c == info->columns[0].name) { need_autoinc = false; break; }
            }
        }
        if (need_autoinc) {
            TableHeap* heap = context_->GetCatalog()->GetTableHeap(table_name_);
            auto it = heap->Begin();
            int count = 0;
            while (it.HasNext()) {
                Tuple t = it.Next(column_types_);
                if (t.ColumnCount() > 0) ++count;
            }
            row_values[idx] = Value::MakeInt(count + 1);
        }
    }
    // ApplyDefaults 已在 InsertExecutor::InsertRow 里走过；这里也覆盖同名 DEFAULT 列，
    // 保证冲突路径下"用户没提供的列也能拿到 DEFAULT"语义一致。
    ApplyDefaults(*info, columns_, row_values);
}

bool UpsertExecutor::HasPrimaryKeyConflict(const std::vector<Value>& row_values,
                                            RID* existing_rid_out) const {
    const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
    if (!info) return false;
    auto groups = info->GetPrimaryKeyGroups();
    if (groups.empty()) return false;
    SystemCatalog* catalog = context_->GetCatalog();
    for (const auto& g : groups) {
        BPlusTree* tree = catalog->GetPrimaryKeyIndexTree(table_name_, g);
        if (tree == nullptr) continue;  // 没索引的 PK 组在 ValidateRowConstraints 全扫兜底
        IndexKey key;
        bool complete = true;
        for (const auto& col_name : g) {
            const ColumnInfo* col = info->GetColumn(col_name);
            if (col == nullptr) { complete = false; break; }
            size_t idx = 0;
            bool found = false;
            for (size_t i = 0; i < info->columns.size(); ++i) {
                if (info->columns[i].name == col_name) { idx = i; found = true; break; }
            }
            if (!found || idx >= row_values.size() || row_values[idx].IsNull()) {
                complete = false;
                break;
            }
            key.values.push_back(row_values[idx]);
        }
        if (!complete) continue;
        RID existing = tree->FindFirst(key);
        if (existing.IsValid()) {
            if (existing_rid_out) *existing_rid_out = existing;
            return true;
        }
    }
    return false;
}

void UpsertExecutor::InsertCandidateRow(std::vector<Value>& row_values) {
    // 与 InsertExecutor::InsertRow 完全一致的写堆顺序：
    //   1) 校验 NOT NULL / 长度 / CHECK
    //   2) 主键唯一性（无索引的 PK 组在这里全扫兜底）
    //   3) 写堆
    //   4) 写索引
    const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
    if (!info) {
        throw CompilerException(ErrorStage::SEMANTIC, "table not found: " + table_name_);
    }
    TableHeap* heap = context_->GetCatalog()->GetTableHeap(table_name_);
    if (!heap) {
        throw CompilerException(ErrorStage::SEMANTIC, "table heap missing: " + table_name_);
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
    }
    // Phase A：把当前事务挂到堆/索引上。
    Transaction* txn = context_->GetTransaction();
    heap->SetActiveTransaction(txn);
    if (!heap->InsertTuple(t, &rid, col_types)) {
        heap->SetActiveTransaction(nullptr);
        throw CompilerException(ErrorStage::SEMANTIC,
            "INSERT failed (no space?)");
    }
    heap->SetActiveTransaction(nullptr);
    InsertIntoIndexes(context_->GetCatalog(), *info, t.GetValues(), rid, txn);
    EmitReturning(t);
}

void UpsertExecutor::UpdateConflictingRow(const std::vector<Value>& candidate_values,
                                           const RID& existing_rid) {
    const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
    if (!info) {
        throw CompilerException(ErrorStage::SEMANTIC, "table not found: " + table_name_);
    }
    TableHeap* heap = context_->GetCatalog()->GetTableHeap(table_name_);
    if (!heap) {
        throw CompilerException(ErrorStage::SEMANTIC, "table heap missing: " + table_name_);
    }

    Tuple cur;
    if (!heap->GetTuple(existing_rid, &cur, column_types_)) {
        throw CompilerException(ErrorStage::SEMANTIC,
            "ON DUPLICATE KEY UPDATE: failed to read existing row");
    }

    // 把候选行按表 schema 投影成 column_name → Value 的 binding，供赋值右侧的
    // VALUES(col) 引用使用。其它列引用（无 VALUES 前缀）由 ExpressionEvaluator 在
    // column_index_map_ 里找，回到 cur。
    std::unordered_map<std::string, Value> values_bind;
    for (size_t i = 0; i < info->columns.size() && i < candidate_values.size(); ++i) {
        values_bind[info->columns[i].name] = candidate_values[i];
    }
    const std::unordered_map<std::string, Value>* saved_bind =
        context_->GetUpsertValuesBind();
    context_->SetUpsertValuesBind(&values_bind);
    ExpressionEvaluator eval(column_index_map_, context_, nullptr);

    std::vector<Value> new_values = cur.GetValues();
    for (const auto& kv : upsert_assignments_) {
        auto it = column_index_map_.find(kv.first);
        if (it == column_index_map_.end()) {
            throw CompilerException(ErrorStage::SEMANTIC,
                "ON DUPLICATE KEY UPDATE: unknown column '" + kv.first + "'");
        }
        size_t idx = it->second;
        if (idx >= new_values.size()) continue;
        new_values[idx] = CoerceToColumnType(
            eval.Evaluate(kv.second, cur), info->columns[idx].data_type);
    }
    // 恢复绑定
    context_->SetUpsertValuesBind(saved_bind);

    Tuple new_t(std::move(new_values));
    // 同样在写堆前走一遍约束校验：CHECK 可能因为 update 改坏。
    {
        std::vector<Value> row_snapshot;
        row_snapshot.reserve(new_t.ColumnCount());
        for (size_t i = 0; i < new_t.ColumnCount(); ++i) {
            row_snapshot.push_back(new_t.GetValue(i));
        }
        ValidateRowConstraints(context_->GetCatalog(), *info, heap,
                               row_snapshot, &existing_rid, context_);
        CheckUniqueIndexes(context_->GetCatalog(), *info, row_snapshot, &existing_rid);
    }
    // Phase A：把当前事务挂到堆/索引上。
    Transaction* txn = context_->GetTransaction();
    DeleteFromIndexes(context_->GetCatalog(), *info, cur.GetValues(), existing_rid, txn);
    heap->SetActiveTransaction(txn);
    bool ok = heap->UpdateTuple(existing_rid, new_t, column_types_);
    heap->SetActiveTransaction(nullptr);
    if (ok) {
        InsertIntoIndexes(context_->GetCatalog(), *info,
                          new_t.GetValues(), existing_rid, txn);
        EmitReturning(new_t);
    } else {
        // 写堆失败：把刚摘掉的旧键放回去，避免索引凭空少一条
        InsertIntoIndexes(context_->GetCatalog(), *info, cur.GetValues(), existing_rid, txn);
        throw CompilerException(ErrorStage::SEMANTIC,
            "ON DUPLICATE KEY UPDATE: heap write failed");
    }
}

void UpsertExecutor::EmitReturning(const Tuple& row) {
    if (returning_exprs_.empty()) return;
    ExpressionEvaluator eval(column_index_map_, context_, nullptr);
    std::vector<Value> out;
    out.reserve(returning_exprs_.size());
    for (const auto& e : returning_exprs_) {
        out.push_back(eval.Evaluate(e, row));
    }
    pending_returning_.push_back(Tuple(std::move(out)));
}

bool UpsertExecutor::Next(Tuple* tuple) {
    // 优先消费 pending RETURNING 行。
    if (pending_pos_ < pending_returning_.size()) {
        if (tuple) *tuple = pending_returning_[pending_pos_++];
        return true;
    }
    pending_returning_.clear();
    pending_pos_ = 0;
    if (executed_) return false;
    executed_ = true;

    for (const auto& row_exprs : values_list_) {
        // 1) 评估 VALUES 中的表达式，得到与表 schema 同长的原始值序列
        std::vector<Value> row_values;
        if (columns_.empty()) {
            // 按表列序提供全部列
            if (row_exprs.size() != column_index_map_.size()) {
                throw CompilerException(ErrorStage::SEMANTIC,
                    "INSERT column count mismatch for " + table_name_);
            }
            row_values.resize(row_exprs.size());
            ExpressionEvaluator eval(column_index_map_);
            for (size_t i = 0; i < row_exprs.size(); ++i) {
                Value v = eval.Evaluate(row_exprs[i], Tuple());
                row_values[i] = CoerceToColumnType(v,
                    context_->GetCatalog()->GetTable(table_name_)->columns[i].data_type);
            }
        } else {
            const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
            row_values.resize(info->columns.size());
            ExpressionEvaluator eval(column_index_map_);
            for (size_t i = 0; i < row_exprs.size() && i < columns_.size(); ++i) {
                auto it = column_index_map_.find(columns_[i]);
                if (it == column_index_map_.end()) {
                    throw CompilerException(ErrorStage::SEMANTIC,
                        "unknown column: " + columns_[i]);
                }
                const auto& target_col = info->columns[it->second];
                Value v = eval.Evaluate(row_exprs[i], Tuple());
                row_values[it->second] = CoerceToColumnType(v, target_col.data_type);
            }
            ApplyDefaults(*info, columns_, row_values);
        }
        // 2) DEFAULT / AUTO_INCREMENT 补齐
        PrepareCandidateRow(row_values);
        // 3) 探测主键冲突
        RID existing_rid;
        if (HasPrimaryKeyConflict(row_values, &existing_rid)) {
            UpdateConflictingRow(row_values, existing_rid);
        } else {
            InsertCandidateRow(row_values);
        }
        ++affected_rows_;
    }

    // 至少消费一次 pending_returning_（一次循环可能产生多行）。
    if (pending_pos_ < pending_returning_.size()) {
        if (tuple) *tuple = pending_returning_[pending_pos_++];
        return true;
    }
    if (tuple) *tuple = Tuple({Value::MakeInt(affected_rows_)});
    return false;
}

}  // namespace sqlcompiler