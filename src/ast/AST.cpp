#include "ast/AST.h"

#include <sstream>

namespace sqlcompiler {

namespace {

// 把 BinaryOperator 翻译成可读字符串
const char* BinaryOpToString(BinaryOperator op) {
    switch (op) {
        case BinaryOperator::ADD: return "+";
        case BinaryOperator::SUB: return "-";
        case BinaryOperator::MUL: return "*";
        case BinaryOperator::DIV: return "/";
        case BinaryOperator::EQUAL:         return "=";
        case BinaryOperator::NOT_EQUAL:     return "<>";
        case BinaryOperator::LESS:          return "<";
        case BinaryOperator::LESS_EQUAL:    return "<=";
        case BinaryOperator::GREATER:       return ">";
        case BinaryOperator::GREATER_EQUAL: return ">=";
        case BinaryOperator::AND: return "AND";
        case BinaryOperator::OR:  return "OR";
    }
    return "?";
}

// 把 UnaryOperator 翻译成可读字符串
const char* UnaryOpToString(UnaryOperator op) {
    switch (op) {
        case UnaryOperator::NOT:    return "NOT";
        case UnaryOperator::NEGATE: return "-";
    }
    return "?";
}

// 把 JoinType 翻译成可读字符串
const char* JoinTypeToString(JoinType type) {
    switch (type) {
        case JoinType::INNER: return "INNER JOIN";
        case JoinType::LEFT:  return "LEFT JOIN";
        case JoinType::RIGHT: return "RIGHT JOIN";
    }
    return "JOIN";
}

}  // namespace

// ============ LiteralExpr ============

LiteralExpr::LiteralExpr(LiteralType literal_type, std::string value)
    : literal_type(literal_type), value(std::move(value)) {}

NodeType LiteralExpr::GetType() const {
    return NodeType::LITERAL_EXPR;
}

std::string LiteralExpr::ToString() const {
    switch (literal_type) {
        case LiteralType::INTEGER:  return value;
        case LiteralType::FLOAT:    return value;
        case LiteralType::STRING:   return "'" + value + "'";
        case LiteralType::BOOLEAN:  return value;
        case LiteralType::NULL_VALUE: return "NULL";
    }
    return value;
}

// ============ ColumnRefExpr ============

ColumnRefExpr::ColumnRefExpr(std::string table_name, std::string column_name)
    : table_name(std::move(table_name)), column_name(std::move(column_name)) {}

NodeType ColumnRefExpr::GetType() const {
    return NodeType::COLUMN_REF_EXPR;
}

std::string ColumnRefExpr::ToString() const {
    if (table_name.empty()) {
        return column_name;
    }
    return table_name + "." + column_name;
}

// ============ BinaryExpr ============

BinaryExpr::BinaryExpr(BinaryOperator op, ExprPtr left, ExprPtr right)
    : op(op), left(std::move(left)), right(std::move(right)) {}

NodeType BinaryExpr::GetType() const {
    return NodeType::BINARY_EXPR;
}

std::string BinaryExpr::ToString() const {
    std::string l = left ? left->ToString() : "<null>";
    std::string r = right ? right->ToString() : "<null>";
    return "(" + l + " " + BinaryOpToString(op) + " " + r + ")";
}

// ============ UnaryExpr ============

UnaryExpr::UnaryExpr(UnaryOperator op, ExprPtr operand)
    : op(op), operand(std::move(operand)) {}

NodeType UnaryExpr::GetType() const {
    return NodeType::UNARY_EXPR;
}

std::string UnaryExpr::ToString() const {
    std::string inner = operand ? operand->ToString() : "<null>";
    return "(" + std::string(UnaryOpToString(op)) + " " + inner + ")";
}

// ============ FunctionCallExpr ============

FunctionCallExpr::FunctionCallExpr(std::string function_name,
                                   std::vector<ExprPtr> arguments)
    : function_name(std::move(function_name)),
      arguments(std::move(arguments)) {}

NodeType FunctionCallExpr::GetType() const {
    return NodeType::FUNCTION_CALL_EXPR;
}

std::string FunctionCallExpr::ToString() const {
    std::ostringstream os;
    os << function_name << "(";
    if (arguments.empty()) {
        os << "*";
    } else {
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            if (i > 0) os << ", ";
            os << (arguments[i] ? arguments[i]->ToString() : "<null>");
        }
    }
    os << ")";
    return os.str();
}

// ============ SelectStatement ============

SelectStatement::SelectStatement() = default;

NodeType SelectStatement::GetType() const {
    return NodeType::SELECT_STMT;
}

