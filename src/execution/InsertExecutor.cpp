#include "execution/InsertExecutor.h"

#include "common/Error.h"
#include "execution/ConstraintChecker.h"
#include "execution/ExecutionEngine.h"
#include "execution/IndexMaintenance.h"
#include "execution/ExpressionEvaluator.h"

#include <unordered_map>

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
                               row_snapshot, nullptr);
        CheckUniqueIndexes(context_->GetCatalog(), *info, row_snapshot, nullptr);
    }
    if (!heap->InsertTuple(t, &rid, col_types)) {
        throw CompilerException(ErrorStage::SEMANTIC,
            "INSERT failed (no space?)");
    }
    InsertIntoIndexes(context_->GetCatalog(), *info, t.GetValues(), rid);
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
    }

    InsertRow(row_values);
    ++current_row_;
    if (tuple) {
        *tuple = Tuple({Value::MakeInt(1)});
    }
    return true;
}

}  // namespace sqlcompiler