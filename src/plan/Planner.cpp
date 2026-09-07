#include "plan/Planner.h"

namespace sqlcompiler {

Planner::Planner(SymbolTable& symbol_table) : symbol_table_(symbol_table) {
    // TODO: 如有需要可补充初始化逻辑
}

PlanNodePtr Planner::CreatePlan(const StatementPtr& statement) {
    // TODO: 根据statement->GetType()分派到具体的Plan*函数
    return nullptr;
}

PlanNodePtr Planner::PlanSelect(const SelectStatement& stmt) {
    // TODO:
    // 1. 构造SeqScanNode(stmt.from_table)作为基础
    // 2. 若有JOIN，依次包装JoinNode
    // 3. 若有WHERE，包装FilterNode
    // 4. 若有GROUP BY/聚合函数，包装AggregateNode
    // 5. 若有HAVING，包装FilterNode
    // 6. 包装ProjectNode(stmt.select_list)
    // 7. 若有ORDER BY，包装SortNode
    // 8. 若有LIMIT，包装LimitNode
    return nullptr;
}

PlanNodePtr Planner::PlanInsert(const InsertStatement& stmt) {
    // TODO: 构造InsertNode
    return nullptr;
}

PlanNodePtr Planner::PlanUpdate(const UpdateStatement& stmt) {
    // TODO: 构造UpdateNode
    return nullptr;
}

PlanNodePtr Planner::PlanDelete(const DeleteStatement& stmt) {
    // TODO: 构造DeleteNode
    return nullptr;
}

PlanNodePtr Planner::PlanCreateTable(const CreateTableStatement& stmt) {
    // TODO: 构造CreateTableNode
    return nullptr;
}

PlanNodePtr Planner::PlanDropTable(const DropTableStatement& stmt) {
    // TODO: 构造DropTableNode
    return nullptr;
}

}  // namespace sqlcompiler
