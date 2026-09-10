#include "execution/DeleteExecutor.h"

#include "execution/ExpressionEvaluator.h"
#include "execution/IndexMaintenance.h"

namespace sqlcompiler {

DeleteExecutor::DeleteExecutor(ExecutionContext* context, std::string table_name,
                                ExprPtr predicate,
                                std::unordered_map<std::string, size_t> column_index_map)
    : Executor(context), table_name_(std::move(table_name)),
      predicate_(std::move(predicate)),
      column_index_map_(std::move(column_index_map)),
      table_heap_(nullptr), executed_(false) {
}

void DeleteExecutor::Init() {
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

bool DeleteExecutor::Next(Tuple* tuple) {
    if (executed_) return false;
    executed_ = true;
    int affected = 0;
    if (!table_heap_ || !iterator_) {
        if (tuple) *tuple = Tuple({Value::MakeInt(affected)});
        return false;
    }
    ExpressionEvaluator eval(column_index_map_);
    while (iterator_->HasNext()) {
        Tuple t = iterator_->Next(column_types_);
        bool match = true;
        if (predicate_) {
            Value v = eval.Evaluate(predicate_, t);
            match = !v.IsNull() && v.AsInt() != 0;
        }
        if (match) {
            // 必须先删索引项再删堆记录：反过来的话，一旦删堆成功而删索引失败，
            // 索引里就留下指向已释放槽位的 RID，走索引查询会读出幽灵行。
            const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
            if (info != nullptr) {
                DeleteFromIndexes(context_->GetCatalog(), *info, t.GetValues(),
                                  t.GetRid());
            }
            table_heap_->DeleteTuple(t.GetRid());
            ++affected;
        }
    }
    if (tuple) *tuple = Tuple({Value::MakeInt(affected)});
    return false;
}

}  // namespace sqlcompiler