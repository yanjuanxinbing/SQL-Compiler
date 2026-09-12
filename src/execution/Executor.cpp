#include "execution/Executor.h"

#include "semantic/SymbolTable.h"
#include "storage/LockManager.h"
#include "storage_engine/TableHeap.h"
#include "txn/Transaction.h"
#include "txn/TransactionManager.h"

namespace sqlcompiler {

namespace {
// SERIALIZABLE 谓词写前检查的锁等待超时（毫秒）。
constexpr int kPredicateWaitMs = 5000;
}  // namespace

ExecutionContext::ExecutionContext(SystemCatalog* catalog,
                                   TransactionManager* txn_manager)
    : catalog_(catalog), txn_manager_(txn_manager) {
}

SystemCatalog* ExecutionContext::GetCatalog() const {
    return catalog_;
}

void ExecutionContext::RegisterCte(const std::string& name, std::vector<Tuple> rows) {
    cte_results_[name] = CteMaterialization{name, std::move(rows)};
}

void ExecutionContext::AppendCteRows(const std::string& name,
                                     const std::vector<Tuple>& rows) {
    auto it = cte_results_.find(name);
    if (it == cte_results_.end()) {
        cte_results_[name] = CteMaterialization{name, rows};
    } else {
        it->second.rows.insert(it->second.rows.end(), rows.begin(), rows.end());
    }
}

const std::vector<Tuple>* ExecutionContext::GetCteRows(const std::string& name) const {
    // 递归 CTE 迭代期间：先看 override 栈顶（最新工作集）。
    auto oit = cte_overrides_.find(name);
    if (oit != cte_overrides_.end() && !oit->second.empty()) {
        return &oit->second.back();
    }
    auto it = cte_results_.find(name);
    if (it == cte_results_.end()) return nullptr;
    return &it->second.rows;
}

bool ExecutionContext::HasCte(const std::string& name) const {
    if (cte_overrides_.count(name) && !cte_overrides_.at(name).empty()) return true;
    return cte_results_.count(name) > 0;
}

void ExecutionContext::PushCteOverride(const std::string& name, std::vector<Tuple> rows) {
    cte_overrides_[name].push_back(std::move(rows));
}

void ExecutionContext::PopCteOverride(const std::string& name) {
    auto it = cte_overrides_.find(name);
    if (it == cte_overrides_.end() || it->second.empty()) return;
    it->second.pop_back();
}

ExecutionContext::RowLockResult ExecutionContext::AcquireRowReadLock(const RID& rid) {
    Transaction* txn = txn_;
    if (txn == nullptr || !txn->IsActive() || txn_manager_ == nullptr) return RowLockResult::kUnused;
    LockManager* lm = txn_manager_->GetLockManager();
    if (lm == nullptr || !rid.IsValid()) return RowLockResult::kUnused;
    // 免读锁：READ UNCOMMITTED（读到未提交写）与 SNAPSHOT（MVCC 快照隔离）
    // 都不取行读锁——后者依赖版本链可见性而非锁来保证读一致性。
    if (txn->GetIsolationLevel() == IsolationLevel::kReadUncommitted ||
        txn->GetIsolationLevel() == IsolationLevel::kSnapshot) {
        return RowLockResult::kUnused;
    }
    const int64_t res = RowResourceId(rid.page_id, rid.slot_num);
    LockResult r = lm->LockShared(txn->GetTxnId(), res, 0);
    if (r != LockResult::kGranted) {
        return (r == LockResult::kDeadlock) ? RowLockResult::kDeadlock : RowLockResult::kTimeout;
    }
    // READ COMMITTED：本语句登记的读锁在语句末释放；SERIALIZABLE 持有到提交（不登记）。
    if (txn->GetIsolationLevel() == IsolationLevel::kReadCommitted) {
        RecordRowReadLock(res);
    }
    return RowLockResult::kOk;
}

ExecutionContext::RowLockResult ExecutionContext::AcquireRowWriteLock(const RID& rid) {
    Transaction* txn = txn_;
    if (txn == nullptr || !txn->IsActive() || txn_manager_ == nullptr) return RowLockResult::kUnused;
    LockManager* lm = txn_manager_->GetLockManager();
    if (lm == nullptr || !rid.IsValid()) return RowLockResult::kUnused;
    LockResult r = lm->LockExclusive(txn->GetTxnId(), RowResourceId(rid.page_id, rid.slot_num), 0);
    if (r != LockResult::kGranted) {
        return (r == LockResult::kDeadlock) ? RowLockResult::kDeadlock : RowLockResult::kTimeout;
    }
    return RowLockResult::kOk;
}

ExecutionContext::RowLockResult ExecutionContext::CheckSerializablePredicate(
    const std::string& table_name, const std::vector<Value>& row) {
    Transaction* txn = txn_;
    if (txn == nullptr || !txn->IsActive() || txn_manager_ == nullptr) return RowLockResult::kUnused;
    if (txn->GetIsolationLevel() != IsolationLevel::kSerializable) return RowLockResult::kUnused;
    LockManager* lm = txn_manager_->GetLockManager();
    if (lm == nullptr || catalog_ == nullptr) return RowLockResult::kUnused;
    const TableInfo* info = catalog_->GetTable(table_name);
    TableHeap* heap = catalog_->GetTableHeap(table_name);
    if (info == nullptr || heap == nullptr) return RowLockResult::kUnused;
    // 主键键：所有标记为 PRIMARY KEY 的列按列序取值。谓词作用在主键键空间。
    std::vector<Value> key_vals;
    key_vals.reserve(info->columns.size());
    for (size_t i = 0; i < info->columns.size() && i < row.size(); ++i) {
        if (info->columns[i].is_primary_key) {
            if (row[i].IsNull()) return RowLockResult::kUnused;
            key_vals.push_back(row[i]);
        }
    }
    if (key_vals.empty()) return RowLockResult::kUnused;  // 无主键：谓词边界未定义
    const int64_t table_rid = static_cast<int64_t>(heap->GetFirstPageId());
    IndexKey key(std::move(key_vals));
    LockResult r = lm->CheckWritePredicate(txn->GetTxnId(), table_rid, key, kPredicateWaitMs);
    if (r != LockResult::kGranted) {
        return (r == LockResult::kDeadlock) ? RowLockResult::kDeadlock : RowLockResult::kTimeout;
    }
    return RowLockResult::kOk;
}

Executor::Executor(ExecutionContext* context) : context_(context) {
}

}  // namespace sqlcompiler