#include "plan/Planner.h"

#include <utility>

#include "common/Error.h"

namespace sqlcompiler {

namespace {

// 递归判断表达式树中是否包含聚合函数（FunctionCallExpr）
// SELECT 列表里出现 SUM/COUNT/AVG/... 等即视为需要聚合
bool ContainsAggregate(const ExprPtr& expr) {
    if (!expr) return false;
    if (expr->GetType() == NodeType::FUNCTION_CALL_EXPR) return true;
    if (expr->GetType() == NodeType::BINARY_EXPR) {
        const auto* bin = static_cast<const BinaryExpr*>(expr.get());
        return ContainsAggregate(bin->left) || ContainsAggregate(bin->right);
    }
    if (expr->GetType() == NodeType::UNARY_EXPR) {
        const auto* un = static_cast<const UnaryExpr*>(expr.get());
        return ContainsAggregate(un->operand);
    }
    if (expr->GetType() == NodeType::FUNCTION_CALL_EXPR) {
        const auto* fc = static_cast<const FunctionCallExpr*>(expr.get());
        for (const auto& arg : fc->arguments) {
            if (ContainsAggregate(arg)) return true;
        }
    }
    return false;
}

// 把 AST 的 SelectStatement 翻译成树形计划：
//   SeqScan -> [Join] -> [Filter(WHERE)] -> [Aggregate] -> [Filter(HAVING)]
//            -> Project -> [Sort] -> [Limit]
PlanNodePtr BuildSelectPlan(const SelectStatement& stmt) {
    // 1) 最底层：扫描 FROM 表
    PlanNodePtr current = std::make_shared<SeqScanNode>(stmt.from_table);

    // 2) JOIN：每多一张表就套一层 JoinNode，Join 的左孩子是当前 current
    for (const auto& jc : stmt.joins) {
        auto right_scan = std::make_shared<SeqScanNode>(jc.table_name);
        auto join = std::make_shared<JoinNode>(jc.join_type, jc.on_condition);
        join->children.push_back(current);
        join->children.push_back(right_scan);
        current = join;
    }

    // 3) WHERE
    if (stmt.where_clause) {
        auto filter = std::make_shared<FilterNode>(stmt.where_clause);
        filter->children.push_back(current);
        current = filter;
    }

    // 4) 聚合：存在 GROUP BY 或 SELECT 中含聚合函数
    bool need_aggregate = !stmt.group_by.empty();
    if (!need_aggregate) {
        for (const auto& e : stmt.select_list) {
            if (ContainsAggregate(e)) { need_aggregate = true; break; }
        }
    }
    if (need_aggregate) {
        auto agg = std::make_shared<AggregateNode>(stmt.group_by, stmt.select_list);
        agg->children.push_back(current);
        current = agg;
    }

    // 5) HAVING
    if (stmt.having_clause) {
        auto having = std::make_shared<FilterNode>(stmt.having_clause);
        having->children.push_back(current);
        current = having;
    }

    // 6) 投影
    auto project = std::make_shared<ProjectNode>(stmt.select_list);
    project->children.push_back(current);
    current = project;

    // 7) ORDER BY
    if (!stmt.order_by.empty()) {
        auto sort = std::make_shared<SortNode>(stmt.order_by);
        sort->children.push_back(current);
        current = sort;
    }

    // 8) LIMIT
    if (stmt.limit >= 0) {
        auto limit = std::make_shared<LimitNode>(stmt.limit);
        limit->children.push_back(current);
        current = limit;
    }

    return current;
}

}  // namespace

Planner::Planner(SymbolTable& symbol_table) : symbol_table_(symbol_table) {
    // 构造时仅保存引用，无额外初始化
}

PlanNodePtr Planner::CreatePlan(const StatementPtr& statement) {
    if (!statement) {
        throw CompilerException(ErrorStage::CODEGEN, "Cannot create plan for null statement");
    }

    switch (statement->GetType()) {
        case NodeType::SELECT_STMT: {
            auto* stmt = static_cast<SelectStatement*>(statement.get());
            return PlanSelect(*stmt);
        }
        case NodeType::INSERT_STMT: {
            auto* stmt = static_cast<InsertStatement*>(statement.get());
            return PlanInsert(*stmt);
        }
        case NodeType::UPDATE_STMT: {
            auto* stmt = static_cast<UpdateStatement*>(statement.get());
            return PlanUpdate(*stmt);
        }
        case NodeType::DELETE_STMT: {
            auto* stmt = static_cast<DeleteStatement*>(statement.get());
            return PlanDelete(*stmt);
        }
        case NodeType::CREATE_TABLE_STMT: {
            auto* stmt = static_cast<CreateTableStatement*>(statement.get());
            return PlanCreateTable(*stmt);
        }
        case NodeType::DROP_TABLE_STMT: {
            auto* stmt = static_cast<DropTableStatement*>(statement.get());
            return PlanDropTable(*stmt);
        }
        default:
            throw CompilerException(ErrorStage::CODEGEN,
                                    "Unsupported statement type for planning");
    }
}

PlanNodePtr Planner::PlanSelect(const SelectStatement& stmt) {
    // 委托给 BuildSelectPlan，可在此处补充 SELECT 相关的语义检查/列裁剪
    return BuildSelectPlan(stmt);
}

PlanNodePtr Planner::PlanInsert(const InsertStatement& stmt) {
    // 插入计划直接承载表名/列/多行 VALUES
    return std::make_shared<InsertNode>(stmt.table_name, stmt.columns, stmt.values_list);
}

PlanNodePtr Planner::PlanUpdate(const UpdateStatement& stmt) {
    // 更新计划承载 SET 子句与可选的 WHERE 谓词
    return std::make_shared<UpdateNode>(stmt.table_name, stmt.assignments, stmt.where_clause);
}

PlanNodePtr Planner::PlanDelete(const DeleteStatement& stmt) {
    // 删除计划承载表名与可选的 WHERE 谓词
    return std::make_shared<DeleteNode>(stmt.table_name, stmt.where_clause);
}

PlanNodePtr Planner::PlanCreateTable(const CreateTableStatement& stmt) {
    // 建表计划承载表名与列定义
    return std::make_shared<CreateTableNode>(stmt.table_name, stmt.columns);
}

PlanNodePtr Planner::PlanDropTable(const DropTableStatement& stmt) {
    // 删表计划仅需表名
    return std::make_shared<DropTableNode>(stmt.table_name);
}

}  // namespace sqlcompiler