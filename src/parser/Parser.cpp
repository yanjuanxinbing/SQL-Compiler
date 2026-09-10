#include <cstdio>
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
        case TokenType::KEYWORD_CREATE: {
            // CREATE 后面可能是 TABLE 或 [UNIQUE] INDEX，需要前瞻一个 token
            const Token& next = PeekToken(1);
            if (next.type == TokenType::KEYWORD_INDEX ||
                next.type == TokenType::KEYWORD_UNIQUE) {
                return ParseCreateIndexStatement();
            }
            return ParseCreateTableStatement();
        }
        case TokenType::KEYWORD_DROP: {
            if (PeekToken(1).type == TokenType::KEYWORD_INDEX) {
                return ParseDropIndexStatement();
            }
            return ParseDropTableStatement();
        }
        case TokenType::KEYWORD_TRUNCATE: {
            // TRUNCATE TABLE x：清空表中的所有数据，但保留表结构
            Advance(); // TRUNCATE
            Expect(TokenType::KEYWORD_TABLE, "expected TABLE after TRUNCATE");
            Token t = Expect(TokenType::IDENTIFIER, "expected table name");
            auto stmt = std::make_shared<TruncateTableStatement>();
            stmt->table_name = t.lexeme;
            return stmt;
        }
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
    // 解析 select list 并同步收集别名（每个表达式后可有 AS 或隐式别名）
    stmt->select_list.clear();
    stmt->select_aliases.clear();
    auto parse_alias = [&]() -> std::string {
        std::string alias;
        if (Match(TokenType::KEYWORD_AS)) {
            Token a = Expect(TokenType::IDENTIFIER, "expected alias name after AS");
            alias = a.lexeme;
        } else if (CurrentToken().type == TokenType::IDENTIFIER &&
                   !Check(TokenType::KEYWORD_FROM) && !Check(TokenType::KEYWORD_WHERE) &&
                   !Check(TokenType::KEYWORD_GROUP) && !Check(TokenType::KEYWORD_HAVING) &&
                   !Check(TokenType::KEYWORD_ORDER) && !Check(TokenType::KEYWORD_LIMIT) &&
                   !Check(TokenType::COMMA) && !Check(TokenType::SEMICOLON) &&
                   !Check(TokenType::RIGHT_PAREN) && !Check(TokenType::KEYWORD_INNER) &&
                   !Check(TokenType::KEYWORD_LEFT) && !Check(TokenType::KEYWORD_RIGHT) &&
                   !Check(TokenType::KEYWORD_JOIN) && !Check(TokenType::KEYWORD_ON) &&
                   !Check(TokenType::KEYWORD_AS) && !IsAtEnd()) {
            alias = CurrentToken().lexeme;
            Advance();
        }
        return alias;
    };
        do {
        ExprPtr e;
        if (Check(TokenType::OP_STAR)) {
            Advance();
            e = std::make_shared<FunctionCallExpr>("*", std::vector<ExprPtr>{});
        } else {
            e = ParseExpression();
        }
        stmt->select_list.push_back(e);
        stmt->select_aliases.push_back(parse_alias());
    } while (Match(TokenType::COMMA));
    if (Check(TokenType::KEYWORD_FROM)) {
        Advance();
        Token table = Expect(TokenType::IDENTIFIER, "expected table name");
        stmt->from_table = table.lexeme;
        if (Match(TokenType::KEYWORD_AS)) {
            Token a = Expect(TokenType::IDENTIFIER, "expected table alias");
            stmt->from_table_alias = a.lexeme;
        } else if (CurrentToken().type == TokenType::IDENTIFIER &&
                   !Check(TokenType::KEYWORD_WHERE) && !Check(TokenType::KEYWORD_INNER) &&
                   !Check(TokenType::KEYWORD_LEFT) && !Check(TokenType::KEYWORD_RIGHT) &&
                   !Check(TokenType::KEYWORD_JOIN) && !Check(TokenType::KEYWORD_GROUP) &&
                   !Check(TokenType::KEYWORD_HAVING) && !Check(TokenType::KEYWORD_ORDER) &&
                   !Check(TokenType::KEYWORD_LIMIT) &&
                   !Check(TokenType::SEMICOLON) && !IsAtEnd()) {
            stmt->from_table_alias = CurrentToken().lexeme;
            Advance();
        }
        stmt->joins = ParseJoinClauses();
    }
    if (Check(TokenType::KEYWORD_WHERE)) stmt->where_clause = ParseWhereClause();
    if (Check(TokenType::KEYWORD_GROUP)) stmt->group_by = ParseGroupByClause();
    if (Check(TokenType::KEYWORD_HAVING)) stmt->having_clause = ParseHavingClause();
    if (Check(TokenType::KEYWORD_ORDER)) stmt->order_by = ParseOrderByClause();
    if (Check(TokenType::KEYWORD_LIMIT)) {
        Advance();
        Token first = Expect(TokenType::INTEGER_LITERAL, "expected integer after LIMIT");
        int first_val = std::atoi(first.lexeme.c_str());
        if (Match(TokenType::COMMA)) {
            stmt->limit_offset = first_val;
            Token second = Expect(TokenType::INTEGER_LITERAL, "expected integer after ','");
            stmt->limit = std::atoi(second.lexeme.c_str());
        } else {
            stmt->limit = first_val;
        }
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
    bool if_not_exists = false;
    if (Check(TokenType::KEYWORD_IF)) {
        Advance();
        Expect(TokenType::KEYWORD_NOT, "expected NOT after IF");
        // EXISTS 作为普通标识符
        if (CurrentToken().type == TokenType::IDENTIFIER &&
            CurrentToken().lexeme == "EXISTS") {
            Advance();
        } else {
            Expect(TokenType::IDENTIFIER, "expected EXISTS after IF NOT");
        }
        if_not_exists = true;
    }
    Token table = Expect(TokenType::IDENTIFIER, "expected table name");
    auto stmt = std::make_shared<CreateTableStatement>();
    stmt->table_name = table.lexeme;
    stmt->if_not_exists = if_not_exists;
    Expect(TokenType::LEFT_PAREN, "expected '(' after table name");
    stmt->columns = ParseColumnDefinitions(*stmt);
    Expect(TokenType::RIGHT_PAREN, "expected ')' after column definitions");
    return stmt;
}

