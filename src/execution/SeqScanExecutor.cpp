#include "execution/SeqScanExecutor.h"

namespace sqlcompiler {

SeqScanExecutor::SeqScanExecutor(ExecutionContext* context, std::string table_name)
    : Executor(context), table_name_(std::move(table_name)), table_heap_(nullptr) {
}

void SeqScanExecutor::Init() {
    table_heap_ = context_->GetCatalog()->GetTableHeap(table_name_);
    const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
    if (info) {
        column_types_.clear();
        column_types_.reserve(info->columns.size());
        for (const auto& c : info->columns) {
            column_types_.push_back(ValueTypeFromString(c.data_type));
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
    *tuple = iterator_->Next(column_types_);
    return true;
}

}  // namespace sqlcompiler