// =============================================================================
// 54_dml：DeleteExecutor 实现（含 RETURNING emit）
// =============================================================================
//
// RETURNING 语义（PG 风格）：
//   - DELETE RETURNING 发出"被删除的行"——pre-image，即删除前的旧行。
//   - 实现要点：每条匹配的候选行先评估 RETURNING（用旧行的值），然后再走
//     DELETE 写路径，最后把预生成的 RETURNING 行塞进 pending 缓冲。下一轮
//     Next 调用时优先消费缓冲；缓冲空才再次驱动 SeqScan。
// =============================================================================

#include "execution/DeleteExecutor.h"

#include "catalog/SystemCatalog.h"
#include "execution/ConstraintChecker.h"
#include "execution/ExpressionEvaluator.h"
#include "execution/IndexMaintenance.h"
#include "execution/TriggerExecutor.h"

namespace sqlcompiler {

DeleteExecutor::DeleteExecutor(ExecutionContext* context, std::string table_name,
                                ExprPtr predicate,
                                std::unordered_map<std::string, size_t> column_index_map,
                                std::vector<ExprPtr> returning_exprs,
                                std::vector<std::string> returning_aliases)
    : Executor(context), table_name_(std::move(table_name)),
      predicate_(std::move(predicate)),
      column_index_map_(std::move(column_index_map)),
      returning_exprs_(std::move(returning_exprs)),
      returning_aliases_(std::move(returning_aliases)),
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
    pending_returning_.clear();
    pending_pos_ = 0;
    if (table_heap_) {
        iterator_ = std::make_unique<TableHeap::Iterator>(table_heap_->Begin());
    }
    // 60_view_trigger: STATEMENT 级 AFTER 触发器重置 fire 标记。
    TriggerExecutor::ResetStatementFireState(context_);
}

bool DeleteExecutor::Next(Tuple* tuple) {
    // 先消费 pending RETURNING 行。
    if (pending_pos_ < pending_returning_.size()) {
        if (tuple) *tuple = pending_returning_[pending_pos_++];
        return true;
    }
    pending_returning_.clear();
    pending_pos_ = 0;
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
            // 53_ddl: 父表行即将被删除 —— 检查所有引用本行的 FK。
            //   - RESTRICT: 若有任一子行引用 → 抛错拒绝。
            //   - CASCADE: 子行被连带删除（在 EnforceParentForeignKeys 内）。
            //   - SET NULL: 子行 FK 列被置 NULL。
            // 该调用必须在 DeleteTuple 之前执行，以便：
            //   1) RESTRICT 模式下抛错能保持 parent 行未删；
            //   2) CASCADE 模式下仍能读到 parent 行做匹配。
            EnforceParentForeignKeys(context_->GetCatalog(), table_name_,
                                     t.GetValues());
            // 必须先删索引项再删堆记录：反过来的话，一旦删堆成功而删索引失败，
            // 索引里就留下指向已释放槽位的 RID，走索引查询会读出幽灵行。
            const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
            Transaction* txn = context_->GetTransaction();
            if (info != nullptr) {
                DeleteFromIndexes(context_->GetCatalog(), *info, t.GetValues(),
                                  t.GetRid(), txn);
            }
            // Phase A：把当前事务挂到堆上，让 DeleteTuple 抓 undo。
            table_heap_->SetActiveTransaction(txn);
            table_heap_->DeleteTuple(t.GetRid());
            table_heap_->SetActiveTransaction(nullptr);
            ++affected;
            // 60_view_trigger: AFTER DELETE 触发器（含 STATEMENT 级）。
            TriggerExecutor::FireAfter(
                context_->GetCatalog(), context_, table_name_,
                TriggerEvent::DELETE, column_index_map_,
                &t.GetValues(), nullptr);
            // 54_dml: DELETE RETURNING 发出 pre-image。
            // 注意：若 EnforceParentForeignKeys 在 CASCADE 模式下连带删除了子行，
            // 我们仍按"被删除的目标行"emit RETURNING，与 PG 语义一致。
            if (!returning_exprs_.empty()) {
                std::vector<Value> out;
                out.reserve(returning_exprs_.size());
                for (const auto& e : returning_exprs_) {
                    out.push_back(eval.Evaluate(e, t));
                }
                pending_returning_.push_back(Tuple(std::move(out)));
            }
        }
    }
    if (pending_pos_ < pending_returning_.size()) {
        if (tuple) *tuple = pending_returning_[pending_pos_++];
        return true;
    }
    if (tuple) *tuple = Tuple({Value::MakeInt(affected)});
    return false;
}

}  // namespace sqlcompiler