StatementPtr Parser::ParseDropTableStatement() {
    Expect(TokenType::KEYWORD_DROP, "expected DROP");
    Expect(TokenType::KEYWORD_TABLE, "expected TABLE");
    bool if_exists = false;
    if (Check(TokenType::KEYWORD_IF)) {
        Advance();
        // EXISTS 未列入关键字表，按普通标识符处理（与 CREATE ... IF NOT EXISTS 一致）
        if (CurrentToken().type == TokenType::IDENTIFIER &&
            CurrentToken().lexeme == "EXISTS") {
            Advance();
        } else {
            Expect(TokenType::IDENTIFIER, "expected EXISTS after IF");
        }
        if_exists = true;
    }
    Token table = Expect(TokenType::IDENTIFIER, "expected table name");
    auto stmt = std::make_shared<DropTableStatement>();
    stmt->table_name = table.lexeme;
    stmt->if_exists = if_exists;
    return stmt;
}

StatementPtr Parser::ParseCreateIndexStatement() {
    Expect(TokenType::KEYWORD_CREATE, "expected CREATE");
    auto stmt = std::make_shared<CreateIndexStatement>();
    if (Match(TokenType::KEYWORD_UNIQUE)) {
        stmt->is_unique = true;
    }
    Expect(TokenType::KEYWORD_INDEX, "expected INDEX");
    Token name = Expect(TokenType::IDENTIFIER, "expected index name");
    stmt->index_name = name.lexeme;
    Expect(TokenType::KEYWORD_ON, "expected ON after index name");
    Token table = Expect(TokenType::IDENTIFIER, "expected table name");
    stmt->table_name = table.lexeme;
    Expect(TokenType::LEFT_PAREN, "expected '(' before index column list");
    do {
        Token col = Expect(TokenType::IDENTIFIER, "expected column name");
        stmt->key_columns.push_back(col.lexeme);
    } while (Match(TokenType::COMMA));
    Expect(TokenType::RIGHT_PAREN, "expected ')' after index column list");
    return stmt;
}

