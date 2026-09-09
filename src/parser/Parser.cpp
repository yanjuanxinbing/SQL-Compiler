#include "parser/Parser.h"

#include "common/Error.h"

#include <cstdlib>
#include <utility>

namespace sqlcompiler {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)), current_(0) {
}

StatementPtr Parser::Parse() {
    if (IsAtEnd()) return nullptr;
    StatementPtr stmt = ParseStatement();
    if (!stmt) return nullptr;
    Match(TokenType::SEMICOLON);
    return stmt;
}

std::vector<StatementPtr> Parser::ParseAll() {
    std::vector<StatementPtr> stmts;
    while (!IsAtEnd()) {
        StatementPtr s = Parse();
        if (!s) break;
        stmts.push_back(std::move(s));
    }
    return stmts;
}

// ================= 基础工具函数 =================

const Token& Parser::CurrentToken() const {
    if (current_ >= tokens_.size()) {
        return tokens_.back();  // should be EOF
    }
    return tokens_[current_];
}

const Token& Parser::PeekToken(int offset) const {
    size_t idx = current_ + static_cast<size_t>(offset);
    if (idx >= tokens_.size()) {
        return tokens_.back();
    }
    return tokens_[idx];
}

Token Parser::Advance() {
    if (current_ >= tokens_.size()) return tokens_.back();
    Token t = tokens_[current_];
    if (current_ + 1 < tokens_.size()) {
        ++current_;
    }
    return t;
}

bool Parser::Check(TokenType type) const {
    if (current_ >= tokens_.size()) return false;
    return tokens_[current_].type == type;
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
    throw CompilerException(ErrorStage::SYNTAX,
        error_message + " (got '" + cur.lexeme + "')",
        cur.line, cur.column);
}

bool Parser::IsAtEnd() const {
    return CurrentToken().type == TokenType::END_OF_FILE;
}

// ================= 语句解析 =================

StatementPtr Parser::ParseStatement() {
    const Token& cur = CurrentToken();
    switch (cur.type) {
        case TokenType::KEYWORD_SELECT: return ParseSelectStatement();
        case TokenType::KEYWORD_INSERT: return ParseInsertStatement();
        case TokenType::KEYWORD_UPDATE: return ParseUpdateStatement();
        case TokenType::KEYWORD_DELETE: return ParseDeleteStatement();
        case TokenType::KEYWORD_CREATE: return ParseCreateTableStatement();
        case TokenType::KEYWORD_DROP:   return ParseDropTableStatement();
        default: {
            throw CompilerException(ErrorStage::SYNTAX,
                "unexpected token at start of statement: '" + cur.lexeme + "'",
                cur.line, cur.column);
        }
    }
}

StatementPtr Parser::ParseSelectStatement() {
    Expect(TokenType::KEYWORD_SELECT, "expected SELECT");
    auto stmt = std::make_shared<SelectStatement>();
    if (Match(TokenType::KEYWORD_DISTINCT)) {
        stmt->is_distinct = true;
    }
    stmt->select_list = ParseSelectList();
    Expect(TokenType::KEYWORD_FROM, "expected FROM");
    Token table = Expect(TokenType::IDENTIFIER, "expected table name");
    stmt->from_table = table.lexeme;
    stmt->joins = ParseJoinClauses();
    if (Check(TokenType::KEYWORD_WHERE)) {
        stmt->where_clause = ParseWhereClause();
    }
    if (Check(TokenType::KEYWORD_GROUP)) {
        stmt->group_by = ParseGroupByClause();
    }
    if (Check(TokenType::KEYWORD_HAVING)) {
        stmt->having_clause = ParseHavingClause();
    }
    if (Check(TokenType::KEYWORD_ORDER)) {
        stmt->order_by = ParseOrderByClause();
    }
    if (Check(TokenType::KEYWORD_LIMIT)) {
        stmt->limit = ParseLimitClause();
    }
    return stmt;
}

