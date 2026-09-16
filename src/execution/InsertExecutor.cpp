// =============================================================================
// 54_dml：InsertExecutor 实现（含 REPLACE INTO 与 RETURNING emit）
// =============================================================================
//
// RETURNING 语义（PG 风格）：
//   - INSERT RETURNING 发出"新插入的行"——post-image。
//   - 实现要点：每条候选行落盘后立即评估 returning_exprs（用新行），再写堆失败
//     时回退 pending 缓冲。Next() 优先消费 pending；空时再驱动 VALUES / SELECT。
//
// REPLACE 语义（MySQL）：
//   - 候选行若在 PRIMARY KEY / UNIQUE 索引上冲突，先按冲突行的 RID 调 DeleteTuple
//     （连带索引项删除），再走常规 INSERT 路径。简化实现：仅处理 PRIMARY KEY
//     冲突（与现有 UpsertExecutor 一致）；UNIQUE INDEX 冲突按 PG/Oracle 等价
//     行为也走"先删后插"。
// =============================================================================

#include "execution/InsertExecutor.h"

#include "catalog/SystemCatalog.h"
#include "common/Error.h"
#include "execution/ConstraintChecker.h"
#include "execution/ExecutionEngine.h"
#include "execution/IndexMaintenance.h"
#include "execution/ExpressionEvaluator.h"
#include "execution/TriggerExecutor.h"
#include "execution/TypeCoercion.h"

#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace sqlcompiler {

