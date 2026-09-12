#include "plan/Planner.h"

#include "catalog/SystemCatalog.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <cctype>
#include <functional>
#include <set>
#include <utility>

namespace sqlcompiler {

namespace {

ExprPtr RewriteAggregateRefs(const ExprPtr& expr,
                              const std::vector<ExprPtr>& aggregate_exprs);

bool ContainsAggregateExpr(const ExprPtr& e) {
    if (!e) return false;
    switch (e->GetType()) {
        case NodeType::LITERAL_EXPR:
        case NodeType::COLUMN_REF_EXPR:
            return false;
        case NodeType::BINARY_EXPR: {
            auto b = std::static_pointer_cast<BinaryExpr>(e);
            return ContainsAggregateExpr(b->left) || ContainsAggregateExpr(b->right);
        }
        case NodeType::UNARY_EXPR: {
            auto u = std::static_pointer_cast<UnaryExpr>(e);
            return ContainsAggregateExpr(u->operand);
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            auto f = std::static_pointer_cast<FunctionCallExpr>(e);
            std::string name;
            for (char c : f->function_name) name.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            // 60_funcs: 把新增的统计 / 有序集合聚合纳入聚合识别，否则
            // SELECT STDDEV(v) FROM stats 会被当成投影表达式漏掉聚合路径。
            if (name == "COUNT" || name == "SUM" || name == "AVG" ||
                name == "MIN" || name == "MAX" ||
                name == "STDDEV" || name == "STDDEV_POP" || name == "STDDEV_SAMP" ||
                name == "VARIANCE" || name == "VAR_POP" || name == "VAR_SAMP" ||
                name == "MEDIAN" ||
                name == "STRING_AGG" || name == "GROUP_CONCAT" ||
                name == "PERCENTILE_CONT" || name == "PERCENTILE_DISC") {
                return true;
            }
            for (auto& a : f->arguments) {
                if (ContainsAggregateExpr(a)) return true;
            }
            return false;
        }
    }
    return false;
}

bool SelectHasAggregate(const SelectStatement& stmt) {
    for (const auto& e : stmt.select_list) {
        if (ContainsAggregateExpr(e)) return true;
    }
    if (stmt.having_clause && ContainsAggregateExpr(stmt.having_clause)) return true;
    return false;
}

// 表达式树中是否出现 WINDOW_FUNC_EXPR（含 FUNCTION_CALL_EXPR 内出现的窗口列）
bool ContainsWindowFunc(const ExprPtr& e) {
    if (!e) return false;
    if (e->GetType() == NodeType::WINDOW_FUNC_EXPR) return true;
    switch (e->GetType()) {
        case NodeType::BINARY_EXPR: {
            auto b = std::static_pointer_cast<BinaryExpr>(e);
            return ContainsWindowFunc(b->left) || ContainsWindowFunc(b->right);
        }
        case NodeType::UNARY_EXPR: {
            auto u = std::static_pointer_cast<UnaryExpr>(e);
            return ContainsWindowFunc(u->operand);
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            auto f = std::static_pointer_cast<FunctionCallExpr>(e);
            for (auto& a : f->arguments) {
                if (ContainsWindowFunc(a)) return true;
            }
            return false;
        }
        default:
            return false;
    }
}

bool SelectHasWindowFunc(const SelectStatement& stmt) {
    for (const auto& e : stmt.select_list) {
        if (ContainsWindowFunc(e)) return true;
    }
    return false;
}

// Rewrite aggregate function calls in `expr` to ColumnRef pointing at the
// position of the matching expression in `aggregate_exprs`.
ExprPtr RewriteAggregateRefs(const ExprPtr& expr,
                              const std::vector<ExprPtr>& aggregate_exprs) {
    if (!expr) return nullptr;
    switch (expr->GetType()) {
        case NodeType::LITERAL_EXPR:
        case NodeType::COLUMN_REF_EXPR:
            return expr;
        case NodeType::BINARY_EXPR: {
            auto b = std::static_pointer_cast<BinaryExpr>(expr);
            auto left = RewriteAggregateRefs(b->left, aggregate_exprs);
            auto right = RewriteAggregateRefs(b->right, aggregate_exprs);
            return std::make_shared<BinaryExpr>(b->op, left, right);
        }
        case NodeType::UNARY_EXPR: {
            auto u = std::static_pointer_cast<UnaryExpr>(expr);
            auto operand = RewriteAggregateRefs(u->operand, aggregate_exprs);
            return std::make_shared<UnaryExpr>(u->op, operand);
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            auto f = std::static_pointer_cast<FunctionCallExpr>(expr);
            std::string name;
            for (char c : f->function_name) name.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            if (name == "COUNT" || name == "SUM" || name == "AVG" ||
                name == "MIN" || name == "MAX" ||
                // 60_funcs: HAVING 中也可能引用 STDDEV/MEDIAN 等聚合
                name == "STDDEV" || name == "STDDEV_POP" || name == "STDDEV_SAMP" ||
                name == "VARIANCE" || name == "VAR_POP" || name == "VAR_SAMP" ||
                name == "MEDIAN" ||
                name == "STRING_AGG" || name == "GROUP_CONCAT" ||
                name == "PERCENTILE_CONT" || name == "PERCENTILE_DISC") {
                // Find matching aggregate in aggregate_exprs by name
                for (size_t i = 0; i < aggregate_exprs.size(); ++i) {
                    const auto& ae = aggregate_exprs[i];
                    if (ae && ae->GetType() == NodeType::FUNCTION_CALL_EXPR) {
                        auto af = std::static_pointer_cast<FunctionCallExpr>(ae);
                        std::string aname;
                        for (char c : af->function_name) aname.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                        if (aname == name) {
                            return std::make_shared<ColumnRefExpr>("", name);
                        }
                    }
                }
                return std::make_shared<ColumnRefExpr>("", name);
            }
            return expr;
        }
    }
    return expr;
}

}  // namespace

Planner::Planner(SystemCatalog* catalog, SymbolTable& symbol_table)
    : catalog_(catalog), symbol_table_(symbol_table) {
}

PlanNodePtr Planner::CreatePlan(const StatementPtr& statement) {
    if (!statement) return nullptr;
    switch (statement->GetType()) {
        case NodeType::SELECT_STMT:
            return PlanSelect(*std::static_pointer_cast<SelectStatement>(statement));
        case NodeType::WITH_STMT:
            return PlanWithClause(*std::static_pointer_cast<WithClauseStatement>(statement));
        case NodeType::INSERT_STMT:
            return PlanInsert(*std::static_pointer_cast<InsertStatement>(statement));
        case NodeType::UPDATE_STMT:
            return PlanUpdate(*std::static_pointer_cast<UpdateStatement>(statement));
        case NodeType::DELETE_STMT:
            return PlanDelete(*std::static_pointer_cast<DeleteStatement>(statement));
        case NodeType::MERGE_STMT:
            return PlanMerge(*std::static_pointer_cast<MergeStatement>(statement));
        case NodeType::CREATE_TABLE_STMT:
            return PlanCreateTable(*std::static_pointer_cast<CreateTableStatement>(statement));
        case NodeType::DROP_TABLE_STMT:
            return PlanDropTable(*std::static_pointer_cast<DropTableStatement>(statement));
        case NodeType::CREATE_INDEX_STMT:
            return PlanCreateIndex(*std::static_pointer_cast<CreateIndexStatement>(statement));
        case NodeType::DROP_INDEX_STMT:
            return PlanDropIndex(*std::static_pointer_cast<DropIndexStatement>(statement));
        case NodeType::TRUNCATE_TABLE_STMT:
            return PlanTruncateTable(*std::static_pointer_cast<TruncateTableStatement>(statement));
        case NodeType::ALTER_TABLE_STMT:
            return PlanAlterTable(*std::static_pointer_cast<AlterStatement>(statement));
        case NodeType::SET_OP_STMT:
            return PlanSetOperation(*std::static_pointer_cast<SetOperationStatement>(statement));
        // ---- 40_txn_view_udf ----
        case NodeType::BEGIN_STMT:
            return PlanBegin(*std::static_pointer_cast<BeginStatement>(statement));
        case NodeType::COMMIT_STMT:
            return PlanCommit(*std::static_pointer_cast<CommitStatement>(statement));
        case NodeType::ROLLBACK_STMT:
            return PlanRollback(*std::static_pointer_cast<RollbackStatement>(statement));
        case NodeType::ROLLBACK_TO_STMT:
            return PlanRollbackTo(*std::static_pointer_cast<RollbackToStatement>(statement));
        case NodeType::SAVEPOINT_STMT:
            return PlanSavepoint(*std::static_pointer_cast<SavepointStatement>(statement));
        case NodeType::RELEASE_SAVEPOINT_STMT:
            return PlanReleaseSavepoint(*std::static_pointer_cast<ReleaseSavepointStatement>(statement));
        // ---- 53_ddl: SCHEMA / SEQUENCE ----
        case NodeType::CREATE_SCHEMA_STMT:
            return PlanCreateSchema(*std::static_pointer_cast<CreateSchemaStatement>(statement));
        case NodeType::DROP_SCHEMA_STMT:
            return PlanDropSchema(*std::static_pointer_cast<DropSchemaStatement>(statement));
        case NodeType::CREATE_SEQUENCE_STMT:
            return PlanCreateSequence(*std::static_pointer_cast<CreateSequenceStatement>(statement));
        case NodeType::DROP_SEQUENCE_STMT:
            return PlanDropSequence(*std::static_pointer_cast<DropSequenceStatement>(statement));
        case NodeType::CREATE_VIEW_STMT:
            return PlanCreateView(*std::static_pointer_cast<CreateViewStatement>(statement));
        case NodeType::DROP_VIEW_STMT:
            return PlanDropView(*std::static_pointer_cast<DropViewStatement>(statement));
        // 60_view_trigger (Category 9): MATERIALIZED VIEW / ALTER MATERIALIZED VIEW
        case NodeType::CREATE_MATERIALIZED_VIEW_STMT:
            return PlanCreateMaterializedView(
                *std::static_pointer_cast<MaterializedViewStatement>(statement));
        case NodeType::ALTER_MATERIALIZED_VIEW_STMT:
            return PlanAlterMaterializedView(
                *std::static_pointer_cast<AlterMaterializedViewStatement>(statement));
        case NodeType::CREATE_TRIGGER_STMT:
            return PlanCreateTrigger(*std::static_pointer_cast<CreateTriggerStatement>(statement));
        case NodeType::DROP_TRIGGER_STMT:
            return PlanDropTrigger(*std::static_pointer_cast<DropTriggerStatement>(statement));
        case NodeType::CREATE_FUNCTION_STMT:
            return PlanCreateFunction(*std::static_pointer_cast<CreateFunctionStatement>(statement));
        case NodeType::DROP_FUNCTION_STMT:
            return PlanDropFunction(*std::static_pointer_cast<DropFunctionStatement>(statement));
        // ---- 59_procs (Category 8) ----
        case NodeType::CREATE_PROCEDURE_STMT:
            return PlanCreateProcedure(*std::static_pointer_cast<CreateProcedureStatement>(statement));
        case NodeType::DROP_PROCEDURE_STMT:
            return PlanDropProcedure(*std::static_pointer_cast<DropProcedureStatement>(statement));
        case NodeType::CALL_STMT:
            return PlanCall(*std::static_pointer_cast<CallStatement>(statement));
        // ---- 46_meta ----
        case NodeType::EXPLAIN_STMT:
            return PlanExplain(*std::static_pointer_cast<ExplainStatement>(statement));
        case NodeType::SHOW_STMT:
            return PlanShow(*std::static_pointer_cast<ShowStatement>(statement));
        default:
            return nullptr;
    }
}

PlanNodePtr Planner::PlanSelect(const SelectStatement& stmt) {
    // 视图展开：若 SELECT FROM 引用了 catalog 中的视图，把视图 SELECT 复制为
    // 派生表（derived_table）。这必须在 PlanSubqueriesInSelect 之前，否则对视图
    // 内部的子查询不会被 plan。
    if (catalog_) {
        TryExpandView(const_cast<SelectStatement&>(stmt));
    }
    // 标记 UDF 调用，让 ExpressionEvaluator 在执行期能从 catalog 取函数体。
    if (catalog_) {
        MarkUdfCallsInSelect(stmt);
    }
    // 递归地把 select_list / where / having / order_by / join 中嵌套的
    // SubqueryExprNode 关联到 subquery_plan 上。
    PlanSubqueriesInSelect(stmt);
    // 55_query: FOR UPDATE / FOR SHARE / FOR NO KEY UPDATE / FOR KEY SHARE
    // 在本单写引擎下是 parse-only hint：解析器已把它记到 stmt.for_update_kind，
    // 此处不构造任何加锁路径。锁语义本身不强制执行（详见
    // tests/sql/55_query.sql 顶部说明）。
    (void)stmt.for_update_kind;
    // 55_query: VALUES 派生表 FROM (VALUES (1,'a'), (2,'b')) AS t(id, name)
    // 把 values_rows / values_column_aliases 装到 ValuesNode 上，子计划 children 为空。
    if (!stmt.values_rows.empty()) {
        auto values = std::make_shared<ValuesNode>(
            stmt.values_rows, stmt.values_column_aliases, stmt.derived_alias);
        PlanNodePtr current = values;
        // outer WHERE
        if (stmt.where_clause) {
            auto f = std::make_shared<FilterNode>(stmt.where_clause);
            f->children.push_back(current);
            current = f;
        }
        bool has_window = SelectHasWindowFunc(stmt);
        if (has_window) {
            auto wn = std::make_shared<WindowNode>(stmt.select_list, stmt.select_aliases,
                                                   stmt.named_windows);
            wn->children.push_back(current);
            current = wn;
        } else {
            auto proj = std::make_shared<ProjectNode>(stmt.select_list, stmt.select_aliases, stmt.is_distinct);
            proj->children.push_back(current);
            current = proj;
        }
        if (!stmt.order_by.empty()) {
            auto s = std::make_shared<SortNode>(stmt.order_by);
            s->children.push_back(current);
            current = s;
        }
        if (stmt.limit >= 0) {
            auto l = std::make_shared<LimitNode>(stmt.limit, stmt.limit_offset);
            l->children.push_back(current);
            current = l;
        }
        return current;
    }
    // 派生表 FROM (SELECT ...) AS alias —— 把子查询递归规划成子树当作 FROM。
    if (stmt.derived_table) {
        PlanNodePtr sub_plan = PlanSelect(*stmt.derived_table);
        // 列引用解析走 `derived_alias.<col>` / 不限定的列名。
        // Planner 不构造额外节点，直接交给 SeqScanExecutor 是不行的，
        // 所以这里把派生表作为 SeqScanNode(target=alias) 之外的子树：
        // 用 SeqScanNode(table_name=derived_alias, table_alias=derived_alias) 作为占位，
        // 但实际数据来自子查询。最简单的实现：把子查询当作 alias 的 ScanExecutor。
        // 为避免重写 SeqScanExecutor，这里把子查询计划用一个 SeqScanNode + 显式
        // 路径替代：直接让 outer PlanSelect 使用子查询的输出元组。
        // 我们用「借用 SeqScanNode 占位」+ ExecutionEngine 内部识别 derived_alias
        // 并改走子查询计划的方式实现。为了不改动 ExecutionEngine，最简洁的
        // 做法是：在 outer PlanSelect 中再插入一个节点（伪 ScanNode），但
        // ExecutionEngine 构造 SeqScan 时若 table_name == "<derived_alias>" 则替换
        // 为子查询计划。这条改动在 ExecutionEngine.cpp 里做（见对应 case）。
        auto scan = std::make_shared<SeqScanNode>(stmt.derived_alias, stmt.derived_alias);
        // 把子计划「挂」到该 SeqScan 的 children 上是不规范的，但 ExecutionEngine
        // 在识别到 derived_alias 时会忽略 SeqScanNode 的 table_name 而改走子计划。
        scan->children.push_back(sub_plan);
        PlanNodePtr current = scan;
        // outer WHERE
        if (stmt.where_clause) {
            auto f = std::make_shared<FilterNode>(stmt.where_clause);
            f->children.push_back(current);
            current = f;
        }
        // outer SELECT list 可能含窗口函数，递归走 PlanSelect 头部逻辑。
        // 复制当前 SelectStatement 把 from_table 留空（derived_table 已显式置位）
        // 的简化路径：直接构造 inner-aware 的后续逻辑：
        // 由于外层 SELECT 的列引用走 `alias.col`，column_index_map 由 ExecutionEngine
        // 通过 derived_alias 映射到子查询输出位置。这里不需要从子表列表中收集表。
        // 直接走与简单 SELECT 相同的尾段（PROJECT / WINDOW / ORDER BY / LIMIT）。
        bool has_window = SelectHasWindowFunc(stmt);
        if (has_window) {
            auto wn = std::make_shared<WindowNode>(stmt.select_list, stmt.select_aliases,
                                                   stmt.named_windows);
            wn->children.push_back(current);
            current = wn;
        } else {
            auto proj = std::make_shared<ProjectNode>(stmt.select_list, stmt.select_aliases, stmt.is_distinct);
            proj->children.push_back(current);
            current = proj;
        }
        if (!stmt.order_by.empty()) {
            auto s = std::make_shared<SortNode>(stmt.order_by);
            s->children.push_back(current);
            current = s;
        }
        if (stmt.limit >= 0) {
            auto l = std::make_shared<LimitNode>(stmt.limit, stmt.limit_offset);
            l->children.push_back(current);
            current = l;
        }
        return current;
    }

    PlanNodePtr current;
    if (!stmt.from_table.empty()) {
        current = std::make_shared<SeqScanNode>(stmt.from_table, stmt.from_table_alias);
    }
    // Joins: each join produces a JoinNode with two children
    //   children[0] = previous chain (left side)
    //   children[1] = new SeqScanNode(j.table_name) (right side)
    for (const auto& j : stmt.joins) {
        ExprPtr effective_condition = j.on_condition;
        // USING / NATURAL 翻译为合成 ON 条件：a.col = b.col AND ...
        if (!j.using_columns.empty() || j.is_natural) {
            std::vector<std::string> cols = j.using_columns;
            if (j.is_natural) {
                cols.clear();
                const TableInfo* left_info = symbol_table_.GetTable(stmt.from_table);
                const TableInfo* right_info = symbol_table_.GetTable(j.table_name);
                if (left_info && right_info) {
                    for (const auto& lc : left_info->columns) {
                        for (const auto& rc : right_info->columns) {
                            if (lc.name == rc.name) {
                                cols.push_back(lc.name);
                                break;
                            }
                        }
                    }
                }
            }
            ExprPtr combined = nullptr;
            std::string left_label = stmt.from_table;
            std::string right_label = j.table_name;
            for (const auto& cn : cols) {
                auto lhs = std::make_shared<ColumnRefExpr>(left_label, cn);
                auto rhs = std::make_shared<ColumnRefExpr>(right_label, cn);
                auto eq = std::make_shared<BinaryExpr>(BinaryOperator::EQUAL, lhs, rhs);
                if (!combined) {
                    combined = eq;
                } else {
                    combined = std::make_shared<BinaryExpr>(
                        BinaryOperator::AND, combined, eq);
                }
            }
            effective_condition = combined;
        }
        // 55_query: LATERAL 派生表 join ——对每条外层行跑一次右子计划（带 outer_bind），
        // 并把产生的右行与左行拼接。Planner 这里生成 ApplyNode；执行器负责每行重算。
        // 右子查询用一个 SeqScanNode 占位（table_alias == table_name）包装，
        // 让 BuildCombinedColumnIndexMapWithDerived 走「派生表」路径按输出列
        // 暴露给外层 cmap，避免把内层 SeqScan 的真实表名写入 outer_cmap。
        if (j.is_lateral) {
            std::string derived_alias = !j.table_alias.empty() ? j.table_alias : j.table_name;
            // 收集右子计划的「内层表名」：from_table_alias 与 joins 的别名/表名，
            // 供 ApplyExecutor 在 ctx 中注册，让右子计划的 evaluator 知道哪些限定
            // 列是内层（不走 outer_bind）。
            std::vector<std::string> lateral_inner_tables;
            if (j.lateral_subquery) {
                if (!j.lateral_subquery->from_table_alias.empty()) {
                    lateral_inner_tables.push_back(j.lateral_subquery->from_table_alias);
                }
                for (const auto& lj : j.lateral_subquery->joins) {
                    if (!lj.table_name.empty()) {
                        lateral_inner_tables.push_back(lj.table_name);
                    }
                    if (!lj.table_alias.empty()) {
                        lateral_inner_tables.push_back(lj.table_alias);
                    }
                }
            }
            auto apply = std::make_shared<ApplyNode>(false, derived_alias,
                                                    lateral_inner_tables);
            apply->children.push_back(current);
            if (j.lateral_subquery) {
                PlanNodePtr lateral_plan = PlanSelect(*j.lateral_subquery);
                auto placeholder = std::make_shared<SeqScanNode>(derived_alias, derived_alias);
                placeholder->children.push_back(lateral_plan);
                apply->children.push_back(placeholder);
            }
            current = apply;
            continue;
        }
        auto join = std::make_shared<JoinNode>(j.join_type, effective_condition);
        join->children.push_back(current);
        join->children.push_back(std::make_shared<SeqScanNode>(j.table_name, j.table_alias));
        current = join;
    }
    // WHERE
    if (current && stmt.where_clause) {
        auto f = std::make_shared<FilterNode>(stmt.where_clause);
        f->children.push_back(current);
        current = f;
    }
    // ---- 60_funcs: GROUPING SETS / ROLLUP / CUBE 展开 ----
    // 把 grouping_sets 拆分为多个"按子集分组"的子 SELECT，UNION ALL 起来。
    // 在 PlanSelect 主流程中，scan_input 已经是 WHERE 之后的子计划。
    if (!stmt.grouping_sets.empty()) {
        current = PlanGroupingSets(stmt, current);
        // 跳过原始的 GROUP BY 路径；HAVING 已经在子 SELECT 内部处理。
        bool has_window = SelectHasWindowFunc(stmt);
        if (has_window) {
            auto wn = std::make_shared<WindowNode>(stmt.select_list, stmt.select_aliases,
                                                   stmt.named_windows);
            if (current) wn->children.push_back(current);
            current = wn;
        } else {
            auto proj = std::make_shared<ProjectNode>(stmt.select_list, stmt.select_aliases, stmt.is_distinct);
            if (current) proj->children.push_back(current);
            current = proj;
        }
        if (!stmt.order_by.empty()) {
            auto s = std::make_shared<SortNode>(stmt.order_by);
            if (current) s->children.push_back(current);
            current = s;
        }
        if (stmt.limit >= 0) {
            auto l = std::make_shared<LimitNode>(stmt.limit, stmt.limit_offset);
            if (current) l->children.push_back(current);
            current = l;
        }
        return current;
    }
    // Aggregate: needed when GROUP BY present OR SELECT/HAVING uses aggregate functions
    bool needs_agg = !stmt.group_by.empty() || SelectHasAggregate(stmt);
    if (needs_agg) {
        // aggregate_exprs = the SELECT list expressions (AggregateExecutor produces
        // one Tuple per group with values matching the SELECT list)
        auto agg = std::make_shared<AggregateNode>(
            stmt.group_by, stmt.select_list, stmt.select_aliases);
        if (current) agg->children.push_back(current);
        current = agg;
    }
    // HAVING (applied to Aggregate output)
    if (stmt.having_clause && needs_agg) {
        // Rewrite aggregate function calls in HAVING predicate into ColumnRef
        // pointing at the corresponding position in the aggregate output tuple.
        std::vector<ExprPtr> aggs;
        for (const auto& e : stmt.select_list) aggs.push_back(e);
        auto having = RewriteAggregateRefs(stmt.having_clause, aggs);
        auto f = std::make_shared<FilterNode>(having);
        f->children.push_back(current);
        current = f;
    }
    bool has_window = SelectHasWindowFunc(stmt);
    if (has_window) {
        auto wn = std::make_shared<WindowNode>(stmt.select_list, stmt.select_aliases,
                                               stmt.named_windows);
        if (current) wn->children.push_back(current);
        current = wn;
    } else {
        // PROJECT (始终在 Sort / Limit 之前，便于 ORDER BY 引用 SELECT 别名)
        auto proj = std::make_shared<ProjectNode>(stmt.select_list, stmt.select_aliases, stmt.is_distinct);
        if (current) proj->children.push_back(current);
        current = proj;
    }
    // ORDER BY (构建于 Project 之后，引用别名时即可见)
    if (!stmt.order_by.empty()) {
        auto s = std::make_shared<SortNode>(stmt.order_by);
        if (current) s->children.push_back(current);
        current = s;
    }
    // LIMIT
    if (stmt.limit >= 0) {
        auto l = std::make_shared<LimitNode>(stmt.limit, stmt.limit_offset);
        l->children.push_back(current);
        current = l;
    }
    return current;
}

PlanNodePtr Planner::PlanInsert(const InsertStatement& stmt) {
    // 43_upsert: ON DUPLICATE KEY UPDATE 路径走 UpsertNode，由 UpsertExecutor 处理。
    // 注意：目前仅支持 VALUES 数据源（query 非空时无候选行，无法直接构造冲突行）。
    if (stmt.has_on_duplicate) {
        auto node = std::make_shared<UpsertNode>(stmt.table_name, stmt.columns,
                                                 stmt.values_list,
                                                 stmt.upsert_assignments);
        node->returning_exprs = stmt.returning_exprs;
        node->returning_aliases = stmt.returning_aliases;
        return node;
    }
    // 60_view_trigger: INSERT INTO view_name —— V1 接受语法并存储 CHECK OPTION。
    // 当 target 是单表 view（无 JOIN、无嵌套子查询）时，把目标表改写为底层表，
    // 让写路径直接进入底层 TableHeap。CHECK OPTION 评估放到执行期由 InsertExecutor
    // 校验（在主约束校验前）。多表 / 嵌套视图保持原 INSERT 语义，由执行期报"表不存在"。
    std::string insert_target = stmt.table_name;
    if (catalog_ != nullptr && !catalog_->HasTable(stmt.table_name)) {
        const SystemCatalog::ViewDefinition* v = catalog_->LookupView(stmt.table_name);
        if (v != nullptr && v->query != nullptr && v->query->joins.empty() &&
            !v->query->from_table.empty() && catalog_->HasTable(v->query->from_table)) {
            insert_target = v->query->from_table;
            InsertStatement& mut = const_cast<InsertStatement&>(stmt);
            mut.table_name = insert_target;
        }
    }
    auto node = std::make_shared<InsertNode>(insert_target, stmt.columns, stmt.values_list);
    // 54_dml: REPLACE INTO 与 RETURNING 透传到执行器。
    node->is_replace = stmt.is_replace;
    node->returning_exprs = stmt.returning_exprs;
    node->returning_aliases = stmt.returning_aliases;
    if (stmt.query) {
        // INSERT INTO dst SELECT ... —— 把 SELECT/WITH/SetOp 转换为内部子计划，
        // 并把它挂到 children[0] 上，作为执行期 INSERT 的"输入源"。
        if (auto sel = std::dynamic_pointer_cast<SelectStatement>(stmt.query)) {
            node->query_plan = PlanSelect(*sel);
        } else if (auto with = std::dynamic_pointer_cast<WithClauseStatement>(stmt.query)) {
            node->query_plan = PlanWithClause(*with);
        } else if (auto sop = std::dynamic_pointer_cast<SetOperationStatement>(stmt.query)) {
            node->query_plan = PlanSetOperation(*sop);
        }
        if (node->query_plan) {
            node->children.push_back(node->query_plan);
        }
    }
    return node;
}

PlanNodePtr Planner::PlanUpdate(const UpdateStatement& stmt) {
    // 54_dml: UPDATE ... FROM —— 走 UpdateFromNode + JoinNode 子计划。
    // 实现：对 stmt.from_sources 做左深 join（与 SELECT 的 join 路径对齐）；
    // 连接条件仍由 where_clause 描述，所以 planner 这里不构造 JoinNode 的 condition
    // —— 把它留给 UpdateFromExecutor 在每行 joined tuple 上评估 WHERE 过滤；
    // 但 ON 条件对 UPDATE FROM 无直接意义，where_clause 中形如 t.id = s.tid 即可。
    // V1 范围：from_sources 仅支持普通表名 + 可选别名（不支持 (SELECT ...) 派生表，
    // parser 已在该路径抛错）。
    if (!stmt.from_sources.empty()) {
        // 把 target 作为最左表，from_sources 逐个右联。每个 join 是 CROSS JOIN
        // （无 ON 条件），由 WHERE 子句统一过滤；这是 PG/Oracle 风格。
        PlanNodePtr joined = std::make_shared<SeqScanNode>(
            stmt.table_name,
            stmt.table_alias.empty() ? stmt.table_name : stmt.table_alias);
        for (const auto& j : stmt.from_sources) {
            auto join = std::make_shared<JoinNode>(JoinType::INNER, nullptr);
            join->children.push_back(joined);
            join->children.push_back(std::make_shared<SeqScanNode>(
                j.table_name,
                j.table_alias.empty() ? j.table_name : j.table_alias));
            joined = join;
        }
        auto node = std::make_shared<UpdateFromNode>(stmt.table_name, stmt.assignments);
        node->target_alias = stmt.table_alias;
        node->returning_exprs = stmt.returning_exprs;
        node->returning_aliases = stmt.returning_aliases;
        node->where_clause = stmt.where_clause;
        node->children.push_back(joined);
        return node;
    }
    auto node = std::make_shared<UpdateNode>(stmt.table_name, stmt.assignments, stmt.where_clause);
    node->target_alias = stmt.table_alias;
    node->returning_exprs = stmt.returning_exprs;
    node->returning_aliases = stmt.returning_aliases;
    return node;
}

PlanNodePtr Planner::PlanDelete(const DeleteStatement& stmt) {
    auto node = std::make_shared<DeleteNode>(stmt.table_name, stmt.where_clause);
    node->returning_exprs = stmt.returning_exprs;
    node->returning_aliases = stmt.returning_aliases;
    return node;
}

PlanNodePtr Planner::PlanMerge(const MergeStatement& stmt) {
    auto node = std::make_shared<MergeNode>(stmt.target_table);
    node->target_alias = stmt.target_alias;
    node->source_table = stmt.source_table;
    node->source_alias = stmt.source_alias;
    node->on_condition = stmt.on_condition;
    node->has_matched_update = stmt.has_matched_update;
    node->matched_assignments = stmt.matched_assignments;
    node->has_not_matched_insert = stmt.has_not_matched_insert;
    node->not_matched_columns = stmt.insert_columns;
    node->not_matched_values = stmt.insert_values;
    // 把 source 转为内部子计划：
    //   - source_query 非空：直接 PlanSelect 该 SelectStatement。
    //   - 否则：source_table 是普通表名，包成 SeqScanNode。
    if (stmt.source_query) {
        ExprPtr on_copy = stmt.on_condition;
        PlanSubqueriesInExpr(on_copy);
        node->on_condition = on_copy;
        PlanSubqueriesInSelect(const_cast<SelectStatement&>(*stmt.source_query));
        node->source_plan = PlanSelect(const_cast<SelectStatement&>(*stmt.source_query));
    } else {
        node->source_plan = std::make_shared<SeqScanNode>(
            stmt.source_table,
            stmt.source_alias.empty() ? stmt.source_table : stmt.source_alias);
    }
    if (node->source_plan) {
        node->children.push_back(node->source_plan);
    }
    return node;
}

PlanNodePtr Planner::PlanCreateTable(const CreateTableStatement& stmt) {
    // 将表级 PRIMARY KEY(a, b, ...) 投影到每列 is_primary_key，便于执行器沿用既有单列 PK 路径。
    std::vector<ColumnDefinition> cols = stmt.columns;
    if (!stmt.primary_keys.empty()) {
        for (const auto& pk : stmt.primary_keys) {
            for (const auto& col_name : pk) {
                for (auto& cd : cols) {
                    if (cd.column_name == col_name) {
                        cd.is_primary_key = true;
                        break;
                    }
                }
            }
        }
    }
    return std::make_shared<CreateTableNode>(stmt.table_name, std::move(cols),
                                             stmt.primary_keys,
                                             stmt.unique_constraints,
                                             stmt.foreign_keys,
                                             stmt.table_checks,
                                             stmt.if_not_exists);
}

PlanNodePtr Planner::PlanDropTable(const DropTableStatement& stmt) {
    return std::make_shared<DropTableNode>(stmt.table_name, stmt.if_exists);
}

PlanNodePtr Planner::PlanCreateIndex(const CreateIndexStatement& stmt) {
    return std::make_shared<CreateIndexNode>(stmt.index_name, stmt.table_name,
                                             stmt.key_columns, stmt.is_unique);
}

PlanNodePtr Planner::PlanDropIndex(const DropIndexStatement& stmt) {
    return std::make_shared<DropIndexNode>(stmt.index_name, stmt.if_exists);
}

PlanNodePtr Planner::PlanTruncateTable(const TruncateTableStatement& stmt) {
    return std::make_shared<TruncateTableNode>(stmt.table_name);
}

PlanNodePtr Planner::PlanAlterTable(const AlterStatement& stmt) {
    auto node = std::make_shared<AlterTableNode>(stmt.action, stmt.table_name);
    node->column_def = stmt.column_def;
    node->drop_column_name = stmt.drop_column_name;
    node->new_table_name = stmt.new_table_name;
    // 53_ddl: RENAME COLUMN 信息透传到执行器。
    node->rename_column_old_name = stmt.rename_column_old_name;
    node->rename_column_new_name = stmt.rename_column_new_name;
    return node;
}

PlanNodePtr Planner::PlanWithClause(const WithClauseStatement& stmt) {
    // 把每个 CTE 包成一个 CteDefineNode：
    //   - 第一个 CteDefineNode 的 children[0] = 下一个 CteDefineNode（或 body）
    //   - CteDefineNode.cte_plan = 该 CTE 自身的物化计划
    //   - 递归 CTE：cte_plan = anchor，anchor_child = 递归部分
    // 执行时 Init 先物化 cte_plan 把结果注册到 context；Next 把控制权交给 children[0]
    // 这样形成"逐个定义、最后跑 body"的串联管道。
    std::vector<std::string> cte_names;
    for (const auto& cte : stmt.ctes) cte_names.push_back(cte.cte_name);

    PlanNodePtr current;
    if (stmt.body) current = PlanSelect(*stmt.body);

    // 把 body 计划里对 CTE 名的 seqScan 改写成 CteBindNode
    if (current && !cte_names.empty()) {
        RewriteCteScans(current, cte_names);
    }

    // 反向遍历：从最后一个 CTE 开始构造
    for (auto it = stmt.ctes.rbegin(); it != stmt.ctes.rend(); ++it) {
        const auto& cte = *it;
        // 递归 CTE：anchor 放进 cte_plan，递归部分（UNION ALL 右支）放进
        // anchor_child；CteDefineExecutor 会迭代运行 anchor_child 直到不再
        // 产出新行。当 parser 把 anchor 装进 cte_query 后，本路径可正常工作；
        // 旧实现（anchor 缺失）走 no-op 分支保持向后兼容。
        if (stmt.is_recursive) {
            auto def = std::make_shared<CteDefineNode>(cte.cte_name, true);
            if (cte.cte_query) {
                def->cte_plan = PlanSelect(*cte.cte_query);
            }
            if (cte.recursive_part) {
                if (auto rs = std::dynamic_pointer_cast<SelectStatement>(cte.recursive_part)) {
                    def->anchor_child = PlanSelect(*rs);
                }
            }
            if (def->cte_plan && !cte_names.empty()) {
                RewriteCteScans(def->cte_plan, cte_names);
            }
            if (def->anchor_child && !cte_names.empty()) {
                RewriteCteScans(def->anchor_child, cte_names);
            }
            def->children.push_back(current);
            current = def;
            continue;
        }
        auto def = std::make_shared<CteDefineNode>(cte.cte_name, false);
        if (cte.cte_query) {
            def->cte_plan = PlanSelect(*cte.cte_query);
        }
        // 对每个 CTE 自身的 cte_plan 做 hint 改写（链式 CTE 引用前 CTE）
        if (def->cte_plan && !cte_names.empty()) {
            RewriteCteScans(def->cte_plan, cte_names);
        }
        if (def->anchor_child && !cte_names.empty()) {
            RewriteCteScans(def->anchor_child, cte_names);
        }
        def->children.push_back(current);
        current = def;
    }
    return current;
}

void Planner::PlanSubqueriesInExpr(ExprPtr& expr) {
    if (!expr) return;
    switch (expr->GetType()) {
        case NodeType::SUBQUERY_EXPR: {
            auto sq = std::static_pointer_cast<SubqueryExprNode>(expr);
            if (!sq->subquery_plan && sq->subquery) {
                sq->subquery_plan = PlanSelect(*sq->subquery);
            }
            // 递归到 subquery 内的子查询
            if (sq->subquery_plan) {
                // 嵌套子查询：暂时跳过 PlanSelect 递归，避免循环依赖
            }
            break;
        }
        case NodeType::BINARY_EXPR: {
            auto b = std::static_pointer_cast<BinaryExpr>(expr);
            PlanSubqueriesInExpr(b->left);
            PlanSubqueriesInExpr(b->right);
            break;
        }
        case NodeType::UNARY_EXPR: {
            auto u = std::static_pointer_cast<UnaryExpr>(expr);
            PlanSubqueriesInExpr(u->operand);
            break;
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            auto f = std::static_pointer_cast<FunctionCallExpr>(expr);
            PlanSubqueriesInExprList(f->arguments);
            break;
        }
        default:
            break;
    }
}

void Planner::PlanSubqueriesInExprList(std::vector<ExprPtr>& list) {
    for (auto& e : list) PlanSubqueriesInExpr(e);
}

void Planner::PlanSubqueriesInSelect(const SelectStatement& stmt) {
    for (const auto& e : stmt.select_list) {
        if (e) {
            auto ce = const_cast<ExprPtr&>(e);
            PlanSubqueriesInExpr(ce);
        }
    }
    if (stmt.where_clause) {
        auto wc = const_cast<ExprPtr&>(stmt.where_clause);
        PlanSubqueriesInExpr(wc);
    }
    for (const auto& e : stmt.group_by) {
        if (e) {
            auto ce = const_cast<ExprPtr&>(e);
            PlanSubqueriesInExpr(ce);
        }
    }
    if (stmt.having_clause) {
        auto hc = const_cast<ExprPtr&>(stmt.having_clause);
        PlanSubqueriesInExpr(hc);
    }
    for (const auto& it : stmt.order_by) {
        auto ce = const_cast<ExprPtr&>(it.expr);
        PlanSubqueriesInExpr(ce);
    }
    for (const auto& j : stmt.joins) {
        PlanSubqueriesInJoin(j);
    }
}

void Planner::PlanSubqueriesInJoin(const JoinClause& j) {
    if (j.on_condition) {
        auto ce = const_cast<ExprPtr&>(j.on_condition);
        PlanSubqueriesInExpr(ce);
    }
}

void Planner::RewriteCteScans(const PlanNodePtr& root,
                              const std::vector<std::string>& cte_names) {
    if (!root) return;
    std::function<void(const PlanNodePtr&)> walk = [&](const PlanNodePtr& n) {
        if (!n) return;
        for (auto& ch : n->children) walk(ch);
        if (n->GetType() == PlanNodeType::SEQ_SCAN) {
            auto ss = std::static_pointer_cast<SeqScanNode>(n);
            for (const auto& cn : cte_names) {
                if (ss->table_name == cn && ss->table_alias.empty()) {
                    ss->table_alias = std::string("__cte__") + cn;
                }
            }
        }
    };
    // 子查询计划挂在 SubqueryExprNode.subquery_plan 上，不属于普通 plan 树，
    // 这里递归把它们一并重写，否则 SELECT 列表/标量子查询里对 CTE 名的引用
    // 会保持原表名而在执行期走 SeqScanExecutor，结果返回 0 行。
    std::function<void(const ExprPtr&)> walk_expr = [&](const ExprPtr& e) {
        if (!e) return;
        switch (e->GetType()) {
            case NodeType::SUBQUERY_EXPR: {
                auto sq = std::static_pointer_cast<SubqueryExprNode>(e);
                if (sq->subquery_plan) walk(sq->subquery_plan);
                break;
            }
            case NodeType::BINARY_EXPR: {
                auto b = std::static_pointer_cast<BinaryExpr>(e);
                walk_expr(b->left);
                walk_expr(b->right);
                break;
            }
            case NodeType::UNARY_EXPR: {
                auto u = std::static_pointer_cast<UnaryExpr>(e);
                walk_expr(u->operand);
                break;
            }
            case NodeType::FUNCTION_CALL_EXPR: {
                auto f = std::static_pointer_cast<FunctionCallExpr>(e);
                for (auto& a : f->arguments) walk_expr(a);
                break;
            }
            case NodeType::CASE_EXPR: {
                auto c = std::static_pointer_cast<CaseExprNode>(e);
                walk_expr(c->subject);
                for (auto& w : c->whens) {
                    walk_expr(w.when_expr);
                    walk_expr(w.then_expr);
                }
                walk_expr(c->else_expr);
                break;
            }
            case NodeType::CAST_EXPR: {
                auto c = std::static_pointer_cast<CastExprNode>(e);
                walk_expr(c->expr);
                break;
            }
            default:
                break;
        }
    };
    // 在计划树上找到所有表达式：扫描各节点 child 之外的 expr 字段并递归。
    // PlanNode 本身没暴露 expr 容器；为此改为深度优先遍历，把每个节点若携带
    // SubqueryExprNode（例如 ProjectNode::columns / FilterNode::predicate /
    // AggregateNode::aggregate_exprs / SortNode::order_items 等）一并重写。
    std::function<void(const PlanNodePtr&)> walk_with_exprs =
        [&](const PlanNodePtr& n) {
        if (!n) return;
        for (auto& ch : n->children) walk_with_exprs(ch);
        switch (n->GetType()) {
            case PlanNodeType::PROJECT: {
                auto p = std::static_pointer_cast<ProjectNode>(n);
                for (auto& c : p->columns) walk_expr(c);
                break;
            }
            case PlanNodeType::FILTER: {
                auto f = std::static_pointer_cast<FilterNode>(n);
                walk_expr(f->predicate);
                break;
            }
            case PlanNodeType::AGGREGATE: {
                auto a = std::static_pointer_cast<AggregateNode>(n);
                for (auto& e : a->group_by_exprs) walk_expr(e);
                for (auto& e : a->aggregate_exprs) walk_expr(e);
                break;
            }
            case PlanNodeType::JOIN: {
                auto j = std::static_pointer_cast<JoinNode>(n);
                walk_expr(j->condition);
                break;
            }
            case PlanNodeType::SORT: {
                auto s = std::static_pointer_cast<SortNode>(n);
                for (auto& it : s->order_items) walk_expr(it.expr);
                break;
            }
            case PlanNodeType::WINDOW: {
                auto w = std::static_pointer_cast<WindowNode>(n);
                for (auto& e : w->select_list) walk_expr(e);
                break;
            }
            default:
                break;
        }
        // 普通 SEQ_SCAN / INDEX_SCAN 等节点已经在 walk() 中处理；这里再调一次
        // 走一遍 children 中的 SeqScanNode，确保 alias 改写完整。
        walk(n);
    };
    walk_with_exprs(root);
}

PlanNodePtr Planner::PlanSetOperation(const SetOperationStatement& stmt) {
    SetOpNode::Kind k = SetOpNode::Kind::UNION;
    switch (stmt.kind) {
        case SetOperationStatement::Kind::UNION:     k = SetOpNode::Kind::UNION; break;
        case SetOperationStatement::Kind::UNION_ALL: k = SetOpNode::Kind::UNION_ALL; break;
        case SetOperationStatement::Kind::INTERSECT: k = SetOpNode::Kind::INTERSECT; break;
        case SetOperationStatement::Kind::EXCEPT:    k = SetOpNode::Kind::EXCEPT; break;
    }
    auto node = std::make_shared<SetOpNode>(k);
    if (stmt.left) {
        if (auto ss = std::dynamic_pointer_cast<SelectStatement>(stmt.left)) {
            node->children.push_back(PlanSelect(*ss));
        } else if (auto so = std::dynamic_pointer_cast<SetOperationStatement>(stmt.left)) {
            node->children.push_back(PlanSetOperation(*so));
        } else if (auto wc = std::dynamic_pointer_cast<WithClauseStatement>(stmt.left)) {
            node->children.push_back(PlanWithClause(*wc));
        }
    }
    if (stmt.right) {
        if (auto ss = std::dynamic_pointer_cast<SelectStatement>(stmt.right)) {
            node->children.push_back(PlanSelect(*ss));
        } else if (auto so = std::dynamic_pointer_cast<SetOperationStatement>(stmt.right)) {
            node->children.push_back(PlanSetOperation(*so));
        } else if (auto wc = std::dynamic_pointer_cast<WithClauseStatement>(stmt.right)) {
            node->children.push_back(PlanWithClause(*wc));
        }
    }
    // 顶层 ORDER BY / LIMIT 包裹在 SetOpNode 之外，保证只对最终结果排序/截断。
    PlanNodePtr current = node;
    if (!stmt.order_by.empty()) {
        auto s = std::make_shared<SortNode>(stmt.order_by);
        s->children.push_back(current);
        current = s;
    }
    if (stmt.limit >= 0) {
        auto l = std::make_shared<LimitNode>(stmt.limit, stmt.limit_offset);
        l->children.push_back(current);
        current = l;
    }
    return current;
}

// ============ 48_acid_undo：事务控制节点 ============
//
// Phase A 把原 no-op 节点替换为带语义的计划节点，由 TransactionExecutor
// 转发到 TransactionManager。Phase B/C/D 不在此处改动。

PlanNodePtr Planner::PlanBegin(const BeginStatement&) {
    return std::make_shared<BeginTxnNode>();
}

PlanNodePtr Planner::PlanCommit(const CommitStatement&) {
    return std::make_shared<CommitTxnNode>();
}

PlanNodePtr Planner::PlanRollback(const RollbackStatement&) {
    return std::make_shared<RollbackTxnNode>();
}

PlanNodePtr Planner::PlanSavepoint(const SavepointStatement& stmt) {
    return std::make_shared<SavepointNode>(stmt.savepoint_name);
}

PlanNodePtr Planner::PlanReleaseSavepoint(const ReleaseSavepointStatement& stmt) {
    // 解析器已支持 ROLLBACK TO name，但 Planner 还没有 PlanRollbackTo 入口——
    // 这里保留兼容性：原 RELEASE SAVEPOINT 仍走 ReleaseSavepointNode。
    // Phase A 测试覆盖了 SAVEPOINT / ROLLBACK TO sp，由 BeginTxnNode/Rollback
    // 路径合流；单独的 ROLLBACK TO 仅在 48_acid_undo 测试里需要，先支持之。
    return std::make_shared<ReleaseSavepointNode>(stmt.savepoint_name);
}

// 新增：PlanRollbackTo —— 由 parser 检测 "ROLLBACK TO" 关键字后调用。
PlanNodePtr Planner::PlanRollbackTo(const RollbackToStatement& stmt) {
    return std::make_shared<RollbackToSavepointNode>(stmt.savepoint_name);
}

PlanNodePtr Planner::PlanCreateView(const CreateViewStatement& stmt) {
    // 把视图定义登记到 catalog；测试只验证语法接受与无副作用。
    // 60_view_trigger: OR REPLACE 时若视图已存在，先 DropView 再 CreateView。
    if (catalog_ && stmt.query) {
        if (stmt.is_or_replace && catalog_->HasView(stmt.view_name)) {
            catalog_->DropView(stmt.view_name);
        }
        SystemCatalog::ViewDefinition def;
        def.view_name = stmt.view_name;
        def.query = stmt.query;
        catalog_->CreateView(def);
        // 60_view_trigger: WITH CHECK OPTION 把视图的 WHERE 条件记到 view_meta_。
        // V1 接受语法并持久化选项，但在写路径上仅对单表视图启用 enforcement。
        if (stmt.with_check_option) {
            catalog_->SetViewCheckOption(stmt.view_name,
                                         stmt.query ? stmt.query->where_clause : nullptr,
                                         stmt.check_option_cascaded);
        }
    }
    return std::make_shared<CreateViewNode>(stmt.view_name);
}

// 60_view_trigger (Category 9): CREATE MATERIALIZED VIEW
//
// V1 实施策略：
//   - 把 SELECT 的输出列定型为 ColumnDefinition（通过 DeriveOutputColumns 走子计划）。
//   - catalog 内注册 MaterializedViewInfo（backing_table = "__mv_<name>"）。
//   - 立即执行 SELECT 并把结果集通过 CreateMaterializedViewExecutor 写入 backing table。
//
// 实现被推迟到 ExecutionEngine 阶段：Planner 这里只构造计划 + 收集列信息，
// 不在 Planner 阶段跑子计划（planner 阶段不应产生副作用）。
PlanNodePtr Planner::PlanCreateMaterializedView(const MaterializedViewStatement& stmt) {
    // 先递归 plan 出子计划，用于在执行器阶段读取定型列。
    if (stmt.query) {
        SelectStatement& mutable_query = const_cast<SelectStatement&>(*stmt.query);
        PlanSelect(mutable_query);
    }
    auto node = std::make_shared<CreateMaterializedViewNode>(
        stmt.view_name, std::vector<ColumnDefinition>{}, stmt.if_not_exists);
    if (stmt.query) {
        node->children.push_back(PlanSelect(*stmt.query));
    }
    // 60_view_trigger: 在 catalog 中登记 MaterializedViewInfo，让后续
    // SELECT * FROM mv 通过 TryExpandView 找到 backing table。
    // query_text 写原始 SELECT 的 ToString()，供 REFRESH 时 Planner 再次 plan。
    // 实际物化（建表 + 数据填充）由 MaterializedViewExecutor 在执行期完成。
    if (catalog_ != nullptr && !catalog_->HasMaterializedView(stmt.view_name)) {
        SystemCatalog::MaterializedViewInfo mv;
        mv.view_name = stmt.view_name;
        mv.backing_table = SystemCatalog::MaterializedViewBackingTable(stmt.view_name);
        mv.columns.clear();
        mv.query_text = stmt.query ? stmt.query->ToString() : "";
        catalog_->CreateMaterializedView(mv);
    }
    return node;
}

PlanNodePtr Planner::PlanAlterMaterializedView(const AlterMaterializedViewStatement& stmt) {
    auto node = std::make_shared<AlterMaterializedViewNode>(stmt.view_name);
    // REFRESH 路径需要重新执行 SELECT；先从 catalog 拿原始 SELECT 文本，解析回 AST，
    // 再 plan 一遍挂到 children[0] 上。
    if (catalog_) {
        const auto* info = catalog_->GetMaterializedView(stmt.view_name);
        if (info != nullptr && !info->query_text.empty()) {
            try {
                Lexer lexer(info->query_text);
                std::vector<Token> tokens = lexer.Tokenize();
                if (tokens.empty() || tokens.back().type != TokenType::END_OF_FILE) {
                    tokens.emplace_back(TokenType::END_OF_FILE, "", 0, 0);
                }
                Parser parser(std::move(tokens));
                StatementPtr parsed = parser.Parse();
                if (parsed && parsed->GetType() == NodeType::SELECT_STMT) {
                    auto sel = std::static_pointer_cast<SelectStatement>(parsed);
                    PlanNodePtr sub = PlanSelect(*sel);
                    if (sub) node->children.push_back(sub);
                }
            } catch (...) {
                // 解析失败时回退为 no-op 子计划；执行期会触发异常。
            }
        }
    }
    return node;
}

PlanNodePtr Planner::PlanDropView(const DropViewStatement& stmt) {
    if (catalog_) {
        catalog_->DropView(stmt.view_name);
    }
    auto n = std::make_shared<DropObjectNode>(
        DropObjectNode::Kind::VIEW, stmt.view_name, stmt.if_exists);
    return n;
}

PlanNodePtr Planner::PlanCreateTrigger(const CreateTriggerStatement& stmt) {
    if (catalog_) {
        SystemCatalog::TriggerDefinition def;
        def.trigger_name = stmt.trigger_name;
        def.timing = stmt.timing;
        def.event = stmt.event;
        def.table_name = stmt.table_name;
        // 60_view_trigger: FOR EACH ROW vs STATEMENT。
        def.for_each_row = stmt.for_each_row;
        def.assignments = stmt.assignments;
        catalog_->CreateTrigger(def);
    }
    return std::make_shared<CreateTriggerNode>(stmt.trigger_name);
}

PlanNodePtr Planner::PlanDropTrigger(const DropTriggerStatement& stmt) {
    if (catalog_) {
        catalog_->DropTrigger(stmt.trigger_name);
    }
    auto n = std::make_shared<DropObjectNode>(
        DropObjectNode::Kind::TRIGGER, stmt.trigger_name, stmt.if_exists);
    return n;
}

PlanNodePtr Planner::PlanCreateFunction(const CreateFunctionStatement& stmt) {
    if (catalog_) {
        SystemCatalog::FunctionDefinition def;
        def.function_name = stmt.function_name;
        def.parameters = stmt.parameters;
        def.return_type = stmt.return_type;
        def.return_char_length = stmt.return_char_length;
        def.body_statements = stmt.body_statements;
        catalog_->CreateFunction(def);
    }
    return std::make_shared<CreateFunctionNode>(stmt.function_name);
}

PlanNodePtr Planner::PlanDropFunction(const DropFunctionStatement& stmt) {
    if (catalog_) {
        catalog_->DropFunction(stmt.function_name);
    }
    auto n = std::make_shared<DropObjectNode>(
        DropObjectNode::Kind::FUNCTION, stmt.function_name, stmt.if_exists);
    return n;
}

// ============ 59_procs (Category 8)：PROCEDURE / CALL ============

PlanNodePtr Planner::PlanCreateProcedure(const CreateProcedureStatement& stmt) {
    if (catalog_) {
        SystemCatalog::ProcedureDefinition def;
        def.procedure_name = stmt.procedure_name;
        def.parameters = stmt.parameters;
        def.body_statements = stmt.body_statements;
        catalog_->CreateProcedure(def);
    }
    return std::make_shared<CreateProcedureNode>(stmt.procedure_name);
}

PlanNodePtr Planner::PlanDropProcedure(const DropProcedureStatement& stmt) {
    if (catalog_) {
        catalog_->DropProcedure(stmt.procedure_name, stmt.if_exists);
    }
    auto n = std::make_shared<DropObjectNode>(
        DropObjectNode::Kind::PROCEDURE, stmt.procedure_name, stmt.if_exists);
    return n;
}

PlanNodePtr Planner::PlanCall(const CallStatement& stmt) {
    // 实参里的子表达式可能含 SUBQUERY；递归 plan 一遍以让子查询具备
    // subquery_plan。CallStatement::arguments 内部元素是 shared_ptr<Expr>，
    // 通过 const_cast 解除 const 让 PlanSubqueriesInExpr 可以改写 subquery_plan。
    std::vector<ExprPtr> args = stmt.arguments;
    for (auto& a : args) PlanSubqueriesInExpr(a);
    return std::make_shared<CallNode>(stmt.procedure_name, args);
}

// ============ 53_ddl: SCHEMA / SEQUENCE ============

PlanNodePtr Planner::PlanCreateSchema(const CreateSchemaStatement& stmt) {
    return std::make_shared<CreateSchemaNode>(stmt.schema_name, stmt.if_not_exists);
}

PlanNodePtr Planner::PlanDropSchema(const DropSchemaStatement& stmt) {
    return std::make_shared<DropSchemaNode>(stmt.schema_name, stmt.if_exists);
}

PlanNodePtr Planner::PlanCreateSequence(const CreateSequenceStatement& stmt) {
    return std::make_shared<CreateSequenceNode>(
        stmt.sequence_name, stmt.start_value, stmt.increment, stmt.if_not_exists);
}

PlanNodePtr Planner::PlanDropSequence(const DropSequenceStatement& stmt) {
    return std::make_shared<DropSequenceNode>(stmt.sequence_name, stmt.if_exists);
}

// ============ 46_meta: EXPLAIN / SHOW ============

// EXPLAIN [ANALYZE] <stmt>
//
// 把 inner 语句先走一遍 Planner（递归 CreatePlan）拿到计划子树，再包成
// ExplainNode。把 inner 计划挂到 children[0]，让 ExplainExecutor 在 Init 阶段
// 可以直接用 inner->ToString() 拿到计划文本。
//
// 注意：我们不在这里执行 inner；inner 是 EXPLAIN 时本就不该真正跑（任务文档
// 要求「不执行，只展示计划」）。
PlanNodePtr Planner::PlanExplain(const ExplainStatement& stmt) {
    auto node = std::make_shared<ExplainNode>(stmt.analyze);
    if (stmt.inner) {
        PlanNodePtr inner_plan = CreatePlan(stmt.inner);
        if (inner_plan) {
            // 执行器按 children[0] 拿 inner 计划即可，不进入常规调度。
            node->children.push_back(inner_plan);
        }
    }
    return node;
}

// SHOW TABLES / SHOW COLUMNS / SHOW INDEX / SHOW CREATE TABLE
//
// 纯元数据查询；执行器不依赖 children，直接读 catalog。target_table 在
// TABLES 形式下为空。
PlanNodePtr Planner::PlanShow(const ShowStatement& stmt) {
    ShowNode::Kind kind = ShowNode::Kind::TABLES;
    switch (stmt.kind) {
        case ShowStatement::Kind::TABLES:       kind = ShowNode::Kind::TABLES; break;
        case ShowStatement::Kind::COLUMNS:      kind = ShowNode::Kind::COLUMNS; break;
        case ShowStatement::Kind::INDEX:        kind = ShowNode::Kind::INDEX; break;
        case ShowStatement::Kind::CREATE_TABLE: kind = ShowNode::Kind::CREATE_TABLE; break;
    }
    return std::make_shared<ShowNode>(kind, stmt.target_table);
}

// 视图展开：把 SELECT FROM view_name 改写为 derived_table（视图 SELECT）。
// 注意：仅在 from_table 命中视图且无 JOIN 时做整体替换；后续要扩展带 JOIN
// 的视图时可继续在本函数中处理。
//
// 60_view_trigger (Category 9):
//   - 物化视图命中时：直接把 from_table 改为 backing table 名（"__mv_<view_name>"）；
//     后端 SeqScanExecutor 会扫这张真实表。视图别名 / WHERE / SELECT list 不变。
//   - 普通视图命中时：与历史行为一致，把视图 SELECT 复制为 derived_table。
bool Planner::TryExpandView(SelectStatement& stmt) {
    if (!catalog_) return false;
    if (stmt.from_table.empty()) return false;
    // 视图只能单表替换：若带 joins，本期不展开。
    if (!stmt.joins.empty()) return false;
    // 必须存在 FROM table（不能是 derived_table 的占位）。
    if (stmt.derived_table) return false;
    // 60_view_trigger: 物化视图优先（直接扫 backing table）。
    const SystemCatalog::MaterializedViewInfo* mv =
        catalog_->LookupMaterializedView(stmt.from_table);
    if (mv != nullptr) {
        stmt.from_table = mv->backing_table;
        // 不修改 derived_table —— SeqScanExecutor 看到普通表名后照常扫堆。
        return true;
    }
    const SystemCatalog::ViewDefinition* view = catalog_->LookupView(stmt.from_table);
    if (view == nullptr || view->query == nullptr) return false;
    // 把视图的 SELECT 复制为 derived_table，并把 from_table 替换为视图别名
    // （让后续 SeqScanNode.table_name == alias 走「派生表占位」路径）。
    std::string alias = stmt.from_table_alias.empty()
                          ? stmt.from_table
                          : stmt.from_table_alias;
    stmt.derived_table = std::make_shared<SelectStatement>(*view->query);
    stmt.derived_alias = alias;
    stmt.from_table.clear();
    stmt.from_table_alias.clear();
    return true;
}

void Planner::MarkUdfCallsInExpr(ExprPtr& expr) const {
    if (!expr) return;
    if (expr->GetType() == NodeType::FUNCTION_CALL_EXPR) {
        auto fc = std::static_pointer_cast<FunctionCallExpr>(expr);
        // 仅当同名函数尚未被 ExpressionEvaluator 识别为内置函数时尝试 UDF。
        // 这里不去判定大小写形式的"是否内置"，因为一旦 catalog 里有同名函数
        // （例如用户定义了 add_one），我们就直接走 UDF 路径，由 evaluator
        // 在内置表里查不到时再回退。
        for (auto& a : fc->arguments) MarkUdfCallsInExpr(a);
    } else if (expr->GetType() == NodeType::BINARY_EXPR) {
        auto b = std::static_pointer_cast<BinaryExpr>(expr);
        MarkUdfCallsInExpr(b->left);
        MarkUdfCallsInExpr(b->right);
    } else if (expr->GetType() == NodeType::UNARY_EXPR) {
        auto u = std::static_pointer_cast<UnaryExpr>(expr);
        MarkUdfCallsInExpr(u->operand);
    } else if (expr->GetType() == NodeType::CASE_EXPR) {
        auto c = std::static_pointer_cast<CaseExprNode>(expr);
        MarkUdfCallsInExpr(c->subject);
        for (auto& w : c->whens) {
            MarkUdfCallsInExpr(w.when_expr);
            MarkUdfCallsInExpr(w.then_expr);
        }
        MarkUdfCallsInExpr(c->else_expr);
    } else if (expr->GetType() == NodeType::CAST_EXPR) {
        auto c = std::static_pointer_cast<CastExprNode>(expr);
        MarkUdfCallsInExpr(c->expr);
    }
}

void Planner::MarkUdfCallsInList(std::vector<ExprPtr>& list) const {
    for (auto& e : list) MarkUdfCallsInExpr(e);
}

void Planner::MarkUdfCallsInSelect(const SelectStatement& stmt) const {
    for (const auto& e : stmt.select_list) {
        auto ce = const_cast<ExprPtr&>(e);
        MarkUdfCallsInExpr(ce);
    }
    if (stmt.where_clause) {
        auto wc = const_cast<ExprPtr&>(stmt.where_clause);
        MarkUdfCallsInExpr(wc);
    }
    for (const auto& e : stmt.group_by) {
        auto ce = const_cast<ExprPtr&>(e);
        MarkUdfCallsInExpr(ce);
    }
    if (stmt.having_clause) {
        auto hc = const_cast<ExprPtr&>(stmt.having_clause);
        MarkUdfCallsInExpr(hc);
    }
    for (const auto& it : stmt.order_by) {
        auto ce = const_cast<ExprPtr&>(it.expr);
        MarkUdfCallsInExpr(ce);
    }
}

// ---- 60_funcs: GROUPING SETS / ROLLUP / CUBE 展开 ----
//
// 输入：stmt（用户 SELECT，含 grouping_sets 字段）；
//       scan_input（已建好的 FROM + JOIN + WHERE 子计划）。
//
// 展开策略：为每个 grouping set 构造一个"等价 SELECT"：
//   - SELECT list 中的"分组列引用"若不在该 set 中，替换为 NULL 字面量；
//   - 非分组列位置（一般是聚合函数调用 / 字面量 / 标量函数）保持原样；
//   - group_by = 该 set 本身；
//   - having 复制到子 SELECT 内（对每个 grouping set 独立应用）；
//   - 不复制 ORDER BY / LIMIT（外层再处理）。
// 然后把每个子 SELECT 递归 PlanSelect，再把它们 UNION ALL 起来。
//
// 收集分组列：把每个 grouping set 里的 ColumnRefExpr 列名收集到一个 set，
// 用作"该 select list 位置是否是分组列"的判定。
PlanNodePtr Planner::PlanGroupingSets(const SelectStatement& stmt, PlanNodePtr scan_input) {
    // 1) 收集所有 grouping set 中出现的列名（作为"分组列"判定集合）。
    //    限定为 ColumnRefExpr（更复杂的列表达式不展开为 set 维度）。
    std::set<std::string> group_col_names;
    for (const auto& gs : stmt.grouping_sets) {
        for (const auto& e : gs) {
            if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
                auto cr = std::static_pointer_cast<ColumnRefExpr>(e);
                group_col_names.insert(cr->column_name);
            }
        }
    }

