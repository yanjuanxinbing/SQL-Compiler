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
};

}  // namespace sqlcompiler
