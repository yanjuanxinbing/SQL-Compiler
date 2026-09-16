#pragma once

#include "ast/AST.h"
#include "plan/Plan.h"
#include "semantic/SymbolTable.h"

namespace sqlcompiler {

class SystemCatalog;

// Planner：将通过语义检查的AST转换为逻辑执行计划树
class Planner {
public:
    Planner(SystemCatalog* catalog, SymbolTable& symbol_table);

    // 将语句转换为逻辑执行计划
    PlanNodePtr CreatePlan(const StatementPtr& statement);

    // 给物化视图执行器使用：根据 SELECT 语句的 select_list 推断每列的输出类型。
    // 不依赖执行期求值 —— 对空表也能正确产出 schema（避免「VARCHAR 占位列」
    // 的回退）。
    //
    // 覆盖：
    //   - 字面量（INTEGER/FLOAT/STRING/BOOLEAN/DATE/TIMESTAMP/TIME/JSON/NULL）
    //   - 列引用（限定 / 未限定；通过 symbol_table 查找源列的类型）
    //   - SELECT * / t.*：按 from_table + joins 的列顺序展开，每列类型来自 catalog
    //   - 算术 / 比较 / 逻辑二元表达式（CONCAT -> VARCHAR；算术按操作数推断；
    //     比较 / 逻辑 -> INT）
    //   - 一元表达式（NOT -> INT；NEGATE -> 操作数类型）
    //   - 函数调用：标量（UPPER / LOWER / SUBSTR / TRIM / REPLACE / LENGTH /
    //     COALESCE / IFNULL / NULLIF / 数学 / 日期函数 / CAST），聚合
    //     （SUM/AVG/COUNT/MIN/MAX/STDDEV/VARIANCE/MEDIAN 按输入类型推断，
    //     COUNT 总是 INT）
    //   - CASE WHEN（取首个 THEN 的类型；空时取 ELSE）
    //   - CAST(expr AS type)
    //   - 子查询（SCALAR 取首列；IN/EXISTS/ANY -> INT）
    //   - 窗口函数（与对应聚合 / 标量函数同型）
    //
    // 失败回退：未知函数 / 无法解析的列时退化为 VARCHAR（与历史 probe 行为一致）。
    // 失败不会抛错 —— 调用方可以基于 ColumnDefinition 直接建表。
    static std::vector<ColumnDefinition> InferSelectOutputSchema(
        const SelectStatement& stmt,
        const SymbolTable& symbol_table);

private:
    SystemCatalog* catalog_;
    SymbolTable& symbol_table_;

