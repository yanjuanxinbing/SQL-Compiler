#include "ast/AST.h"

#include <sstream>

namespace sqlcompiler {

namespace {

const char* LiteralTypeToString(LiteralType t) {
    switch (t) {
        case LiteralType::INTEGER:    return "INT";
        case LiteralType::FLOAT:      return "FLOAT";
        case LiteralType::STRING:     return "STRING";
        case LiteralType::NULL_VALUE: return "NULL";
        case LiteralType::BOOLEAN:    return "BOOL";
    }
    return "?";
}

const char* BinaryOpToString(BinaryOperator op) {
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
        case BinaryOperator::LIKE:           return "LIKE";
        case BinaryOperator::IN_LIST:        return "IN";
        case BinaryOperator::BETWEEN:        return "BETWEEN";
        case BinaryOperator::IS_NULL:        return "IS NULL";
        case BinaryOperator::IS_NOT_NULL:    return "IS NOT NULL";
    }
    return "?";
}

const char* UnaryOpToString(UnaryOperator op) {
    switch (op) {
        case UnaryOperator::NOT:    return "NOT";
        case UnaryOperator::NEGATE: return "-";
    }
    return "?";
}

const char* JoinTypeToString(JoinType t) {
    switch (t) {
        case JoinType::INNER: return "INNER";
        case JoinType::LEFT:  return "LEFT";
        case JoinType::RIGHT: return "RIGHT";
    }
    return "?";
}

}  // namespace

// ============ LiteralExpr ============

LiteralExpr::LiteralExpr(LiteralType literal_type, std::string value)
    : literal_type(literal_type), value(std::move(value)) {
}

NodeType LiteralExpr::GetType() const {
    return NodeType::LITERAL_EXPR;
}

std::string LiteralExpr::ToString() const {
    if (literal_type == LiteralType::STRING) {
        return "'" + value + "'";
    }
    if (literal_type == LiteralType::NULL_VALUE) {
        return "NULL";
    }
    return value;
}

// ============ ColumnRefExpr ============

ColumnRefExpr::ColumnRefExpr(std::string table_name, std::string column_name)
    : table_name(std::move(table_name)), column_name(std::move(column_name)) {
}

NodeType ColumnRefExpr::GetType() const {
    return NodeType::COLUMN_REF_EXPR;
}

std::string ColumnRefExpr::ToString() const {
    if (!table_name.empty()) {
        return table_name + "." + column_name;
    }
    return column_name;
}

// ============ BinaryExpr ============

BinaryExpr::BinaryExpr(BinaryOperator op, ExprPtr left, ExprPtr right)
    : op(op), left(std::move(left)), right(std::move(right)) {
}

NodeType BinaryExpr::GetType() const {
    return NodeType::BINARY_EXPR;
}

std::string BinaryExpr::ToString() const {
    std::string l = left ? left->ToString() : "?";
    std::string r = right ? right->ToString() : "?";
    return "(" + l + " " + BinaryOpToString(op) + " " + r + ")";
}

// ============ UnaryExpr ============

UnaryExpr::UnaryExpr(UnaryOperator op, ExprPtr operand)
    : op(op), operand(std::move(operand)) {
}

NodeType UnaryExpr::GetType() const {
    return NodeType::UNARY_EXPR;
}

std::string UnaryExpr::ToString() const {
    std::string inner = operand ? operand->ToString() : "?";
    if (op == UnaryOperator::NOT) {
        return "NOT (" + inner + ")";
    }
    return "-" + inner;
}

// ============ FunctionCallExpr ============

FunctionCallExpr::FunctionCallExpr(std::string function_name, std::vector<ExprPtr> arguments)
    : function_name(std::move(function_name)), arguments(std::move(arguments)) {
}

NodeType FunctionCallExpr::GetType() const {
    return NodeType::FUNCTION_CALL_EXPR;
}

std::string FunctionCallExpr::ToString() const {
    std::ostringstream oss;
    oss << function_name << "(";
    if (function_name == "*" || (arguments.empty() && function_name == "*")) {
        oss << "*";
    } else {
        for (size_t i = 0; i < arguments.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << (arguments[i] ? arguments[i]->ToString() : "?");
        }
    }
    oss << ")";
    return oss.str();
}

