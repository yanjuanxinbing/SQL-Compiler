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
    if (filter_expr) {
        oss << " FILTER (WHERE " << filter_expr->ToString() << ")";
    }
    if (!within_group_order_by.empty()) {
        oss << " WITHIN GROUP (ORDER BY ";
        for (size_t i = 0; i < within_group_order_by.size(); ++i) {
            if (i > 0) oss << ", ";
            const auto& ob = within_group_order_by[i];
            oss << (ob.expr ? ob.expr->ToString() : "?");
            if (!ob.ascending) oss << " DESC";
        }
        oss << ")";
    }
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
    if (!grouping_sets.empty()) {
        oss << " GROUPING SETS (";
        for (size_t g = 0; g < grouping_sets.size(); ++g) {
            if (g > 0) oss << ", ";
            oss << "(";
            for (size_t i = 0; i < grouping_sets[g].size(); ++i) {
                if (i > 0) oss << ", ";
                oss << (grouping_sets[g][i] ? grouping_sets[g][i]->ToString() : "");
            }
            oss << ")";
        }
        oss << ")";
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
        case AlterAction::RENAME_COLUMN:
            oss << "RENAME COLUMN " << rename_column_old_name
                << " TO " << rename_column_new_name;
            break;
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
    oss << ") ";
    oss << (ignore_nulls ? "IGNORE NULLS " : "RESPECT NULLS ");
    oss << "OVER ";
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
        case Kind::LIKE:        oss << "LIKE";        break;
        case Kind::ILIKE:       oss << "ILIKE";       break;
        case Kind::REGEXP:      oss << "REGEXP";      break;
        case Kind::RLIKE:       oss << "RLIKE";       break;
        case Kind::SIMILAR_TO:  oss << "SIMILAR TO";  break;
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

// 60_view_trigger (Category 9)
MaterializedViewStatement::MaterializedViewStatement() {
}
NodeType MaterializedViewStatement::GetType() const {
    return NodeType::CREATE_MATERIALIZED_VIEW_STMT;
}
std::string MaterializedViewStatement::ToString() const {
    std::ostringstream oss;
    oss << "CREATE MATERIALIZED VIEW " << view_name << " AS ";
    if (query) oss << query->ToString();
    return oss.str();
}

// 60_view_trigger (Category 9)
AlterMaterializedViewStatement::AlterMaterializedViewStatement() {
}
NodeType AlterMaterializedViewStatement::GetType() const {
    return NodeType::ALTER_MATERIALIZED_VIEW_STMT;
}
std::string AlterMaterializedViewStatement::ToString() const {
    return std::string("ALTER MATERIALIZED VIEW ") + view_name + " REFRESH";
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
    oss << "ON " << table_name << " FOR EACH "
        << (for_each_row ? "ROW " : "STATEMENT ") << "SET ";
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
    if (default_expr) out += " DEFAULT " + default_expr->ToString();
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

// ============ 59_procs (Category 8)：过程语言扩展语句 ============

LoopStatement::LoopStatement() {
}
NodeType LoopStatement::GetType() const { return NodeType::LOOP_STMT; }
std::string LoopStatement::ToString() const {
    std::ostringstream oss;
    if (!label.empty()) oss << label << ": ";
    oss << "LOOP";
    for (const auto& s : body) {
        if (s) oss << " " << s->ToString() << ";";
    }
    oss << " END LOOP";
    if (!label.empty()) oss << " " << label;
    return oss.str();
}

RepeatStatement::RepeatStatement() {
}
NodeType RepeatStatement::GetType() const { return NodeType::REPEAT_STMT; }
std::string RepeatStatement::ToString() const {
    std::ostringstream oss;
    if (!label.empty()) oss << label << ": ";
    oss << "REPEAT";
    for (const auto& s : body) {
        if (s) oss << " " << s->ToString() << ";";
    }
    oss << " UNTIL " << (until_expr ? until_expr->ToString() : "?")
        << " END REPEAT";
    if (!label.empty()) oss << " " << label;
    return oss.str();
}

CaseStatement::CaseStatement() {
}
NodeType CaseStatement::GetType() const { return NodeType::CASE_STMT; }
std::string CaseStatement::ToString() const {
    std::ostringstream oss;
    oss << "CASE";
    if (subject) oss << " " << subject->ToString();
    for (const auto& w : whens) {
        oss << " WHEN " << (w.when_expr ? w.when_expr->ToString() : "?") << " THEN";
        for (const auto& s : w.body) {
            if (s) oss << " " << s->ToString() << ";";
        }
    }
    if (!else_body.empty()) {
        oss << " ELSE";
        for (const auto& s : else_body) {
            if (s) oss << " " << s->ToString() << ";";
        }
    }
    oss << " END CASE";
    return oss.str();
}

LeaveStatement::LeaveStatement(std::string l) : label(std::move(l)) {
}
NodeType LeaveStatement::GetType() const { return NodeType::LEAVE_STMT; }
std::string LeaveStatement::ToString() const {
    return "LEAVE " + label;
}

IterateStatement::IterateStatement(std::string l) : label(std::move(l)) {
}
NodeType IterateStatement::GetType() const { return NodeType::ITERATE_STMT; }
std::string IterateStatement::ToString() const {
    return "ITERATE " + label;
}

SignalStatement::SignalStatement() {
}
NodeType SignalStatement::GetType() const { return NodeType::SIGNAL_STMT; }
std::string SignalStatement::ToString() const {
    std::ostringstream oss;
    oss << "SIGNAL SQLSTATE '" << sqlstate << "' SET MESSAGE_TEXT = '"
        << message_text << "'";
    return oss.str();
}

DeclareHandlerStatement::DeclareHandlerStatement() {
}
NodeType DeclareHandlerStatement::GetType() const { return NodeType::DECLARE_HANDLER_STMT; }
std::string DeclareHandlerStatement::ToString() const {
    std::ostringstream oss;
    oss << "DECLARE ";
    switch (type) {
        case Type::CONTINUE: oss << "CONTINUE "; break;
        case Type::EXIT:     oss << "EXIT ";     break;
        case Type::UNDO:     oss << "UNDO ";     break;
    }
    oss << "HANDLER FOR ";
    switch (cond_kind) {
        case CondKind::SQLEXCEPTION: oss << "SQLEXCEPTION"; break;
        case CondKind::SQLWARNING:   oss << "SQLWARNING";   break;
        case CondKind::NOT_FOUND:    oss << "NOT FOUND";    break;
        case CondKind::SQLSTATE:     oss << "SQLSTATE '" << cond_sqlstate << "'"; break;
    }
    oss << " " << (body ? body->ToString() : ";");
    return oss.str();
}

DeclareCursorStatement::DeclareCursorStatement(std::string cursor_name, SelectStatementPtr q)
    : cursor_name(std::move(cursor_name)), query(std::move(q)) {
}
NodeType DeclareCursorStatement::GetType() const { return NodeType::DECLARE_CURSOR_STMT; }
std::string DeclareCursorStatement::ToString() const {
    std::ostringstream oss;
    oss << "DECLARE " << cursor_name << " CURSOR FOR "
        << (query ? query->ToString() : "<null>");
    return oss.str();
}

CursorOpenStatement::CursorOpenStatement(std::string cursor_name)
    : cursor_name(std::move(cursor_name)) {
}
NodeType CursorOpenStatement::GetType() const { return NodeType::CURSOR_OPEN_STMT; }
std::string CursorOpenStatement::ToString() const {
    return "OPEN " + cursor_name;
}

CursorFetchStatement::CursorFetchStatement(std::string cursor_name,
                                           std::vector<std::string> into_vars)
    : cursor_name(std::move(cursor_name)), into_vars(std::move(into_vars)) {
}
NodeType CursorFetchStatement::GetType() const { return NodeType::CURSOR_FETCH_STMT; }
std::string CursorFetchStatement::ToString() const {
    std::ostringstream oss;
    oss << "FETCH " << cursor_name << " INTO";
    for (size_t i = 0; i < into_vars.size(); ++i) {
        if (i) oss << ",";
        oss << " " << into_vars[i];
    }
    return oss.str();
}

CursorCloseStatement::CursorCloseStatement(std::string cursor_name)
    : cursor_name(std::move(cursor_name)) {
}
NodeType CursorCloseStatement::GetType() const { return NodeType::CURSOR_CLOSE_STMT; }
std::string CursorCloseStatement::ToString() const {
    return "CLOSE " + cursor_name;
}

// ============ 59_procs (Category 8)：PROCEDURE / CALL ============

CreateProcedureStatement::CreateProcedureStatement() {
}
NodeType CreateProcedureStatement::GetType() const { return NodeType::CREATE_PROCEDURE_STMT; }
std::string CreateProcedureStatement::ToString() const {
    std::ostringstream oss;
    oss << "CREATE PROCEDURE " << procedure_name << "(";
    for (size_t i = 0; i < parameters.size(); ++i) {
        if (i) oss << ", ";
        switch (parameters[i].mode) {
            case 0: break;
            case 1: oss << "OUT "; break;
            case 2: oss << "INOUT "; break;
        }
        oss << parameters[i].name << " " << parameters[i].data_type;
    }
    oss << ") BEGIN ... END";
    return oss.str();
}

DropProcedureStatement::DropProcedureStatement() {
}
NodeType DropProcedureStatement::GetType() const { return NodeType::DROP_PROCEDURE_STMT; }
std::string DropProcedureStatement::ToString() const {
    return std::string("DROP PROCEDURE ") + (if_exists ? "IF EXISTS " : "") +
           procedure_name;
}

CallStatement::CallStatement() {
}
NodeType CallStatement::GetType() const { return NodeType::CALL_STMT; }
std::string CallStatement::ToString() const {
    std::ostringstream oss;
    oss << "CALL " << procedure_name << "(";
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (i) oss << ", ";
        oss << (arguments[i] ? arguments[i]->ToString() : "?");
    }
    oss << ")";
    return oss.str();
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

// ============ 53_ddl: SCHEMA / SEQUENCE / NEXTVAL ============

CreateSchemaStatement::CreateSchemaStatement() = default;
CreateSchemaStatement::CreateSchemaStatement(std::string name)
    : schema_name(std::move(name)) {}

NodeType CreateSchemaStatement::GetType() const {
    return NodeType::CREATE_SCHEMA_STMT;
}
std::string CreateSchemaStatement::ToString() const {
    std::ostringstream oss;
    oss << "CREATE SCHEMA ";
    if (if_not_exists) oss << "IF NOT EXISTS ";
    oss << schema_name;
    return oss.str();
}

DropSchemaStatement::DropSchemaStatement() = default;
DropSchemaStatement::DropSchemaStatement(std::string name)
    : schema_name(std::move(name)) {}

NodeType DropSchemaStatement::GetType() const {
    return NodeType::DROP_SCHEMA_STMT;
}
std::string DropSchemaStatement::ToString() const {
    std::ostringstream oss;
    oss << "DROP SCHEMA ";
    if (if_exists) oss << "IF EXISTS ";
    oss << schema_name;
    return oss.str();
}

CreateSequenceStatement::CreateSequenceStatement() = default;

NodeType CreateSequenceStatement::GetType() const {
    return NodeType::CREATE_SEQUENCE_STMT;
}
std::string CreateSequenceStatement::ToString() const {
    std::ostringstream oss;
    oss << "CREATE SEQUENCE ";
    if (if_not_exists) oss << "IF NOT EXISTS ";
    oss << sequence_name << " START " << start_value
        << " INCREMENT " << increment;
    return oss.str();
}

DropSequenceStatement::DropSequenceStatement() = default;
DropSequenceStatement::DropSequenceStatement(std::string name)
    : sequence_name(std::move(name)) {}

NodeType DropSequenceStatement::GetType() const {
    return NodeType::DROP_SEQUENCE_STMT;
}
std::string DropSequenceStatement::ToString() const {
    std::ostringstream oss;
    oss << "DROP SEQUENCE ";
    if (if_exists) oss << "IF EXISTS ";
    oss << sequence_name;
    return oss.str();
}

NextvalExpr::NextvalExpr(std::string sequence_name)
    : sequence_name(std::move(sequence_name)) {}

NodeType NextvalExpr::GetType() const {
    return NodeType::NEXTVAL_EXPR;
}
std::string NextvalExpr::ToString() const {
    return "NEXTVAL FOR " + sequence_name;
}

// ============ 54_dml：MERGE 语句 ============

MergeStatement::MergeStatement() {
}

NodeType MergeStatement::GetType() const {
    return NodeType::MERGE_STMT;
}

std::string MergeStatement::ToString() const {
    std::ostringstream oss;
    oss << "MERGE INTO " << target_table;
    if (!target_alias.empty()) oss << " AS " << target_alias;
    oss << " USING ";
    if (source_query) {
        oss << "(" << source_query->ToString() << ")";
    } else {
        oss << source_table;
    }
    if (!source_alias.empty()) oss << " AS " << source_alias;
    oss << " ON " << (on_condition ? on_condition->ToString() : "?");
    if (has_matched_update) {
        oss << " WHEN MATCHED THEN UPDATE SET ";
        for (size_t i = 0; i < matched_assignments.size(); ++i) {
            if (i) oss << ", ";
            oss << matched_assignments[i].first << " = "
                << (matched_assignments[i].second
                        ? matched_assignments[i].second->ToString()
                        : "?");
        }
    }
    if (has_not_matched_insert) {
        oss << " WHEN NOT MATCHED THEN INSERT (";
        for (size_t i = 0; i < insert_columns.size(); ++i) {
            if (i) oss << ", ";
            oss << insert_columns[i];
        }
        oss << ") VALUES (";
        for (size_t i = 0; i < insert_values.size(); ++i) {
            if (i) oss << ", ";
            oss << (insert_values[i] ? insert_values[i]->ToString() : "?");
        }
        oss << ")";
    }
    return oss.str();
}

}  // namespace sqlcompiler