std::string SelectStatement::ToString() const {
    std::ostringstream os;
    os << "SELECT ";
    if (is_distinct) {
        os << "DISTINCT ";
    }

    // select_list
    if (select_list.empty()) {
        os << "*";
    } else {
        for (std::size_t i = 0; i < select_list.size(); ++i) {
            if (i > 0) os << ", ";
            os << (select_list[i] ? select_list[i]->ToString() : "<null>");
        }
    }

    // FROM
    if (!from_table.empty()) {
        os << " FROM " << from_table;
    }

    // JOINs
    for (const auto& j : joins) {
        os << " " << JoinTypeToString(j.join_type) << " " << j.table_name;
        if (j.on_condition) {
            os << " ON " << j.on_condition->ToString();
        }
    }

    // WHERE
    if (where_clause) {
        os << " WHERE " << where_clause->ToString();
    }

    // GROUP BY
    if (!group_by.empty()) {
        os << " GROUP BY ";
        for (std::size_t i = 0; i < group_by.size(); ++i) {
            if (i > 0) os << ", ";
            os << (group_by[i] ? group_by[i]->ToString() : "<null>");
        }
    }

    // HAVING
    if (having_clause) {
        os << " HAVING " << having_clause->ToString();
    }

    // ORDER BY
    if (!order_by.empty()) {
        os << " ORDER BY ";
        for (std::size_t i = 0; i < order_by.size(); ++i) {
            if (i > 0) os << ", ";
            os << (order_by[i].expr ? order_by[i].expr->ToString() : "<null>");
            os << (order_by[i].ascending ? " ASC" : " DESC");
        }
    }

    // LIMIT
    if (limit >= 0) {
        os << " LIMIT " << limit;
    }

    return os.str();
}

// ============ InsertStatement ============

InsertStatement::InsertStatement() = default;

NodeType InsertStatement::GetType() const {
    return NodeType::INSERT_STMT;
}

std::string InsertStatement::ToString() const {
    std::ostringstream os;
    os << "INSERT INTO " << table_name;

    if (!columns.empty()) {
        os << " (";
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (i > 0) os << ", ";
            os << columns[i];
        }
        os << ")";
    }

    os << " VALUES ";
    for (std::size_t r = 0; r < values_list.size(); ++r) {
        if (r > 0) os << ", ";
        os << "(";
        const auto& row = values_list[r];
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i > 0) os << ", ";
            os << (row[i] ? row[i]->ToString() : "<null>");
        }
        os << ")";
    }
    return os.str();
}

// ============ UpdateStatement ============

UpdateStatement::UpdateStatement() = default;

NodeType UpdateStatement::GetType() const {
    return NodeType::UPDATE_STMT;
}

std::string UpdateStatement::ToString() const {
    std::ostringstream os;
    os << "UPDATE " << table_name << " SET ";
    for (std::size_t i = 0; i < assignments.size(); ++i) {
        if (i > 0) os << ", ";
        os << assignments[i].first << " = "
           << (assignments[i].second ? assignments[i].second->ToString()
                                     : "<null>");
    }
    if (where_clause) {
        os << " WHERE " << where_clause->ToString();
    }
    return os.str();
}

// ============ DeleteStatement ============

DeleteStatement::DeleteStatement() = default;

NodeType DeleteStatement::GetType() const {
    return NodeType::DELETE_STMT;
}

std::string DeleteStatement::ToString() const {
    std::ostringstream os;
    os << "DELETE FROM " << table_name;
    if (where_clause) {
        os << " WHERE " << where_clause->ToString();
    }
    return os.str();
}

// ============ CreateTableStatement ============

CreateTableStatement::CreateTableStatement() = default;

NodeType CreateTableStatement::GetType() const {
    return NodeType::CREATE_TABLE_STMT;
}

std::string CreateTableStatement::ToString() const {
    std::ostringstream os;
    os << "CREATE TABLE " << table_name << " (";
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) os << ", ";
        const auto& col = columns[i];
        os << col.column_name << " " << col.data_type;
        if (col.is_primary_key) os << " PRIMARY KEY";
        if (col.is_not_null)    os << " NOT NULL";
    }
    os << ")";
    return os.str();
}

// ============ DropTableStatement ============

DropTableStatement::DropTableStatement() = default;

NodeType DropTableStatement::GetType() const {
    return NodeType::DROP_TABLE_STMT;
}

std::string DropTableStatement::ToString() const {
    return "DROP TABLE " + table_name;
}

}  // namespace sqlcompiler
