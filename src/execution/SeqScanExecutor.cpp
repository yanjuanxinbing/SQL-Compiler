#include "execution/SeqScanExecutor.h"

#include <stdexcept>

#include "txn/Transaction.h"
#include "txn/TransactionManager.h"

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
        // MVCC 快照隔离：仅当存在活动显式 kSnapshot 事务时，把本事务的快照水位与
        // 共享 CommitTracker 挂到堆上，GetTuple/Iterator 按快照过滤版本（免读锁）。
        // 其余隔离级别/自动提交不设快照，走现有锁基读（保持既有语义不变）。
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

bool SeqScanExecutor::Next(Tuple* tuple) {
    if (!iterator_) return false;
    if (!iterator_->HasNext()) return false;
    if (!tuple) return true;
    *tuple = iterator_->Next(column_types_);
    // T2 行级读锁：显式事务内逐行取 S 锁（READ COMMITTED 登记、语句末释放）。
    auto rl = context_->AcquireRowReadLock(tuple->GetRid());
    if (rl == ExecutionContext::RowLockResult::kDeadlock ||
        rl == ExecutionContext::RowLockResult::kTimeout) {
        throw std::runtime_error(
            rl == ExecutionContext::RowLockResult::kDeadlock
                ? "isolation deadlock on row read (statement aborted)"
                : "isolation row lock wait timed out (statement aborted)");
    }
    return true;
}

}  // namespace sqlcompiler