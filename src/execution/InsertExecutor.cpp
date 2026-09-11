#include "execution/InsertExecutor.h"

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

// 将值强制转换为与列声明一致的类型，避免 INT 字面量被写入 FLOAT 列导致读取错位
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

// 评估列上的 DEFAULT 表达式。当前实现只接受字面量：
//   INTEGER / FLOAT / STRING / NULL_VALUE；遇到 FUNCTION_CALL / SUBQUERY 等
//   复杂形态时抛 "default expression not supported"。
//
// 为什么收紧到字面量：执行层在「行即将落地」时评估 DEFAULT，没有当前行的
// 列值可用，复杂表达式（哪怕是 CURRENT_TIMESTAMP）需要外部时钟或上下文，
// 故按任务说明显式拒收。
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
            // boolean 字面量不直接对应本实现的 ValueType，归一化为 INTEGER
            return Value::MakeInt(
                (lit->value != "0" && lit->value != "false" && lit->value != "FALSE") ? 1 : 0);
    }
    return Value::MakeNull();
}

// 替换 row_values 中「用户未提供」且列上有 DEFAULT 的位置。
// explicit_columns 为 INSERT 语句的列名列表（空表示按表定义列序插全部列）。
//   - 空列表：用户没声明列名，所有列都视为"已提供"，跳过 DEFAULT；
//     VALUES 路径中调用方按 row_exprs 数量等于 info.columns.size() 校验过。
//   - 非空列表：row_values[i] 为 NULL 且第 i 列不在 explicit_columns 中时
//     替换为 DEFAULT；用户在 explicit_columns 中显式给 NULL 时 row_values
//     也是 NULL，但该列在 explicit_columns 中，故本函数不会覆盖，保留
//     用户的 NULL 选择（符合 SQL 标准 DEFAULT 语义）。
void ApplyDefaults(const TableInfo& info,
                   const std::vector<std::string>& explicit_columns,
                   std::vector<Value>& row_values) {
    // 把 explicit_columns 转成「下标集合」便于 O(1) 判定。
    std::unordered_set<std::string> explicit_names;
    explicit_names.reserve(explicit_columns.size());
    for (const auto& c : explicit_columns) explicit_names.insert(c);
    for (size_t i = 0; i < info.columns.size() && i < row_values.size(); ++i) {
        if (!row_values[i].IsNull()) continue;
        if (!info.columns[i].default_expr) continue;
        // explicit_columns 为空时，按 SQL 标准视为用户按表定义列序显式提供；
        // 此时 DEFAULT 不应覆盖任何已有位置（包括 NULL），保持现有 row_values。
        if (explicit_columns.empty()) continue;
        // 列出现在 INSERT 列名列表里 → 用户已显式提供（即便给了 NULL），不覆盖
        if (explicit_names.count(info.columns[i].name) > 0) continue;
        Value v = EvaluateDefaultLiteral(info.columns[i].default_expr,
                                          info.columns[i].name);
        row_values[i] = CoerceToColumnType(v, info.columns[i].data_type);
    }
}

}  // namespace

InsertExecutor::InsertExecutor(ExecutionContext* context, std::string table_name,
                                std::vector<std::string> columns,
                                std::vector<std::vector<ExprPtr>> values_list)
    : Executor(context), table_name_(std::move(table_name)),
      columns_(std::move(columns)), values_list_(std::move(values_list)),
      current_row_(0) {
}

InsertExecutor::InsertExecutor(ExecutionContext* context, std::string table_name,
                                std::vector<std::string> columns,
                                PlanNodePtr query_plan)
    : Executor(context), table_name_(std::move(table_name)),
      columns_(std::move(columns)), current_row_(0) {
    if (query_plan) {
        ExecutionEngine engine(context_->GetCatalog());
        source_ = engine.BuildExecutor(query_plan, context_);
    }
}

void InsertExecutor::Init() {
    current_row_ = 0;
    if (source_) source_->Init();
}

