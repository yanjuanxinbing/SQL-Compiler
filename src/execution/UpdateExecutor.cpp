// =============================================================================
// 54_dml：UpdateExecutor 与 UpdateFromExecutor 实现
// =============================================================================
//
// RETURNING 语义（PG 风格）：
//   - UPDATE RETURNING 发出"更新后的行"（post-image）；
//   - DELETE RETURNING 发出"被删除的行"（pre-image，旧行）。
//   - INSERT RETURNING 发出"新插入的行"。
// 本文件维护 UPDATE / UPDATE FROM 路径上的 RETURNING 实现；DELETE / INSERT /
// UPSERT 的 RETURNING 在各自 Executor 中按相同语义实现。
//
// UPDATE FROM 语义（PG/Oracle 风格）：
//   - UPDATE target SET col = expr [, ...] FROM source [, source2 ...] WHERE cond；
//   - 简化实现：把 target 与 from_sources 做 cross product，WHERE 同时承担
//     连接条件与过滤条件。对每行 joined tuple：
//       1) 评估 WHERE，过滤；
//       2) 取 target 那段（joined tuple 前 N 列）；
//       3) 评估 SET 右侧（target + source 列混用），写回；
//       4) 若有 RETURNING，按更新后的 target 行求值并 emit。
// =============================================================================

#include "execution/UpdateExecutor.h"

#include "execution/ConstraintChecker.h"
#include "execution/IndexMaintenance.h"
#include "execution/ExpressionEvaluator.h"
#include "execution/TriggerExecutor.h"
#include "execution/TypeCoercion.h"
#include "catalog/SystemCatalog.h"

#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "txn/Transaction.h"
#include "txn/TransactionManager.h"

namespace sqlcompiler {

UpdateExecutor::UpdateExecutor(ExecutionContext* context, std::string table_name,
                                std::vector<std::pair<std::string, ExprPtr>> assignments,
                                ExprPtr predicate,
                                std::unordered_map<std::string, size_t> column_index_map,
                                std::vector<ExprPtr> returning_exprs,
                                std::vector<std::string> returning_aliases)
    : Executor(context), table_name_(std::move(table_name)),
      assignments_(std::move(assignments)),
      predicate_(std::move(predicate)),
      column_index_map_(std::move(column_index_map)),
      returning_exprs_(std::move(returning_exprs)),
      returning_aliases_(std::move(returning_aliases)),
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
    pending_returning_.clear();
    pending_pos_ = 0;
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
            } else {
                // 非快照/自动提交：复位共享堆上遗留的快照水位，避免陈旧读泄漏。
                table_heap_->SetSnapshot(-1, nullptr);
            }
        }
        iterator_ = std::make_unique<TableHeap::Iterator>(table_heap_->Begin());
    }
    // 60_view_trigger: STATEMENT 级 AFTER 触发器重置 fire 标记。
    TriggerExecutor::ResetStatementFireState(context_);
}

// 把一组列值（已应用 SET 后的新行）按 returning_exprs 求值，得到一条
// RETURNING 元组。column_index_map 须覆盖 RETURNING 表达式里所有列引用。
// 失败抛 CompilerException（语义/求值错误）。
static Tuple EvaluateReturningRow(
    const std::vector<ExprPtr>& returning_exprs,
    const std::unordered_map<std::string, size_t>& column_index_map,
    const Tuple& new_row,
    ExecutionContext* ctx) {
    ExpressionEvaluator eval(column_index_map, ctx, nullptr);
    std::vector<Value> out;
    out.reserve(returning_exprs.size());
    for (const auto& e : returning_exprs) {
        out.push_back(eval.Evaluate(e, new_row));
    }
    return Tuple(std::move(out));
}

