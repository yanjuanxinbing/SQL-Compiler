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
        case NodeType::CREATE_INDEX_STMT:
            ok = AnalyzeCreateIndex(*std::static_pointer_cast<CreateIndexStatement>(statement));
            break;
        case NodeType::DROP_INDEX_STMT:
            ok = AnalyzeDropIndex(*std::static_pointer_cast<DropIndexStatement>(statement));
            break;
        case NodeType::TRUNCATE_TABLE_STMT:
            ok = AnalyzeTruncateTable(*std::static_pointer_cast<TruncateTableStatement>(statement));
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
    if (!stmt.from_table.empty()) {
        ok &= CheckTableExists(stmt.from_table);
    }

    // 收集所有真实表名（不含别名），用于 CheckColumnExists 跨表查找
    std::vector<std::string> real_tables;
    if (!stmt.from_table.empty()) real_tables.push_back(stmt.from_table);
    for (auto& j : stmt.joins) {
        ok &= CheckTableExists(j.table_name);
        real_tables.push_back(j.table_name);
    }
    // 没有 FROM 的查询（如 SELECT 1）：跳过 table/column 检查
    if (stmt.from_table.empty()) return ok;
    for (auto& e : stmt.select_list) ok &= CheckExpressionMulti(e, real_tables);
    if (stmt.where_clause) ok &= CheckExpressionMulti(stmt.where_clause, real_tables);
    for (auto& e : stmt.group_by) ok &= CheckExpressionMulti(e, real_tables);
    if (stmt.having_clause) ok &= CheckExpressionMulti(stmt.having_clause, real_tables);
    for (auto& it : stmt.order_by) ok &= CheckExpressionMulti(it.expr, real_tables);
    for (auto& j : stmt.joins) {
        if (j.on_condition) ok &= CheckExpressionMulti(j.on_condition, real_tables);
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
    if (symbol_table_.HasTable(stmt.table_name) && !stmt.if_not_exists) {
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
        // Validate type (支持 BIGINT/INTEGER/DOUBLE/DECIMAL/CHAR/TEXT/STRING 等全部归一化类型)
        std::string up;
        for (char c : stmt.columns[i].data_type)
            up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        if (up != "INT" && up != "INTEGER" && up != "BIGINT" &&
            up != "FLOAT" && up != "DOUBLE" && up != "DECIMAL" &&
            up != "VARCHAR" && up != "CHAR" && up != "TEXT" && up != "STRING") {
            AddError("unsupported column type: " + stmt.columns[i].data_type);
            ok = false;
        }
    }
    // 表级 PRIMARY KEY(a, b, ...) 引用的列必须存在于列定义中。
    for (const auto& pk : stmt.primary_keys) {
        for (const auto& pk_col : pk) {
            bool found = false;
            for (const auto& cd : stmt.columns) {
                if (cd.column_name == pk_col) { found = true; break; }
            }
            if (!found) {
                AddError("PRIMARY KEY references unknown column: " + pk_col);
                ok = false;
            }
        }
    }
    return ok;
}

bool SemanticAnalyzer::AnalyzeDropTable(const DropTableStatement& stmt) {
    // IF EXISTS 时不存在也不算错误，交由执行阶段静默跳过
    if (stmt.if_exists) return true;
    return CheckTableExists(stmt.table_name);
}

bool SemanticAnalyzer::AnalyzeCreateIndex(const CreateIndexStatement& stmt) {
    if (!CheckTableExists(stmt.table_name)) return false;
    const TableInfo* table = symbol_table_.GetTable(stmt.table_name);
    if (table == nullptr) return false;
    if (stmt.key_columns.empty()) {
        AddError("index must have at least one column");
        return false;
    }
    bool ok = true;
    for (const auto& col : stmt.key_columns) {
        if (table->GetColumn(col) == nullptr) {
            AddError("column not found: " + stmt.table_name + "." + col);
            ok = false;
        }
    }
    // 列的可索引性（长度上限、非空）留给执行阶段的 SystemCatalog::CreateIndex 统一
    // 判定，避免同一套规则在两处各写一遍而漂移。
    return ok;
}