StatementPtr Parser::ParseDropIndexStatement() {
    Expect(TokenType::KEYWORD_DROP, "expected DROP");
    Expect(TokenType::KEYWORD_INDEX, "expected INDEX");
    auto stmt = std::make_shared<DropIndexStatement>();
    if (Check(TokenType::KEYWORD_IF)) {
        Advance();
        // EXISTS 未列入关键字表，按普通标识符处理（与 DROP TABLE IF EXISTS 一致）
        if (CurrentToken().type == TokenType::IDENTIFIER &&
            CurrentToken().lexeme == "EXISTS") {
            Advance();
        } else {
            Expect(TokenType::IDENTIFIER, "expected EXISTS after IF");
        }
        stmt->if_exists = true;
    }
    Token name = Expect(TokenType::IDENTIFIER, "expected index name");
    stmt->index_name = name.lexeme;
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
        if (Check(TokenType::OP_STAR)) {
            Advance();
            list.push_back(std::make_shared<FunctionCallExpr>("*", std::vector<ExprPtr>{}));
            continue;
        }
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
    // 表可名（user u INNER JOIN)
    if (CurrentToken().type == TokenType::IDENTIFIER &&
        !Check(TokenType::KEYWORD_ON)) {
        jc.table_alias = CurrentToken().lexeme;
        Advance();
    }
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
        // ASC / DESC 既可以是关键字也可以是普通标识符（统一大小写）
        if (CurrentToken().type == TokenType::KEYWORD_ASC ||
            CurrentToken().type == TokenType::KEYWORD_DESC) {
            it.ascending = (CurrentToken().type == TokenType::KEYWORD_ASC);
            Advance();
        } else if (CurrentToken().type == TokenType::IDENTIFIER) {
            std::string up = CurrentToken().lexeme;
            for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (up == "ASC") { it.ascending = true; Advance(); }
            else if (up == "DESC") { it.ascending = false; Advance(); }
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

std::vector<ColumnDefinition> Parser::ParseColumnDefinitions(CreateTableStatement& stmt) {
    std::vector<ColumnDefinition> cols;
    auto parse_table_pk = [&]() {
        // 当前 token 已是 KEYWORD_PRIMARY；语法形式：PRIMARY KEY (col1, col2, ...)
        Advance();  // PRIMARY
        Expect(TokenType::KEYWORD_KEY, "expected KEY after PRIMARY");
        Expect(TokenType::LEFT_PAREN, "expected '(' after PRIMARY KEY");
        std::vector<std::string> pk_cols;
        Token c = Expect(TokenType::IDENTIFIER, "expected column name in PRIMARY KEY");
        pk_cols.push_back(c.lexeme);
        while (Match(TokenType::COMMA)) {
            Token cc = Expect(TokenType::IDENTIFIER, "expected column name in PRIMARY KEY");
            pk_cols.push_back(cc.lexeme);
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' after PRIMARY KEY column list");
        stmt.primary_keys.push_back(std::move(pk_cols));
    };

    // First element may be either a column definition or a table-level PK constraint.
    if (Check(TokenType::KEYWORD_PRIMARY)) {
        parse_table_pk();
    } else {
        cols.push_back(ParseColumnDefinition());
    }
    while (Match(TokenType::COMMA)) {
        if (Check(TokenType::KEYWORD_PRIMARY)) {
            parse_table_pk();
        } else {
            cols.push_back(ParseColumnDefinition());
        }
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
    // 可选类型参数：VARCHAR(N) / CHAR(N) 等
    if (Match(TokenType::LEFT_PAREN)) {
        // 记录长度上限，供 INSERT/UPDATE 时做长度约束校验
        if (CurrentToken().type == TokenType::INTEGER_LITERAL) {
            try {
                cd.char_length = static_cast<int32_t>(std::stol(CurrentToken().lexeme));
            } catch (...) {
                cd.char_length = -1;
            }
            Advance();
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' after type parameter");
    }
    auto skip_auto_inc = [&]() {
        if (CurrentToken().type == TokenType::IDENTIFIER &&
            CurrentToken().lexeme == "AUTO_INCREMENT") {
            Advance();
        }
    };
    skip_auto_inc();
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
    skip_auto_inc();
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

    // IS [NOT] NULL (postfix)
    if (Check(TokenType::KEYWORD_IS)) {
        Advance();
        bool is_not = Check(TokenType::KEYWORD_NOT);
        if (is_not) Advance();
        Expect(TokenType::KEYWORD_NULL, "expected NULL after IS [NOT]");
        return std::make_shared<BinaryExpr>(
            is_not ? BinaryOperator::IS_NOT_NULL : BinaryOperator::IS_NULL,
            left, nullptr);
    }

    // LIKE <pattern>
    if (Check(TokenType::KEYWORD_LIKE)) {
        Advance();
        ExprPtr right = ParseAdditiveExpr();
        return std::make_shared<BinaryExpr>(BinaryOperator::LIKE, left, right);
    }

    // IN (val1, val2, ...)
    if (Check(TokenType::KEYWORD_IN)) {
        Advance();
        Expect(TokenType::LEFT_PAREN, "expected '(' after IN");
        std::vector<ExprPtr> values;
        if (!Check(TokenType::RIGHT_PAREN)) {
            values.push_back(ParseExpression());
            while (Match(TokenType::COMMA)) {
                values.push_back(ParseExpression());
            }
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' after IN list");
        // Encode as BinaryExpr with IN_LIST and a special list operand:
        // We wrap the values list in a synthetic FunctionCallExpr so the
        // expression evaluator can iterate over them.
        auto list_expr = std::make_shared<FunctionCallExpr>("__IN_LIST__", values);
        return std::make_shared<BinaryExpr>(BinaryOperator::IN_LIST, left, list_expr);
    }

    // BETWEEN x AND y
    if (Check(TokenType::KEYWORD_BETWEEN)) {
        Advance();
        ExprPtr low = ParseAdditiveExpr();
        Expect(TokenType::KEYWORD_AND, "expected AND after BETWEEN");
        ExprPtr high = ParseAdditiveExpr();
        // Encode BETWEEN as a synthetic BinaryExpr with BETWEEN and high
        // being the original right operand; low stored as a side info via
        // a FunctionCallExpr wrapping {low, high}.
        auto range = std::make_shared<FunctionCallExpr>("__BETWEEN_RANGE__",
            std::vector<ExprPtr>{low, high});
        return std::make_shared<BinaryExpr>(BinaryOperator::BETWEEN, left, range);
    }

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
    while (Check(TokenType::OP_PLUS) || Check(TokenType::OP_MINUS) ||
           Check(TokenType::OP_CONCAT)) {
        BinaryOperator op = Check(TokenType::OP_PLUS)  ? BinaryOperator::ADD
                          : Check(TokenType::OP_MINUS) ? BinaryOperator::SUB
                                                       : BinaryOperator::CONCAT;
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
        bool distinct = false;
        if (!Check(TokenType::RIGHT_PAREN)) {
            // Handle COUNT(*) — DISTINCT 与 * 互斥
            if (Check(TokenType::KEYWORD_DISTINCT)) {
                Advance();
                distinct = true;
                args = ParseExpressionList();
            } else if (Check(TokenType::OP_STAR)) {
                Advance();
                args.push_back(std::make_shared<LiteralExpr>(LiteralType::INTEGER, "1"));
            } else {
                args = ParseExpressionList();
            }
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' after function arguments");
        auto fc = std::make_shared<FunctionCallExpr>(first.lexeme, args);
        fc->is_distinct = distinct;
        return fc;
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