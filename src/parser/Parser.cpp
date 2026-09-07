#include "parser/Parser.h"

#include "common/Error.h"

namespace sqlcompiler {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)), current_(0) {
    // TODO: 如有需要可补充初始化逻辑
}

StatementPtr Parser::Parse() {
    // TODO: 调用ParseStatement()解析单条语句，可选地Match(SEMICOLON)吸收结尾分号
    return nullptr;
}

std::vector<StatementPtr> Parser::ParseAll() {
    // TODO: 循环调用Parse()直到IsAtEnd()，收集所有语句并返回
    return {};
}

// ================= 基础工具函数 =================

const Token& Parser::CurrentToken() const {
    // TODO: 返回tokens_[current_]
    return tokens_[current_];
}

const Token& Parser::PeekToken(int offset) const {
    // TODO: 返回tokens_[current_ + offset]，注意越界处理（返回EOF Token）
    return tokens_[current_];
}

Token Parser::Advance() {
    // TODO: 返回当前Token，并将current_前移一位（若未越界）
    return Token();
}

bool Parser::Check(TokenType type) const {
    // TODO: 判断CurrentToken().type是否等于type
    return false;
}

bool Parser::Match(TokenType type) {
    // TODO: 若Check(type)为真，则Advance()并返回true，否则返回false
    return false;
}

Token Parser::Expect(TokenType type, const std::string& error_message) {
    // TODO: 若Check(type)为真则Advance()并返回该Token，
    // 否则抛出CompilerException(ErrorStage::SYNTAX, error_message, ...)
    return Token();
}

bool Parser::IsAtEnd() const {
    // TODO: 判断CurrentToken().type是否为END_OF_FILE
    return true;
}

// ================= 语句解析 =================

StatementPtr Parser::ParseStatement() {
    // TODO: 根据CurrentToken().type分派到具体的Parse*Statement()函数
    // SELECT / INSERT / UPDATE / DELETE / CREATE / DROP
    return nullptr;
}

StatementPtr Parser::ParseSelectStatement() {
    // TODO:
    // 1. Expect(KEYWORD_SELECT)
    // 2. 可选 DISTINCT
    // 3. ParseSelectList()
    // 4. Expect(KEYWORD_FROM) + 表名
    // 5. ParseJoinClauses()
    // 6. 可选 ParseWhereClause()
    // 7. 可选 GROUP BY -> ParseGroupByClause()
    // 8. 可选 HAVING -> ParseHavingClause()
    // 9. 可选 ORDER BY -> ParseOrderByClause()
    // 10. 可选 LIMIT -> ParseLimitClause()
    return nullptr;
}

StatementPtr Parser::ParseInsertStatement() {
    // TODO:
    // 1. Expect(KEYWORD_INSERT) + Expect(KEYWORD_INTO) + 表名
    // 2. 可选的列名列表 (col1, col2, ...)
    // 3. Expect(KEYWORD_VALUES)
    // 4. 一个或多个 (expr, expr, ...) 值列表，逗号分隔
    return nullptr;
}

StatementPtr Parser::ParseUpdateStatement() {
    // TODO:
    // 1. Expect(KEYWORD_UPDATE) + 表名
    // 2. Expect(KEYWORD_SET) + 一个或多个 col = expr，逗号分隔
    // 3. 可选 ParseWhereClause()
    return nullptr;
}

StatementPtr Parser::ParseDeleteStatement() {
    // TODO:
    // 1. Expect(KEYWORD_DELETE) + Expect(KEYWORD_FROM) + 表名
    // 2. 可选 ParseWhereClause()
    return nullptr;
}

StatementPtr Parser::ParseCreateTableStatement() {
    // TODO:
    // 1. Expect(KEYWORD_CREATE) + Expect(KEYWORD_TABLE) + 表名
    // 2. Expect(LEFT_PAREN) + ParseColumnDefinitions() + Expect(RIGHT_PAREN)
    return nullptr;
}

StatementPtr Parser::ParseDropTableStatement() {
    // TODO: Expect(KEYWORD_DROP) + Expect(KEYWORD_TABLE) + 表名
    return nullptr;
}

// ================= 子句解析 =================

std::vector<ExprPtr> Parser::ParseSelectList() {
    // TODO: 解析逗号分隔的表达式列表（支持 * 通配符），可选AS别名（若需要可扩展AST）
    return {};
}

std::vector<JoinClause> Parser::ParseJoinClauses() {
    // TODO: 循环解析0个或多个JOIN子句，直到不再匹配JOIN相关关键字
    return {};
}

