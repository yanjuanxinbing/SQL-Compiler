#include "plan/Planner.h"

#include <cctype>
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
            if (name == "COUNT" || name == "SUM" || name == "AVG" ||
                name == "MIN" || name == "MAX") {
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
                name == "MIN" || name == "MAX") {
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
    PlanNodePtr current;
    if (!stmt.from_table.empty()) {
        current = std::make_shared<SeqScanNode>(stmt.from_table, stmt.from_table_alias);
    }
    // Joins: each join produces a JoinNode with two children
    //   children[0] = previous chain (left side)
    //   children[1] = new SeqScanNode(j.table_name) (right side)
    for (const auto& j : stmt.joins) {
        auto join = std::make_shared<JoinNode>(j.join_type, j.on_condition);
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
    // Aggregate: needed when GROUP BY present OR SELECT/HAVING uses aggregate functions
    bool needs_agg = !stmt.group_by.empty() || SelectHasAggregate(stmt);
    if (needs_agg) {
        // aggregate_exprs = the SELECT list expressions (AggregateExecutor produces
        // one Tuple per group with values matching the SELECT list)
        auto agg = std::make_shared<AggregateNode>(stmt.group_by, stmt.select_list);
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
    // PROJECT
    auto proj = std::make_shared<ProjectNode>(stmt.select_list, stmt.select_aliases, stmt.is_distinct);
    if (current) proj->children.push_back(current);
    current = proj;
    // ORDER BY
    if (!stmt.order_by.empty()) {
        auto s = std::make_shared<SortNode>(stmt.order_by);
        s->children.push_back(current);
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