#include "plan/Plan.h"

namespace sqlcompiler {

// ============ SeqScanNode ============

SeqScanNode::SeqScanNode(std::string table_name) : table_name(std::move(table_name)) {
    // TODO
}

PlanNodeType SeqScanNode::GetType() const {
    // TODO: 返回 PlanNodeType::SEQ_SCAN
    return PlanNodeType::SEQ_SCAN;
}

std::string SeqScanNode::ToString() const {
    // TODO
    return "";
}

// ============ FilterNode ============

FilterNode::FilterNode(ExprPtr predicate) : predicate(std::move(predicate)) {
    // TODO
}

PlanNodeType FilterNode::GetType() const {
    // TODO: 返回 PlanNodeType::FILTER
    return PlanNodeType::FILTER;
}

std::string FilterNode::ToString() const {
    // TODO
    return "";
}

// ============ ProjectNode ============

ProjectNode::ProjectNode(std::vector<ExprPtr> columns) : columns(std::move(columns)) {
    // TODO
}

PlanNodeType ProjectNode::GetType() const {
    // TODO: 返回 PlanNodeType::PROJECT
    return PlanNodeType::PROJECT;
}

std::string ProjectNode::ToString() const {
    // TODO
    return "";
}

// ============ JoinNode ============

JoinNode::JoinNode(JoinType join_type, ExprPtr condition)
    : join_type(join_type), condition(std::move(condition)) {
    // TODO
}

PlanNodeType JoinNode::GetType() const {
    // TODO: 返回 PlanNodeType::JOIN
    return PlanNodeType::JOIN;
}

std::string JoinNode::ToString() const {
    // TODO
    return "";
}

// ============ SortNode ============

SortNode::SortNode(std::vector<OrderByItem> order_items) : order_items(std::move(order_items)) {
    // TODO
}

PlanNodeType SortNode::GetType() const {
    // TODO: 返回 PlanNodeType::SORT
    return PlanNodeType::SORT;
}

std::string SortNode::ToString() const {
    // TODO
    return "";
}

// ============ LimitNode ============

LimitNode::LimitNode(int limit_count) : limit_count(limit_count) {
    // TODO
}

PlanNodeType LimitNode::GetType() const {
    // TODO: 返回 PlanNodeType::LIMIT
    return PlanNodeType::LIMIT;
}

std::string LimitNode::ToString() const {
    // TODO
    return "";
}

// ============ AggregateNode ============

AggregateNode::AggregateNode(std::vector<ExprPtr> group_by_exprs,
                              std::vector<ExprPtr> aggregate_exprs)
    : group_by_exprs(std::move(group_by_exprs)), aggregate_exprs(std::move(aggregate_exprs)) {
    // TODO
}

PlanNodeType AggregateNode::GetType() const {
    // TODO: 返回 PlanNodeType::AGGREGATE
    return PlanNodeType::AGGREGATE;
}

std::string AggregateNode::ToString() const {
    // TODO
    return "";
}

// ============ InsertNode ============

InsertNode::InsertNode(std::string table_name, std::vector<std::string> columns,
                        std::vector<std::vector<ExprPtr>> values_list)
    : table_name(std::move(table_name)),
      columns(std::move(columns)),
      values_list(std::move(values_list)) {
    // TODO
}

PlanNodeType InsertNode::GetType() const {
    // TODO: 返回 PlanNodeType::INSERT
    return PlanNodeType::INSERT;
}

std::string InsertNode::ToString() const {
    // TODO
    return "";
}

// ============ UpdateNode ============

UpdateNode::UpdateNode(std::string table_name,
                        std::vector<std::pair<std::string, ExprPtr>> assignments,
                        ExprPtr predicate)
    : table_name(std::move(table_name)),
      assignments(std::move(assignments)),
      predicate(std::move(predicate)) {
    // TODO
}

PlanNodeType UpdateNode::GetType() const {
    // TODO: 返回 PlanNodeType::UPDATE
    return PlanNodeType::UPDATE;
}

std::string UpdateNode::ToString() const {
    // TODO
    return "";
}

// ============ DeleteNode ============

DeleteNode::DeleteNode(std::string table_name, ExprPtr predicate)
    : table_name(std::move(table_name)), predicate(std::move(predicate)) {
    // TODO
}

PlanNodeType DeleteNode::GetType() const {
    // TODO: 返回 PlanNodeType::DELETE
    return PlanNodeType::DELETE;
}

std::string DeleteNode::ToString() const {
    // TODO
    return "";
}

// ============ CreateTableNode ============

CreateTableNode::CreateTableNode(std::string table_name, std::vector<ColumnDefinition> columns)
    : table_name(std::move(table_name)), columns(std::move(columns)) {
    // TODO
}

PlanNodeType CreateTableNode::GetType() const {
    // TODO: 返回 PlanNodeType::CREATE_TABLE
    return PlanNodeType::CREATE_TABLE;
}

std::string CreateTableNode::ToString() const {
    // TODO
    return "";
}

// ============ DropTableNode ============

DropTableNode::DropTableNode(std::string table_name) : table_name(std::move(table_name)) {
    // TODO
}

PlanNodeType DropTableNode::GetType() const {
    // TODO: 返回 PlanNodeType::DROP_TABLE
    return PlanNodeType::DROP_TABLE;
}

std::string DropTableNode::ToString() const {
    // TODO
    return "";
}

}  // namespace sqlcompiler
