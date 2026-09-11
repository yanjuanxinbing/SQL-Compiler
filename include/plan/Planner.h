#pragma once

#include "ast/AST.h"
#include "plan/Plan.h"
#include "semantic/SymbolTable.h"

namespace sqlcompiler {

// Planner：将通过语义检查的AST转换为逻辑执行计划树
class Planner {
public:
    explicit Planner(SymbolTable& symbol_table);

    // 将语句转换为逻辑执行计划
    PlanNodePtr CreatePlan(const StatementPtr& statement);

private:
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
    PlanNodePtr PlanWithClause(const WithClauseStatement& stmt);
    PlanNodePtr PlanSetOperation(const SetOperationStatement& stmt);
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
};

}  // namespace sqlcompiler