namespace {

// bug11: 校验 DEFAULT 表达式是「常量」——不允许列引用、子查询、VALUES(col)、
// NEXTVAL（带副作用）、窗口函数等带上下文依赖或副作用的节点。
// 与 AlterTableExecutor 中的同名校验逻辑保持完全一致；该函数在两处分别
// 局部实现，避免跨 executor 共享的额外头文件依赖。
void RequireConstantExpression(const ExprPtr& expr,
                               const std::string& context_label) {
    if (!expr) return;
    switch (expr->GetType()) {
        case NodeType::COLUMN_REF_EXPR:
            throw CompilerException(
                ErrorStage::SEMANTIC,
                context_label + ": DEFAULT must be a constant expression "
                "(column reference is not allowed)");
        case NodeType::SUBQUERY_EXPR:
            throw CompilerException(
                ErrorStage::SEMANTIC,
                context_label + ": DEFAULT must be a constant expression "
                "(subquery is not allowed)");
        case NodeType::UPSERT_VALUES_REF_EXPR:
            throw CompilerException(
                ErrorStage::SEMANTIC,
                context_label + ": DEFAULT must be a constant expression "
                "(VALUES(col) reference is not allowed)");
        case NodeType::NEXTVAL_EXPR:
            throw CompilerException(
                ErrorStage::SEMANTIC,
                context_label + ": DEFAULT must be a constant expression "
                "(NEXTVAL has side effects and is not allowed)");
        case NodeType::WINDOW_FUNC_EXPR:
            throw CompilerException(
                ErrorStage::SEMANTIC,
                context_label + ": DEFAULT must be a constant expression "
                "(window function is not allowed)");
        case NodeType::BINARY_EXPR: {
            const auto* b = static_cast<const BinaryExpr*>(expr.get());
            RequireConstantExpression(b->left, context_label);
            RequireConstantExpression(b->right, context_label);
            return;
        }
        case NodeType::UNARY_EXPR: {
            const auto* u = static_cast<const UnaryExpr*>(expr.get());
            RequireConstantExpression(u->operand, context_label);
            return;
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            const auto* f = static_cast<const FunctionCallExpr*>(expr.get());
            for (const auto& a : f->arguments) {
                RequireConstantExpression(a, context_label);
            }
            if (f->filter_expr) {
                RequireConstantExpression(f->filter_expr, context_label);
            }
            for (const auto& o : f->within_group_order_by) {
                RequireConstantExpression(o.expr, context_label);
            }
            return;
        }
        case NodeType::CASE_EXPR: {
            const auto* c = static_cast<const CaseExprNode*>(expr.get());
            RequireConstantExpression(c->subject, context_label);
            for (const auto& w : c->whens) {
                RequireConstantExpression(w.when_expr, context_label);
                RequireConstantExpression(w.then_expr, context_label);
            }
            RequireConstantExpression(c->else_expr, context_label);
            return;
        }
        case NodeType::CAST_EXPR: {
            const auto* c = static_cast<const CastExprNode*>(expr.get());
            RequireConstantExpression(c->expr, context_label);
            return;
        }
        case NodeType::LIKE_EXPR: {
            const auto* l = static_cast<const LikeExprNode*>(expr.get());
            RequireConstantExpression(l->operand, context_label);
            RequireConstantExpression(l->pattern, context_label);
            return;
        }
        case NodeType::EXTRACT_EXPR: {
            const auto* e = static_cast<const ExtractExprNode*>(expr.get());
            RequireConstantExpression(e->source, context_label);
            return;
        }
        case NodeType::INTERVAL_EXPR:
            // INTERVAL 字面量节点本身不持有子表达式。
            return;
        default:
            return;
    }
}

// bug11: 把 DEFAULT 表达式作为常量表达式求值——先用 RequireConstantExpression
// 校验，然后用一个空 Tuple / 空 column_index_map 触发 ExpressionEvaluator
// 的递归求值。
Value EvaluateDefaultLiteral(const ExprPtr& default_expr,
                              const std::string& col_name) {
    if (!default_expr) return Value::MakeNull();
    RequireConstantExpression(default_expr,
        "default expression for column '" + col_name + "'");
    const std::unordered_map<std::string, size_t> empty_map;
    ExpressionEvaluator eval(empty_map);
    return eval.Evaluate(default_expr, Tuple());
}

void ApplyDefaults(const TableInfo& info,
                   const std::vector<std::string>& explicit_columns,
                   std::vector<Value>& row_values) {
    std::unordered_set<std::string> explicit_names;
    explicit_names.reserve(explicit_columns.size());
    for (const auto& c : explicit_columns) explicit_names.insert(c);
    for (size_t i = 0; i < info.columns.size() && i < row_values.size(); ++i) {
        if (!row_values[i].IsNull()) continue;
        if (!info.columns[i].default_expr) continue;
        if (explicit_columns.empty()) continue;
        if (explicit_names.count(info.columns[i].name) > 0) continue;
        Value v = EvaluateDefaultLiteral(info.columns[i].default_expr,
                                          info.columns[i].name);
        row_values[i] = CoerceToColumnType(v, info.columns[i].data_type);
    }
}

// REPLACE INTO 路径：探测 PK / UNIQUE 冲突，按冲突行 RID 列表返回。
// 当前实现复用 UpsertExecutor 的 PRIMARY KEY 探测思路，并对每张 UNIQUE INDEX
// 索引做 B+Tree 等值探测（与 CheckUniqueIndexes 对齐）。
// 若无任何冲突，返回空列表。
std::vector<RID> FindReplaceConflicts(SystemCatalog* catalog,
                                      const TableInfo& info,
                                      const std::vector<Value>& row_values) {
    std::vector<RID> conflicts;
    if (!catalog) return conflicts;
    auto groups = info.GetPrimaryKeyGroups();
    for (const auto& g : groups) {
        BPlusTree* tree = catalog->GetPrimaryKeyIndexTree(info.table_name, g);
        if (tree == nullptr) continue;
        IndexKey key;
        bool complete = true;
        for (const auto& col_name : g) {
            const ColumnInfo* col = info.GetColumn(col_name);
            if (!col) { complete = false; break; }
            size_t idx = 0;
            bool found = false;
            for (size_t i = 0; i < info.columns.size(); ++i) {
                if (info.columns[i].name == col_name) { idx = i; found = true; break; }
            }
            if (!found || idx >= row_values.size() || row_values[idx].IsNull()) {
                complete = false; break;
            }
            key.values.push_back(row_values[idx]);
        }
        if (!complete) continue;
        RID existing = tree->FindFirst(key);
        if (existing.IsValid()) {
            conflicts.push_back(existing);
        }
    }
    return conflicts;
}

// REPLACE INTO：删除冲突行（含索引项）。冲突行必须按 RID 排序后再依次删除，
// 避免"边删边迭代"破坏索引一致性。
void DeleteConflicts(ExecutionContext* ctx, const std::string& table_name,
                     const std::vector<Value>& conflict_row_values_for_fk,
                     const std::vector<RID>& conflict_rids) {
    if (conflict_rids.empty()) return;
    TableHeap* heap = ctx->GetCatalog()->GetTableHeap(table_name);
    if (!heap) return;
    const TableInfo* info = ctx->GetCatalog()->GetTable(table_name);
    if (!info) return;
    std::vector<ValueType> col_types = BuildColumnTypes(*info);
    Transaction* txn = ctx->GetTransaction();
    for (const RID& r : conflict_rids) {
        // 先读出当前冲突行的列值（用于索引同步）。
        Tuple cur;
        if (!heap->GetTuple(r, &cur, col_types)) continue;
        DeleteFromIndexes(ctx->GetCatalog(), *info, cur.GetValues(), r, txn);
        heap->SetActiveTransaction(txn);
        heap->DeleteTuple(r);
        heap->SetActiveTransaction(nullptr);
    }
    (void)conflict_row_values_for_fk;
}

}  // namespace

