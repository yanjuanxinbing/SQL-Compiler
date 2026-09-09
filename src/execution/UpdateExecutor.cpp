#include "execution/UpdateExecutor.h"

#include "execution/ExpressionEvaluator.h"

#include <unordered_map>
#include <vector>

namespace sqlcompiler {

UpdateExecutor::UpdateExecutor(ExecutionContext* context, std::string table_name,
                                std::vector<std::pair<std::string, ExprPtr>> assignments,
                                ExprPtr predicate,
                                std::unordered_map<std::string, size_t> column_index_map)
    : Executor(context), table_name_(std::move(table_name)),
      assignments_(std::move(assignments)),
      predicate_(std::move(predicate)),
      column_index_map_(std::move(column_index_map)),
      table_heap_(nullptr), executed_(false) {
}

void UpdateExecutor::Init() {
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

bool UpdateExecutor::Next(Tuple* tuple) {
    if (executed_) return false;
    executed_ = true;
    int affected = 0;
    if (!table_heap_ || !iterator_) {
        if (tuple) *tuple = Tuple({Value::MakeInt(affected)});
        return false;
    }
    // Snapshot all RIDs first to avoid Halloween problem during in-place updates.
    std::vector<RID> rids;
    while (iterator_->HasNext()) {
        Tuple t = iterator_->Next(column_types_);
        if (t.GetRid().IsValid()) {
            rids.push_back(t.GetRid());
        }
    }
    ExpressionEvaluator eval(column_index_map_);
    for (const RID& r : rids) {
        Tuple cur;
        if (!table_heap_->GetTuple(r, &cur, column_types_)) continue;
        bool match = true;
        if (predicate_) {
            Value v = eval.Evaluate(predicate_, cur);
            match = !v.IsNull() && v.AsInt() != 0;
        }
        if (!match) continue;
        std::vector<Value> new_values = cur.GetValues();
        // Apply each assignment by column name
        for (const auto& kv : assignments_) {
            auto it = column_index_map_.find(kv.first);
            if (it == column_index_map_.end()) continue;
            size_t idx = it->second;
            if (idx >= new_values.size()) continue;
            new_values[idx] = eval.Evaluate(kv.second, cur);
        }
        Tuple new_t(std::move(new_values));
        if (table_heap_->UpdateTuple(r, new_t)) {
            ++affected;
        }
    }
    if (tuple) *tuple = Tuple({Value::MakeInt(affected)});
    return false;
}

}  // namespace sqlcompiler