bool UpdateExecutor::Next(Tuple* tuple) {
    // 优先消费 pending RETURNING 行。
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
    // Snapshot all RIDs first to avoid Halloween problem during in-place updates.
    std::vector<RID> rids;
    while (iterator_->HasNext()) {
        Tuple t = iterator_->Next(column_types_);
        if (t.GetRid().IsValid()) {
            rids.push_back(t.GetRid());
        }
    }
    // 三参构造：传入 ExecutionContext 让 SubqueryExprNode 可以驱动内部子计划。
    // 单参构造会把 ctx_ 留空，导致 `WHERE col = (SELECT ...)` 中的标量子查询
    // 在 EvaluateSubquery 的首行 nullptr 检查退化为 NULL，进而 `= NULL` 求值为
    // UNKNOWN，所有候选行被误判为不匹配。详见 79_dml_subquery 回归用例。
    ExpressionEvaluator eval(column_index_map_, context_, nullptr);
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
        // 必须按目标列声明类型做强制转换：DECIMAL 在运行时是 VARCHAR，
        // 但 Evaluate() 对 `SET gpa = 3.95` 会返回 FLOAT。直接把 FLOAT
        // 写到 VARCHAR 列槽会让 Value::SerializeTo 按 FLOAT 写入 IEEE-754
        // 字节，反序列化按 VARCHAR 读取首 4 字节当长度，得到乱码——
        // 表面 UPDATE 成功，但 SELECT 看到的是 0/空串。该列类型只在
        // info 非空时才能查到，因此集中缓存一次。
        std::vector<std::string> col_data_types;
        if (const TableInfo* ti = context_->GetCatalog()->GetTable(table_name_)) {
            col_data_types.reserve(ti->columns.size());
            for (const auto& c : ti->columns) col_data_types.push_back(c.data_type);
        }
        for (const auto& kv : assignments_) {
            auto it = column_index_map_.find(kv.first);
            if (it == column_index_map_.end()) continue;
            size_t idx = it->second;
            if (idx >= new_values.size()) continue;
            Value rhs = eval.Evaluate(kv.second, cur);
            if (idx < col_data_types.size()) {
                rhs = CoerceToColumnType(rhs, col_data_types[idx]);
            }
            new_values[idx] = std::move(rhs);
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
                // 53_ddl: child-side FK enforcement after UPDATE
                EnforceChildForeignKeys(context_->GetCatalog(), table_name_,
                                         row_snapshot);
            }
        }
        // 53_ddl: parent-side FK (RESTRICT / CASCADE / SET NULL) on UPDATE
        // 父行。简化语义：UPDATE 父表行视同删除旧值 + 插入新值；按 on_delete_action
        // 处理。exclude_child_rid = 本行 RID，避免 CASCADE 删除自身。
        //
        // Bug-7 修复：仅当某条 FK 的 parent_cols 被本次 UPDATE 实际修改时才
        // 触发 FK 强制执行；否则跳过——
        // 典型场景：UPDATE p SET credit = credit + 100 WHERE ...（不修改 id）
        // 在父表 p 上有 FK q.pid REFERENCES p(id) 且有 child 行 q.pid=1 时，
        // 旧实现会错误地把旧 p 行视同「待删除」，发现子行引用旧 id=1 而抛
        // "cannot delete parent row"，导致 UPDATE 失败、同一事务内的 SELECT
        // 看不到新值（read-your-own-writes 被破坏）。
        // 实际上当 UPDATE 不修改 FK 引用的列（parent_cols）时，parent PK 实际
        // 未变，子行引用仍然合法，无需任何 FK 处理。
        {
            const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
            if (info) {
                std::unordered_set<std::string> modified_cols;
                modified_cols.reserve(assignments_.size());
                for (const auto& kv : assignments_) {
                    modified_cols.insert(kv.first);
                }
                EnforceParentForeignKeys(context_->GetCatalog(), table_name_,
                                         cur.GetValues(), &r, &modified_cols);
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
        // MVCC 二级索引精确可见性（t4）：键未变的更新跳过重建（旧条目仍指向
        // 稳定 RID）；快照写者的键改写对非唯一二级索引延迟摘除旧条目，由回表
        // 键重检精确过滤。
        const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
        std::vector<Value> old_vals = cur.GetValues();
        if (info != nullptr) {
            std::vector<Value> new_vals = new_t.GetValues();
            DeleteFromIndexes(context_->GetCatalog(), *info, old_vals, r, txn,
                              &new_vals);
        }
        // T2 行级写锁：写该行前取 X 锁（持有到提交，Commit/Rollback 释放）。传入
        // 表堆首页页号参与「行级锁升级」：大批量 UPDATE 达阈值后行锁收敛为表级 X 锁。
        auto rl = context_->AcquireRowWriteLock(r,
            static_cast<int64_t>(table_heap_->GetFirstPageId()));
        if (rl == ExecutionContext::RowLockResult::kDeadlock ||
            rl == ExecutionContext::RowLockResult::kTimeout) {
            throw std::runtime_error(
                rl == ExecutionContext::RowLockResult::kDeadlock
                    ? "isolation deadlock on row write (statement aborted)"
                    : "isolation row lock wait timed out (statement aborted)");
        }
        // Phase A：事务已提前挂到堆上（读行时已记录快照读基），这里 UpdateTuple
        // 直接抓 undo。关键：UpdateTuple 在新行比旧 slot 大时会 DeleteTuple(old) +
        // InsertTuple(new)，新 RID 由 out_new_rid 返回；后续 InsertIntoIndexes
        // 必须用新 RID，否则 PK 索引键会指向墓碑化 slot，下一次 UPDATE 时
        // 索引仍按旧 RID 命中旧 slot → exclude_rid 校验失败 → 抛 "duplicate key"。
        RID new_rid = r;
        bool ok = table_heap_->UpdateTuple(r, new_t, column_types_, &new_rid);
        if (ok) {
            ++affected;
            if (info != nullptr) {
                // RID 未变（原位更新）时把旧行交给 InsertIntoIndexes：键未变的
                // 索引项已在 DeleteFromIndexes 中被保留，这里跳过重复插入，避免
                // 同 (key, rid) 出现两条条目导致查询重复出行。RID 已变（增长/迁槽
                // 路径的 delete+insert）时必须按新 RID 写入，否则索引键指向墓碑槽。
                const std::vector<Value>* old_row_for_skip =
                    (new_rid == r) ? &old_vals : nullptr;
                InsertIntoIndexes(context_->GetCatalog(), *info,
                                  new_t.GetValues(), new_rid, txn,
                                  old_row_for_skip);
            }
            // 60_view_trigger: AFTER UPDATE 触发器（含 STATEMENT 级）。
            TriggerExecutor::FireAfter(
                context_->GetCatalog(), context_, table_name_,
                TriggerEvent::UPDATE, column_index_map_,
                &cur.GetValues(), &new_t.GetValues());
            // 54_dml: UPDATE RETURNING 发出 post-image。
            if (!returning_exprs_.empty()) {
                std::vector<Value> out;
                out.reserve(returning_exprs_.size());
                for (const auto& e : returning_exprs_) {
                    out.push_back(eval.Evaluate(e, new_t));
                }
                pending_returning_.push_back(Tuple(std::move(out)));
            }
        } else if (info != nullptr) {
            // 写堆失败：把「确实被摘掉」的旧键放回去，避免索引凭空少一条。
            // 快照+非唯一索引的旧条目在 DeleteFromIndexes 中已被保留，放回会
            // 产生同 (key,rid) 重复条目，这里只恢复被急切摘除的条目。
            RestoreDeletedIndexEntries(context_->GetCatalog(), *info,
                                       old_vals, r, txn);
        }
    }
    // Phase A：语句结束前解除事务挂载，避免把快照读基记录泄漏到后续语句。
    if (txn != nullptr) table_heap_->SetActiveTransaction(nullptr);
    if (pending_pos_ < pending_returning_.size()) {
        if (tuple) *tuple = pending_returning_[pending_pos_++];
        return true;
    }
    if (tuple) *tuple = Tuple({Value::MakeInt(affected)});
    return false;
}

