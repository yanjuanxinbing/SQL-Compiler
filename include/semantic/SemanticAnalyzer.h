#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/AST.h"
#include "semantic/SymbolTable.h"

namespace sqlcompiler {

// 语义错误描述
struct SemanticError {
    std::string message;
    int line = -1;
};

// 语义分析器：在AST上进行表/列存在性检查、类型检查等
class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(SymbolTable& symbol_table);

    // 分析入口，返回是否通过语义检查
    bool Analyze(const StatementPtr& statement);

    // 获取分析过程中收集到的所有错误
    const std::vector<SemanticError>& GetErrors() const;

    // 清空错误列表，便于复用同一个分析器实例
    void ClearErrors();

private:
    SymbolTable& symbol_table_;
    std::vector<SemanticError> errors_;

    // ---- 各语句类型的语义检查 ----
    bool AnalyzeSelect(const SelectStatement& stmt);
    bool AnalyzeInsert(const InsertStatement& stmt);
    bool AnalyzeUpdate(const UpdateStatement& stmt);
    bool AnalyzeDelete(const DeleteStatement& stmt);
    bool AnalyzeCreateTable(const CreateTableStatement& stmt);
    bool AnalyzeDropTable(const DropTableStatement& stmt);
    bool AnalyzeTruncateTable(const TruncateTableStatement& stmt);

    // ---- 通用检查函数 ----
    bool CheckTableExists(const std::string& table_name);
    bool CheckColumnExists(const std::string& table_name, const std::string& column_name);
    bool CheckExpression(const ExprPtr& expr, const std::string& table_name);
    bool CheckExpressionMulti(const ExprPtr& expr, const std::vector<std::string>& tables);

    void AddError(const std::string& message, int line = -1);
};

}  // namespace sqlcompiler
