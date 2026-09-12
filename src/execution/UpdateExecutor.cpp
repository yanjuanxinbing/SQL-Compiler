#include "execution/UpdateExecutor.h"

#include "execution/ConstraintChecker.h"
#include "execution/IndexMaintenance.h"
#include "execution/ExpressionEvaluator.h"

#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "txn/Transaction.h"
#include "txn/TransactionManager.h"

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
        // MVCC 快照隔离：与 SeqScanExecutor 一致，扫描/读行前把本事务快照水位与
        // 共享 CommitTracker 挂到堆上，GetTuple 按快照过滤版本。否则更新会把同一
        // 逻辑行的新旧版本槽位都视为独立行处理，且 RecordSnapshotRead 不会记录，
        // first-committer-wins 退化为失败。
        if (context_ != nullptr) {
            Transaction* tx = context_->GetTransaction();
            TransactionManager* mgr = context_->GetTransactionManager();
            CommitTracker* tracker =
                (mgr != nullptr) ? mgr->GetCommitTracker() : nullptr;
            if (tx != nullptr && tx->IsActive() &&
                tx->GetIsolationLevel() == IsolationLevel::kSnapshot &&
                tracker != nullptr) {
                table_heap_->SetSnapshot(tx->GetSnapshotCsn(), tracker);
            }
        }
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
    // Phase A：把当前事务挂到堆上。必须在「读行取快照读基」之前就绪，否则
    // GetTuple 的 RecordSnapshotRead（供 first-committer-wins 用）不会记录，
    // UpdateTuple 会退化为以物理 head 为基，导致冲突检测失效。
    Transaction* txn = context_->GetTransaction();
    if (txn != nullptr) table_heap_->SetActiveTransaction(txn);
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
        // 与 INSERT 走同一套约束校验；exclude_rid 传本行自身，避免「主键未改动的
        // 原地更新」被误判为重复键。
        {
            const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
            if (info) {
                std::vector<Value> row_snapshot;
                row_snapshot.reserve(new_t.ColumnCount());
                for (size_t i = 0; i < new_t.ColumnCount(); ++i) {
                    row_snapshot.push_back(new_t.GetValue(i));
                }
                ValidateRowConstraints(context_->GetCatalog(), *info,
                                       table_heap_, row_snapshot, &r, context_);
                CheckUniqueIndexes(context_->GetCatalog(), *info, row_snapshot, &r);
            }
        }
        // SERIALIZABLE 谓词写前检查（防幻读）：按更新后的主键键判冲突。
        auto pr = context_->CheckSerializablePredicate(table_name_, new_t.GetValues());
        if (pr == ExecutionContext::RowLockResult::kDeadlock ||
            pr == ExecutionContext::RowLockResult::kTimeout) {
            throw std::runtime_error(
                pr == ExecutionContext::RowLockResult::kDeadlock
                    ? "isolation deadlock on predicate (statement aborted)"
                    : "isolation predicate lock wait timed out (statement aborted)");
        }
        // 索引同步：先摘掉旧键，写堆成功后再挂上新键。
        // 顺序反过来（先插新键）会让唯一索引在「键未变」时自己撞自己。
        const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
        if (info != nullptr) {
            DeleteFromIndexes(context_->GetCatalog(), *info, cur.GetValues(), r, txn);
        }
        // T2 行级写锁：写该行前取 X 锁（持有到提交，Commit/Rollback 释放）。
        auto rl = context_->AcquireRowWriteLock(r);
        if (rl == ExecutionContext::RowLockResult::kDeadlock ||
            rl == ExecutionContext::RowLockResult::kTimeout) {
            throw std::runtime_error(
                rl == ExecutionContext::RowLockResult::kDeadlock
                    ? "isolation deadlock on row write (statement aborted)"
                    : "isolation row lock wait timed out (statement aborted)");
        }
        // Phase A：事务已提前挂到堆上（读行时已记录快照读基）。这里 UpdateTuple
        // 直接抓 undo；上一条语句结束前必须 Unset，避免把挂载泄漏到下一语句。
        bool ok = table_heap_->UpdateTuple(r, new_t, column_types_);
        if (ok) {
            ++affected;
            if (info != nullptr) {
                InsertIntoIndexes(context_->GetCatalog(), *info,
                                  new_t.GetValues(), r, txn);
            }
        } else if (info != nullptr) {
            // 写堆失败：把刚摘掉的旧键放回去，避免索引凭空少一条
            InsertIntoIndexes(context_->GetCatalog(), *info, cur.GetValues(), r, txn);
        }
    }
    // Phase A：语句结束前解除事务挂载，避免把快照读基记录泄漏到后续语句。
    if (txn != nullptr) table_heap_->SetActiveTransaction(nullptr);
    if (tuple) *tuple = Tuple({Value::MakeInt(affected)});
    return false;
}

}  // namespace sqlcompiler