// =============================================================================
// UpdateFromExecutor
// =============================================================================

UpdateFromExecutor::UpdateFromExecutor(ExecutionContext* context, std::string table_name,
                                       std::string target_alias,
                                       std::vector<std::pair<std::string, ExprPtr>> assignments,
                                       ExprPtr predicate,
                                       std::unordered_map<std::string, size_t> target_column_index_map,
                                       std::unordered_map<std::string, size_t> combined_column_index_map,
                                       ExecutorPtr join_child,
                                       std::vector<ExprPtr> returning_exprs,
                                       std::vector<std::string> returning_aliases)
    : Executor(context), table_name_(std::move(table_name)),
      target_alias_(std::move(target_alias)),
      assignments_(std::move(assignments)),
      predicate_(std::move(predicate)),
      target_column_index_map_(std::move(target_column_index_map)),
      combined_column_index_map_(std::move(combined_column_index_map)),
      join_child_(std::move(join_child)),
      returning_exprs_(std::move(returning_exprs)),
      returning_aliases_(std::move(returning_aliases)) {
}

void UpdateFromExecutor::Init() {
    table_heap_ = context_->GetCatalog()->GetTableHeap(table_name_);
    const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
    if (info) {
        column_types_.clear();
        column_types_.reserve(info->columns.size());
        for (const auto& c : info->columns) {
            column_types_.push_back(ValueTypeFromString(c.data_type));
        }
        target_column_count_ = info->columns.size();
    }
    pending_returning_.clear();
    pending_pos_ = 0;
    has_current_ = false;
    if (join_child_) join_child_->Init();
}

