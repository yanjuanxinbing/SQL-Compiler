#include "plan/Plan.h"

#include <sstream>
#include <utility>

namespace sqlcompiler {

namespace {

// 把二元运算符枚举转成可读字符串，仅供 PlanNode 的 ToString 调试输出使用
std::string BinaryOpToString(BinaryOperator op) {
    switch (op) {
        case BinaryOperator::ADD:            return "+";
        case BinaryOperator::SUB:            return "-";
        case BinaryOperator::MUL:            return "*";
        case BinaryOperator::DIV:            return "/";
        case BinaryOperator::EQUAL:          return "=";
        case BinaryOperator::NOT_EQUAL:      return "<>";
        case BinaryOperator::LESS:           return "<";
        case BinaryOperator::LESS_EQUAL:     return "<=";
        case BinaryOperator::GREATER:        return ">";
        case BinaryOperator::GREATER_EQUAL:  return ">=";
        case BinaryOperator::AND:            return "AND";
        case BinaryOperator::OR:             return "OR";
    }
    return "?";
}

// 把连接类型转成可读字符串
std::string JoinTypeToString(JoinType type) {
    switch (type) {
        case JoinType::INNER: return "INNER";
        case JoinType::LEFT:  return "LEFT";
        case JoinType::RIGHT: return "RIGHT";
    }
    return "UNKNOWN";
}

// 把表达式列表拼成 "[expr1, expr2, ...]" 形式
std::string ExprListToString(const std::vector<ExprPtr>& exprs) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < exprs.size(); ++i) {
        if (i > 0) oss << ", ";
        if (exprs[i]) oss << exprs[i]->ToString();
        else          oss << "<null>";
    }
    oss << "]";
    return oss.str();
}

// 把字符串列表拼成 "[s1, s2, ...]" 形式
std::string StringListToString(const std::vector<std::string>& items) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << items[i];
    }
    oss << "]";
    return oss.str();
}

}  // namespace

// ============ SeqScanNode ============

SeqScanNode::SeqScanNode(std::string table_name) : table_name(std::move(table_name)) {
    // 构造完成即可，无额外初始化
}

PlanNodeType SeqScanNode::GetType() const {
    return PlanNodeType::SEQ_SCAN;
}

std::string SeqScanNode::ToString() const {
    std::ostringstream oss;
    oss << "SeqScan(table=" << table_name << ")";
    return oss.str();
}

// ============ FilterNode ============

FilterNode::FilterNode(ExprPtr predicate) : predicate(std::move(predicate)) {
}

PlanNodeType FilterNode::GetType() const {
    return PlanNodeType::FILTER;
}

std::string FilterNode::ToString() const {
    std::ostringstream oss;
    oss << "Filter(predicate=";
    if (predicate) oss << predicate->ToString();
    else           oss << "<null>";
    oss << ")";
    return oss.str();
}

// ============ ProjectNode ============

ProjectNode::ProjectNode(std::vector<ExprPtr> columns) : columns(std::move(columns)) {
}

PlanNodeType ProjectNode::GetType() const {
    return PlanNodeType::PROJECT;
}

std::string ProjectNode::ToString() const {
    std::ostringstream oss;
    oss << "Project(columns=" << ExprListToString(columns) << ")";
    return oss.str();
}

// ============ JoinNode ============

JoinNode::JoinNode(JoinType join_type, ExprPtr condition)
    : join_type(join_type), condition(std::move(condition)) {
}

PlanNodeType JoinNode::GetType() const {
    return PlanNodeType::JOIN;
}

std::string JoinNode::ToString() const {
    std::ostringstream oss;
    oss << "Join(type=" << JoinTypeToString(join_type)
        << ", condition=";
    if (condition) oss << condition->ToString();
    else           oss << "<null>";
    oss << ")";
    return oss.str();
}

// ============ SortNode ============

SortNode::SortNode(std::vector<OrderByItem> order_items) : order_items(std::move(order_items)) {
}

