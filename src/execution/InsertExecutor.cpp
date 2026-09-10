#include "execution/InsertExecutor.h"

#include "common/Error.h"
#include "execution/ExpressionEvaluator.h"

#include <unordered_map>

namespace sqlcompiler {

InsertExecutor::InsertExecutor(ExecutionContext* context, std::string table_name,
                                std::vector<std::string> columns,
                                std::vector<std::vector<ExprPtr>> values_list)
    : Executor(context), table_name_(std::move(table_name)),
      columns_(std::move(columns)), values_list_(std::move(values_list)),
      current_row_(0) {
}

void InsertExecutor::Init() {
    current_row_ = 0;
}

bool InsertExecutor::Next(Tuple* tuple) {
    if (current_row_ >= values_list_.size()) return false;
    const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
    if (!info) {
        throw CompilerException(ErrorStage::SEMANTIC, "table not found: " + table_name_);
    }
    TableHeap* heap = context_->GetCatalog()->GetTableHeap(table_name_);
    if (!heap) {
        throw CompilerException(ErrorStage::SEMANTIC, "table heap missing: " + table_name_);
    }

    // Build column_index_map for this table (in declaration order)
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
            row_values[i] = eval.Evaluate(row_exprs[i], Tuple());
        }
    } else {
        // Place each value into the column index specified by columns_
        for (size_t i = 0; i < row_exprs.size() && i < columns_.size(); ++i) {
            auto it = idx_map.find(columns_[i]);
            if (it == idx_map.end()) {
                throw CompilerException(ErrorStage::SEMANTIC,
                    "unknown column: " + columns_[i]);
            }
            row_values[it->second] = eval.Evaluate(row_exprs[i], Tuple());
        }
    }

    // AUTO_INCREMENT: 首列为 PRIMARY KEY 且未赋值时自动编号
    if (!info->columns.empty() && info->columns[0].is_primary_key) {
        size_t idx = 0;
        bool need_autoinc = row_values[idx].IsNull();
        if (!need_autoinc && row_values[idx].GetType() == ValueType::INTEGER &&
            columns_.size() > 0) {
            // 检查该列是否在 columns_ 中且被赋了值
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
    if (!heap->InsertTuple(t, &rid)) {
        throw CompilerException(ErrorStage::SEMANTIC,
            "INSERT failed (no space?)");
    }
    ++current_row_;
    if (tuple) {
        *tuple = Tuple({Value::MakeInt(1)});
    }
    return true;
}

}  // namespace sqlcompiler