JoinClause Parser::ParseJoinClause() {
    // TODO:
    // 1. 解析可选的 INNER/LEFT/RIGHT 关键字确定JoinType
    // 2. Expect(KEYWORD_JOIN) + 表名
    // 3. Expect(KEYWORD_ON) + ParseExpression() 作为连接条件
    return JoinClause();
}

ExprPtr Parser::ParseWhereClause() {
    // TODO: Expect(KEYWORD_WHERE) + ParseExpression()
    return nullptr;
}

std::vector<ExprPtr> Parser::ParseGroupByClause() {
    // TODO: Expect(KEYWORD_GROUP) + Expect(KEYWORD_BY) + 逗号分隔的表达式列表
    return {};
}

ExprPtr Parser::ParseHavingClause() {
    // TODO: Expect(KEYWORD_HAVING) + ParseExpression()
    return nullptr;
}

std::vector<OrderByItem> Parser::ParseOrderByClause() {
    // TODO: Expect(KEYWORD_ORDER) + Expect(KEYWORD_BY) +
    // 逗号分隔的 (表达式 [ASC|DESC]) 列表
    return {};
}

int Parser::ParseLimitClause() {
    // TODO: Expect(KEYWORD_LIMIT) + 整数字面量，返回其数值
    return -1;
}

std::vector<ColumnDefinition> Parser::ParseColumnDefinitions() {
    // TODO: 循环解析逗号分隔的列定义，直到遇到RIGHT_PAREN
    return {};
}

ColumnDefinition Parser::ParseColumnDefinition() {
    // TODO:
    // 1. 列名（IDENTIFIER）
    // 2. 数据类型（KEYWORD_INT / KEYWORD_VARCHAR / KEYWORD_FLOAT 等）
    // 3. 可选 PRIMARY KEY / NOT NULL 约束
    return ColumnDefinition();
}

// ================= 表达式解析（递归下降） =================

ExprPtr Parser::ParseExpression() {
    // TODO: 表达式解析入口，通常等价于ParseOrExpr()
    return ParseOrExpr();
}

ExprPtr Parser::ParseOrExpr() {
    // TODO: 解析 ParseAndExpr() (OR ParseAndExpr())*
    return nullptr;
}

ExprPtr Parser::ParseAndExpr() {
    // TODO: 解析 ParseNotExpr() (AND ParseNotExpr())*
    return nullptr;
}

ExprPtr Parser::ParseNotExpr() {
    // TODO: 解析可选前缀 NOT，构造UnaryExpr(UnaryOperator::NOT, ...)
    return nullptr;
}

ExprPtr Parser::ParseComparisonExpr() {
    // TODO: 解析 ParseAdditiveExpr() ( (=|!=|<>|<|<=|>|>=) ParseAdditiveExpr() )?
    return nullptr;
}

ExprPtr Parser::ParseAdditiveExpr() {
    // TODO: 解析 ParseMultiplicativeExpr() ((+|-) ParseMultiplicativeExpr())*
    return nullptr;
}

ExprPtr Parser::ParseMultiplicativeExpr() {
    // TODO: 解析 ParseUnaryExpr() ((*|/) ParseUnaryExpr())*
    return nullptr;
}

ExprPtr Parser::ParseUnaryExpr() {
    // TODO: 解析可选前缀 '-'，构造UnaryExpr(UnaryOperator::NEGATE, ...)，否则转发到ParsePrimaryExpr()
    return nullptr;
}

ExprPtr Parser::ParsePrimaryExpr() {
    // TODO: 处理以下情况：
    // 1. 数字/字符串/NULL 字面量 -> LiteralExpr
    // 2. 标识符 -> ParseColumnRefOrFunctionCall()
    // 3. 左括号 '(' 表达式 ')' -> 括号表达式
    return nullptr;
}

std::vector<ExprPtr> Parser::ParseExpressionList() {
    // TODO: 解析逗号分隔的表达式列表，直到遇到右括号或语句结束
    return {};
}

ExprPtr Parser::ParseColumnRefOrFunctionCall() {
    // TODO:
    // 1. 读取第一个标识符
    // 2. 若紧跟'('，视为函数调用，解析参数列表 -> FunctionCallExpr
    // 3. 若紧跟'.'，视为 table.column -> ColumnRefExpr
    // 4. 否则视为不带表名的列引用 -> ColumnRefExpr
    return nullptr;
}

}  // namespace sqlcompiler
