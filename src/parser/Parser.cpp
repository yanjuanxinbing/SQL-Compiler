#include "parser/Parser.h"

#include <stdexcept>
#include <string>

#include "common/Error.h"

namespace sqlcompiler {

// ================= 构造与基础工具 =================

Parser::Parser(std::vector<Token> tokens)
    : tokens_(std::move(tokens)), current_(0) {}

StatementPtr Parser::Parse() {
    StatementPtr stmt = ParseStatement();
    // 吃掉语句结尾的分号（若有）
    Match(TokenType::SEMICOLON);
    return stmt;
}

std::vector<StatementPtr> Parser::ParseAll() {
    std::vector<StatementPtr> stmts;
    while (!IsAtEnd()) {
        // 跳过可能存在的连续分号
        while (Match(TokenType::SEMICOLON)) {
        }
        if (IsAtEnd()) break;
        StatementPtr stmt = ParseStatement();
        if (stmt) {
            stmts.push_back(std::move(stmt));
        }
        // 吃掉分隔分号
        Match(TokenType::SEMICOLON);
    }
    return stmts;
}

const Token& Parser::CurrentToken() const {
    return tokens_[current_];
}

const Token& Parser::PeekToken(int offset) const {
    if (current_ + offset >= tokens_.size()) {
        return tokens_.back();  // 越界返回最后一个（通常为 EOF）
    }
    return tokens_[current_ + offset];
}

Token Parser::Advance() {
    if (!IsAtEnd()) {
        ++current_;
    }
    return tokens_[current_ - 1];
}

bool Parser::Check(TokenType type) const {
    return CurrentToken().type == type;
}

bool Parser::Match(TokenType type) {
    if (Check(type)) {
        Advance();
        return true;
    }
    return false;
}

Token Parser::Expect(TokenType type, const std::string& error_message) {
    if (Check(type)) {
        return Advance();
    }
    const Token& cur = CurrentToken();
    std::string msg = error_message + " (got '" + cur.lexeme + "' at line "
                      + std::to_string(cur.line) + ", col "
                      + std::to_string(cur.column) + ")";
    throw CompilerException(ErrorStage::SYNTAX, msg, cur.line, cur.column);
}

bool Parser::IsAtEnd() const {
    return CurrentToken().type == TokenType::END_OF_FILE;
}

// ================= 语句解析 =================

StatementPtr Parser::ParseStatement() {
    const Token& tok = CurrentToken();
    switch (tok.type) {
        case TokenType::KEYWORD_SELECT: return ParseSelectStatement();
        case TokenType::KEYWORD_INSERT: return ParseInsertStatement();
        case TokenType::KEYWORD_UPDATE: return ParseUpdateStatement();
        case TokenType::KEYWORD_DELETE: return ParseDeleteStatement();
        case TokenType::KEYWORD_CREATE: return ParseCreateTableStatement();
        case TokenType::KEYWORD_DROP:   return ParseDropTableStatement();
        default: {
            std::string msg = "Unexpected token at start of statement: '"
                              + tok.lexeme + "'";
            throw CompilerException(ErrorStage::SYNTAX, msg, tok.line, tok.column);
        }
    }
}

StatementPtr Parser::ParseSelectStatement() {
    auto stmt = std::make_shared<SelectStatement>();

    Expect(TokenType::KEYWORD_SELECT, "Expected SELECT");
    stmt->is_distinct = Match(TokenType::KEYWORD_DISTINCT);

    stmt->select_list = ParseSelectList();

    Expect(TokenType::KEYWORD_FROM, "Expected FROM");
    Token table = Expect(TokenType::IDENTIFIER, "Expected table name after FROM");
    stmt->from_table = table.lexeme;

    stmt->joins = ParseJoinClauses();

    if (Match(TokenType::KEYWORD_WHERE)) {
        stmt->where_clause = ParseExpression();
    }

    if (Match(TokenType::KEYWORD_GROUP)) {
        Expect(TokenType::KEYWORD_BY, "Expected BY after GROUP");
        stmt->group_by = ParseExpressionList();
    }

    if (Match(TokenType::KEYWORD_HAVING)) {
        stmt->having_clause = ParseExpression();
    }

    if (Match(TokenType::KEYWORD_ORDER)) {
        Expect(TokenType::KEYWORD_BY, "Expected BY after ORDER");
        stmt->order_by = ParseOrderByClause();
    }

    if (Match(TokenType::KEYWORD_LIMIT)) {
        stmt->limit = ParseLimitClause();
    }

    return stmt;
}

StatementPtr Parser::ParseInsertStatement() {
    auto stmt = std::make_shared<InsertStatement>();

    Expect(TokenType::KEYWORD_INSERT, "Expected INSERT");
    Expect(TokenType::KEYWORD_INTO, "Expected INTO after INSERT");
    Token table = Expect(TokenType::IDENTIFIER, "Expected table name after INSERT INTO");
    stmt->table_name = table.lexeme;

    // 可选列名列表
    if (Check(TokenType::LEFT_PAREN)) {
        Advance();
        // 可能为空: INSERT INTO t () VALUES ... — 我们允许，但要至少有一个
        if (!Check(TokenType::RIGHT_PAREN)) {
            Token col = Expect(TokenType::IDENTIFIER, "Expected column name");
            stmt->columns.push_back(col.lexeme);
            while (Match(TokenType::COMMA)) {
                Token c = Expect(TokenType::IDENTIFIER, "Expected column name");
                stmt->columns.push_back(c.lexeme);
            }
        }
        Expect(TokenType::RIGHT_PAREN, "Expected ')' after column list");
    }

    Expect(TokenType::KEYWORD_VALUES, "Expected VALUES");

    // 至少一行 (expr, expr, ...)，逗号分隔多行
    do {
        Expect(TokenType::LEFT_PAREN, "Expected '(' to start VALUES row");
        std::vector<ExprPtr> row;
        // 允许空行 VALUES ()
        if (!Check(TokenType::RIGHT_PAREN)) {
            row.push_back(ParseExpression());
            while (Match(TokenType::COMMA)) {
                row.push_back(ParseExpression());
            }
        }
        Expect(TokenType::RIGHT_PAREN, "Expected ')' after VALUES row");
        stmt->values_list.push_back(std::move(row));
    } while (Match(TokenType::COMMA));

    return stmt;
}

StatementPtr Parser::ParseUpdateStatement() {
    auto stmt = std::make_shared<UpdateStatement>();

    Expect(TokenType::KEYWORD_UPDATE, "Expected UPDATE");
    Token table = Expect(TokenType::IDENTIFIER, "Expected table name after UPDATE");
    stmt->table_name = table.lexeme;

    Expect(TokenType::KEYWORD_SET, "Expected SET after table name");

    // 至少一个 col = expr，逗号分隔
    do {
        Token col = Expect(TokenType::IDENTIFIER,
                           "Expected column name on left side of assignment");
        Expect(TokenType::OP_EQUAL, "Expected '=' in assignment");
        ExprPtr value = ParseExpression();
        stmt->assignments.emplace_back(col.lexeme, std::move(value));
    } while (Match(TokenType::COMMA));

    if (Match(TokenType::KEYWORD_WHERE)) {
        stmt->where_clause = ParseExpression();
    }

    return stmt;
}

StatementPtr Parser::ParseDeleteStatement() {
    auto stmt = std::make_shared<DeleteStatement>();

    Expect(TokenType::KEYWORD_DELETE, "Expected DELETE");
    Expect(TokenType::KEYWORD_FROM, "Expected FROM after DELETE");
    Token table = Expect(TokenType::IDENTIFIER,
                         "Expected table name after DELETE FROM");
    stmt->table_name = table.lexeme;

    if (Match(TokenType::KEYWORD_WHERE)) {
        stmt->where_clause = ParseExpression();
    }

    return stmt;
}

StatementPtr Parser::ParseCreateTableStatement() {
    auto stmt = std::make_shared<CreateTableStatement>();

    Expect(TokenType::KEYWORD_CREATE, "Expected CREATE");
    Expect(TokenType::KEYWORD_TABLE, "Expected TABLE after CREATE");
    Token table = Expect(TokenType::IDENTIFIER,
                         "Expected table name after CREATE TABLE");
    stmt->table_name = table.lexeme;

    Expect(TokenType::LEFT_PAREN, "Expected '(' after table name");
    stmt->columns = ParseColumnDefinitions();
    Expect(TokenType::RIGHT_PAREN, "Expected ')' after column definitions");

    return stmt;
}

StatementPtr Parser::ParseDropTableStatement() {
    auto stmt = std::make_shared<DropTableStatement>();

    Expect(TokenType::KEYWORD_DROP, "Expected DROP");
    Expect(TokenType::KEYWORD_TABLE, "Expected TABLE after DROP");
    Token table = Expect(TokenType::IDENTIFIER,
                         "Expected table name after DROP TABLE");
    stmt->table_name = table.lexeme;

    return stmt;
}

// ================= 子句解析 =================

std::vector<ExprPtr> Parser::ParseSelectList() {
    std::vector<ExprPtr> list;
    // SELECT *
    if (Match(TokenType::OP_STAR)) {
        // 用一个列名为 "*" 的占位列引用表示全选（AST 中以空 column_name 区分）
        list.push_back(std::make_shared<ColumnRefExpr>("", "*"));
        return list;
    }

    list.push_back(ParseExpression());
    while (Match(TokenType::COMMA)) {
        list.push_back(ParseExpression());
    }
    return list;
}

std::vector<JoinClause> Parser::ParseJoinClauses() {
    std::vector<JoinClause> joins;
    while (true) {
        JoinType type = JoinType::INNER;
        bool saw_join_kw = false;
        if (Match(TokenType::KEYWORD_INNER)) {
            type = JoinType::INNER;
            saw_join_kw = true;
        } else if (Match(TokenType::KEYWORD_LEFT)) {
            type = JoinType::LEFT;
            saw_join_kw = true;
        } else if (Match(TokenType::KEYWORD_RIGHT)) {
            type = JoinType::RIGHT;
            saw_join_kw = true;
        }
        if (Check(TokenType::KEYWORD_JOIN)) {
            Advance();
            saw_join_kw = true;
        } else if (saw_join_kw) {
            const Token& cur = CurrentToken();
            throw CompilerException(ErrorStage::SYNTAX,
                                    "Expected JOIN keyword",
                                    cur.line, cur.column);
        }
        if (!saw_join_kw) break;

        JoinClause jc;
        jc.join_type = type;
        Token t = Expect(TokenType::IDENTIFIER, "Expected table name after JOIN");
        jc.table_name = t.lexeme;
        Expect(TokenType::KEYWORD_ON, "Expected ON in JOIN clause");
        jc.on_condition = ParseExpression();
        joins.push_back(std::move(jc));
    }
    return joins;
}

ExprPtr Parser::ParseWhereClause() {
    Expect(TokenType::KEYWORD_WHERE, "Expected WHERE");
    return ParseExpression();
}

std::vector<ExprPtr> Parser::ParseGroupByClause() {
    Expect(TokenType::KEYWORD_GROUP, "Expected GROUP");
    Expect(TokenType::KEYWORD_BY, "Expected BY after GROUP");
    return ParseExpressionList();
}

ExprPtr Parser::ParseHavingClause() {
    Expect(TokenType::KEYWORD_HAVING, "Expected HAVING");
    return ParseExpression();
}

std::vector<OrderByItem> Parser::ParseOrderByClause() {
    std::vector<OrderByItem> items;
    do {
        OrderByItem item;
        item.expr = ParseExpression();
        // ASC / DESC 当前不是独立关键字,按大小写不敏感的 IDENTIFIER 识别
        if (Check(TokenType::IDENTIFIER)) {
            const std::string& lex = CurrentToken().lexeme;
            std::string upper = lex;
            for (char& c : upper) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            if (upper == "ASC") {
                Advance();
                item.ascending = true;
            } else if (upper == "DESC") {
                Advance();
                item.ascending = false;
            }
        }
        items.push_back(std::move(item));
    } while (Match(TokenType::COMMA));
    return items;
}

int Parser::ParseLimitClause() {
    Token lit = Expect(TokenType::INTEGER_LITERAL,
                       "Expected integer literal after LIMIT");
    try {
        return std::stoi(lit.lexeme);
    } catch (const std::exception&) {
        throw CompilerException(ErrorStage::SYNTAX,
                                "Invalid LIMIT value: " + lit.lexeme,
                                lit.line, lit.column);
    }
}

std::vector<ColumnDefinition> Parser::ParseColumnDefinitions() {
    std::vector<ColumnDefinition> defs;
    defs.push_back(ParseColumnDefinition());
    while (Match(TokenType::COMMA)) {
        defs.push_back(ParseColumnDefinition());
    }
    return defs;
}

ColumnDefinition Parser::ParseColumnDefinition() {
    ColumnDefinition def;
    Token name = Expect(TokenType::IDENTIFIER, "Expected column name");
    def.column_name = name.lexeme;

    TokenType tt = CurrentToken().type;
    if (tt == TokenType::KEYWORD_INT) {
        def.data_type = "INT";
        Advance();
    } else if (tt == TokenType::KEYWORD_VARCHAR) {
        def.data_type = "VARCHAR";
        Advance();
        // 可选 VARCHAR(n)
        if (Match(TokenType::LEFT_PAREN)) {
            Token len = Expect(TokenType::INTEGER_LITERAL,
                               "Expected length after VARCHAR(");
            def.data_type += "(" + len.lexeme + ")";
            Expect(TokenType::RIGHT_PAREN, "Expected ')' after VARCHAR length");
        }
    } else if (tt == TokenType::KEYWORD_FLOAT) {
        def.data_type = "FLOAT";
        Advance();
    } else {
        const Token& cur = CurrentToken();
        throw CompilerException(ErrorStage::SYNTAX,
                                "Expected column data type, got '" + cur.lexeme + "'",
                                cur.line, cur.column);
    }

    // 约束：可选 PRIMARY KEY / NOT NULL
    while (true) {
        if (Match(TokenType::KEYWORD_PRIMARY)) {
            Expect(TokenType::KEYWORD_KEY, "Expected KEY after PRIMARY");
            def.is_primary_key = true;
        } else if (Match(TokenType::KEYWORD_NOT)) {
            Expect(TokenType::KEYWORD_NULL, "Expected NULL after NOT");
            def.is_not_null = true;
        } else {
            break;
        }
    }
    return def;
}

// ================= 表达式解析 =================

ExprPtr Parser::ParseExpression() {
    return ParseOrExpr();
}

ExprPtr Parser::ParseOrExpr() {
    ExprPtr expr = ParseAndExpr();
    while (Match(TokenType::KEYWORD_OR)) {
        ExprPtr right = ParseAndExpr();
        expr = std::make_shared<BinaryExpr>(BinaryOperator::OR, expr, right);
    }
    return expr;
}

ExprPtr Parser::ParseAndExpr() {
    ExprPtr expr = ParseNotExpr();
    while (Match(TokenType::KEYWORD_AND)) {
        ExprPtr right = ParseNotExpr();
        expr = std::make_shared<BinaryExpr>(BinaryOperator::AND, expr, right);
    }
    return expr;
}

ExprPtr Parser::ParseNotExpr() {
    if (Match(TokenType::KEYWORD_NOT)) {
        ExprPtr inner = ParseNotExpr();
        return std::make_shared<UnaryExpr>(UnaryOperator::NOT, inner);
    }
    return ParseComparisonExpr();
}

ExprPtr Parser::ParseComparisonExpr() {
    ExprPtr expr = ParseAdditiveExpr();
    BinaryOperator op;
    bool matched = true;
    switch (CurrentToken().type) {
        case TokenType::OP_EQUAL:         op = BinaryOperator::EQUAL; break;
        case TokenType::OP_NOT_EQUAL:     op = BinaryOperator::NOT_EQUAL; break;
        case TokenType::OP_LESS:          op = BinaryOperator::LESS; break;
        case TokenType::OP_LESS_EQUAL:    op = BinaryOperator::LESS_EQUAL; break;
        case TokenType::OP_GREATER:       op = BinaryOperator::GREATER; break;
        case TokenType::OP_GREATER_EQUAL: op = BinaryOperator::GREATER_EQUAL; break;
        default: matched = false; break;
    }
    if (!matched) return expr;
    Advance();
    ExprPtr right = ParseAdditiveExpr();
    return std::make_shared<BinaryExpr>(op, expr, right);
}

ExprPtr Parser::ParseAdditiveExpr() {
    ExprPtr expr = ParseMultiplicativeExpr();
    while (true) {
        BinaryOperator op;
        if (Match(TokenType::OP_PLUS)) {
            op = BinaryOperator::ADD;
        } else if (Match(TokenType::OP_MINUS)) {
            op = BinaryOperator::SUB;
        } else {
            break;
        }
        ExprPtr right = ParseMultiplicativeExpr();
        expr = std::make_shared<BinaryExpr>(op, expr, right);
    }
    return expr;
}

ExprPtr Parser::ParseMultiplicativeExpr() {
    ExprPtr expr = ParseUnaryExpr();
    while (true) {
        BinaryOperator op;
        if (Match(TokenType::OP_STAR)) {
            op = BinaryOperator::MUL;
        } else if (Match(TokenType::OP_SLASH)) {
            op = BinaryOperator::DIV;
        } else {
            break;
        }
        ExprPtr right = ParseUnaryExpr();
        expr = std::make_shared<BinaryExpr>(op, expr, right);
    }
    return expr;
}

ExprPtr Parser::ParseUnaryExpr() {
    if (Match(TokenType::OP_MINUS)) {
        ExprPtr inner = ParseUnaryExpr();
        return std::make_shared<UnaryExpr>(UnaryOperator::NEGATE, inner);
    }
    return ParsePrimaryExpr();
}

ExprPtr Parser::ParsePrimaryExpr() {
    const Token& tok = CurrentToken();

    // 数字字面量
    if (tok.type == TokenType::INTEGER_LITERAL) {
        Advance();
        return std::make_shared<LiteralExpr>(LiteralType::INTEGER, tok.lexeme);
    }
    if (tok.type == TokenType::FLOAT_LITERAL) {
        Advance();
        return std::make_shared<LiteralExpr>(LiteralType::FLOAT, tok.lexeme);
    }
    if (tok.type == TokenType::STRING_LITERAL) {
        Advance();
        return std::make_shared<LiteralExpr>(LiteralType::STRING, tok.lexeme);
    }
    // NULL / TRUE / FALSE 由关键字 NULL 处理
    if (tok.type == TokenType::KEYWORD_NULL) {
        Advance();
        return std::make_shared<LiteralExpr>(LiteralType::NULL_VALUE, "NULL");
    }

    // 括号表达式
    if (Match(TokenType::LEFT_PAREN)) {
        ExprPtr inner = ParseExpression();
        Expect(TokenType::RIGHT_PAREN, "Expected ')' after expression");
        return inner;
    }

    // 标识符：可能是列引用、限定列、函数调用
    if (tok.type == TokenType::IDENTIFIER) {
        return ParseColumnRefOrFunctionCall();
    }

    // 星号单独出现在表达式上下文：当成 "*" 字面列引用（用于 SELECT * 已在外层处理）
    if (tok.type == TokenType::OP_STAR) {
        Advance();
        return std::make_shared<ColumnRefExpr>("", "*");
    }

    std::string msg = "Unexpected token in expression: '" + tok.lexeme + "'";
    throw CompilerException(ErrorStage::SYNTAX, msg, tok.line, tok.column);
}

std::vector<ExprPtr> Parser::ParseExpressionList() {
    std::vector<ExprPtr> list;
    list.push_back(ParseExpression());
    while (Match(TokenType::COMMA)) {
        list.push_back(ParseExpression());
    }
    return list;
}

ExprPtr Parser::ParseColumnRefOrFunctionCall() {
    Token first = Expect(TokenType::IDENTIFIER,
                         "Expected identifier in expression");
    const std::string& name = first.lexeme;

    // 函数调用: name( ... )
    if (Check(TokenType::LEFT_PAREN)) {
        Advance();
        std::vector<ExprPtr> args;
        if (!Check(TokenType::RIGHT_PAREN)) {
            // 支持 COUNT(*) 之类的写法：直接看到 )
            if (Check(TokenType::OP_STAR)) {
                Advance();
            } else {
                args.push_back(ParseExpression());
                while (Match(TokenType::COMMA)) {
                    args.push_back(ParseExpression());
                }
            }
        }
        Expect(TokenType::RIGHT_PAREN, "Expected ')' after function arguments");
        return std::make_shared<FunctionCallExpr>(name, std::move(args));
    }

    // 限定列引用: name.name
    if (Match(TokenType::DOT)) {
        Token col = Expect(TokenType::IDENTIFIER,
                           "Expected column name after '.'");
        return std::make_shared<ColumnRefExpr>(name, col.lexeme);
    }

    // 裸列引用
    return std::make_shared<ColumnRefExpr>("", name);
}

}  // namespace sqlcompiler
