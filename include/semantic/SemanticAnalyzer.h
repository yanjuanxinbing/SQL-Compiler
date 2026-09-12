#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/AST.h"
#include "semantic/SymbolTable.h"

namespace sqlcompiler {

class SystemCatalog;

// 语义错误描述
struct SemanticError {
    std::string message;
    int line = -1;
};

// 语义分析器：在AST上进行表/列存在性检查、类型检查等
class SemanticAnalyzer {
public:
    SemanticAnalyzer(SystemCatalog* catalog, SymbolTable& symbol_table);

    // 分析入口，返回是否通过语义检查
    bool Analyze(const StatementPtr& statement);

    // 获取分析过程中收集到的所有错误
    const std::vector<SemanticError>& GetErrors() const;

    // 清空错误列表，便于复用同一个分析器实例
    void ClearErrors();

private:
    SystemCatalog* catalog_ = nullptr;
    SymbolTable& symbol_table_;
    std::vector<SemanticError> errors_;

    // ---- 各语句类型的语义检查 ----
    bool AnalyzeSelect(const SelectStatement& stmt);
    bool AnalyzeInsert(const InsertStatement& stmt);
    bool AnalyzeUpdate(const UpdateStatement& stmt);
    bool AnalyzeDelete(const DeleteStatement& stmt);
    bool AnalyzeMerge(const MergeStatement& stmt);
    bool AnalyzeCreateTable(const CreateTableStatement& stmt);
    bool AnalyzeDropTable(const DropTableStatement& stmt);
    bool AnalyzeCreateIndex(const CreateIndexStatement& stmt);
    bool AnalyzeDropIndex(const DropIndexStatement& stmt);
    bool AnalyzeTruncateTable(const TruncateTableStatement& stmt);
    bool AnalyzeAlterTable(const AlterStatement& stmt);

    // 内部递归版本：不调用 ClearErrors，便于在嵌套语句（如 SET_OP_STMT）中
    // 累积所有子树产生的错误。
    bool AnalyzeInternal(const StatementPtr& statement, bool& ok);

    // ---- 通用检查函数 ----
    bool CheckTableExists(const std::string& table_name);
    bool CheckColumnExists(const std::string& table_name, const std::string& column_name);
    bool CheckExpression(const ExprPtr& expr, const std::string& table_name);

    // 别名映射：每个 pair 是 (alias_or_name, real_table_name)。用于把限定列
    // 引用（如 `e.dept_id`）中的别名解析回真实表，以便在「该别名对应的那张表」
    // 上校验列存在性，避免「跨表找到列就放过」的语义漏检。
    using TableAliasMap = std::vector<std::pair<std::string, std::string>>;

    bool CheckExpressionMulti(const ExprPtr& expr,
                              const std::vector<std::string>& tables,
                              const TableAliasMap& aliases = {});
    // 与 CheckExpressionMulti 类似，但额外允许 aliases 中的名字解析为列引用。
    // 用于 ORDER BY / HAVING / 同 SELECT 列表中靠后的项，使 SELECT 别名在这些位置可见。
    bool CheckExpressionMultiWithAliases(const ExprPtr& expr,
                                         const std::vector<std::string>& tables,
                                         const std::vector<std::string>& aliases,
                                         const TableAliasMap& table_aliases = {});

    void AddError(const std::string& message, int line = -1);
};

}  // namespace sqlcompiler
