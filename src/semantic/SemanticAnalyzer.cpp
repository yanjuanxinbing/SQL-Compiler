#include "semantic/SemanticAnalyzer.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <vector>

namespace sqlcompiler {

namespace {

// SQL 关键字大小写不敏感的等价比较
bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// 当前实现支持的数据类型白名单
bool IsValidDataType(const std::string& data_type) {
    if (data_type.empty()) return false;
    // 形式化为 "<BASE>" 或 "<BASE>(n)"，例如 VARCHAR(255)
    std::string base;
    for (char c : data_type) {
        if (c == '(') break;
        base.push_back(c);
    }
    return EqualsIgnoreCase(base, "INT") ||
           EqualsIgnoreCase(base, "INTEGER") ||
           EqualsIgnoreCase(base, "FLOAT") ||
           EqualsIgnoreCase(base, "DOUBLE") ||
           EqualsIgnoreCase(base, "VARCHAR") ||
           EqualsIgnoreCase(base, "CHAR") ||
           EqualsIgnoreCase(base, "TEXT") ||
           EqualsIgnoreCase(base, "BOOLEAN") ||
           EqualsIgnoreCase(base, "BOOL") ||
           EqualsIgnoreCase(base, "DATE");
}

}  // namespace

SemanticAnalyzer::SemanticAnalyzer(SymbolTable& symbol_table)
    : symbol_table_(symbol_table) {}

bool SemanticAnalyzer::Analyze(const StatementPtr& statement) {
    ClearErrors();
    if (!statement) {
        AddError("Null statement");
        return false;
    }
    switch (statement->GetType()) {
        case NodeType::SELECT_STMT:
            return AnalyzeSelect(*static_cast<const SelectStatement*>(statement.get()));
        case NodeType::INSERT_STMT:
            return AnalyzeInsert(*static_cast<const InsertStatement*>(statement.get()));
        case NodeType::UPDATE_STMT:
            return AnalyzeUpdate(*static_cast<const UpdateStatement*>(statement.get()));
        case NodeType::DELETE_STMT:
            return AnalyzeDelete(*static_cast<const DeleteStatement*>(statement.get()));
        case NodeType::CREATE_TABLE_STMT:
            return AnalyzeCreateTable(*static_cast<const CreateTableStatement*>(statement.get()));
        case NodeType::DROP_TABLE_STMT:
            return AnalyzeDropTable(*static_cast<const DropTableStatement*>(statement.get()));
        default:
            AddError("Unsupported statement type in semantic analysis");
            return false;
    }
}

const std::vector<SemanticError>& SemanticAnalyzer::GetErrors() const {
    return errors_;
}

void SemanticAnalyzer::ClearErrors() {
    errors_.clear();
}

// ============ 各语句分析 ============

bool SemanticAnalyzer::AnalyzeSelect(const SelectStatement& stmt) {
    bool ok = true;

    // 主表必须存在
    if (!CheckTableExists(stmt.from_table)) {
        ok = false;
    }

    // 收集本次查询可见的所有表（FROM + JOINs），用于后续解析列引用
    std::vector<std::string> visible_tables;
    if (!stmt.from_table.empty()) {
        visible_tables.push_back(stmt.from_table);
    }
    for (const auto& j : stmt.joins) {
        if (!CheckTableExists(j.table_name)) {
            ok = false;
        } else {
            visible_tables.push_back(j.table_name);
        }
        if (j.on_condition) {
            if (!CheckExpressionInScope(j.on_condition, visible_tables)) {
                ok = false;
            }
        } else {
            AddError("JOIN clause missing ON condition for table '" + j.table_name + "'");
            ok = false;
        }
    }

    // 在 SELECT 作用域内做表达式检查（多表）
    auto check_in_scope = [&](const ExprPtr& e) -> bool {
        return CheckExpressionInScope(e, visible_tables);
    };

    for (const auto& e : stmt.select_list) {
        if (!check_in_scope(e)) ok = false;
    }
    if (stmt.where_clause && !check_in_scope(stmt.where_clause)) ok = false;
    for (const auto& e : stmt.group_by) {
        if (!check_in_scope(e)) ok = false;
    }
    if (stmt.having_clause && !check_in_scope(stmt.having_clause)) ok = false;
    for (const auto& ob : stmt.order_by) {
        if (ob.expr && !check_in_scope(ob.expr)) ok = false;
    }
    return ok;
}

bool SemanticAnalyzer::AnalyzeInsert(const InsertStatement& stmt) {
    bool ok = true;
    if (!CheckTableExists(stmt.table_name)) return false;

    const TableInfo* table = symbol_table_.GetTable(stmt.table_name);
    // 表已经存在，前面 CheckTableExists 通过，所以 table 不为 nullptr
    const std::size_t expected_cols =
        stmt.columns.empty() ? table->columns.size() : stmt.columns.size();

    // 显式列名：必须都存在于表中
    for (const auto& col : stmt.columns) {
        if (!table->HasColumn(col)) {
            AddError("Column '" + col + "' does not exist in table '" + stmt.table_name + "'");
            ok = false;
        }
    }

    // VALUES 行的列数必须一致
    for (std::size_t r = 0; r < stmt.values_list.size(); ++r) {
        if (stmt.values_list[r].size() != expected_cols) {
            AddError("INSERT row " + std::to_string(r + 1) +
                     " has " + std::to_string(stmt.values_list[r].size()) +
                     " values, expected " + std::to_string(expected_cols));
            ok = false;
        }
    }
    return ok;
}

bool SemanticAnalyzer::AnalyzeUpdate(const UpdateStatement& stmt) {
    bool ok = true;
    if (!CheckTableExists(stmt.table_name)) return false;

    std::vector<std::string> scope;
    if (!stmt.table_name.empty()) scope.push_back(stmt.table_name);

    for (const auto& assign : stmt.assignments) {
        if (!CheckColumnExists(stmt.table_name, assign.first)) {
            ok = false;
        }
        if (assign.second && !CheckExpressionInScope(assign.second, scope)) {
            ok = false;
        }
    }
    if (stmt.where_clause && !CheckExpressionInScope(stmt.where_clause, scope)) {
        ok = false;
    }
    return ok;
}

bool SemanticAnalyzer::AnalyzeDelete(const DeleteStatement& stmt) {
    bool ok = CheckTableExists(stmt.table_name);
    std::vector<std::string> scope;
    if (!stmt.table_name.empty()) scope.push_back(stmt.table_name);
    if (stmt.where_clause && !CheckExpressionInScope(stmt.where_clause, scope)) {
        ok = false;
    }
    return ok;
}

bool SemanticAnalyzer::AnalyzeCreateTable(const CreateTableStatement& stmt) {
    bool ok = true;

    if (stmt.table_name.empty()) {
        AddError("CREATE TABLE missing table name");
        ok = false;
    } else if (symbol_table_.HasTable(stmt.table_name)) {
        AddError("Table '" + stmt.table_name + "' already exists");
        ok = false;
    }

    if (stmt.columns.empty()) {
        AddError("CREATE TABLE '" + stmt.table_name + "' has no column definitions");
        ok = false;
    }

    std::unordered_set<std::string> seen;
    int pk_count = 0;
    for (const auto& col : stmt.columns) {
        if (col.column_name.empty()) {
            AddError("CREATE TABLE column missing name");
            ok = false;
            continue;
        }
        if (!seen.insert(col.column_name).second) {
            AddError("Duplicate column name '" + col.column_name +
                     "' in CREATE TABLE '" + stmt.table_name + "'");
            ok = false;
        }
        if (!IsValidDataType(col.data_type)) {
            AddError("Invalid data type '" + col.data_type +
                     "' for column '" + col.column_name + "'");
            ok = false;
        }
        if (col.is_primary_key) ++pk_count;
    }
    if (pk_count > 1) {
        AddError("Multiple PRIMARY KEY definitions in table '" + stmt.table_name + "'");
        ok = false;
    }

    // 通过校验后才把表写进目录，方便后续语句引用
    if (ok) {
        symbol_table_.AddTableFromCreateStatement(stmt);
    }
    return ok;
}

bool SemanticAnalyzer::AnalyzeDropTable(const DropTableStatement& stmt) {
    return CheckTableExists(stmt.table_name);
}

// ============ 通用检查 ============

bool SemanticAnalyzer::CheckTableExists(const std::string& table_name) {
    if (symbol_table_.HasTable(table_name)) return true;
    AddError("Table '" + table_name + "' does not exist");
    return false;
}

bool SemanticAnalyzer::CheckColumnExists(const std::string& table_name,
                                          const std::string& column_name) {
    const TableInfo* table = symbol_table_.GetTable(table_name);
    if (!table) {
        AddError("Table '" + table_name + "' does not exist");
        return false;
    }
    if (table->HasColumn(column_name)) return true;
    AddError("Column '" + column_name + "' does not exist in table '" + table_name + "'");
    return false;
}

namespace {

// 用一组可选的可见表查找列：限定列在指定表中找，裸列在任意可见表中找到即可
bool ResolveColumn(const ColumnRefExpr& cref,
                   const std::vector<std::string>& visible_tables,
                   const SymbolTable& catalog) {
    if (!cref.table_name.empty()) {
        const TableInfo* t = catalog.GetTable(cref.table_name);
        if (!t) return false;
        return t->HasColumn(cref.column_name);
    }
    // 裸列：在任意一张可见表中存在即可
    for (const auto& tn : visible_tables) {
        const TableInfo* t = catalog.GetTable(tn);
        if (t && t->HasColumn(cref.column_name)) return true;
    }
    return false;
}

std::string ColumnRefDescription(const ColumnRefExpr& cref) {
    if (cref.table_name.empty()) return cref.column_name;
    return cref.table_name + "." + cref.column_name;
}

}  // namespace

bool SemanticAnalyzer::CheckExpressionInScope(
        const ExprPtr& expr, const std::vector<std::string>& visible_tables) {
    if (!expr) return true;  // 空表达式视为合法（外层已决定是否允许）
    switch (expr->GetType()) {
        case NodeType::LITERAL_EXPR:
            return true;

        case NodeType::COLUMN_REF_EXPR: {
            const auto& cref = static_cast<const ColumnRefExpr&>(*expr);
            // "*" 表示全列占位，由上层处理；这里直接放行
            if (cref.column_name == "*") return true;

            // 如果限定到具体表：直接查该表；否则只在可见表范围内匹配
            if (!cref.table_name.empty()) {
                if (CheckColumnExists(cref.table_name, cref.column_name)) return true;
                AddError("Unknown column reference '" + ColumnRefDescription(cref) + "'");
                return false;
            }
            if (ResolveColumn(cref, visible_tables, symbol_table_)) return true;
            AddError("Unknown column '" + cref.column_name +
                     "' (no table in scope contains it)");
            return false;
        }

        case NodeType::BINARY_EXPR: {
            const auto& be = static_cast<const BinaryExpr&>(*expr);
            bool ok = true;
            if (!CheckExpressionInScope(be.left,  visible_tables)) ok = false;
            if (!CheckExpressionInScope(be.right, visible_tables)) ok = false;
            return ok;
        }

        case NodeType::UNARY_EXPR: {
            const auto& ue = static_cast<const UnaryExpr&>(*expr);
            return CheckExpressionInScope(ue.operand, visible_tables);
        }

        case NodeType::FUNCTION_CALL_EXPR: {
            const auto& fc = static_cast<const FunctionCallExpr&>(*expr);
            // COUNT(*) 形式的参数列表会含一个 ColumnRefExpr("", "*")，已在 COL_REF 分支放行
            for (const auto& a : fc.arguments) {
                if (!CheckExpressionInScope(a, visible_tables)) return false;
            }
            return true;
        }

        default:
            AddError("Unsupported expression node in semantic check");
            return false;
    }
}

bool SemanticAnalyzer::CheckExpression(const ExprPtr& expr,
                                       const std::string& table_name) {
    std::vector<std::string> tables;
    if (!table_name.empty()) tables.push_back(table_name);
    return CheckExpressionInScope(expr, tables);
}

void SemanticAnalyzer::AddError(const std::string& message, int line) {
    errors_.push_back({message, line});
}

}  // namespace sqlcompiler