PlanNodeType SortNode::GetType() const {
    return PlanNodeType::SORT;
}

std::string SortNode::ToString() const {
    std::ostringstream oss;
    oss << "Sort(items=[";
    for (size_t i = 0; i < order_items.size(); ++i) {
        if (i > 0) oss << ", ";
        if (order_items[i].expr) oss << order_items[i].expr->ToString();
        else                     oss << "<null>";
        oss << (order_items[i].ascending ? " ASC" : " DESC");
    }
    oss << "])";
    return oss.str();
}

// ============ LimitNode ============

LimitNode::LimitNode(int limit_count) : limit_count(limit_count) {
}

PlanNodeType LimitNode::GetType() const {
    return PlanNodeType::LIMIT;
}

std::string LimitNode::ToString() const {
    std::ostringstream oss;
    oss << "Limit(count=" << limit_count << ")";
    return oss.str();
}

// ============ AggregateNode ============

AggregateNode::AggregateNode(std::vector<ExprPtr> group_by_exprs,
                              std::vector<ExprPtr> aggregate_exprs)
    : group_by_exprs(std::move(group_by_exprs)),
      aggregate_exprs(std::move(aggregate_exprs)) {
}

PlanNodeType AggregateNode::GetType() const {
    return PlanNodeType::AGGREGATE;
}

std::string AggregateNode::ToString() const {
    std::ostringstream oss;
    oss << "Aggregate(group_by=" << ExprListToString(group_by_exprs)
        << ", aggs=" << ExprListToString(aggregate_exprs) << ")";
    return oss.str();
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
    std::ostringstream oss;
    oss << "Insert(table=" << table_name
        << ", columns=" << StringListToString(columns)
        << ", rows=" << values_list.size() << ")";
    return oss.str();
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
    std::ostringstream oss;
    oss << "Update(table=" << table_name << ", assignments=[";
    for (size_t i = 0; i < assignments.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << assignments[i].first << "=";
        if (assignments[i].second) oss << assignments[i].second->ToString();
        else                       oss << "<null>";
    }
    oss << "], predicate=";
    if (predicate) oss << predicate->ToString();
    else           oss << "<null>";
    oss << ")";
    return oss.str();
}

// ============ DeleteNode ============

DeleteNode::DeleteNode(std::string table_name, ExprPtr predicate)
    : table_name(std::move(table_name)), predicate(std::move(predicate)) {
}

PlanNodeType DeleteNode::GetType() const {
    return PlanNodeType::DELETE;
}

std::string DeleteNode::ToString() const {
    std::ostringstream oss;
    oss << "Delete(table=" << table_name << ", predicate=";
    if (predicate) oss << predicate->ToString();
    else           oss << "<null>";
    oss << ")";
    return oss.str();
}

// ============ CreateTableNode ============

CreateTableNode::CreateTableNode(std::string table_name, std::vector<ColumnDefinition> columns)
    : table_name(std::move(table_name)), columns(std::move(columns)) {
}

PlanNodeType CreateTableNode::GetType() const {
    return PlanNodeType::CREATE_TABLE;
}

std::string CreateTableNode::ToString() const {
    std::ostringstream oss;
    oss << "CreateTable(table=" << table_name << ", columns=[";
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << columns[i].column_name << " " << columns[i].data_type;
        if (columns[i].is_primary_key) oss << " PRIMARY KEY";
        if (columns[i].is_not_null)     oss << " NOT NULL";
    }
    oss << "])";
    return oss.str();
}

// ============ DropTableNode ============

DropTableNode::DropTableNode(std::string table_name) : table_name(std::move(table_name)) {
}

PlanNodeType DropTableNode::GetType() const {
    return PlanNodeType::DROP_TABLE;
}

std::string DropTableNode::ToString() const {
    std::ostringstream oss;
    oss << "DropTable(table=" << table_name << ")";
    return oss.str();
}

}  // namespace sqlcompiler