    PlanNodePtr PlanSelect(const SelectStatement& stmt);
    // 共享 JOIN 链构造：在 current（FROM 段已建好的子计划）之上，把 stmt.joins
    // 逐个构造成 JoinNode / ApplyNode / 派生表占位 SeqScanNode 并挂入链尾。
    // 当 FROM 是普通表时 left_label = stmt.from_table，USING / NATURAL 的合成
    // ON 条件按左表名生成 ColumnRefExpr；当 FROM 是派生表时
    // left_label = stmt.derived_alias，避免派生表与 JOIN 真实表混在同一 SELECT
    // 时 NATURAL/USING 走错 left_label（Bug 13 修复）。
    PlanNodePtr PlanJoins(PlanNodePtr current, const SelectStatement& stmt,
                          const std::string& left_label);
    PlanNodePtr PlanInsert(const InsertStatement& stmt);
    PlanNodePtr PlanUpdate(const UpdateStatement& stmt);
    PlanNodePtr PlanDelete(const DeleteStatement& stmt);
    PlanNodePtr PlanMerge(const MergeStatement& stmt);
    PlanNodePtr PlanCreateTable(const CreateTableStatement& stmt);
    PlanNodePtr PlanDropTable(const DropTableStatement& stmt);
    PlanNodePtr PlanCreateIndex(const CreateIndexStatement& stmt);
    PlanNodePtr PlanDropIndex(const DropIndexStatement& stmt);
    PlanNodePtr PlanTruncateTable(const TruncateTableStatement& stmt);
    PlanNodePtr PlanAlterTable(const AlterStatement& stmt);
    PlanNodePtr PlanWithClause(const WithClauseStatement& stmt);
    PlanNodePtr PlanSetOperation(const SetOperationStatement& stmt);
    PlanNodePtr PlanBegin(const BeginStatement& stmt);
    PlanNodePtr PlanCommit(const CommitStatement& stmt);
    PlanNodePtr PlanRollback(const RollbackStatement& stmt);
    PlanNodePtr PlanSavepoint(const SavepointStatement& stmt);
    PlanNodePtr PlanReleaseSavepoint(const ReleaseSavepointStatement& stmt);
    PlanNodePtr PlanSetIsolation(const SetIsolationStatement& stmt);
    PlanNodePtr PlanRollbackTo(const RollbackToStatement& stmt);
    PlanNodePtr PlanCreateView(const CreateViewStatement& stmt);
    PlanNodePtr PlanDropView(const DropViewStatement& stmt);
    // 60_view_trigger (Category 9): MATERIALIZED VIEW 计划入口
    PlanNodePtr PlanCreateMaterializedView(const MaterializedViewStatement& stmt);
    PlanNodePtr PlanAlterMaterializedView(const AlterMaterializedViewStatement& stmt);
    PlanNodePtr PlanCreateTrigger(const CreateTriggerStatement& stmt);
    PlanNodePtr PlanDropTrigger(const DropTriggerStatement& stmt);
    PlanNodePtr PlanCreateFunction(const CreateFunctionStatement& stmt);
    PlanNodePtr PlanDropFunction(const DropFunctionStatement& stmt);
    // ---- 59_procs (Category 8) ----
    PlanNodePtr PlanCreateProcedure(const CreateProcedureStatement& stmt);
    PlanNodePtr PlanDropProcedure(const DropProcedureStatement& stmt);
    PlanNodePtr PlanCall(const CallStatement& stmt);
    // ---- 53_ddl: SCHEMA / SEQUENCE ----
    PlanNodePtr PlanCreateSchema(const CreateSchemaStatement& stmt);
    PlanNodePtr PlanDropSchema(const DropSchemaStatement& stmt);
    PlanNodePtr PlanCreateSequence(const CreateSequenceStatement& stmt);
    PlanNodePtr PlanDropSequence(const DropSequenceStatement& stmt);
    // ---- 46_meta ----
    PlanNodePtr PlanExplain(const ExplainStatement& stmt);
    PlanNodePtr PlanShow(const ShowStatement& stmt);
    // Walk the plan tree and annotate seqScanNodes whose table_name matches a
    // CTE name so the executor routes them to the CTE materialization.
    void RewriteCteScans(const PlanNodePtr& root,
                         const std::vector<std::string>& cte_names);
    // Recursively traverse an expression tree; for every SubqueryExprNode whose
    // subquery_plan is null, plan its inner SELECT and assign to subquery_plan.
    void PlanSubqueriesInExpr(ExprPtr& expr);
    void PlanSubqueriesInExprList(std::vector<ExprPtr>& list);
    void PlanSubqueriesInSelect(const SelectStatement& stmt);
    void PlanSubqueriesInJoin(const JoinClause& j);
    // 当 SELECT FROM 的表名在 catalog 中是已注册的视图时，把视图 SELECT 复制为
    // 派生表子计划（from_table 改为视图名 + alias，derived_table 填充 SELECT）。
    // 返回 true 表示发生了替换。
    bool TryExpandView(SelectStatement& stmt);
    // UDF 引用：把 select_list 中对已注册 UDF 的 FunctionCallExpr 标记为「UDF」，
    // 由 ExpressionEvaluator 在执行期通过 ExecutionContext 调取 catalog 求值。
    // 当前实现走 FunctionCallExpr 直接调用，ExpressionEvaluator 检测到未实现的
    // 函数名后会回退到 UDF 求值路径。
    void MarkUdfCallsInExpr(ExprPtr& expr) const;
    void MarkUdfCallsInList(std::vector<ExprPtr>& list) const;
    void MarkUdfCallsInSelect(const SelectStatement& stmt) const;

    // ---- 60_funcs: GROUPING SETS / ROLLUP / CUBE 展开 ----
    // 把 stmt 中 grouping_sets 拆解为多个"按子集分组"的子 SELECT，
    // 各子 SELECT 的 SELECT list 把不在该子集的分组列替换为 NULL，
    // 最后用 UNION ALL 把它们合并。scan_input 是 FROM + WHERE + JOIN
    // 链已经建好的子计划；HAVING 复制到每个子 SELECT 内部。
    PlanNodePtr PlanGroupingSets(const SelectStatement& stmt, PlanNodePtr scan_input);

    // 共享 SELECT 尾段：在已经建好的 FROM/JOIN/derived/VALUES 链之上，
    // 按需构造 AggregateNode → HAVING 重写 → Window/Project → ORDER BY（重写）→ LIMIT。
    // 三个 PlanSelect 分支（main / values_rows / derived_table）共用此函数，
    // 避免 derived_table / values_rows 分支漏掉聚合路径导致 SUM() 被当标量函数返回 NULL。
    PlanNodePtr PlanAggregateTail(PlanNodePtr current, const SelectStatement& stmt);

    // ---- Bug 12: SELECT list 中 * 出现在其他表达式旁时的列展开 ----
    // parser 把 * 记为 FunctionCallExpr("*", [])，ProjectExecutor 单独处理
    // `SELECT *`（全表透传）时正确，但当 select_list 含有其他表达式时（如
    // `SELECT 'X' AS c, * FROM p`），* 被当作普通函数调用走 EvaluateFunctionCall
    // 返回 1，导致输出列错位。该方法在 Planner 阶段把 select_list 中的 *
    // 原地展开为 from_table + joins 的所有列对应的 ColumnRefExpr 列表，
    // 同步扩展 select_aliases（每列以自身列名作为别名）；这样下游 Project /
    // Window 节点收到的 select_list 与最终输出列一一对应，cmap 映射无需修正。
    // 仅在 from_table 非空（且无 derived_table / derived_set_op / values_rows）
    // 时展开；其余情况保留 * 走既有路径（ProjectExecutor 的 pass-through 或
    // WindowExecutor 的子 Tuple 透传），避免破坏 VALUES / 派生表 / UNION 链路。
    void ExpandSelectStarInList(SelectStatement& stmt);
};

}  // namespace sqlcompiler
