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
        case LiteralType::DATE:       return "DATE";
        case LiteralType::TIMESTAMP:  return "TIMESTAMP";
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
        case BinaryOperator::CONCAT:         return "||";
        case BinaryOperator::LIKE:           return "LIKE";
        case BinaryOperator::IN_LIST:        return "IN";
        case BinaryOperator::BETWEEN:        return "BETWEEN";
        case BinaryOperator::IS_NULL:        return "IS NULL";
        case BinaryOperator::IS_NOT_NULL:    return "IS NOT NULL";
        case BinaryOperator::INTERVAL_ADD:   return "+";
        case BinaryOperator::INTERVAL_SUB:   return "-";
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
        case JoinType::INNER:     return "INNER";
        case JoinType::LEFT:      return "LEFT";
        case JoinType::RIGHT:     return "RIGHT";
        case JoinType::FULL_OUTER:return "FULL OUTER";
        case JoinType::CROSS:     return "CROSS";
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
    if (literal_type == LiteralType::DATE) {
        return "DATE '" + value + "'";
    }
    if (literal_type == LiteralType::TIMESTAMP) {
        return "TIMESTAMP '" + value + "'";
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
    if (is_distinct) oss << "DISTINCT ";
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
    if (query) {
        oss << " " << query->ToString();
        if (has_on_duplicate && !upsert_assignments.empty()) {
            oss << " ON DUPLICATE KEY UPDATE ";
            for (size_t i = 0; i < upsert_assignments.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << upsert_assignments[i].first << " = "
                << (upsert_assignments[i].second ? upsert_assignments[i].second->ToString() : "?");
            }
        }
        return oss.str();
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
    if (has_on_duplicate && !upsert_assignments.empty()) {
        oss << " ON DUPLICATE KEY UPDATE ";
        for (size_t i = 0; i < upsert_assignments.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << upsert_assignments[i].first << " = "
                << (upsert_assignments[i].second ? upsert_assignments[i].second->ToString() : "?");
        }
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
    for (const auto& pk : primary_keys) {
        if (!columns.empty()) oss << ", ";
        oss << "PRIMARY KEY(";
        for (size_t i = 0; i < pk.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << pk[i];
        }
        oss << ")";
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

// ============ CreateIndexStatement ============

CreateIndexStatement::CreateIndexStatement() {
}

NodeType CreateIndexStatement::GetType() const {
    return NodeType::CREATE_INDEX_STMT;
}

std::string CreateIndexStatement::ToString() const {
    std::string out = "CREATE ";
    if (is_unique) out += "UNIQUE ";
    out += "INDEX " + index_name + " ON " + table_name + "(";
    for (size_t i = 0; i < key_columns.size(); ++i) {
        if (i) out += ", ";
        out += key_columns[i];
    }
    out += ")";
    return out;
}

// ============ DropIndexStatement ============

DropIndexStatement::DropIndexStatement() {
}

NodeType DropIndexStatement::GetType() const {
    return NodeType::DROP_INDEX_STMT;
}

std::string DropIndexStatement::ToString() const {
    return std::string("DROP INDEX ") + (if_exists ? "IF EXISTS " : "") + index_name;
}

// ============ TruncateTableStatement ============

TruncateTableStatement::TruncateTableStatement() {
}

NodeType TruncateTableStatement::GetType() const {
    return NodeType::TRUNCATE_TABLE_STMT;
}

std::string TruncateTableStatement::ToString() const {
    return "TRUNCATE TABLE " + table_name;
}

// ============ AlterStatement ============

AlterStatement::AlterStatement() {
}

NodeType AlterStatement::GetType() const {
    return NodeType::ALTER_TABLE_STMT;
}

std::string AlterStatement::ToString() const {
    std::ostringstream oss;
    oss << "ALTER TABLE " << table_name << " ";
    switch (action) {
        case AlterAction::ADD_COLUMN: {
            oss << "ADD COLUMN ";
            if (column_def) {
                oss << column_def->column_name << " " << column_def->data_type;
                if (column_def->char_length > 0) {
                    oss << "(" << column_def->char_length << ")";
                }
            }
            break;
        }
        case AlterAction::DROP_COLUMN:
            oss << "DROP COLUMN " << drop_column_name;
            break;
        case AlterAction::RENAME_TO:
            oss << "RENAME TO " << new_table_name;
            break;
        case AlterAction::MODIFY_COLUMN: {
            oss << "MODIFY COLUMN ";
            if (column_def) {
                oss << column_def->column_name << " " << column_def->data_type;
            }
            break;
        }
    }
    return oss.str();
}

// ============ CaseExprNode ============

CaseExprNode::CaseExprNode() {
}

NodeType CaseExprNode::GetType() const {
    return NodeType::CASE_EXPR;
}

std::string CaseExprNode::ToString() const {
    std::ostringstream oss;
    oss << "CASE";
    if (subject) {
        oss << " " << subject->ToString();
    }
    for (const auto& w : whens) {
        oss << " WHEN " << (w.when_expr ? w.when_expr->ToString() : "?")
            << " THEN " << (w.then_expr ? w.then_expr->ToString() : "?");
    }
    if (else_expr) {
        oss << " ELSE " << else_expr->ToString();
    }
    oss << " END";
    return oss.str();
}

// ============ CastExprNode ============

CastExprNode::CastExprNode(ExprPtr expr, std::string target_type)
    : expr(std::move(expr)), target_type(std::move(target_type)) {
}

NodeType CastExprNode::GetType() const {
    return NodeType::CAST_EXPR;
}

std::string CastExprNode::ToString() const {
    std::string inner = expr ? expr->ToString() : "?";
    return "CAST(" + inner + " AS " + target_type + ")";
}

// ============ WindowFuncNode ============

WindowFuncNode::WindowFuncNode(std::string function_name,
                                std::vector<ExprPtr> arguments,
                                WindowSpec spec,
                                std::string window_name)
    : function_name(std::move(function_name)),
      arguments(std::move(arguments)),
      spec(std::move(spec)),
      window_name(std::move(window_name)) {
}

NodeType WindowFuncNode::GetType() const {
    return NodeType::WINDOW_FUNC_EXPR;
}

std::string WindowFuncNode::ToString() const {
    std::ostringstream oss;
    oss << function_name << "(";
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << (arguments[i] ? arguments[i]->ToString() : "?");
    }
    oss << ") OVER ";
    if (!window_name.empty()) {
        oss << window_name;
    } else {
        oss << "(";
        if (!spec.partition_by.empty()) {
            oss << "PARTITION BY ";
            for (size_t i = 0; i < spec.partition_by.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << (spec.partition_by[i] ? spec.partition_by[i]->ToString() : "?");
            }
            oss << " ";
        }
        if (!spec.order_by.empty()) {
            oss << "ORDER BY ";
            for (size_t i = 0; i < spec.order_by.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << (spec.order_by[i].expr ? spec.order_by[i].expr->ToString() : "?")
                    << (spec.order_by[i].ascending ? " ASC" : " DESC");
            }
        }
        oss << ")";
    }
    return oss.str();
}

// ============ SubqueryExprNode ============

SubqueryExprNode::SubqueryExprNode(SubqueryType kind,
                                    SelectStatementPtr subquery,
                                    std::string comparison_op,
                                    ExprPtr outer_expr)
    : kind(kind),
      subquery(std::move(subquery)),
      comparison_op(std::move(comparison_op)),
      outer_expr(std::move(outer_expr)) {
}

NodeType SubqueryExprNode::GetType() const {
    return NodeType::SUBQUERY_EXPR;
}

std::string SubqueryExprNode::ToString() const {
    std::ostringstream oss;
    if (kind == SubqueryType::EXISTS) {
        oss << "EXISTS(";
        if (subquery) oss << subquery->ToString();
        oss << ")";
    } else if (kind == SubqueryType::IN) {
        oss << "(" << (outer_expr ? outer_expr->ToString() : "?") << " IN (";
        if (subquery) oss << subquery->ToString();
        oss << "))";
    } else if (kind == SubqueryType::ANY) {
        oss << "(" << (outer_expr ? outer_expr->ToString() : "?")
            << " " << comparison_op << " ANY (";
        if (subquery) oss << subquery->ToString();
        oss << "))";
    } else {
        oss << "(";
        if (subquery) oss << subquery->ToString();
        oss << ")";
    }
    return oss.str();
}

// ============ UpsertValuesRefExpr (43_upsert) ============

UpsertValuesRefExpr::UpsertValuesRefExpr(std::string column_name)
    : column_name(std::move(column_name)) {
}

NodeType UpsertValuesRefExpr::GetType() const {
    return NodeType::UPSERT_VALUES_REF_EXPR;
}

std::string UpsertValuesRefExpr::ToString() const {
    return "VALUES(" + column_name + ")";
}

// ============ LikeExprNode (44_pattern_match) ============

LikeExprNode::LikeExprNode(Kind kind,
                           ExprPtr operand,
                           ExprPtr pattern,
                           char escape_char,
                           bool has_escape)
    : kind(kind),
      operand(std::move(operand)),
      pattern(std::move(pattern)),
      escape_char(escape_char),
      has_escape(has_escape) {
}

NodeType LikeExprNode::GetType() const {
    return NodeType::LIKE_EXPR;
}

std::string LikeExprNode::ToString() const {
    std::ostringstream oss;
    oss << "("
        << (operand ? operand->ToString() : "?") << " ";
    switch (kind) {
        case Kind::LIKE:   oss << "LIKE";   break;
        case Kind::ILIKE:  oss << "ILIKE";  break;
        case Kind::REGEXP: oss << "REGEXP"; break;
        case Kind::RLIKE:  oss << "RLIKE";  break;
    }
    oss << " " << (pattern ? pattern->ToString() : "?");
    if (has_escape) {
        oss << " ESCAPE '" << escape_char << "'";
    }
    oss << ")";
    return oss.str();
}

// ============ WithClauseStatement ============

WithClauseStatement::WithClauseStatement() {
}

NodeType WithClauseStatement::GetType() const {
    return NodeType::WITH_STMT;
}

std::string WithClauseStatement::ToString() const {
    std::ostringstream oss;
    oss << "WITH ";
    if (is_recursive) oss << "RECURSIVE ";
    for (size_t i = 0; i < ctes.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << ctes[i].cte_name << " AS (";
        if (ctes[i].cte_query) oss << ctes[i].cte_query->ToString();
        oss << ")";
    }
    if (body) oss << " " << body->ToString();
    return oss.str();
}

// ============ SetOperationStatement ============

SetOperationStatement::SetOperationStatement() {
}

NodeType SetOperationStatement::GetType() const {
    return NodeType::SET_OP_STMT;
}

std::string SetOperationStatement::ToString() const {
    std::ostringstream oss;
    if (left) oss << left->ToString() << " ";
    switch (kind) {
        case Kind::UNION:      oss << "UNION "; break;
        case Kind::UNION_ALL:  oss << "UNION ALL "; break;
        case Kind::INTERSECT:  oss << "INTERSECT "; break;
        case Kind::EXCEPT:     oss << "EXCEPT "; break;
    }
    if (right) oss << right->ToString();
    return oss.str();
}

// ============ 40_txn_view_udf：事务 / 视图 / 触发器 / UDF ============

BeginStatement::BeginStatement() {
}
NodeType BeginStatement::GetType() const { return NodeType::BEGIN_STMT; }
std::string BeginStatement::ToString() const { return "BEGIN"; }

CommitStatement::CommitStatement() {
}
NodeType CommitStatement::GetType() const { return NodeType::COMMIT_STMT; }
std::string CommitStatement::ToString() const { return "COMMIT"; }

RollbackStatement::RollbackStatement() {
}
NodeType RollbackStatement::GetType() const { return NodeType::ROLLBACK_STMT; }
std::string RollbackStatement::ToString() const { return "ROLLBACK"; }

RollbackToStatement::RollbackToStatement() = default;
NodeType RollbackToStatement::GetType() const { return NodeType::ROLLBACK_TO_STMT; }
std::string RollbackToStatement::ToString() const {
    return "ROLLBACK TO " + savepoint_name;
}

SavepointStatement::SavepointStatement(std::string name)
    : savepoint_name(std::move(name)) {
}
NodeType SavepointStatement::GetType() const { return NodeType::SAVEPOINT_STMT; }
std::string SavepointStatement::ToString() const {
    return "SAVEPOINT " + savepoint_name;
}

ReleaseSavepointStatement::ReleaseSavepointStatement(std::string name)
    : savepoint_name(std::move(name)) {
}
NodeType ReleaseSavepointStatement::GetType() const { return NodeType::RELEASE_SAVEPOINT_STMT; }
std::string ReleaseSavepointStatement::ToString() const {
    return "RELEASE SAVEPOINT " + savepoint_name;
}

CreateViewStatement::CreateViewStatement() {
}
NodeType CreateViewStatement::GetType() const { return NodeType::CREATE_VIEW_STMT; }
std::string CreateViewStatement::ToString() const {
    std::ostringstream oss;
    oss << "CREATE VIEW " << view_name << " AS ";
    if (query) oss << query->ToString();
    return oss.str();
}

DropViewStatement::DropViewStatement() {
}
NodeType DropViewStatement::GetType() const { return NodeType::DROP_VIEW_STMT; }
std::string DropViewStatement::ToString() const {
    return std::string("DROP VIEW ") + (if_exists ? "IF EXISTS " : "") + view_name;
}

CreateTriggerStatement::CreateTriggerStatement() {
}
NodeType CreateTriggerStatement::GetType() const { return NodeType::CREATE_TRIGGER_STMT; }
std::string CreateTriggerStatement::ToString() const {
    std::ostringstream oss;
    oss << "CREATE TRIGGER " << trigger_name << " "
        << (timing == TriggerTiming::BEFORE ? "BEFORE " : "AFTER ");
    switch (event) {
        case TriggerEvent::INSERT: oss << "INSERT "; break;
        case TriggerEvent::UPDATE: oss << "UPDATE "; break;
        case TriggerEvent::DELETE: oss << "DELETE "; break;
    }
    oss << "ON " << table_name << " FOR EACH ROW SET ";
    for (size_t i = 0; i < assignments.size(); ++i) {
        if (i) oss << ", ";
        oss << assignments[i].first << " = "
            << (assignments[i].second ? assignments[i].second->ToString() : "?");
    }
    return oss.str();
}

DropTriggerStatement::DropTriggerStatement() {
}
NodeType DropTriggerStatement::GetType() const { return NodeType::DROP_TRIGGER_STMT; }
std::string DropTriggerStatement::ToString() const {
    return std::string("DROP TRIGGER ") + (if_exists ? "IF EXISTS " : "") + trigger_name;
}

CreateFunctionStatement::CreateFunctionStatement() {
}
NodeType CreateFunctionStatement::GetType() const { return NodeType::CREATE_FUNCTION_STMT; }
std::string CreateFunctionStatement::ToString() const {
    std::ostringstream oss;
    oss << "CREATE FUNCTION " << function_name << "(";
    for (size_t i = 0; i < parameters.size(); ++i) {
        if (i) oss << ", ";
        oss << parameters[i].name << " " << parameters[i].data_type;
    }
    oss << ") RETURNS " << return_type << " BEGIN";
    for (const auto& s : body_statements) {
        if (s) oss << " " << s->ToString() << ";";
    }
    oss << " END";
    return oss.str();
}

// ============ 47_udf_trigger_view: UDF 函数体语句节点 ============

DeclareVarStatement::DeclareVarStatement(std::string var_name, std::string data_type,
                                         int32_t char_length)
    : var_name(std::move(var_name)), data_type(std::move(data_type)),
      char_length(char_length) {
}
NodeType DeclareVarStatement::GetType() const { return NodeType::DECLARE_VAR_STMT; }
std::string DeclareVarStatement::ToString() const {
    std::string out = "DECLARE " + var_name + " " + data_type;
    if (char_length > 0) out += "(" + std::to_string(char_length) + ")";
    return out;
}

SetVarStatement::SetVarStatement(std::string target, ExprPtr expr)
    : target(std::move(target)), expr(std::move(expr)) {
}
NodeType SetVarStatement::GetType() const { return NodeType::SET_VAR_STMT; }
std::string SetVarStatement::ToString() const {
    return "SET " + target + " = " + (expr ? expr->ToString() : "?");
}

IfStatement::IfStatement() {
}
NodeType IfStatement::GetType() const { return NodeType::IF_STMT; }
std::string IfStatement::ToString() const {
    std::ostringstream oss;
    oss << "IF " << (condition ? condition->ToString() : "?") << " THEN";
    for (const auto& s : then_body) {
        if (s) oss << " " << s->ToString() << ";";
    }
    for (const auto& ec : elseif_clauses) {
        oss << " ELSEIF " << (ec.condition ? ec.condition->ToString() : "?") << " THEN";
        for (const auto& s : ec.body) {
            if (s) oss << " " << s->ToString() << ";";
        }
    }
    if (!else_body.empty()) {
        oss << " ELSE";
        for (const auto& s : else_body) {
            if (s) oss << " " << s->ToString() << ";";
        }
    }
    oss << " END IF";
    return oss.str();
}

WhileStatement::WhileStatement() {
}
NodeType WhileStatement::GetType() const { return NodeType::WHILE_STMT; }
std::string WhileStatement::ToString() const {
    std::ostringstream oss;
    oss << "WHILE " << (condition ? condition->ToString() : "?") << " DO";
    for (const auto& s : body) {
        if (s) oss << " " << s->ToString() << ";";
    }
    oss << " END WHILE";
    return oss.str();
}

ReturnStatement::ReturnStatement(ExprPtr expr) : expr(std::move(expr)) {
}
NodeType ReturnStatement::GetType() const { return NodeType::RETURN_STMT; }
std::string ReturnStatement::ToString() const {
    if (expr) return "RETURN " + expr->ToString();
    return "RETURN";
}

DropFunctionStatement::DropFunctionStatement() {
}
NodeType DropFunctionStatement::GetType() const { return NodeType::DROP_FUNCTION_STMT; }
std::string DropFunctionStatement::ToString() const {
    return std::string("DROP FUNCTION ") + (if_exists ? "IF EXISTS " : "") + function_name;
}

// ============ 45_datetime：EXTRACT / INTERVAL ============

namespace {
const char* ExtractFieldName(int field) {
    // 与 IntervalUnit 对应；用整型值便于不引入对 DateTime.h 的依赖。
    switch (field) {
        case 0: return "YEAR";   // IntervalUnit::YEAR
        case 1: return "MONTH";  // IntervalUnit::MONTH
        case 2: return "DAY";    // IntervalUnit::DAY
        case 3: return "HOUR";   // IntervalUnit::HOUR
        case 4: return "MINUTE"; // IntervalUnit::MINUTE
        case 5: return "SECOND"; // IntervalUnit::SECOND
    }
    return "?";
}
}  // namespace

ExtractExprNode::ExtractExprNode(int field, ExprPtr source)
    : field(field), source(std::move(source)) {
}

NodeType ExtractExprNode::GetType() const {
    return NodeType::EXTRACT_EXPR;
}

std::string ExtractExprNode::ToString() const {
    std::ostringstream oss;
    oss << "EXTRACT(" << ExtractFieldName(field) << " FROM "
        << (source ? source->ToString() : "?") << ")";
    return oss.str();
}

IntervalExprNode::IntervalExprNode(int64_t count, int unit)
    : count(count), unit(unit) {
}

NodeType IntervalExprNode::GetType() const {
    return NodeType::INTERVAL_EXPR;
}

std::string IntervalExprNode::ToString() const {
    std::ostringstream oss;
    oss << "INTERVAL " << count << " " << ExtractFieldName(unit);
    return oss.str();
}

// ============ 46_meta：EXPLAIN / SHOW ============

ExplainStatement::ExplainStatement() {
}
NodeType ExplainStatement::GetType() const {
    return NodeType::EXPLAIN_STMT;
}
std::string ExplainStatement::ToString() const {
    std::ostringstream oss;
    oss << "EXPLAIN";
    if (analyze) oss << " ANALYZE";
    if (inner) oss << " " << inner->ToString();
    return oss.str();
}

ShowStatement::ShowStatement() {
}
NodeType ShowStatement::GetType() const {
    return NodeType::SHOW_STMT;
}
std::string ShowStatement::ToString() const {
    std::ostringstream oss;
    oss << "SHOW ";
    switch (kind) {
        case Kind::TABLES:       oss << "TABLES"; break;
        case Kind::COLUMNS:      oss << "COLUMNS FROM " << target_table; break;
        case Kind::INDEX:        oss << "INDEX FROM " << target_table; break;
        case Kind::CREATE_TABLE: oss << "CREATE TABLE " << target_table; break;
    }
    return oss.str();
}

}  // namespace sqlcompiler