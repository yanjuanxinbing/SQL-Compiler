#include "semantic/SemanticAnalyzer.h"

#include <utility>

namespace sqlcompiler {

SemanticAnalyzer::SemanticAnalyzer(SymbolTable& symbol_table) : symbol_table_(symbol_table) {
}

bool SemanticAnalyzer::Analyze(const StatementPtr& statement) {
    ClearErrors();
    if (!statement) {
        AddError("null statement");
        return false;
    }
    bool ok = false;
    switch (statement->GetType()) {
        case NodeType::SELECT_STMT:
            ok = AnalyzeSelect(*std::static_pointer_cast<SelectStatement>(statement));
            break;
        case NodeType::INSERT_STMT:
            ok = AnalyzeInsert(*std::static_pointer_cast<InsertStatement>(statement));
            break;
        case NodeType::UPDATE_STMT:
            ok = AnalyzeUpdate(*std::static_pointer_cast<UpdateStatement>(statement));
            break;
        case NodeType::DELETE_STMT:
            ok = AnalyzeDelete(*std::static_pointer_cast<DeleteStatement>(statement));
            break;
        case NodeType::CREATE_TABLE_STMT:
            ok = AnalyzeCreateTable(*std::static_pointer_cast<CreateTableStatement>(statement));
            break;
        case NodeType::DROP_TABLE_STMT:
            ok = AnalyzeDropTable(*std::static_pointer_cast<DropTableStatement>(statement));
            break;
        default:
            AddError("unsupported statement type");
            ok = false;
    }
    return ok && errors_.empty();
}

const std::vector<SemanticError>& SemanticAnalyzer::GetErrors() const {
    return errors_;
}

void SemanticAnalyzer::ClearErrors() {
    errors_.clear();
}

bool SemanticAnalyzer::AnalyzeSelect(const SelectStatement& stmt) {
    bool ok = true;
    ok &= CheckTableExists(stmt.from_table);
    for (auto& e : stmt.select_list) ok &= CheckExpression(e, stmt.from_table);
    if (stmt.where_clause) ok &= CheckExpression(stmt.where_clause, stmt.from_table);
    for (auto& e : stmt.group_by) ok &= CheckExpression(e, stmt.from_table);
    if (stmt.having_clause) ok &= CheckExpression(stmt.having_clause, stmt.from_table);
    for (auto& it : stmt.order_by) ok &= CheckExpression(it.expr, stmt.from_table);
    for (auto& j : stmt.joins) {
        ok &= CheckTableExists(j.table_name);
        if (j.on_condition) ok &= CheckExpression(j.on_condition, stmt.from_table);
    }
    return ok;
}

bool SemanticAnalyzer::AnalyzeInsert(const InsertStatement& stmt) {
    bool ok = CheckTableExists(stmt.table_name);
    const TableInfo* info = symbol_table_.GetTable(stmt.table_name);
    if (!info) return false;
    int expected = static_cast<int>(info->columns.size());
    for (auto& cn : stmt.columns) {
        ok &= CheckColumnExists(stmt.table_name, cn);
    }
    int actual_cols = static_cast<int>(stmt.columns.size());
    if (actual_cols == 0) actual_cols = expected;
    for (auto& row : stmt.values_list) {
        if (static_cast<int>(row.size()) != actual_cols) {
            AddError("INSERT column count mismatch: got " +
                std::to_string(row.size()) + " expected " +
                std::to_string(actual_cols));
            ok = false;
        }
    }
    return ok;
}

bool SemanticAnalyzer::AnalyzeUpdate(const UpdateStatement& stmt) {
    bool ok = CheckTableExists(stmt.table_name);
    for (auto& kv : stmt.assignments) {
        ok &= CheckColumnExists(stmt.table_name, kv.first);
        ok &= CheckExpression(kv.second, stmt.table_name);
    }
    if (stmt.where_clause) ok &= CheckExpression(stmt.where_clause, stmt.table_name);
    return ok;
}

bool SemanticAnalyzer::AnalyzeDelete(const DeleteStatement& stmt) {
    bool ok = CheckTableExists(stmt.table_name);
    if (stmt.where_clause) ok &= CheckExpression(stmt.where_clause, stmt.table_name);
    return ok;
}

bool SemanticAnalyzer::AnalyzeCreateTable(const CreateTableStatement& stmt) {
    bool ok = true;
    if (symbol_table_.HasTable(stmt.table_name)) {
        AddError("table already exists: " + stmt.table_name);
        ok = false;
    }
    // Check duplicate column names
    for (size_t i = 0; i < stmt.columns.size(); ++i) {
        for (size_t j = i + 1; j < stmt.columns.size(); ++j) {
            const auto& a = stmt.columns[i].column_name;
            const auto& b = stmt.columns[j].column_name;
            std::string ua, ub;
            for (char c : a) ua.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            for (char c : b) ub.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            if (ua == ub) {
                AddError("duplicate column name: " + a);
                ok = false;
            }
        }
        // Validate type
        if (stmt.columns[i].data_type != "INT" &&
            stmt.columns[i].data_type != "FLOAT" &&
            stmt.columns[i].data_type != "VARCHAR") {
            AddError("unsupported column type: " + stmt.columns[i].data_type);
            ok = false;
        }
    }
    return ok;
}

bool SemanticAnalyzer::AnalyzeDropTable(const DropTableStatement& stmt) {
    return CheckTableExists(stmt.table_name);
}

bool SemanticAnalyzer::CheckTableExists(const std::string& table_name) {
    if (symbol_table_.HasTable(table_name)) return true;
    AddError("table not found: " + table_name);
    return false;
}

bool SemanticAnalyzer::CheckColumnExists(const std::string& table_name,
                                          const std::string& column_name) {
    const TableInfo* info = symbol_table_.GetTable(table_name);
    if (!info) {
        AddError("table not found: " + table_name);
        return false;
    }
    if (info->HasColumn(column_name)) return true;
    AddError("column not found: " + table_name + "." + column_name);
    return false;
}

bool SemanticAnalyzer::CheckExpression(const ExprPtr& expr, const std::string& table_name) {
    if (!expr) return true;
    bool ok = true;
    switch (expr->GetType()) {
        case NodeType::LITERAL_EXPR:
            return true;
        case NodeType::COLUMN_REF_EXPR: {
            auto cr = std::static_pointer_cast<ColumnRefExpr>(expr);
            ok &= CheckColumnExists(table_name, cr->column_name);
            return ok;
        }
        case NodeType::BINARY_EXPR: {
            auto be = std::static_pointer_cast<BinaryExpr>(expr);
            ok &= CheckExpression(be->left, table_name);
            ok &= CheckExpression(be->right, table_name);
            return ok;
        }
        case NodeType::UNARY_EXPR: {
            auto ue = std::static_pointer_cast<UnaryExpr>(expr);
            return CheckExpression(ue->operand, table_name);
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            auto fc = std::static_pointer_cast<FunctionCallExpr>(expr);
            for (auto& a : fc->arguments) ok &= CheckExpression(a, table_name);
            return ok;
        }
        default:
            return true;
    }
}

void SemanticAnalyzer::AddError(const std::string& message, int line) {
    SemanticError err;
    err.message = message;
    err.line = line;
    errors_.push_back(std::move(err));
}

}  // namespace sqlcompiler