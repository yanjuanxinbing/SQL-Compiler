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

// 行级锁升级（v3 自适应）：是否在某张表上尝试升级为表级 X 锁（多粒度锁，见
// LockManager::TryEscalateTable）不再用固定阈值——小表批量写很快覆盖大半行、应提前
// 升级收敛锁条目；大表过早升级会放大表级互斥、应延后。阈值由
// LockManager::ComputeEscalationThreshold 依据「该表已登记行数」（规模近似）与
// 「历史升级冲突采样」（kWouldBlock 次数）动态计算：小表提前、大表延后、冲突降阈。
}  // namespace

ExecutionContext::ExecutionContext(SystemCatalog* catalog,
                                   TransactionManager* txn_manager,
                                   SubqueryCacheStats* subquery_stats)
    : catalog_(catalog), txn_manager_(txn_manager), subquery_stats_(subquery_stats) {
}

SystemCatalog* ExecutionContext::GetCatalog() const {
    return catalog_;
}

void ExecutionContext::CacheSubqueryRows(const void* key, std::vector<Tuple> rows) {
    subquery_cache_[key] = std::move(rows);
    ++subquery_materialize_count_;
    if (subquery_stats_ != nullptr) ++subquery_stats_->materialize_count;
}

const std::vector<Tuple>* ExecutionContext::GetCachedSubqueryRows(const void* key) {
    auto it = subquery_cache_.find(key);
    if (it == subquery_cache_.end()) return nullptr;
    ++subquery_cache_hit_count_;
    if (subquery_stats_ != nullptr) ++subquery_stats_->hit_count;
    return &it->second;
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

ExecutionContext::RowLockResult ExecutionContext::AcquireRowReadLock(
    const RID& rid, int64_t table_res) {
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
    // table_res 作为表提示传入：行读锁与所属表锁路由到同一分片（G6 分片锁）。
    LockResult r = lm->LockShared(txn->GetTxnId(), res, 0, table_res);
    if (r != LockResult::kGranted) {
        return (r == LockResult::kDeadlock) ? RowLockResult::kDeadlock : RowLockResult::kTimeout;
    }
    // READ COMMITTED：本语句登记的读锁在语句末释放；SERIALIZABLE 持有到提交（不登记）。
    if (txn->GetIsolationLevel() == IsolationLevel::kReadCommitted) {
        RecordRowReadLock(res, table_res);
    }
    return RowLockResult::kOk;
}

ExecutionContext::RowLockResult ExecutionContext::AcquireRowWriteLock(
    const RID& rid, int64_t table_res) {
    Transaction* txn = txn_;
    if (txn == nullptr || !txn->IsActive() || txn_manager_ == nullptr) return RowLockResult::kUnused;
    LockManager* lm = txn_manager_->GetLockManager();
    if (lm == nullptr || !rid.IsValid()) return RowLockResult::kUnused;
    // 行级锁升级（v2）：本事务已对 table_res 升级为表级锁 → 该表行访问由表锁覆盖，
    // 直接放行（无需逐行取锁）。语义与逐行持 X 锁一致（见 LockManager 多粒度冲突矩阵）。
    if (table_res >= 0 && lm->IsTableEscalated(txn->GetTxnId(), table_res)) {
        return RowLockResult::kOk;
    }
    const int64_t row_res = RowResourceId(rid.page_id, rid.slot_num);
    // table_res 作为表提示传入：行写锁与所属表锁路由到同一分片（G6 分片锁）。
    LockResult r = lm->LockExclusive(txn->GetTxnId(), row_res, 0, table_res);
    if (r != LockResult::kGranted) {
        return (r == LockResult::kDeadlock) ? RowLockResult::kDeadlock : RowLockResult::kTimeout;
    }
    // 行级锁升级（v3 自适应）：登记行锁归属；行写锁数达到自适应阈值后尝试升级。
    //   * 阈值 = LockManager::ComputeEscalationThreshold(表已登记行数, 升级冲突采样)：
    //     小表（<256 行）提前升级、大表（>=4096 行）延后升级、升级曾失败（kWouldBlock）
    //     则降阈更早再试——冲突一消解立即收敛。
    //   * 升级成功（kGranted）→ 行锁已由表锁替换，后续行访问直接放行，锁条目数
    //     从 O(行) 收敛到 O(表)（长事务批量写的锁表膨胀、UnlockAll/死锁检测开销下降）。
    //   * 升级失败（kWouldBlock，他事务持冲突行/表锁）→ 回退为继续逐行持锁，
    //     正确性不受影响（只是不享受收敛收益）。
    if (table_res >= 0) {
        lm->RegisterRowGroup(row_res, table_res);
        const size_t thr = LockManager::ComputeEscalationThreshold(
            lm->GetRegisteredRowCount(table_res), lm->GetTableConflictCount(table_res));
        if (lm->CountRowLocks(txn->GetTxnId(), table_res) >= thr) {
            LockResult er = lm->TryEscalateTable(txn->GetTxnId(), table_res,
                                                 LockMode::kExclusive);
            if (er == LockResult::kDeadlock) return RowLockResult::kDeadlock;
        }
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
    TableHeap* heap = catalog_->GetTableHeap(table_name);
    if (heap == nullptr) return RowLockResult::kUnused;
    // Phase 5：写前检查泛化为「按受影响行」——表级全表谓词命中任意写；逐列检查
    // 本行各列值是否命中该列注册的读谓词（含任意非主键列）。row 与表模式按列序
    // 对齐；内部一次加锁收集全部冲突持有者。
    const int64_t table_rid = static_cast<int64_t>(heap->GetFirstPageId());
    LockResult r = lm->CheckWritePredicateRow(txn->GetTxnId(), table_rid, row, kPredicateWaitMs);
    if (r != LockResult::kGranted) {
        return (r == LockResult::kDeadlock) ? RowLockResult::kDeadlock : RowLockResult::kTimeout;
    }
    return RowLockResult::kOk;
}

Executor::Executor(ExecutionContext* context) : context_(context) {
}

}  // namespace sqlcompiler