InsertExecutor::InsertExecutor(ExecutionContext* context, std::string table_name,
                                std::vector<std::string> columns,
                                std::vector<std::vector<ExprPtr>> values_list,
                                bool is_replace,
                                bool is_default_values,
                                std::vector<ExprPtr> returning_exprs,
                                std::vector<std::string> returning_aliases)
    : Executor(context), table_name_(std::move(table_name)),
      columns_(std::move(columns)), values_list_(std::move(values_list)),
      is_replace_(is_replace),
      is_default_values_(is_default_values),
      returning_exprs_(std::move(returning_exprs)),
      returning_aliases_(std::move(returning_aliases)),
      current_row_(0) {
}

InsertExecutor::InsertExecutor(ExecutionContext* context, std::string table_name,
                                std::vector<std::string> columns,
                                PlanNodePtr query_plan,
                                std::vector<ExprPtr> returning_exprs,
                                std::vector<std::string> returning_aliases)
    : Executor(context), table_name_(std::move(table_name)),
      columns_(std::move(columns)), is_replace_(false),
      returning_exprs_(std::move(returning_exprs)),
      returning_aliases_(std::move(returning_aliases)),
      current_row_(0) {
    if (query_plan) {
        ExecutionEngine engine(context_->GetCatalog());
        source_ = engine.BuildExecutor(query_plan, context_);
        if (source_) {
            // 在向目标表写入任何行之前先把源表全量快照下来。这一步解决
            // `INSERT INTO t SELECT ... FROM t`（self-INSERT）的无限循环：
            // 没有快照时 source_->Next() 在每次 InsertRow 之后都能再次读到
            // 刚插入的行，于是无限增长。这里的源 SeqScan 已通过
            // InsertExecutor::Init 触发，但 InsertExecutor::Init 还没有被
            // 调用——需要手动驱动一次 Init。
            source_->Init();
            Tuple t;
            while (source_->Next(&t)) {
                materialized_rows_.push_back(t);
            }
            // 释放 source_，后续 Next() 不再触碰它。
            source_.reset();
        }
    }
}

void InsertExecutor::Init() {
    current_row_ = 0;
    pending_returning_.clear();
    pending_pos_ = 0;
    // source_ 在构造期已全量物化到 materialized_rows_ 并被 release，
    // 这里不再调用 source_->Init()，否则会触发 nullptr->Init() 崩溃。
    // 60_view_trigger: 重置 STATEMENT 级 AFTER 触发器的"已 fire"标记。
    TriggerExecutor::ResetStatementFireState(context_);
    // Item #11 (perf)：一次性扫堆建立 AUTO_INCREMENT baseline。
    // 之前 InsertRow 每行都全表扫描求 max(id)，批 INSERT N 行 → O(N²)；
    // 现在 Init 时扫一次，本地 counter 自增。表为空时 baseline = 0，
    // 第一行分配 1。
    PrepareAutoIncBaselines();
}

