#include "execution/SeqScanExecutor.h"

#include "execution/ExpressionEvaluator.h"

namespace sqlcompiler {

SeqScanExecutor::SeqScanExecutor(ExecutionContext* context, std::string table_name,
                                 std::string table_alias, ExprPtr predicate)
    : Executor(context),
      table_name_(std::move(table_name)),
      table_alias_(std::move(table_alias)),
      predicate_(std::move(predicate)),
      table_heap_(nullptr) {
}

void SeqScanExecutor::Init() {
    SystemCatalog* catalog = context_->GetCatalog();
    table_heap_ = catalog->GetTableHeap(table_name_);
    const TableInfo* info = catalog->GetTable(table_name_);
    if (info) {
        column_types_.clear();
        column_types_.reserve(info->columns.size());
        // 列下标映射：把 `<table>.<col>`、`<alias>.<col>`、`<col>` 三种写法
        // 都登记到 column_index_map_。与 BuildCombinedColumnIndexMap 保持一致，
        // 下推谓词的 ColumnRefExpr 都能正确解析到位置。
        column_index_map_.clear();
        for (size_t i = 0; i < info->columns.size(); ++i) {
            const auto& c = info->columns[i];
            column_index_map_[table_name_ + "." + c.name] = i;
            if (!table_alias_.empty() && table_alias_ != table_name_) {
                column_index_map_[table_alias_ + "." + c.name] = i;
            }
            // 未限定列名：first-table-wins（这里只有一个表，无冲突）
            column_index_map_[c.name] = i;
        }
        for (size_t i = 0; i < info->columns.size(); ++i) {
            column_types_.push_back(ValueTypeFromString(info->columns[i].data_type));
        }
    }
    if (table_heap_) {
        iterator_ = std::make_unique<TableHeap::Iterator>(table_heap_->Begin());
    }
}

bool SeqScanExecutor::Next(Tuple* tuple) {
    if (!iterator_) return false;
    if (!iterator_->HasNext()) return false;
    if (!tuple) return true;

    if (!predicate_) {
        *tuple = iterator_->Next(column_types_);
        return true;
    }

    // 有下推谓词：拉一条 Tuple，按列下标映射求值；false 则继续拉下一条。
    // 与 IndexScanExecutor::Next 的 residual_predicate 处理保持一致：
    // NULL 或 0 视为不通过。
    ExpressionEvaluator eval(column_index_map_, context_, nullptr);
    while (iterator_->HasNext()) {
        Tuple t = iterator_->Next(column_types_);
        Value v = eval.Evaluate(predicate_, t);
        if (!v.IsNull() && v.AsInt() != 0) {
            *tuple = std::move(t);
            return true;
        }
    }
    return false;
}

}  // namespace sqlcompiler