// ============ SelectStatement ============

SelectStatement::SelectStatement() {
}

NodeType SelectStatement::GetType() const {
    return NodeType::SELECT_STMT;
}

std::string SelectStatement::ToString() const {
    std::ostringstream oss;
    oss << "SELECT ";
    if (is_distinct) oss << "DISTINCT ";
    for (size_t i = 0; i < select_list.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << (select_list[i] ? select_list[i]->ToString() : "?");
    }
    if (!from_table.empty()) {
        oss << " FROM " << from_table;
    }
    for (const auto& j : joins) {
        oss << " " << JoinTypeToString(j.join_type) << " JOIN "
            << j.table_name << " ON "
            << (j.on_condition ? j.on_condition->ToString() : "?");
    }
    if (where_clause) {
        oss << " WHERE " << where_clause->ToString();
    }
    if (!group_by.empty()) {
        oss << " GROUP BY ";
        for (size_t i = 0; i < group_by.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << (group_by[i] ? group_by[i]->ToString() : "?");
        }
    }
    if (having_clause) {
        oss << " HAVING " << having_clause->ToString();
    }
    if (!order_by.empty()) {
        oss << " ORDER BY ";
        for (size_t i = 0; i < order_by.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << (order_by[i].expr ? order_by[i].expr->ToString() : "?")
                << (order_by[i].ascending ? " ASC" : " DESC");
        }
    }
    if (limit >= 0) {
        oss << " LIMIT " << limit;
    }
    return oss.str();
}

// ============ InsertStatement ============

InsertStatement::InsertStatement() {
}

NodeType InsertStatement::GetType() const {
    return NodeType::INSERT_STMT;
}

std::string InsertStatement::ToString() const {
    std::ostringstream oss;
    oss << "INSERT INTO " << table_name;
    if (!columns.empty()) {
        oss << " (";
        for (size_t i = 0; i < columns.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << columns[i];
        }
        oss << ")";
    }
    oss << " VALUES ";
    for (size_t i = 0; i < values_list.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << "(";
        for (size_t j = 0; j < values_list[i].size(); ++j) {
            if (j > 0) oss << ", ";
            oss << (values_list[i][j] ? values_list[i][j]->ToString() : "?");
        }
        oss << ")";
    }
    return oss.str();
}

// ============ UpdateStatement ============

UpdateStatement::UpdateStatement() {
}

NodeType UpdateStatement::GetType() const {
    return NodeType::UPDATE_STMT;
}

std::string UpdateStatement::ToString() const {
    std::ostringstream oss;
    oss << "UPDATE " << table_name << " SET ";
    for (size_t i = 0; i < assignments.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << assignments[i].first << " = "
            << (assignments[i].second ? assignments[i].second->ToString() : "?");
    }
    if (where_clause) {
        oss << " WHERE " << where_clause->ToString();
    }
    return oss.str();
}

// ============ DeleteStatement ============

DeleteStatement::DeleteStatement() {
}

NodeType DeleteStatement::GetType() const {
    return NodeType::DELETE_STMT;
}

std::string DeleteStatement::ToString() const {
    std::ostringstream oss;
    oss << "DELETE FROM " << table_name;
    if (where_clause) {
        oss << " WHERE " << where_clause->ToString();
    }
    return oss.str();
}

// ============ CreateTableStatement ============

CreateTableStatement::CreateTableStatement() {
}

NodeType CreateTableStatement::GetType() const {
    return NodeType::CREATE_TABLE_STMT;
}

std::string CreateTableStatement::ToString() const {
    std::ostringstream oss;
    oss << "CREATE TABLE " << table_name << " (";
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << columns[i].column_name << " " << columns[i].data_type;
        if (columns[i].is_primary_key) oss << " PRIMARY KEY";
        if (columns[i].is_not_null) oss << " NOT NULL";
    }
    oss << ")";
    return oss.str();
}

// ============ DropTableStatement ============

DropTableStatement::DropTableStatement() {
}

NodeType DropTableStatement::GetType() const {
    return NodeType::DROP_TABLE_STMT;
}

std::string DropTableStatement::ToString() const {
    return "DROP TABLE " + table_name;
}

}  // namespace sqlcompiler