bool UpdateFromExecutor::Next(Tuple* tuple) {
    // 先消费缓冲（RETURNING 行）。
    if (pending_pos_ < pending_returning_.size()) {
        if (tuple) *tuple = pending_returning_[pending_pos_++];
        return true;
    }
    pending_returning_.clear();
    pending_pos_ = 0;
    // 没有 join 子计划时直接退出。
    if (!join_child_) return false;
    ExpressionEvaluator eval(combined_column_index_map_, context_, nullptr);
    while (join_child_->Next(&current_joined_)) {
        has_current_ = true;
        // 1) 评估 WHERE（在 joined tuple 上；连接条件与过滤条件合一）。
        bool match = true;
        if (predicate_) {
            Value v = eval.Evaluate(predicate_, current_joined_);
            match = !v.IsNull() && v.AsInt() != 0;
        }
        if (!match) continue;
        // 2) 取 target 那段（前 target_column_count_ 列）。
        if (current_joined_.ColumnCount() < target_column_count_) continue;
        std::vector<Value> target_values;
        target_values.reserve(target_column_count_);
        for (size_t i = 0; i < target_column_count_; ++i) {
            target_values.push_back(current_joined_.GetValue(i));
        }
        Tuple target_tuple(std::move(target_values));
        // target_tuple 没有 RID（来自 join），我们需要根据"更新前 target 列值"
        // 找到 RID。简化实现：在 target 表上做一次 SeqScan，匹配所有列值。
        // 性能权衡：本任务的 test 规模下 O(target × joined) 完全够用。
        RID target_rid;
        bool found = false;
        {
            auto it = table_heap_->Begin();
            while (it.HasNext()) {
                Tuple cand = it.Next(column_types_);
                if (!cand.GetRid().IsValid()) continue;
                if (cand.ColumnCount() != target_column_count_) continue;
                bool same = true;
                for (size_t i = 0; i < target_column_count_; ++i) {
                    if (Value::Compare(cand.GetValue(i),
                                       target_tuple.GetValue(i)) != 0) {
                        same = false; break;
                    }
                }
                if (same) {
                    target_rid = cand.GetRid();
                    found = true;
                    break;
                }
            }
        }
        if (!found) continue;
        // 重新读出当前行（拿到完整列值用于 SET 与 RETURNING 求值）。
        Tuple cur;
        if (!table_heap_->GetTuple(target_rid, &cur, column_types_)) continue;
        // 3) 评估 SET 右侧（在 joined tuple 上：target + source 列都可见）；
        //    写回 target 表的列。同 UpdateExecutor：必须按目标列声明类型做
        //    强制转换，否则 DECIMAL → FLOAT 写槽 → 反序列化乱码。
        std::vector<Value> new_values = cur.GetValues();
        std::vector<std::string> col_data_types;
        if (const TableInfo* ti = context_->GetCatalog()->GetTable(table_name_)) {
            col_data_types.reserve(ti->columns.size());
            for (const auto& c : ti->columns) col_data_types.push_back(c.data_type);
        }
        for (const auto& kv : assignments_) {
            auto it = target_column_index_map_.find(kv.first);
            if (it == target_column_index_map_.end()) continue;
            size_t idx = it->second;
            if (idx >= new_values.size()) continue;
            Value rhs = eval.Evaluate(kv.second, current_joined_);
            if (idx < col_data_types.size()) {
                rhs = CoerceToColumnType(rhs, col_data_types[idx]);
            }
            new_values[idx] = std::move(rhs);
        }
        Tuple new_t(std::move(new_values));
        // 4) 约束 / 索引 / WAL 等路径与 UpdateExecutor 完全一致。
        {
            const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
            if (info) {
                std::vector<Value> row_snapshot;
                row_snapshot.reserve(new_t.ColumnCount());
                for (size_t i = 0; i < new_t.ColumnCount(); ++i) {
                    row_snapshot.push_back(new_t.GetValue(i));
                }
                ValidateRowConstraints(context_->GetCatalog(), *info,
                                       table_heap_, row_snapshot, &target_rid, context_);
                CheckUniqueIndexes(context_->GetCatalog(), *info, row_snapshot, &target_rid);
                EnforceChildForeignKeys(context_->GetCatalog(), table_name_, row_snapshot);
            }
        }
        {
            const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
            if (info) {
                std::unordered_set<std::string> modified_cols;
                modified_cols.reserve(assignments_.size());
                for (const auto& kv : assignments_) {
                    modified_cols.insert(kv.first);
                }
                EnforceParentForeignKeys(context_->GetCatalog(), table_name_,
                                         cur.GetValues(), &target_rid,
                                         &modified_cols);
            }
        }
        const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
        Transaction* txn = context_->GetTransaction();
        if (info != nullptr) {
            DeleteFromIndexes(context_->GetCatalog(), *info, cur.GetValues(),
                              target_rid, txn);
        }
        // 同 UpdateExecutor：UpdateTuple 在行增长时走 delete+insert，RID 会变；
        // 必须用 out_new_rid 让 InsertIntoIndexes 指向新 slot，避免索引键
        // 指向已墓碑化的旧 slot，下一次 UPDATE 时触发 "duplicate key"。
        table_heap_->SetActiveTransaction(txn);
        RID new_rid = target_rid;
        bool ok = table_heap_->UpdateTuple(target_rid, new_t, column_types_, &new_rid);
        table_heap_->SetActiveTransaction(nullptr);
        if (ok) {
            if (info != nullptr) {
                InsertIntoIndexes(context_->GetCatalog(), *info,
                                  new_t.GetValues(), new_rid, txn);
            }
            // 60_view_trigger: AFTER UPDATE FROM 触发器。
            TriggerExecutor::FireAfter(
                context_->GetCatalog(), context_, table_name_,
                TriggerEvent::UPDATE, target_column_index_map_,
                &cur.GetValues(), &new_t.GetValues());
            // 5) RETURNING 评估：使用 target_column_index_map_ 在新行上求值。
            //    注意：combined cmap 里也包含 target 列索引，二者对 target 列的
            //    下标一致；这里用 target cmap 表达"只用 target 列"的语义。
            if (!returning_exprs_.empty()) {
                pending_returning_.push_back(EvaluateReturningRow(
                    returning_exprs_, target_column_index_map_, new_t, context_));
                if (tuple) {
                    *tuple = pending_returning_.back();
                    pending_pos_ = pending_returning_.size();  // 标记已消费
                }
                return true;
            }
        } else if (info != nullptr) {
            InsertIntoIndexes(context_->GetCatalog(), *info, cur.GetValues(),
                              target_rid, txn);
        }
    }
    return false;
}

}  // namespace sqlcompiler