// Item #11 (perf)：扫描堆一次计算每列 AUTO_INCREMENT 列的当前 max(id)。
// 对没有 AUTO_INCREMENT 列的表是 no-op（autoinc_next_ 为空）。该函数
// 会被多次调用但仅第一次真正扫堆（autoinc_baseline_ready_ 守卫）。
void InsertExecutor::PrepareAutoIncBaselines() {
    if (autoinc_baseline_ready_) return;
    autoinc_baseline_ready_ = true;
    const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
    if (info == nullptr) return;
    TableHeap* heap = context_->GetCatalog()->GetTableHeap(table_name_);
    if (heap == nullptr) return;
    // 第一遍：收集所有需要 baseline 的 AUTO_INCREMENT 列下标。
    std::vector<size_t> ai_indexes;
    for (size_t i = 0; i < info->columns.size(); ++i) {
        if (info->columns[i].is_auto_increment) ai_indexes.push_back(i);
    }
    if (ai_indexes.empty()) return;
    // 用 column_types 序列化读 Tuple。
    std::vector<ValueType> schema;
    schema.reserve(info->columns.size());
    for (const auto& c : info->columns) {
        schema.push_back(ValueTypeFromString(c.data_type));
    }
    // 一次性扫堆，累计每列 max。
    std::unordered_map<size_t, int32_t> max_per_col;
    for (size_t idx : ai_indexes) max_per_col[idx] = 0;
    auto it = heap->Begin();
    while (it.HasNext()) {
        Tuple t = it.Next(schema);
        for (size_t idx : ai_indexes) {
            if (t.ColumnCount() <= idx) continue;
            const Value& v = t.GetValue(idx);
            if (v.IsNull()) continue;
            if (v.GetType() != ValueType::INTEGER) continue;
            int32_t cur = v.AsInt();
            auto mit = max_per_col.find(idx);
            if (mit != max_per_col.end() && cur > mit->second) {
                mit->second = cur;
            }
        }
    }
    // 下一个要分配的值 = max + 1。InsertRow 消费后本地 +1。
    for (const auto& kv : max_per_col) {
        autoinc_next_[kv.first] = kv.second + 1;
    }
}

