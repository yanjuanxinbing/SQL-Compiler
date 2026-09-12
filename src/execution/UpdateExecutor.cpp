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
#include "catalog/SystemCatalog.h"

#include <unordered_map>
#include <vector>

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
        {
            const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
            if (info) {
                EnforceParentForeignKeys(context_->GetCatalog(), table_name_,
                                         cur.GetValues(), &r);
            }
        }
        // 索引同步：先摘掉旧键，写堆成功后再挂上新键。
        // 顺序反过来（先插新键）会让唯一索引在「键未变」时自己撞自己。
        const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
        Transaction* txn = context_->GetTransaction();
        if (info != nullptr) {
            DeleteFromIndexes(context_->GetCatalog(), *info, cur.GetValues(), r, txn);
        }
        // Phase A：把当前事务挂到堆上，让 UpdateTuple 抓 undo。
        table_heap_->SetActiveTransaction(txn);
        bool ok = table_heap_->UpdateTuple(r, new_t, column_types_);
        table_heap_->SetActiveTransaction(nullptr);
        if (ok) {
            ++affected;
            if (info != nullptr) {
                InsertIntoIndexes(context_->GetCatalog(), *info,
                                  new_t.GetValues(), r, txn);
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
            // 写堆失败：把刚摘掉的旧键放回去，避免索引凭空少一条
            InsertIntoIndexes(context_->GetCatalog(), *info, cur.GetValues(), r, txn);
        }
    }
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
        //    写回 target 表的列。
        std::vector<Value> new_values = cur.GetValues();
        for (const auto& kv : assignments_) {
            auto it = target_column_index_map_.find(kv.first);
            if (it == target_column_index_map_.end()) continue;
            size_t idx = it->second;
            if (idx >= new_values.size()) continue;
            new_values[idx] = eval.Evaluate(kv.second, current_joined_);
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
                EnforceParentForeignKeys(context_->GetCatalog(), table_name_,
                                         cur.GetValues(), &target_rid);
            }
        }
        const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
        Transaction* txn = context_->GetTransaction();
        if (info != nullptr) {
            DeleteFromIndexes(context_->GetCatalog(), *info, cur.GetValues(),
                              target_rid, txn);
        }
        table_heap_->SetActiveTransaction(txn);
        bool ok = table_heap_->UpdateTuple(target_rid, new_t, column_types_);
        table_heap_->SetActiveTransaction(nullptr);
        if (ok) {
            if (info != nullptr) {
                InsertIntoIndexes(context_->GetCatalog(), *info,
                                  new_t.GetValues(), target_rid, txn);
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
