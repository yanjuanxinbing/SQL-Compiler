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
    StatementPtr ParseSelectStatement();
    StatementPtr ParseInsertStatement();
    StatementPtr ParseUpdateStatement();
    StatementPtr ParseDeleteStatement();
    StatementPtr ParseCreateTableStatement();
    StatementPtr ParseDropTableStatement();
    StatementPtr ParseCreateIndexStatement();
    StatementPtr ParseDropIndexStatement();

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

    // 解析 [table.]column 或 table.* 形式的列引用
    ExprPtr ParseColumnRefOrFunctionCall();
};

}  // namespace sqlcompiler
