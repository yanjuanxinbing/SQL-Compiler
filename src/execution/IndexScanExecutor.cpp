#include "execution/IndexScanExecutor.h"

#include "execution/ConstraintChecker.h"
#include "execution/ExpressionEvaluator.h"
#include "txn/Transaction.h"
#include "txn/TransactionManager.h"

#include <stdexcept>

namespace sqlcompiler {

IndexScanExecutor::IndexScanExecutor(
    ExecutionContext* context, std::shared_ptr<IndexScanNode> node,
    std::unordered_map<std::string, size_t> column_index_map)
    : Executor(context), node_(std::move(node)),
      column_index_map_(std::move(column_index_map)) {
}

void IndexScanExecutor::Init() {
    SystemCatalog* catalog = context_->GetCatalog();
    table_heap_ = catalog->GetTableHeap(node_->table_name);
    tree_ = catalog->GetIndexTree(node_->index_name);
    const TableInfo* info = catalog->GetTable(node_->table_name);
    if (info != nullptr) column_types_ = BuildColumnTypes(*info);
    // MVCC 快照隔离：kSnapshot 下把本事务快照水位与共享 CommitTracker 挂到堆上，
    // 回表 GetTuple 按快照过滤版本（免读锁）。
    if (table_heap_ != nullptr) {
        Transaction* txn = context_->GetTransaction();
        TransactionManager* mgr = context_->GetTransactionManager();
        if (txn != nullptr && txn->IsActive() &&
            txn->GetIsolationLevel() == IsolationLevel::kSnapshot &&
            mgr != nullptr && mgr->GetCommitTracker() != nullptr) {
            table_heap_->SetSnapshot(txn->GetSnapshotCsn(), mgr->GetCommitTracker());
        } else {
            // 非快照/自动提交读：复位共享堆上遗留的快照水位，避免陈旧读泄漏。
            table_heap_->SetSnapshot(-1, nullptr);
        }
    }
    if (tree_ == nullptr) return;

    // MVCC 精确可见性（t4）：解析被扫描索引的键列，供回表后做键重检（过滤
    // 快照写者延迟保留的陈旧条目）。目录里找不到索引元数据时该过滤整体停用。
    if (catalog != nullptr) {
        for (const IndexInfo* idx : catalog->GetIndexesForTable(node_->table_name)) {
            if (idx != nullptr && idx->index_name == node_->index_name) {
                index_key_columns_ = idx->key_columns;
                break;
            }
        }
    }
    seen_rids_.clear();

    if (node_->low_key.empty()) {
        cursor_ = tree_->Begin();
    } else {
        cursor_ = tree_->LowerBound(IndexKey(node_->low_key));
    }
}

bool IndexScanExecutor::BeyondUpperBound(const IndexKey& key) const {
    if (node_->high_key.empty()) return false;
    const IndexKey high(node_->high_key);
    const int c = CompareKeyOnly(key, high);
    return node_->high_inclusive ? (c > 0) : (c >= 0);
}

// MVCC 精确可见性（t4）：键重检——回表得到的「本快照可见版本」必须真的落在
// 扫描区间内。索引里延迟保留的陈旧条目（快照写者键改写/逻辑删除）指向的行，
// 其可见版本键可能与条目键不同，据此过滤。
bool IndexScanExecutor::InScanBounds(const IndexKey& key) const {
    if (!node_->low_key.empty()) {
        const int c = CompareKeyOnly(key, IndexKey(node_->low_key));
        if (node_->low_inclusive ? (c < 0) : (c <= 0)) return false;
    }
    if (!node_->high_key.empty()) {
        const int c = CompareKeyOnly(key, IndexKey(node_->high_key));
        if (node_->high_inclusive ? (c > 0) : (c >= 0)) return false;
    }
    return true;
}

bool IndexScanExecutor::Next(Tuple* tuple) {
    if (!cursor_ || table_heap_ == nullptr) return false;

    ExpressionEvaluator eval(column_index_map_);
    IndexKey key;
    RID rid;
    while (cursor_->Next(&key, &rid)) {
        // 下界是开区间时，LowerBound 会把等于下界的项也带出来，这里跳过
        if (!node_->low_key.empty() && !node_->low_inclusive) {
            if (CompareKeyOnly(key, IndexKey(node_->low_key)) == 0) continue;
        }
        if (BeyondUpperBound(key)) return false;

        Tuple t;
        // 回表。取不到说明索引项指向的记录已不存在——正常情况下不该发生
        // （删除路径会同步摘掉索引项），这里跳过而不是报错，避免一条陈旧索引项
        // 让整条查询失败。
        if (!table_heap_->GetTuple(rid, &t, column_types_)) continue;

        // MVCC 精确可见性（t4）：按 RID 去重 + 可见版本键重检。快照写者的键改写/
        // 逻辑删除在非唯一二级索引中延迟保留旧条目：同一稳定 RID 可能命中新旧多
        // 条目（范围扫描会重复返回同一行），且旧条目的可见版本键与扫描区间不符。
        if (!index_key_columns_.empty()) {
            if (!seen_rids_.insert(rid).second) continue;  // 已返回过该逻辑行
            IndexKey tuple_key;
            bool key_ok = true;
            for (const auto& col : index_key_columns_) {
                auto it = column_index_map_.find(col);
                if (it == column_index_map_.end() ||
                    it->second >= t.ColumnCount()) {
                    key_ok = false;
                    break;
                }
                tuple_key.values.push_back(t.GetValue(it->second));
            }
            if (key_ok && !InScanBounds(tuple_key)) continue;
        }

        if (node_->residual_predicate) {
            Value v = eval.Evaluate(node_->residual_predicate, t);
            if (v.IsNull() || v.AsInt() == 0) continue;
        }
        if (tuple != nullptr) *tuple = std::move(t);
        // T2 行级读锁：显式事务内逐行取 S 锁（READ COMMITTED 登记、语句末释放）。
        auto rl = context_->AcquireRowReadLock(t.GetRid());
        if (rl == ExecutionContext::RowLockResult::kDeadlock ||
            rl == ExecutionContext::RowLockResult::kTimeout) {
            throw std::runtime_error(
                rl == ExecutionContext::RowLockResult::kDeadlock
                    ? "isolation deadlock on row read (statement aborted)"
                    : "isolation row lock wait timed out (statement aborted)");
        }
        return true;
    }
    return false;
}

}  // namespace sqlcompiler