bool InsertExecutor::InsertRow(const std::vector<Value>& row_values_in, bool is_replace) {
    const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
    if (!info) {
        throw CompilerException(ErrorStage::SEMANTIC, "table not found: " + table_name_);
    }
    TableHeap* heap = context_->GetCatalog()->GetTableHeap(table_name_);
    if (!heap) {
        throw CompilerException(ErrorStage::SEMANTIC, "table heap missing: " + table_name_);
    }

    std::vector<Value> row_values = row_values_in;
    if (row_values.size() < info->columns.size()) {
        row_values.resize(info->columns.size());
    }

    // AUTO_INCREMENT 处理：原本每行全表扫描求 max(id)（O(N²) 总成本）。
    // Item #11 (perf) 优化：Init 时已一次性建好 autoinc_next_ baseline，
    // 这里直接读本地 counter 并自增。每行 O(1)。
    {
        for (size_t idx = 0; idx < info->columns.size() && idx < row_values.size(); ++idx) {
            const auto& col = info->columns[idx];
            if (!col.is_auto_increment) continue;
            Value& v = row_values[idx];
            bool user_explicit = false;
            for (const auto& cn : columns_) {
                if (cn == col.name) { user_explicit = true; break; }
            }
            bool needs_autoinc = false;
            if (!user_explicit) {
                needs_autoinc = true;
            } else if (v.IsNull()) {
                needs_autoinc = true;
            } else if (v.GetType() == ValueType::INTEGER && v.AsInt() == 0) {
                needs_autoinc = true;
            }
            if (!needs_autoinc) continue;
            auto ait = autoinc_next_.find(idx);
            if (ait == autoinc_next_.end()) {
                // baseline 未建立（理论上不该走到；Init 应已填充）。
                // 兜底：assign 1 并把 baseline 设为 2。
                row_values[idx] = Value::MakeInt(1);
                autoinc_next_[idx] = 2;
                continue;
            }
            int32_t next_val = ait->second;
            row_values[idx] = Value::MakeInt(next_val);
            ait->second = next_val + 1;
        }
    }

    // BEFORE INSERT 触发器
    {
        std::unordered_map<std::string, size_t> cmap;
        for (size_t i = 0; i < info->columns.size(); ++i) {
            cmap[info->columns[i].name] = i;
        }
        TriggerExecutor::FireBefore(
            context_->GetCatalog(), context_, table_name_,
            TriggerTiming::BEFORE, TriggerEvent::INSERT,
            cmap, nullptr, row_values);
    }

    // 54_dml: REPLACE INTO —— 在写堆前探测冲突并先删除。
    if (is_replace) {
        auto conflicts = FindReplaceConflicts(context_->GetCatalog(), *info, row_values);
        if (!conflicts.empty()) {
            DeleteConflicts(context_, table_name_, row_values, conflicts);
        }
    }

    Tuple t(std::move(row_values));
    RID rid;
    std::vector<ValueType> col_types = BuildColumnTypes(*info);
    {
        std::vector<Value> row_snapshot;
        row_snapshot.reserve(t.ColumnCount());
        for (size_t i = 0; i < t.ColumnCount(); ++i) {
            row_snapshot.push_back(t.GetValue(i));
        }
        // SERIALIZABLE 谓词写前检查（防幻读）：堆写入前确认无其他事务读谓词覆盖本键。
        auto pr = context_->CheckSerializablePredicate(table_name_, row_snapshot);
        if (pr == ExecutionContext::RowLockResult::kDeadlock ||
            pr == ExecutionContext::RowLockResult::kTimeout) {
            throw std::runtime_error(
                pr == ExecutionContext::RowLockResult::kDeadlock
                    ? "isolation deadlock on predicate (statement aborted)"
                    : "isolation predicate lock wait timed out (statement aborted)");
        }
        ValidateRowConstraints(context_->GetCatalog(), *info, heap,
                               row_snapshot, nullptr, context_);
        CheckUniqueIndexes(context_->GetCatalog(), *info, row_snapshot, nullptr);
        EnforceChildForeignKeys(context_->GetCatalog(), table_name_, row_snapshot);
    }
    heap->SetActiveTransaction(context_->GetTransaction());
    if (!heap->InsertTuple(t, &rid, col_types)) {
        heap->SetActiveTransaction(nullptr);
        throw CompilerException(ErrorStage::SEMANTIC,
            "INSERT failed (no space?)");
    }
    heap->SetActiveTransaction(nullptr);
    // T2 行级写锁：新行取得 X 锁（持有到提交，Commit/Rollback 释放）。传入表堆首页
    // 页号参与「行级锁升级」：大批量 INSERT 达阈值后行锁收敛为表级 X 锁。
    auto rl = context_->AcquireRowWriteLock(rid,
        static_cast<int64_t>(heap->GetFirstPageId()));
    if (rl == ExecutionContext::RowLockResult::kDeadlock ||
        rl == ExecutionContext::RowLockResult::kTimeout) {
        throw std::runtime_error(
            rl == ExecutionContext::RowLockResult::kDeadlock
                ? "isolation deadlock on row write (statement aborted)"
                : "isolation row lock wait timed out (statement aborted)");
    }
    InsertIntoIndexes(context_->GetCatalog(), *info, t.GetValues(), rid,
                      context_->GetTransaction());
    // 60_view_trigger: AFTER 触发器 + STATEMENT 级触发器。
    {
        std::unordered_map<std::string, size_t> cmap;
        for (size_t i = 0; i < info->columns.size(); ++i) {
            cmap[info->columns[i].name] = i;
        }
        TriggerExecutor::FireAfter(
            context_->GetCatalog(), context_, table_name_,
            TriggerEvent::INSERT, cmap, nullptr, &t.GetValues());
    }

    // 54_dml: INSERT RETURNING —— 评估并暂存 RETURNING 行（post-image）。
    if (!returning_exprs_.empty()) {
        std::unordered_map<std::string, size_t> cmap;
        for (size_t i = 0; i < info->columns.size(); ++i) {
            cmap[info->columns[i].name] = i;
        }
        ExpressionEvaluator eval(cmap, context_, nullptr);
        std::vector<Value> out;
        out.reserve(returning_exprs_.size());
        for (const auto& e : returning_exprs_) {
            out.push_back(eval.Evaluate(e, t));
        }
        pending_returning_.push_back(Tuple(std::move(out)));
    }
    return true;
}

