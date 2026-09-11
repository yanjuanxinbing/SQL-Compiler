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
        case PlanNodeType::CREATE_INDEX: {
            auto& n = static_cast<const CreateIndexNode&>(node);
            oss << "CreateIndex(" << n.index_name << " on " << n.table_name << ")";
            break;
        }
        case PlanNodeType::DROP_INDEX: {
            auto& n = static_cast<const DropIndexNode&>(node);
            oss << "DropIndex(" << n.index_name << ")";
            break;
        }
        case PlanNodeType::INDEX_SCAN: {
            auto& n = static_cast<const IndexScanNode&>(node);
            oss << "IndexScan(" << n.table_name << " using " << n.index_name << ")";
            break;
        }
        case PlanNodeType::TRUNCATE_TABLE: {
            auto& n = static_cast<const TruncateTableNode&>(node);
            oss << "TruncateTable(" << n.table_name << ")";
            break;
        }
        case PlanNodeType::SET_OP: {
            auto& n = static_cast<const SetOpNode&>(node);
            const char* kn = "?";
            switch (n.kind) {
                case SetOpNode::Kind::UNION: kn = "UNION"; break;
                case SetOpNode::Kind::UNION_ALL: kn = "UNION ALL"; break;
                case SetOpNode::Kind::INTERSECT: kn = "INTERSECT"; break;
                case SetOpNode::Kind::EXCEPT: kn = "EXCEPT"; break;
            }
            oss << "SetOp(" << kn << ")";
            break;
        }
        case PlanNodeType::WINDOW: {
            auto& n = static_cast<const WindowNode&>(node);
            oss << "Window(";
            for (size_t i = 0; i < n.select_list.size(); ++i) {
                if (i) oss << ", ";
                oss << (n.select_list[i] ? n.select_list[i]->ToString() : "?");
            }
            oss << ")";
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
                              std::vector<ExprPtr> aggregate_exprs,
                              std::vector<std::string> aliases)
    : group_by_exprs(std::move(group_by_exprs)),
      aggregate_exprs(std::move(aggregate_exprs)),
      aliases(std::move(aliases)) {
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

CreateTableNode::CreateTableNode(std::string table_name, std::vector<ColumnDefinition> columns,
                                 std::vector<std::vector<std::string>> primary_keys,
                                 bool if_not_exists)
    : table_name(std::move(table_name)), columns(std::move(columns)),
      primary_keys(std::move(primary_keys)), if_not_exists(if_not_exists) {
}

PlanNodeType CreateTableNode::GetType() const {
    return PlanNodeType::CREATE_TABLE;
}

std::string CreateTableNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ DropTableNode ============

DropTableNode::DropTableNode(std::string table_name, bool if_exists)
    : table_name(std::move(table_name)), if_exists(if_exists) {
}

PlanNodeType DropTableNode::GetType() const {
    return PlanNodeType::DROP_TABLE;
}

std::string DropTableNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ IndexScanNode ============

IndexScanNode::IndexScanNode(std::string table_name, std::string index_name,
                             std::string table_alias)
    : table_name(std::move(table_name)), index_name(std::move(index_name)),
      table_alias(std::move(table_alias)) {
}

PlanNodeType IndexScanNode::GetType() const {
    return PlanNodeType::INDEX_SCAN;
}

std::string IndexScanNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ CreateIndexNode ============

CreateIndexNode::CreateIndexNode(std::string index_name, std::string table_name,
                                 std::vector<std::string> key_columns,
                                 bool is_unique)
    : index_name(std::move(index_name)), table_name(std::move(table_name)),
      key_columns(std::move(key_columns)), is_unique(is_unique) {
}

PlanNodeType CreateIndexNode::GetType() const {
    return PlanNodeType::CREATE_INDEX;
}

std::string CreateIndexNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ DropIndexNode ============

DropIndexNode::DropIndexNode(std::string index_name, bool if_exists)
    : index_name(std::move(index_name)), if_exists(if_exists) {
}

PlanNodeType DropIndexNode::GetType() const {
    return PlanNodeType::DROP_INDEX;
}

std::string DropIndexNode::ToString() const {
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

// ============ SetOpNode ============

SetOpNode::SetOpNode(Kind kind) : kind(kind) {
}

PlanNodeType SetOpNode::GetType() const {
    return PlanNodeType::SET_OP;
}

std::string SetOpNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ WindowNode ============

WindowNode::WindowNode(std::vector<ExprPtr> select_list,
                       std::vector<std::string> aliases,
                       std::vector<std::pair<std::string, WindowSpec>> named_windows)
    : select_list(std::move(select_list)),
      aliases(std::move(aliases)),
      named_windows(std::move(named_windows)) {
}

PlanNodeType WindowNode::GetType() const {
    return PlanNodeType::WINDOW;
}

std::string WindowNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ SubqueryNode ============

SubqueryNode::SubqueryNode(SubqueryType kind, ExprPtr outer_expr,
                            std::string comparison_op)
    : kind(kind),
      outer_expr(std::move(outer_expr)),
      comparison_op(std::move(comparison_op)) {
}

PlanNodeType SubqueryNode::GetType() const {
    return PlanNodeType::SUBQUERY;
}

std::string SubqueryNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ CteDefineNode ============

CteDefineNode::CteDefineNode(std::string cte_name, bool is_recursive)
    : cte_name(std::move(cte_name)), is_recursive(is_recursive) {
}

PlanNodeType CteDefineNode::GetType() const {
    return PlanNodeType::CTE_DEFINE;
}

std::string CteDefineNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ CteBindNode ============

CteBindNode::CteBindNode(std::string cte_name)
    : cte_name(std::move(cte_name)) {
}

PlanNodeType CteBindNode::GetType() const {
    return PlanNodeType::CTE_BIND;
}

std::string CteBindNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

}  // namespace sqlcompiler