bool SemanticAnalyzer::AnalyzeDropIndex(const DropIndexStatement& stmt) {
    // 索引是否存在只有 SystemCatalog 知道（SymbolTable 不持有索引元数据），
    // 因此存在性检查放在执行阶段。
    (void)stmt;
    return true;
}

bool SemanticAnalyzer::AnalyzeTruncateTable(const TruncateTableStatement& stmt) {
    return CheckTableExists(stmt.table_name);
}

bool SemanticAnalyzer::CheckTableExists(const std::string& table_name) {
    if (symbol_table_.HasTable(table_name)) return true;
    AddError("table not found: " + table_name);
    return false;
}

bool SemanticAnalyzer::CheckColumnExists(const std::string& table_name,
                                          const std::string& column_name) {
    // table_name may be a comma-separated list of table names (for JOIN).
    auto check_one = [&](const std::string& t) -> bool {
        const TableInfo* info = symbol_table_.GetTable(t);
        if (info && info->HasColumn(column_name)) return true;
        return false;
    };
    if (table_name.find(',') != std::string::npos) {
        size_t start = 0;
        while (start < table_name.size()) {
            size_t end = table_name.find(',', start);
            std::string t = table_name.substr(start,
                end == std::string::npos ? std::string::npos : end - start);
            if (check_one(t)) return true;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        AddError("column not found: " + column_name);
        return false;
    }
    if (check_one(table_name)) return true;
    AddError("column not found: " + table_name + "." + column_name);
    return false;
}


// 跨表列检查（支持表别名）：先按限定列名查指定表；否则在所有真实表中查找
bool SemanticAnalyzer::CheckExpressionMulti(const ExprPtr& expr,
                                          const std::vector<std::string>& tables) {
    if (!expr) return true;
    switch (expr->GetType()) {
        case NodeType::LITERAL_EXPR:
            return true;
        case NodeType::COLUMN_REF_EXPR: {
            auto cr = std::static_pointer_cast<ColumnRefExpr>(expr);
            // 对于限定列（table.col）：如果 cr->table_name 是某个真实表的别名或名字，能在任一表中找到该列即可
            for (const auto& t : tables) {
                const TableInfo* info = symbol_table_.GetTable(t);
                if (info && info->HasColumn(cr->column_name)) return true;
            }
            if (cr->table_name.empty()) {
                AddError("column not found: " + cr->column_name);
            }
            // qualified 但表名无法识别：容忍（交由执行器校验）
            return true;
        }
        case NodeType::BINARY_EXPR: {
            auto be = std::static_pointer_cast<BinaryExpr>(expr);
            return CheckExpressionMulti(be->left, tables) &&
                   CheckExpressionMulti(be->right, tables);
        }
        case NodeType::UNARY_EXPR: {
            auto ue = std::static_pointer_cast<UnaryExpr>(expr);
            return CheckExpressionMulti(ue->operand, tables);
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            auto fc = std::static_pointer_cast<FunctionCallExpr>(expr);
            bool ok = true;
            for (auto& a : fc->arguments) ok &= CheckExpressionMulti(a, tables);
            return ok;
        }
        default:
            return true;
    }
}

bool SemanticAnalyzer::CheckExpression(const ExprPtr& expr, const std::string& table_name) {
    if (!expr) return true;
    bool ok = true;
    switch (expr->GetType()) {
        case NodeType::LITERAL_EXPR:
            return true;
        case NodeType::COLUMN_REF_EXPR: {
            auto cr = std::static_pointer_cast<ColumnRefExpr>(expr);
            // If the column reference is qualified, verify the specific table.
            if (!cr->table_name.empty()) {
                ok &= CheckColumnExists(cr->table_name, cr->column_name);
            } else {
                ok &= CheckColumnExists(table_name, cr->column_name);
            }
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