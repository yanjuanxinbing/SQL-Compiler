#include "ast/AST.h"

namespace sqlcompiler {

// ============ LiteralExpr ============

LiteralExpr::LiteralExpr(LiteralType literal_type, std::string value)
    : literal_type(literal_type), value(std::move(value)) {
    // TODO
}

NodeType LiteralExpr::GetType() const {
    // TODO: 返回 NodeType::LITERAL_EXPR
    return NodeType::LITERAL_EXPR;
}

std::string LiteralExpr::ToString() const {
    // TODO: 返回字面量的可读表示
    return "";
}

// ============ ColumnRefExpr ============

ColumnRefExpr::ColumnRefExpr(std::string table_name, std::string column_name)
    : table_name(std::move(table_name)), column_name(std::move(column_name)) {
    // TODO
}

NodeType ColumnRefExpr::GetType() const {
    // TODO: 返回 NodeType::COLUMN_REF_EXPR
    return NodeType::COLUMN_REF_EXPR;
}

std::string ColumnRefExpr::ToString() const {
    // TODO: 返回形如 "table.column" 或 "column" 的字符串
    return "";
}

// ============ BinaryExpr ============

BinaryExpr::BinaryExpr(BinaryOperator op, ExprPtr left, ExprPtr right)
    : op(op), left(std::move(left)), right(std::move(right)) {
    // TODO
}

NodeType BinaryExpr::GetType() const {
    // TODO: 返回 NodeType::BINARY_EXPR
    return NodeType::BINARY_EXPR;
}

std::string BinaryExpr::ToString() const {
    // TODO: 返回形如 "(left op right)" 的字符串
    return "";
}

// ============ UnaryExpr ============

UnaryExpr::UnaryExpr(UnaryOperator op, ExprPtr operand)
    : op(op), operand(std::move(operand)) {
    // TODO
}

NodeType UnaryExpr::GetType() const {
    // TODO: 返回 NodeType::UNARY_EXPR
    return NodeType::UNARY_EXPR;
}

std::string UnaryExpr::ToString() const {
    // TODO: 返回形如 "NOT (operand)" 或 "-(operand)" 的字符串
    return "";
}

// ============ FunctionCallExpr ============

FunctionCallExpr::FunctionCallExpr(std::string function_name, std::vector<ExprPtr> arguments)
    : function_name(std::move(function_name)), arguments(std::move(arguments)) {
    // TODO
}

NodeType FunctionCallExpr::GetType() const {
    // TODO: 返回 NodeType::FUNCTION_CALL_EXPR
    return NodeType::FUNCTION_CALL_EXPR;
}

std::string FunctionCallExpr::ToString() const {
    // TODO: 返回形如 "COUNT(*)" 的字符串
    return "";
}

// ============ SelectStatement ============

SelectStatement::SelectStatement() {
    // TODO: 初始化各成员默认值（若头文件中未使用默认成员初始化器）
}

NodeType SelectStatement::GetType() const {
    // TODO: 返回 NodeType::SELECT_STMT
    return NodeType::SELECT_STMT;
}

std::string SelectStatement::ToString() const {
    // TODO: 拼接输出完整的SELECT语句结构，便于调试
    return "";
}

// ============ InsertStatement ============

InsertStatement::InsertStatement() {
    // TODO
}

NodeType InsertStatement::GetType() const {
    // TODO: 返回 NodeType::INSERT_STMT
    return NodeType::INSERT_STMT;
}

std::string InsertStatement::ToString() const {
    // TODO
    return "";
}

// ============ UpdateStatement ============

UpdateStatement::UpdateStatement() {
    // TODO
}

NodeType UpdateStatement::GetType() const {
    // TODO: 返回 NodeType::UPDATE_STMT
    return NodeType::UPDATE_STMT;
}

std::string UpdateStatement::ToString() const {
    // TODO
    return "";
}

// ============ DeleteStatement ============

DeleteStatement::DeleteStatement() {
    // TODO
}

NodeType DeleteStatement::GetType() const {
    // TODO: 返回 NodeType::DELETE_STMT
    return NodeType::DELETE_STMT;
}

std::string DeleteStatement::ToString() const {
    // TODO
    return "";
}

// ============ CreateTableStatement ============

CreateTableStatement::CreateTableStatement() {
    // TODO
}

NodeType CreateTableStatement::GetType() const {
    // TODO: 返回 NodeType::CREATE_TABLE_STMT
    return NodeType::CREATE_TABLE_STMT;
}

std::string CreateTableStatement::ToString() const {
    // TODO
    return "";
}

// ============ DropTableStatement ============

DropTableStatement::DropTableStatement() {
    // TODO
}

NodeType DropTableStatement::GetType() const {
    // TODO: 返回 NodeType::DROP_TABLE_STMT
    return NodeType::DROP_TABLE_STMT;
}

std::string DropTableStatement::ToString() const {
    // TODO
    return "";
}

}  // namespace sqlcompiler
