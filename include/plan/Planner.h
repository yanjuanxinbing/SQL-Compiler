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
    PlanNodePtr PlanTruncateTable(const TruncateTableStatement& stmt);
};

}  // namespace sqlcompiler
