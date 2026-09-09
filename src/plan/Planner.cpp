#include "plan/Planner.h"

#include <utility>

namespace sqlcompiler {

Planner::Planner(SymbolTable& symbol_table) : symbol_table_(symbol_table) {
}

PlanNodePtr Planner::CreatePlan(const StatementPtr& statement) {
    if (!statement) return nullptr;
    switch (statement->GetType()) {
        case NodeType::SELECT_STMT:
            return PlanSelect(*std::static_pointer_cast<SelectStatement>(statement));
        case NodeType::INSERT_STMT:
            return PlanInsert(*std::static_pointer_cast<InsertStatement>(statement));
        case NodeType::UPDATE_STMT:
            return PlanUpdate(*std::static_pointer_cast<UpdateStatement>(statement));
        case NodeType::DELETE_STMT:
            return PlanDelete(*std::static_pointer_cast<DeleteStatement>(statement));
        case NodeType::CREATE_TABLE_STMT:
            return PlanCreateTable(*std::static_pointer_cast<CreateTableStatement>(statement));
        case NodeType::DROP_TABLE_STMT:
            return PlanDropTable(*std::static_pointer_cast<DropTableStatement>(statement));
        default:
            return nullptr;
    }
}

PlanNodePtr Planner::PlanSelect(const SelectStatement& stmt) {
    PlanNodePtr current = std::make_shared<SeqScanNode>(stmt.from_table);
    // Joins (modeled as a single JoinNode for MVP)
    for (const auto& j : stmt.joins) {
        auto join = std::make_shared<JoinNode>(j.join_type, j.on_condition);
        join->children.push_back(current);
        current = join;
    }
    // WHERE
    if (stmt.where_clause) {
        auto f = std::make_shared<FilterNode>(stmt.where_clause);
        f->children.push_back(current);
        current = f;
    }
    // GROUP BY -> AggregateNode (placeholder)
    if (!stmt.group_by.empty()) {
        std::vector<ExprPtr> aggs;
        auto agg = std::make_shared<AggregateNode>(stmt.group_by, aggs);
        agg->children.push_back(current);
        current = agg;
    }
    // HAVING
    if (stmt.having_clause) {
        auto f = std::make_shared<FilterNode>(stmt.having_clause);
        f->children.push_back(current);
        current = f;
    }
    // PROJECT
    auto proj = std::make_shared<ProjectNode>(stmt.select_list);
    proj->children.push_back(current);
    current = proj;
    // ORDER BY
    if (!stmt.order_by.empty()) {
        auto s = std::make_shared<SortNode>(stmt.order_by);
        s->children.push_back(current);
        current = s;
    }
    // LIMIT
    if (stmt.limit >= 0) {
        auto l = std::make_shared<LimitNode>(stmt.limit);
        l->children.push_back(current);
        current = l;
    }
    return current;
}

PlanNodePtr Planner::PlanInsert(const InsertStatement& stmt) {
    return std::make_shared<InsertNode>(stmt.table_name, stmt.columns, stmt.values_list);
}

PlanNodePtr Planner::PlanUpdate(const UpdateStatement& stmt) {
    return std::make_shared<UpdateNode>(stmt.table_name, stmt.assignments, stmt.where_clause);
}

PlanNodePtr Planner::PlanDelete(const DeleteStatement& stmt) {
    return std::make_shared<DeleteNode>(stmt.table_name, stmt.where_clause);
}

PlanNodePtr Planner::PlanCreateTable(const CreateTableStatement& stmt) {
    return std::make_shared<CreateTableNode>(stmt.table_name, stmt.columns);
}

PlanNodePtr Planner::PlanDropTable(const DropTableStatement& stmt) {
    return std::make_shared<DropTableNode>(stmt.table_name);
}

}  // namespace sqlcompiler