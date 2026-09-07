#include "semantic/SemanticAnalyzer.h"

namespace sqlcompiler {

SemanticAnalyzer::SemanticAnalyzer(SymbolTable& symbol_table) : symbol_table_(symbol_table) {
    // TODO: 如有需要可补充初始化逻辑
}

bool SemanticAnalyzer::Analyze(const StatementPtr& statement) {
    // TODO: 根据statement->GetType()分派到具体的Analyze*函数
    return false;
}

const std::vector<SemanticError>& SemanticAnalyzer::GetErrors() const {
    // TODO: 返回errors_
    return errors_;
}

void SemanticAnalyzer::ClearErrors() {
    // TODO: 清空errors_
}

bool SemanticAnalyzer::AnalyzeSelect(const SelectStatement& stmt) {
    // TODO:
    // 1. CheckTableExists(stmt.from_table)
    // 2. 对select_list / where_clause / group_by / having_clause / order_by 中的表达式
    //    调用CheckExpression()进行列存在性检查
    // 3. 对joins中的每个JoinClause检查表是否存在、ON条件是否合法
    return false;
}

bool SemanticAnalyzer::AnalyzeInsert(const InsertStatement& stmt) {
    // TODO:
    // 1. CheckTableExists(stmt.table_name)
    // 2. 检查columns是否都存在于表中
    // 3. 检查values_list中每一行的列数是否与columns（或表定义）数量一致
    return false;
}

bool SemanticAnalyzer::AnalyzeUpdate(const UpdateStatement& stmt) {
    // TODO:
    // 1. CheckTableExists(stmt.table_name)
    // 2. 检查assignments中的每个列是否存在
    // 3. 检查where_clause（若存在）
    return false;
}

bool SemanticAnalyzer::AnalyzeDelete(const DeleteStatement& stmt) {
    // TODO:
    // 1. CheckTableExists(stmt.table_name)
    // 2. 检查where_clause（若存在）
    return false;
}

bool SemanticAnalyzer::AnalyzeCreateTable(const CreateTableStatement& stmt) {
    // TODO:
    // 1. 检查表名是否已存在（不应重复建表）
    // 2. 检查列名是否重复、数据类型是否合法
    return false;
}

bool SemanticAnalyzer::AnalyzeDropTable(const DropTableStatement& stmt) {
    // TODO: CheckTableExists(stmt.table_name)
    return false;
}

bool SemanticAnalyzer::CheckTableExists(const std::string& table_name) {
    // TODO: 调用symbol_table_.HasTable()，若不存在则AddError()并返回false
    return false;
}

bool SemanticAnalyzer::CheckColumnExists(const std::string& table_name,
                                          const std::string& column_name) {
    // TODO: 结合symbol_table_查找表结构，判断列是否存在，不存在则AddError()
    return false;
}

bool SemanticAnalyzer::CheckExpression(const ExprPtr& expr, const std::string& table_name) {
    // TODO: 递归遍历表达式树（LiteralExpr/ColumnRefExpr/BinaryExpr/UnaryExpr/FunctionCallExpr），
    // 对ColumnRefExpr调用CheckColumnExists()
    return false;
}

void SemanticAnalyzer::AddError(const std::string& message, int line) {
    // TODO: 构造SemanticError并追加到errors_
}

}  // namespace sqlcompiler
