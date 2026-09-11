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

private:
    SystemCatalog* catalog_;
    SymbolTable& symbol_table_;

    PlanNodePtr PlanSelect(const SelectStatement& stmt);
    PlanNodePtr PlanInsert(const InsertStatement& stmt);
    PlanNodePtr PlanUpdate(const UpdateStatement& stmt);
    PlanNodePtr PlanDelete(const DeleteStatement& stmt);
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
    PlanNodePtr PlanCreateView(const CreateViewStatement& stmt);
    PlanNodePtr PlanDropView(const DropViewStatement& stmt);
    PlanNodePtr PlanCreateTrigger(const CreateTriggerStatement& stmt);
    PlanNodePtr PlanDropTrigger(const DropTriggerStatement& stmt);
    PlanNodePtr PlanCreateFunction(const CreateFunctionStatement& stmt);
    PlanNodePtr PlanDropFunction(const DropFunctionStatement& stmt);
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
};

}  // namespace sqlcompiler
