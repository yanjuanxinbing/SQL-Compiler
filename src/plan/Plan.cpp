#include "plan/Plan.h"

#include <sstream>

namespace sqlcompiler {

namespace {

const char* JoinTypeName(JoinType t) {
    switch (t) {
        case JoinType::INNER: return "INNER";
        case JoinType::LEFT:  return "LEFT";
        case JoinType::RIGHT: return "RIGHT";
    }
    return "?";
}

std::string Indent(int depth) {
    return std::string(static_cast<size_t>(depth) * 2, ' ');
}

std::string NodeBodyToString(const PlanNode& node, int depth) {
    std::ostringstream oss;
    oss << Indent(depth);
    switch (node.GetType()) {
        case PlanNodeType::SEQ_SCAN: {
            auto& n = static_cast<const SeqScanNode&>(node);
            oss << "SeqScan(" << n.table_name << ")";
            break;
        }
        case PlanNodeType::FILTER: {
            auto& n = static_cast<const FilterNode&>(node);
            oss << "Filter(" << (n.predicate ? n.predicate->ToString() : "?") << ")";
            break;
        }
        case PlanNodeType::PROJECT: {
            auto& n = static_cast<const ProjectNode&>(node);
            oss << "Project(";
            for (size_t i = 0; i < n.columns.size(); ++i) {
                if (i) oss << ", ";
                oss << (n.columns[i] ? n.columns[i]->ToString() : "?");
            }
            oss << ")";
            break;
        }
        case PlanNodeType::JOIN: {
            auto& n = static_cast<const JoinNode&>(node);
            oss << "Join(" << JoinTypeName(n.join_type) << ", "
                << (n.condition ? n.condition->ToString() : "?") << ")";
            break;
        }
        case PlanNodeType::SORT: {
            auto& n = static_cast<const SortNode&>(node);
            oss << "Sort(";
            for (size_t i = 0; i < n.order_items.size(); ++i) {
                if (i) oss << ", ";
                oss << (n.order_items[i].expr ? n.order_items[i].expr->ToString() : "?")
                    << (n.order_items[i].ascending ? " ASC" : " DESC");
            }
            oss << ")";
            break;
        }
        case PlanNodeType::LIMIT: {
            auto& n = static_cast<const LimitNode&>(node);
            oss << "Limit(" << n.limit_count << ")";
            break;
        }
        case PlanNodeType::AGGREGATE: {
            auto& n = static_cast<const AggregateNode&>(node);
            oss << "Aggregate(";
            oss << "GROUP BY [";
            for (size_t i = 0; i < n.group_by_exprs.size(); ++i) {
                if (i) oss << ", ";
                oss << (n.group_by_exprs[i] ? n.group_by_exprs[i]->ToString() : "?");
            }
            oss << "], AGG [";
            for (size_t i = 0; i < n.aggregate_exprs.size(); ++i) {
                if (i) oss << ", ";
                oss << (n.aggregate_exprs[i] ? n.aggregate_exprs[i]->ToString() : "?");
            }
            oss << "])";
            break;
        }
        case PlanNodeType::INSERT: {
            auto& n = static_cast<const InsertNode&>(node);
            oss << "Insert(" << n.table_name << ", "
                << n.values_list.size() << " rows)";
            break;
        }
        case PlanNodeType::UPDATE: {
            auto& n = static_cast<const UpdateNode&>(node);
            oss << "Update(" << n.table_name << ")";
            break;
        }
        case PlanNodeType::DELETE: {
            auto& n = static_cast<const DeleteNode&>(node);
            oss << "Delete(" << n.table_name << ")";
            break;
        }
        case PlanNodeType::CREATE_TABLE: {
            auto& n = static_cast<const CreateTableNode&>(node);
            oss << "CreateTable(" << n.table_name << ", "
                << n.columns.size() << " cols)";
            break;
        }
        case PlanNodeType::DROP_TABLE: {
            auto& n = static_cast<const DropTableNode&>(node);
            oss << "DropTable(" << n.table_name << ")";
            break;
        }
        case PlanNodeType::TRUNCATE_TABLE: {
            auto& n = static_cast<const TruncateTableNode&>(node);
            oss << "TruncateTable(" << n.table_name << ")";
            break;
        }
    }
    oss << "\n";
    for (auto& child : node.children) {
        if (child) oss << NodeBodyToString(*child, depth + 1);
    }
    return oss.str();
}

}  // namespace

// ============ SeqScanNode ============

SeqScanNode::SeqScanNode(std::string table_name, std::string table_alias)
    : table_name(std::move(table_name)), table_alias(std::move(table_alias)) {
}

PlanNodeType SeqScanNode::GetType() const {
    return PlanNodeType::SEQ_SCAN;
}

std::string SeqScanNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ FilterNode ============

FilterNode::FilterNode(ExprPtr predicate) : predicate(std::move(predicate)) {
}

PlanNodeType FilterNode::GetType() const {
    return PlanNodeType::FILTER;
}

std::string FilterNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ ProjectNode ============

ProjectNode::ProjectNode(std::vector<ExprPtr> columns,
                          std::vector<std::string> aliases,
                          bool is_distinct)
    : columns(std::move(columns)), aliases(std::move(aliases)), is_distinct(is_distinct) {
}

PlanNodeType ProjectNode::GetType() const {
    return PlanNodeType::PROJECT;
}

std::string ProjectNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ JoinNode ============

JoinNode::JoinNode(JoinType join_type, ExprPtr condition)
    : join_type(join_type), condition(std::move(condition)) {
}

PlanNodeType JoinNode::GetType() const {
    return PlanNodeType::JOIN;
}

std::string JoinNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ SortNode ============

SortNode::SortNode(std::vector<OrderByItem> order_items) : order_items(std::move(order_items)) {
}

PlanNodeType SortNode::GetType() const {
    return PlanNodeType::SORT;
}

std::string SortNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ LimitNode ============

LimitNode::LimitNode(int limit_count, int offset)
    : limit_count(limit_count), offset(offset) {
}

PlanNodeType LimitNode::GetType() const {
    return PlanNodeType::LIMIT;
}

std::string LimitNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ AggregateNode ============

AggregateNode::AggregateNode(std::vector<ExprPtr> group_by_exprs,
                              std::vector<ExprPtr> aggregate_exprs)
    : group_by_exprs(std::move(group_by_exprs)), aggregate_exprs(std::move(aggregate_exprs)) {
}

PlanNodeType AggregateNode::GetType() const {
    return PlanNodeType::AGGREGATE;
}

std::string AggregateNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ InsertNode ============

InsertNode::InsertNode(std::string table_name, std::vector<std::string> columns,
                        std::vector<std::vector<ExprPtr>> values_list)
    : table_name(std::move(table_name)),
      columns(std::move(columns)),
      values_list(std::move(values_list)) {
}

PlanNodeType InsertNode::GetType() const {
    return PlanNodeType::INSERT;
}

std::string InsertNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ UpdateNode ============

UpdateNode::UpdateNode(std::string table_name,
                        std::vector<std::pair<std::string, ExprPtr>> assignments,
                        ExprPtr predicate)
    : table_name(std::move(table_name)),
      assignments(std::move(assignments)),
      predicate(std::move(predicate)) {
}

PlanNodeType UpdateNode::GetType() const {
    return PlanNodeType::UPDATE;
}

std::string UpdateNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ DeleteNode ============

DeleteNode::DeleteNode(std::string table_name, ExprPtr predicate)
    : table_name(std::move(table_name)), predicate(std::move(predicate)) {
}

PlanNodeType DeleteNode::GetType() const {
    return PlanNodeType::DELETE;
}

std::string DeleteNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ CreateTableNode ============

CreateTableNode::CreateTableNode(std::string table_name, std::vector<ColumnDefinition> columns)
    : table_name(std::move(table_name)), columns(std::move(columns)) {
}

PlanNodeType CreateTableNode::GetType() const {
    return PlanNodeType::CREATE_TABLE;
}

std::string CreateTableNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ DropTableNode ============

DropTableNode::DropTableNode(std::string table_name) : table_name(std::move(table_name)) {
}

PlanNodeType DropTableNode::GetType() const {
    return PlanNodeType::DROP_TABLE;
}

std::string DropTableNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ TruncateTableNode ============

TruncateTableNode::TruncateTableNode(std::string table_name) : table_name(std::move(table_name)) {
}

PlanNodeType TruncateTableNode::GetType() const {
    return PlanNodeType::TRUNCATE_TABLE;
}

std::string TruncateTableNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

}  // namespace sqlcompiler