    // 2) 为每个 grouping set 构造合成 SELECT 并规划为子计划。
    std::vector<PlanNodePtr> sub_plans;
    for (const auto& gs : stmt.grouping_sets) {
        std::set<std::string> in_this_set;
        for (const auto& e : gs) {
            if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
                auto cr = std::static_pointer_cast<ColumnRefExpr>(e);
                in_this_set.insert(cr->column_name);
            }
        }

        SelectStatement inner;
        inner.from_table = stmt.from_table;
        inner.from_table_alias = stmt.from_table_alias;
        inner.joins = stmt.joins;
        inner.where_clause = stmt.where_clause;
        inner.having_clause = stmt.having_clause;
        inner.select_aliases = stmt.select_aliases;

        for (const auto& e : stmt.select_list) {
            if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
                auto cr = std::static_pointer_cast<ColumnRefExpr>(e);
                if (group_col_names.count(cr->column_name)) {
                    if (in_this_set.count(cr->column_name)) {
                        inner.select_list.push_back(e);
                    } else {
                        // 该 set 不含此分组列 → 替换为 NULL
                        inner.select_list.push_back(
                            std::make_shared<LiteralExpr>(LiteralType::NULL_VALUE, "NULL"));
                    }
                    continue;
                }
            }
            inner.select_list.push_back(e);
        }
        inner.group_by = gs;

        PlanNodePtr sub = PlanSelect(inner);
        sub_plans.push_back(sub);
    }

    // 3) UNION ALL 合并所有子计划。SetOpNode 左孩子为第一个 plan，
    //    右孩子逐个链接（与既有 SetOperation 计划一致）。
    if (sub_plans.empty()) return nullptr;
    PlanNodePtr result = sub_plans[0];
    for (size_t i = 1; i < sub_plans.size(); ++i) {
        auto sop = std::make_shared<SetOpNode>(SetOpNode::Kind::UNION_ALL);
        sop->children.push_back(result);
        sop->children.push_back(sub_plans[i]);
        result = sop;
    }
    return result;
}

}  // namespace sqlcompiler