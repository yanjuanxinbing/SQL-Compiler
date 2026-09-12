#include "execution/DeleteExecutor.h"

#include "execution/ExpressionEvaluator.h"
#include "execution/IndexMaintenance.h"

#include <stdexcept>

#include "txn/Transaction.h"
#include "txn/TransactionManager.h"

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
        // MVCC 快照隔离：与 SeqScanExecutor 一致，在读堆前把本事务快照水位与共享
        // CommitTracker 挂到堆上，GetTuple/Iterator 按快照过滤版本。否则 DELETE 扫描
        // 会把同一逻辑行的新旧版本槽位都当作独立行删除，回滚后数据损坏。
        if (context_ != nullptr) {
            Transaction* txn = context_->GetTransaction();
            TransactionManager* mgr = context_->GetTransactionManager();
            CommitTracker* tracker =
                (mgr != nullptr) ? mgr->GetCommitTracker() : nullptr;
            if (txn != nullptr && txn->IsActive() &&
                txn->GetIsolationLevel() == IsolationLevel::kSnapshot &&
                tracker != nullptr) {
                table_heap_->SetSnapshot(txn->GetSnapshotCsn(), tracker);
            }
        }
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
    // Phase A：把当前事务挂到堆上。必须在「扫描读行取快照读基」之前就绪，否则
    // GetTuple 的 RecordSnapshotRead（供 first-committer-wins 用）不会记录，
    // DeleteTuple 无法拿到正确的 FCW base。
    Transaction* txn = context_->GetTransaction();
    if (txn != nullptr) table_heap_->SetActiveTransaction(txn);
    while (iterator_->HasNext()) {
        Tuple t = iterator_->Next(column_types_);
        bool match = true;
        if (predicate_) {
            Value v = eval.Evaluate(predicate_, t);
            match = !v.IsNull() && v.AsInt() != 0;
        }
        if (match) {
            // SERIALIZABLE 谓词写前检查（防幻读）。
            auto pr = context_->CheckSerializablePredicate(table_name_, t.GetValues());
            if (pr == ExecutionContext::RowLockResult::kDeadlock ||
                pr == ExecutionContext::RowLockResult::kTimeout) {
                throw std::runtime_error(
                    pr == ExecutionContext::RowLockResult::kDeadlock
                        ? "isolation deadlock on predicate (statement aborted)"
                        : "isolation predicate lock wait timed out (statement aborted)");
            }
            // 必须先删索引项再删堆记录：反过来的话，一旦删堆成功而删索引失败，
            // 索引里就留下指向已释放槽位的 RID，走索引查询会读出幽灵行。
            const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
            if (info != nullptr) {
                DeleteFromIndexes(context_->GetCatalog(), *info, t.GetValues(),
                                  t.GetRid(), txn);
            }
            // T2 行级写锁：删该行前取 X 锁（持有到提交，Commit/Rollback 释放）。
            auto rl = context_->AcquireRowWriteLock(t.GetRid());
            if (rl == ExecutionContext::RowLockResult::kDeadlock ||
                rl == ExecutionContext::RowLockResult::kTimeout) {
                throw std::runtime_error(
                    rl == ExecutionContext::RowLockResult::kDeadlock
                        ? "isolation deadlock on row write (statement aborted)"
                        : "isolation row lock wait timed out (statement aborted)");
            }
            // 事务已提前挂到堆上（扫描读行时已记录快照读基）。
            table_heap_->DeleteTuple(t.GetRid());
            ++affected;
        }
    }
    // Phase A：语句结束前解除事务挂载，避免把快照读基记录泄漏到后续语句。
    if (txn != nullptr) table_heap_->SetActiveTransaction(nullptr);
    if (tuple) *tuple = Tuple({Value::MakeInt(affected)});
    return false;
}

}  // namespace sqlcompiler