StatementPtr Parser::ParseInsertStatement() {
    Expect(TokenType::KEYWORD_INSERT, "expected INSERT");
    Expect(TokenType::KEYWORD_INTO, "expected INTO");
    Token table = Expect(TokenType::IDENTIFIER, "expected table name");
    auto stmt = std::make_shared<InsertStatement>();
    stmt->table_name = table.lexeme;
    if (Match(TokenType::LEFT_PAREN)) {
        while (!Check(TokenType::RIGHT_PAREN) && !IsAtEnd()) {
            Token col = Expect(TokenType::IDENTIFIER, "expected column name");
            stmt->columns.push_back(col.lexeme);
            if (!Match(TokenType::COMMA)) break;
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' after column list");
    }
    Expect(TokenType::KEYWORD_VALUES, "expected VALUES");
    do {
        Expect(TokenType::LEFT_PAREN, "expected '(' to start VALUES row");
        std::vector<ExprPtr> row;
        while (!Check(TokenType::RIGHT_PAREN) && !IsAtEnd()) {
            row.push_back(ParseExpression());
            if (!Match(TokenType::COMMA)) break;
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' after VALUES row");
        stmt->values_list.push_back(std::move(row));
    } while (Match(TokenType::COMMA));
    return stmt;
}

StatementPtr Parser::ParseUpdateStatement() {
    Expect(TokenType::KEYWORD_UPDATE, "expected UPDATE");
    Token table = Expect(TokenType::IDENTIFIER, "expected table name");
    auto stmt = std::make_shared<UpdateStatement>();
    stmt->table_name = table.lexeme;
    Expect(TokenType::KEYWORD_SET, "expected SET");
    do {
        Token col = Expect(TokenType::IDENTIFIER, "expected column name");
        Expect(TokenType::OP_EQUAL, "expected '=' in assignment");
        ExprPtr expr = ParseExpression();
        stmt->assignments.push_back({col.lexeme, expr});
    } while (Match(TokenType::COMMA));
    if (Check(TokenType::KEYWORD_WHERE)) {
        stmt->where_clause = ParseWhereClause();
    }
    return stmt;
}

StatementPtr Parser::ParseDeleteStatement() {
    Expect(TokenType::KEYWORD_DELETE, "expected DELETE");
    Expect(TokenType::KEYWORD_FROM, "expected FROM");
    Token table = Expect(TokenType::IDENTIFIER, "expected table name");
    auto stmt = std::make_shared<DeleteStatement>();
    stmt->table_name = table.lexeme;
    if (Check(TokenType::KEYWORD_WHERE)) {
        stmt->where_clause = ParseWhereClause();
    }
    return stmt;
}

StatementPtr Parser::ParseCreateTableStatement() {
    Expect(TokenType::KEYWORD_CREATE, "expected CREATE");
    Expect(TokenType::KEYWORD_TABLE, "expected TABLE");
    Token table = Expect(TokenType::IDENTIFIER, "expected table name");
    auto stmt = std::make_shared<CreateTableStatement>();
    stmt->table_name = table.lexeme;
    Expect(TokenType::LEFT_PAREN, "expected '(' after table name");
    stmt->columns = ParseColumnDefinitions();
    Expect(TokenType::RIGHT_PAREN, "expected ')' after column definitions");
    return stmt;
}

StatementPtr Parser::ParseDropTableStatement() {
    Expect(TokenType::KEYWORD_DROP, "expected DROP");
    Expect(TokenType::KEYWORD_TABLE, "expected TABLE");
    Token table = Expect(TokenType::IDENTIFIER, "expected table name");
    auto stmt = std::make_shared<DropTableStatement>();
    stmt->table_name = table.lexeme;
    return stmt;
}

// ================= 子句解析 =================

std::vector<ExprPtr> Parser::ParseSelectList() {
    std::vector<ExprPtr> list;
    if (Match(TokenType::OP_STAR)) {
        // Represent "*" as a special FunctionCallExpr with name "*"
        auto star = std::make_shared<FunctionCallExpr>("*", std::vector<ExprPtr>{});
        list.push_back(star);
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
    while (Check(TokenType::KEYWORD_INNER) ||
           Check(TokenType::KEYWORD_LEFT)  ||
           Check(TokenType::KEYWORD_RIGHT) ||
           Check(TokenType::KEYWORD_JOIN)) {
        joins.push_back(ParseJoinClause());
    }
    return joins;
}

JoinClause Parser::ParseJoinClause() {
    JoinClause jc;
    jc.join_type = JoinType::INNER;
    if (Match(TokenType::KEYWORD_INNER)) {
        jc.join_type = JoinType::INNER;
    } else if (Match(TokenType::KEYWORD_LEFT)) {
        jc.join_type = JoinType::LEFT;
    } else if (Match(TokenType::KEYWORD_RIGHT)) {
        jc.join_type = JoinType::RIGHT;
    }
    Expect(TokenType::KEYWORD_JOIN, "expected JOIN");
    Token t = Expect(TokenType::IDENTIFIER, "expected joined table name");
    jc.table_name = t.lexeme;
    Expect(TokenType::KEYWORD_ON, "expected ON");
    jc.on_condition = ParseExpression();
    return jc;
}

ExprPtr Parser::ParseWhereClause() {
    Expect(TokenType::KEYWORD_WHERE, "expected WHERE");
    return ParseExpression();
}

std::vector<ExprPtr> Parser::ParseGroupByClause() {
    Expect(TokenType::KEYWORD_GROUP, "expected GROUP");
    Expect(TokenType::KEYWORD_BY, "expected BY");
    std::vector<ExprPtr> group;
    group.push_back(ParseExpression());
    while (Match(TokenType::COMMA)) {
        group.push_back(ParseExpression());
    }
    return group;
}

ExprPtr Parser::ParseHavingClause() {
    Expect(TokenType::KEYWORD_HAVING, "expected HAVING");
    return ParseExpression();
}

std::vector<OrderByItem> Parser::ParseOrderByClause() {
    Expect(TokenType::KEYWORD_ORDER, "expected ORDER");
    Expect(TokenType::KEYWORD_BY, "expected BY");
    std::vector<OrderByItem> items;
    do {
        OrderByItem it;
        it.expr = ParseExpression();
        if (Match(TokenType::KEYWORD_BY)) {
            // shouldn't really happen
        }
        // ASC / DESC keyword detection: BY is used for ORDER BY grouping, not here.
        // We treat any trailing ASC/DESC keywords as order direction.
        items.push_back(it);
    } while (false); // single pass; multi-ordering can be added later
    // Note: re-parse properly handling comma-separated and ASC/DESC
    items.clear();
    do {
        OrderByItem it;
        it.expr = ParseExpression();
        if (Check(TokenType::KEYWORD_BY)) {
            // ambiguous - probably means ASC (default), so consume nothing more
        } else if (Match(TokenType::IDENTIFIER)) {
            // Not a keyword; treat identifier as... don't decrement. Stop here.
            // We can't really un-advance. This is a known limitation.
        }
        // The original grammar in README supports ASC/DESC keywords but they aren't
        // TokenType keywords - they're just identifiers. We accept either
        // a special token-like identifier "ASC"/"DESC" treated as ascending flag.
        items.push_back(it);
    } while (false);

    // Best-effort: rewrite using proper ASC/DESC handling with identifier checks.
    items.clear();
    do {
        OrderByItem it;
        it.expr = ParseExpression();
        if (CurrentToken().type == TokenType::IDENTIFIER) {
            std::string up = CurrentToken().lexeme;
            for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (up == "ASC") {
                it.ascending = true;
                Advance();
            } else if (up == "DESC") {
                it.ascending = false;
                Advance();
            }
        }
        items.push_back(it);
    } while (Match(TokenType::COMMA));
    return items;
}

int Parser::ParseLimitClause() {
    Expect(TokenType::KEYWORD_LIMIT, "expected LIMIT");
    Token n = Expect(TokenType::INTEGER_LITERAL, "expected integer after LIMIT");
    return std::atoi(n.lexeme.c_str());
}

std::vector<ColumnDefinition> Parser::ParseColumnDefinitions() {
    std::vector<ColumnDefinition> cols;
    cols.push_back(ParseColumnDefinition());
    while (Match(TokenType::COMMA)) {
        cols.push_back(ParseColumnDefinition());
    }
    return cols;
}

ColumnDefinition Parser::ParseColumnDefinition() {
    ColumnDefinition cd;
    Token name = Expect(TokenType::IDENTIFIER, "expected column name");
    cd.column_name = name.lexeme;
    const Token& ty = CurrentToken();
    if (ty.type == TokenType::KEYWORD_INT) {
        cd.data_type = "INT";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_VARCHAR) {
        cd.data_type = "VARCHAR";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_FLOAT) {
        cd.data_type = "FLOAT";
        Advance();
    } else if (ty.type == TokenType::IDENTIFIER) {
        cd.data_type = ty.lexeme;
        Advance();
    } else {
        throw CompilerException(ErrorStage::SYNTAX,
            "expected column type", ty.line, ty.column);
    }
    if (Check(TokenType::KEYWORD_PRIMARY)) {
        Advance();
        Expect(TokenType::KEYWORD_KEY, "expected KEY after PRIMARY");
        cd.is_primary_key = true;
    }
    if (Check(TokenType::KEYWORD_NOT)) {
        Advance();
        Expect(TokenType::KEYWORD_NULL, "expected NULL after NOT");
        cd.is_not_null = true;
    }
    return cd;
}

// ================= 表达式解析（递归下降） =================

ExprPtr Parser::ParseExpression() {
    return ParseOrExpr();
}

ExprPtr Parser::ParseOrExpr() {
    ExprPtr left = ParseAndExpr();
    while (Check(TokenType::KEYWORD_OR)) {
        Advance();
        ExprPtr right = ParseAndExpr();
        left = std::make_shared<BinaryExpr>(BinaryOperator::OR, left, right);
    }
    return left;
}

ExprPtr Parser::ParseAndExpr() {
    ExprPtr left = ParseNotExpr();
    while (Check(TokenType::KEYWORD_AND)) {
        Advance();
        ExprPtr right = ParseNotExpr();
        left = std::make_shared<BinaryExpr>(BinaryOperator::AND, left, right);
    }
    return left;
}

ExprPtr Parser::ParseNotExpr() {
    if (Match(TokenType::KEYWORD_NOT)) {
        ExprPtr operand = ParseNotExpr();
        return std::make_shared<UnaryExpr>(UnaryOperator::NOT, operand);
    }
    return ParseComparisonExpr();
}

ExprPtr Parser::ParseComparisonExpr() {
    ExprPtr left = ParseAdditiveExpr();
    const Token& cur = CurrentToken();
    BinaryOperator op;
    bool matched = true;
    switch (cur.type) {
        case TokenType::OP_EQUAL:         op = BinaryOperator::EQUAL; break;
        case TokenType::OP_NOT_EQUAL:     op = BinaryOperator::NOT_EQUAL; break;
        case TokenType::OP_LESS:          op = BinaryOperator::LESS; break;
        case TokenType::OP_LESS_EQUAL:    op = BinaryOperator::LESS_EQUAL; break;
        case TokenType::OP_GREATER:       op = BinaryOperator::GREATER; break;
        case TokenType::OP_GREATER_EQUAL: op = BinaryOperator::GREATER_EQUAL; break;
        default: matched = false; break;
    }
    if (!matched) return left;
    Advance();
    ExprPtr right = ParseAdditiveExpr();
    return std::make_shared<BinaryExpr>(op, left, right);
}

ExprPtr Parser::ParseAdditiveExpr() {
    ExprPtr left = ParseMultiplicativeExpr();
    while (Check(TokenType::OP_PLUS) || Check(TokenType::OP_MINUS)) {
        BinaryOperator op = Check(TokenType::OP_PLUS) ? BinaryOperator::ADD : BinaryOperator::SUB;
        Advance();
        ExprPtr right = ParseMultiplicativeExpr();
        left = std::make_shared<BinaryExpr>(op, left, right);
    }
    return left;
}

ExprPtr Parser::ParseMultiplicativeExpr() {
    ExprPtr left = ParseUnaryExpr();
    while (Check(TokenType::OP_STAR) || Check(TokenType::OP_SLASH)) {
        BinaryOperator op = Check(TokenType::OP_STAR) ? BinaryOperator::MUL : BinaryOperator::DIV;
        Advance();
        ExprPtr right = ParseUnaryExpr();
        left = std::make_shared<BinaryExpr>(op, left, right);
    }
    return left;
}

ExprPtr Parser::ParseUnaryExpr() {
    if (Match(TokenType::OP_MINUS)) {
        ExprPtr operand = ParseUnaryExpr();
        return std::make_shared<UnaryExpr>(UnaryOperator::NEGATE, operand);
    }
    return ParsePrimaryExpr();
}

ExprPtr Parser::ParsePrimaryExpr() {
    const Token& cur = CurrentToken();
    if (cur.type == TokenType::LEFT_PAREN) {
        Advance();
        ExprPtr inner = ParseExpression();
        Expect(TokenType::RIGHT_PAREN, "expected ')' after expression");
        return inner;
    }
    if (cur.type == TokenType::INTEGER_LITERAL) {
        Advance();
        return std::make_shared<LiteralExpr>(LiteralType::INTEGER, cur.lexeme);
    }
    if (cur.type == TokenType::FLOAT_LITERAL) {
        Advance();
        return std::make_shared<LiteralExpr>(LiteralType::FLOAT, cur.lexeme);
    }
    if (cur.type == TokenType::STRING_LITERAL) {
        Advance();
        return std::make_shared<LiteralExpr>(LiteralType::STRING, cur.lexeme);
    }
    if (cur.type == TokenType::KEYWORD_NULL) {
        Advance();
        return std::make_shared<LiteralExpr>(LiteralType::NULL_VALUE, "NULL");
    }
    if (cur.type == TokenType::IDENTIFIER) {
        return ParseColumnRefOrFunctionCall();
    }
    throw CompilerException(ErrorStage::SYNTAX,
        "unexpected token in expression: '" + cur.lexeme + "'",
        cur.line, cur.column);
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
    Token first = Expect(TokenType::IDENTIFIER, "expected identifier");
    if (Match(TokenType::LEFT_PAREN)) {
        // Function call
        std::vector<ExprPtr> args;
        if (!Check(TokenType::RIGHT_PAREN)) {
            // Handle COUNT(*)
            if (Check(TokenType::OP_STAR)) {
                Advance();
                args.push_back(std::make_shared<LiteralExpr>(LiteralType::INTEGER, "1"));
            } else {
                args = ParseExpressionList();
            }
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' after function arguments");
        return std::make_shared<FunctionCallExpr>(first.lexeme, args);
    }
    if (Match(TokenType::DOT)) {
        // Could be t.column or t.*
        if (Check(TokenType::OP_STAR)) {
            Advance();
            return std::make_shared<FunctionCallExpr>("*", std::vector<ExprPtr>{});
        }
        Token second = Expect(TokenType::IDENTIFIER, "expected column name after '.'");
        return std::make_shared<ColumnRefExpr>(first.lexeme, second.lexeme);
    }
    return std::make_shared<ColumnRefExpr>("", first.lexeme);
}

}  // namespace sqlcompiler