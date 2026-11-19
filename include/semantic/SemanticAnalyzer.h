#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/AST.h"
#include "common/Error.h"
#include "semantic/SemanticErrorStage.h"
#include "semantic/SymbolTable.h"

namespace sqlcompiler {

class SystemCatalog;

// 语义错误描述（Spec 1.3）：stage + line + column + kind + message。
// 用户可见的错误消息由 stage/kind/line/column/message 五元组组装而成。
struct SemanticError {
    // 错误发生在哪个阶段。语义分析阶段的错误统一为 ErrorStage::SEMANTIC，
    // 保留字段便于将来扩展（例如约束校验抛错）。
    ErrorStage stage = ErrorStage::SEMANTIC;

    // 阶段内的细分类。Other 表示不适合以上任何分类的兜底。
    SemanticErrorKind kind = SemanticErrorKind::Other;

    // 用户可读的错误原因。
    std::string message;

    // 源码位置（1-based）。-1 表示该错误不携带位置信息。
    int line = -1;
    int column = -1;
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

    // item #16: 单次递归遍历 SET_OP 子树，返回「叶子 SELECT 的列数 + 所有
    // SELECT 的别名集合」。原实现用 2 个 lambda + 2 次 AnalyzeInternal 递归，
    // 对深度为 D 的子树会跑 4×O(D) = O(4D)；新版合并成一次 walk，返回值
    // 同时填充两种用途，walk 次数降到 1×O(D)。
    struct SetOpWalkResult {
        int col_count;                 // -1 表示子树无 SELECT_STMT 叶子
        std::vector<std::string> aliases;  // 累积的 SELECT 别名（用于 ORDER BY 解析）
    };
    SetOpWalkResult WalkSetOpTree(const StatementPtr& statement) const;

    // item #17: 根据 SELECT_STMT + cte_column_aliases 派生 CTE 列信息。
    // 把 WITH_STMT 里两个几乎相同的列派生块（recursive 锚点 vs non-recursive body）
    // 抽取到这里。返回的 vector 已 push 到 ti.columns（就地修改）。
    // 函数语义保持与原代码一致：优先使用 cte_column_aliases，否则用 select_aliases，
    // 否则用 SELECT * 展开的源表列，否则用 col<index> 占位。
    void BuildCteColumns(TableInfo& ti,
                         const SelectStatement& select,
                         const std::vector<std::string>& cte_column_aliases) const;

    // ---- 通用检查函数 ----
    // 当 Node 非空时从节点读取 line/column，否则使用默认值 -1。
    // 一些检查函数没有 AST 节点上下文（如纯字符串检查函数），可以显式
    // 传 line/column 或保持默认 -1。
    bool CheckTableExists(const std::string& table_name,
                          int line = -1, int column = -1);
    bool CheckColumnExists(const std::string& table_name,
                           const std::string& column_name,
                           int line = -1, int column = -1);
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

    // 添加一个语义错误。stage 默认为 ErrorStage::SEMANTIC；kind 决定
    // 错误消息里 `[<Kind>]` 标签；line/column 在 -1 时表示无位置信息。
    // 这是 SemanticAnalyzer 内部唯一的错误入口，外部代码不应直接构造
    // SemanticError 并 push_back。
    void AddError(SemanticErrorKind kind,
                  const std::string& message,
                  int line = -1,
                  int column = -1,
                  ErrorStage stage = ErrorStage::SEMANTIC);

    // "Did you mean" suggestion: among `candidates`, find the names whose
    // Levenshtein distance to `bad_name` is <= 2, sort by (distance, name)
    // ascending, and return a human-readable hint string. Returns the empty
    // string when no candidate qualifies, so callers can simply append the
    // result with a space when non-empty.
    std::string SuggestClosestName(
        const std::string& bad_name,
        const std::vector<std::string>& candidates) const;
};

}  // namespace sqlcompiler