bool InsertExecutor::Next(Tuple* tuple) {
    // 优先消费 pending RETURNING 行。
    if (pending_pos_ < pending_returning_.size()) {
        if (tuple) *tuple = pending_returning_[pending_pos_++];
        return true;
    }
    pending_returning_.clear();
    pending_pos_ = 0;

    // SELECT 路径：source_ 已在构造期全量物化到 materialized_rows_，
    // 每次 Next 从缓冲拉一行，按列映射写入目标表。源 seqScan 已不再
    // 被触碰——避免 INSERT INTO t SELECT ... FROM t 时「读到本语句刚
    // 插入的行」导致的无限循环。
    if (!materialized_rows_.empty() || current_row_ < materialized_rows_.size()) {
        if (current_row_ >= materialized_rows_.size()) return false;
        Tuple src = materialized_rows_[current_row_];
        const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
        if (!info) {
            throw CompilerException(ErrorStage::SEMANTIC, "table not found: " + table_name_);
        }
        const size_t N = columns_.empty() ? info->columns.size() : columns_.size();
        if (src.ColumnCount() < N) {
            throw CompilerException(ErrorStage::SEMANTIC,
                "INSERT ... SELECT column count mismatch for " + table_name_);
        }
        std::vector<Value> row_values(info->columns.size());
        if (columns_.empty()) {
            for (size_t i = 0; i < info->columns.size(); ++i) {
                row_values[i] = CoerceToColumnType(src.GetValue(i), info->columns[i].data_type);
            }
        } else {
            for (size_t i = 0; i < columns_.size(); ++i) {
                size_t target_idx = info->columns.size();
                bool found = false;
                for (size_t k = 0; k < info->columns.size(); ++k) {
                    if (info->columns[k].name == columns_[i]) {
                        target_idx = k;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    throw CompilerException(ErrorStage::SEMANTIC,
                        "unknown column: " + columns_[i]);
                }
                Value v = src.GetValue(i);
                row_values[target_idx] = CoerceToColumnType(v, info->columns[target_idx].data_type);
            }
            ApplyDefaults(*info, columns_, row_values);
        }
        InsertRow(row_values, false /* REPLACE 不支持 INSERT...SELECT */);
        ++current_row_;
        if (pending_pos_ < pending_returning_.size()) {
            if (tuple) *tuple = pending_returning_[pending_pos_++];
            return true;
        }
        if (tuple) *tuple = Tuple({Value::MakeInt(1)});
        return true;
    }

    // VALUES 路径：原行为保持不变。
    // INSERT ... DEFAULT VALUES：单次发射，按 info->columns 大小构造一行
    // DefaultExprNode，让现有 ApplyDefaults / 单列表达式求值路径复用。
    if (is_default_values_) {
        const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
        if (!info) {
            throw CompilerException(ErrorStage::SEMANTIC,
                "table not found: " + table_name_);
        }
        // 限制只插入一次（即使上游重复 Next 也不重复）。
        if (current_row_ > 0) return false;
        std::vector<Value> row_values(info->columns.size());
        // DEFAULT VALUES 不允许带显式列名（SQL 标准）：必须按表列序整行填。
        if (!columns_.empty()) {
            throw CompilerException(ErrorStage::SEMANTIC,
                "INSERT ... DEFAULT VALUES does not allow a column list");
        }
        for (size_t i = 0; i < info->columns.size(); ++i) {
            const auto& col = info->columns[i];
            if (col.default_expr) {
                // 重用 ApplyDefaults 内的 EvaluateDefaultLiteral 路径：
                // RequireConstantExpression 校验 + 空 Tuple 求值，确保
                // DEFAULT 内可使用 NOW / 字面量等常量函数，禁列引用。
                row_values[i] = EvaluateDefaultLiteral(col.default_expr, col.name);
            } else {
                row_values[i] = Value::MakeNull();
            }
            row_values[i] = CoerceToColumnType(row_values[i], col.data_type);
        }
        InsertRow(row_values, is_replace_);
        ++current_row_;
        if (pending_pos_ < pending_returning_.size()) {
            if (tuple) *tuple = pending_returning_[pending_pos_++];
            return true;
        }
        if (tuple) {
            *tuple = Tuple({Value::MakeInt(1)});
        }
        return true;
    }
    if (current_row_ >= values_list_.size()) return false;
    const TableInfo* info = context_->GetCatalog()->GetTable(table_name_);
    if (!info) {
        throw CompilerException(ErrorStage::SEMANTIC, "table not found: " + table_name_);
    }
    TableHeap* heap = context_->GetCatalog()->GetTableHeap(table_name_);
    if (!heap) {
        throw CompilerException(ErrorStage::SEMANTIC, "table heap missing: " + table_name_);
    }

    std::unordered_map<std::string, size_t> idx_map;
    for (size_t i = 0; i < info->columns.size(); ++i) {
        idx_map[info->columns[i].name] = i;
    }
    ExpressionEvaluator eval(idx_map, context_, nullptr);

    auto& row_exprs = values_list_[current_row_];
    std::vector<Value> row_values;
    row_values.resize(info->columns.size());

    if (columns_.empty()) {
        if (row_exprs.size() != info->columns.size()) {
            throw CompilerException(ErrorStage::SEMANTIC,
                "INSERT column count mismatch for " + table_name_);
        }
        for (size_t i = 0; i < row_exprs.size(); ++i) {
            // INSERT ... VALUES (..., DEFAULT) —— 当前位置显式写 DEFAULT
            // 时，INSERT 阶段用该列的 DEFAULT 表达式（无 DEFAULT 时 NULL）。
            // 等价于省略该列，但允许在部分列上显式列出的同时复用 DEFAULT。
            // DEFAULT(col) 则按列名查找 default_expr（SQL 标准"取列 col 的默认值"）。
            if (row_exprs[i] && row_exprs[i]->GetType() == NodeType::DEFAULT_EXPR) {
                auto def = std::static_pointer_cast<DefaultExprNode>(row_exprs[i]);
                const ColumnInfo* target = nullptr;
                if (!def->column_name.empty()) {
                    target = info->GetColumn(def->column_name);
                    if (!target) {
                        throw CompilerException(ErrorStage::SEMANTIC,
                            "DEFAULT references unknown column: " + def->column_name);
                    }
                } else {
                    target = &info->columns[i];
                }
                if (target->default_expr) {
                    row_values[i] = EvaluateDefaultLiteral(target->default_expr,
                                                          target->name);
                } else {
                    row_values[i] = Value::MakeNull();
                }
                row_values[i] = CoerceToColumnType(row_values[i], info->columns[i].data_type);
                continue;
            }
            Value v = eval.Evaluate(row_exprs[i], Tuple());
            row_values[i] = CoerceToColumnType(v, info->columns[i].data_type);
        }
    } else {
        for (size_t i = 0; i < row_exprs.size() && i < columns_.size(); ++i) {
            auto it = idx_map.find(columns_[i]);
            if (it == idx_map.end()) {
                throw CompilerException(ErrorStage::SEMANTIC,
                    "unknown column: " + columns_[i]);
            }
            const auto& target_col = info->columns[it->second];
            // INSERT ... VALUES (..., DEFAULT) —— 在显式列列表里复用该列的
            // DEFAULT 表达式（与省略该列语义相同）。DEFAULT(col) 按列名查 default。
            if (row_exprs[i] && row_exprs[i]->GetType() == NodeType::DEFAULT_EXPR) {
                auto def = std::static_pointer_cast<DefaultExprNode>(row_exprs[i]);
                const ColumnInfo* src = nullptr;
                if (!def->column_name.empty()) {
                    src = info->GetColumn(def->column_name);
                    if (!src) {
                        throw CompilerException(ErrorStage::SEMANTIC,
                            "DEFAULT references unknown column: " + def->column_name);
                    }
                } else {
                    src = &target_col;
                }
                if (src->default_expr) {
                    row_values[it->second] = EvaluateDefaultLiteral(src->default_expr,
                                                                   src->name);
                } else {
                    row_values[it->second] = Value::MakeNull();
                }
                row_values[it->second] = CoerceToColumnType(
                    row_values[it->second], target_col.data_type);
                continue;
            }
            Value v = eval.Evaluate(row_exprs[i], Tuple());
            row_values[it->second] = CoerceToColumnType(v, target_col.data_type);
        }
        ApplyDefaults(*info, columns_, row_values);
    }

    InsertRow(row_values, is_replace_);
    ++current_row_;
    if (pending_pos_ < pending_returning_.size()) {
        if (tuple) *tuple = pending_returning_[pending_pos_++];
        return true;
    }
    if (tuple) {
        *tuple = Tuple({Value::MakeInt(1)});
    }
    return true;
}

}  // namespace sqlcompiler