bool InsertExecutor::InsertRow(const std::vector<Value>& row_values_in) {
    const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
    if (!info) {
        throw CompilerException(ErrorStage::SEMANTIC, "table not found: " + table_name_);
    }
    TableHeap* heap = context_->GetCatalog()->GetTableHeap(table_name_);
    if (!heap) {
        throw CompilerException(ErrorStage::SEMANTIC, "table heap missing: " + table_name_);
    }

    // 先复制到局部缓冲，避免就地修改入参
    std::vector<Value> row_values = row_values_in;
    if (row_values.size() < info->columns.size()) {
        row_values.resize(info->columns.size());
    }

    // AUTO_INCREMENT: 首列为 PRIMARY KEY 且未赋值时自动编号
    if (!info->columns.empty() && info->columns[0].is_primary_key) {
        size_t idx = 0;
        bool need_autoinc = row_values[idx].IsNull();
        if (!need_autoinc && row_values[idx].GetType() == ValueType::INTEGER &&
            columns_.size() > 0) {
            for (const auto& c : columns_) {
                if (c == info->columns[0].name) { need_autoinc = false; break; }
            }
        }
        if (need_autoinc) {
            std::vector<ValueType> schema;
            for (const auto& c : info->columns) {
                if (c.data_type == "INT" || c.data_type == "INTEGER" || c.data_type == "BIGINT")
                    schema.push_back(ValueType::INTEGER);
                else if (c.data_type == "FLOAT" || c.data_type == "DOUBLE" || c.data_type == "DECIMAL")
                    schema.push_back(ValueType::FLOAT);
                else
                    schema.push_back(ValueType::VARCHAR);
            }
            auto it = heap->Begin();
            int count = 0;
            while (it.HasNext()) {
                Tuple t = it.Next(schema);
                if (t.ColumnCount() > 0) ++count;
            }
            row_values[idx] = Value::MakeInt(count + 1);
        }
    }

    // BEFORE INSERT 触发器：把当前候选行作为 NEW 喂给触发器；OLD 在 INSERT 上
    // 各列为 NULL。触发器可能改写 NEW.col 字段；改写后的 row_values 用于实际
    // 落盘。AFTER INSERT 触发器在堆写入完成后触发（FireAfter 调用见本函数末尾）。
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
    // Phase A：把当前事务挂到堆/索引上，让写路径抓 undo。
    heap->SetActiveTransaction(context_->GetTransaction());
    if (!heap->InsertTuple(t, &rid, col_types)) {
        heap->SetActiveTransaction(nullptr);
        throw CompilerException(ErrorStage::SEMANTIC,
            "INSERT failed (no space?)");
    }
    heap->SetActiveTransaction(nullptr);
    InsertIntoIndexes(context_->GetCatalog(), *info, t.GetValues(), rid,
                      context_->GetTransaction());
    // AFTER INSERT 触发器：仅日志（按任务文档约定保留为 no-op）。
    TriggerExecutor::FireAfter(context_->GetCatalog(), table_name_,
                                TriggerEvent::INSERT);
    return true;
}

bool InsertExecutor::Next(Tuple* tuple) {
    // SELECT 路径：每次 Next 从 source_ 拉一行，按列映射写入目标表。
    if (source_) {
        Tuple src;
        if (!source_->Next(&src)) return false;
        const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
        if (!info) {
            throw CompilerException(ErrorStage::SEMANTIC, "table not found: " + table_name_);
        }
        // ProjectExecutor 的输出布局是 [select_values ++ underlying_tuple]，
        // SELECT * 的特例直接返回 underlying（不含前缀）。无论哪种，前 N 列
        // 都已经是 select_list 求值后的"按 SELECT 顺序"的值，N == select_list 大小。
        // 这里统一按"源行前 N 列"取数：columns_ 为空时 N = 目标列数；否则 N = columns_ 大小。
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
            // 目标表上未被源覆盖、且挂 DEFAULT 的列填上默认值。
            ApplyDefaults(*info, columns_, row_values);
        }
        InsertRow(row_values);
        ++current_row_;
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
    ExpressionEvaluator eval(idx_map);

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
        // columns_ 为空视为用户按表定义列序显式提供全部列，DEFAULT 不覆盖。
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
        // 列出列名 → 未列出的列若挂 DEFAULT 则用默认值填。
        ApplyDefaults(*info, columns_, row_values);
    }

    InsertRow(row_values);
    ++current_row_;
    if (tuple) {
        *tuple = Tuple({Value::MakeInt(1)});
    }
    return true;
}

}  // namespace sqlcompiler