#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/AST.h"
#include "lexer/Token.h"

namespace sqlcompiler {

// 递归下降语法分析器：将Token序列转换为AST
class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    // 解析单条SQL语句（遇到第一个分号或EOF结束）
    StatementPtr Parse();

    // 解析以分号分隔的多条SQL语句
    std::vector<StatementPtr> ParseAll();

private:
    std::vector<Token> tokens_;
    size_t current_;

    // ---- 基础工具函数 ----
    const Token& CurrentToken() const;
    const Token& PeekToken(int offset = 1) const;
    Token Advance();
    bool Check(TokenType type) const;
    bool Match(TokenType type);
    Token Expect(TokenType type, const std::string& error_message);
    bool IsAtEnd() const;

    // ---- 语句解析 ----
    StatementPtr ParseStatement();
    StatementPtr ParseSelectStatement(bool consume_trailers = true);
    StatementPtr ParseInsertStatement();
    StatementPtr ParseUpdateStatement();
    StatementPtr ParseDeleteStatement();
    StatementPtr ParseCreateTableStatement();
    StatementPtr ParseDropTableStatement();
    StatementPtr ParseCreateIndexStatement();
    StatementPtr ParseDropIndexStatement();
    StatementPtr ParseAlterTableStatement();
    // ---- 40_txn_view_udf: 事务 / 视图 / 触发器 / UDF ----
    StatementPtr ParseBeginStatement();
    StatementPtr ParseCommitStatement();
    StatementPtr ParseRollbackStatement();
    StatementPtr ParseSavepointStatement();
    StatementPtr ParseReleaseSavepointStatement();
    StatementPtr ParseCreateViewStatement();
    StatementPtr ParseDropViewStatement();
    StatementPtr ParseCreateTriggerStatement();
    StatementPtr ParseDropTriggerStatement();
    StatementPtr ParseCreateFunctionStatement();
    StatementPtr ParseDropFunctionStatement();
    // ---- 46_meta: 元命令 ----
    StatementPtr ParseExplainStatement();
    StatementPtr ParseShowStatement();

    // ---- 子句解析 ----
    std::vector<ExprPtr> ParseSelectList();
    std::vector<JoinClause> ParseJoinClauses();
    JoinClause ParseJoinClause();
    ExprPtr ParseWhereClause();
    std::vector<ExprPtr> ParseGroupByClause();
    ExprPtr ParseHavingClause();
    std::vector<OrderByItem> ParseOrderByClause();
    int ParseLimitClause();
    std::vector<ColumnDefinition> ParseColumnDefinitions(CreateTableStatement& stmt);
    ColumnDefinition ParseColumnDefinition();

    // ---- 表达式解析（按优先级从低到高分层的递归下降） ----
    ExprPtr ParseExpression();
    ExprPtr ParseOrExpr();
    ExprPtr ParseAndExpr();
    ExprPtr ParseNotExpr();
    ExprPtr ParseComparisonExpr();
    ExprPtr ParseAdditiveExpr();
    ExprPtr ParseMultiplicativeExpr();
    ExprPtr ParseUnaryExpr();
    ExprPtr ParsePrimaryExpr();
    std::vector<ExprPtr> ParseExpressionList();

    // ---- 47_udf_trigger_view: UDF 函数体语句 ----
    // 解析一条语句（用于 BEGIN ... END 块内部）。可识别：
    //   RETURN [expr];
    //   DECLARE name TYPE;
    //   SET name = expr;  （name 可为 "NEW.col" / "OLD.col"）
    //   IF cond THEN stmts [ELSEIF ...] [ELSE ...] END IF;
    //   WHILE cond DO stmts END WHILE;
    StatementPtr ParseFunctionBodyStatement();
    // 解析一段语句列表直到遇到 end_token 为止（不含 end_token）。
    std::vector<StatementPtr> ParseFunctionBodyUntil(TokenType end_token);

    // 解析 [table.]column 或 table.* 形式的列引用
    ExprPtr ParseColumnRefOrFunctionCall();

    // ---- 27–33 新增语法 ----
    // 解析 WHERE/HAVING 中可选的前导 NOT（保留以兼容旧调用）
    // 解析简单 CASE / 搜索式 CASE
    ExprPtr ParseCaseExpression();
    // 解析 CAST(expr AS type)
    ExprPtr ParseCastExpression();
    // 45_datetime: 解析 EXTRACT(field FROM source)
    ExprPtr ParseExtractExpression();
    // 45_datetime: 解析 INTERVAL <n> <unit>（调用前已消耗 INTERVAL 关键字）
    ExprPtr ParseIntervalExpression();
    // 解析 ON DUPLICATE KEY UPDATE 末尾的 SET 子句：
    //   col = expr [, col = expr ...]
    // expr 中可出现 VALUES(col) 形式（在 ParsePrimaryExpr 中处理）。
    std::vector<std::pair<std::string, ExprPtr>> ParseUpsertAssignments();
    // 解析形如 (SELECT ...) / EXISTS (SELECT ...) / ... IN (SELECT ...) / expr op ANY (SELECT ...) 的子查询
    ExprPtr ParseSubqueryExpression(ExprPtr left_operand, const std::string& comparison_op,
                                    SubqueryType forced_kind = SubqueryType::SCALAR);
    // 解析 OVER (...) 或 OVER w
    WindowSpec ParseOverSpec();
    // 解析 SELECT 后置的 WINDOW 子句（命名窗口）
    std::vector<std::pair<std::string, WindowSpec>> ParseWindowClause();
    // 解析函数调用后的 OVER (...) 子句（若无 OVER 则返回 nullptr）。
    // 假定 '(' 已经被当前调用方消耗，进入时当前 token 应为 OVER。
    std::shared_ptr<WindowFuncNode> ParseOverClause(const std::string& func_name,
                                                     std::vector<ExprPtr>& args);
    // 解析 SELECT 末尾的 UNION/INTERSECT/EXCEPT 链（左递归式，返回 StatementPtr）
    StatementPtr ParseSetOperationTail(StatementPtr left);
    // 解析 WITH ... SELECT（不含 set op tail；set op 由调用者处理）
    StatementPtr ParseWithClause();
    // 解析 SELECT 的 FROM 段（含派生表 (SELECT ...) AS alias）
    void ParseFromClause(SelectStatement& stmt);
    // 单个 FROM 表项
    void ParseFromTableRef(SelectStatement& stmt);
    // 解析 SELECT 语句并自动连接尾部 set-op 链
    StatementPtr ParseSelectStatementWithSetOps();
};

}  // namespace sqlcompiler
