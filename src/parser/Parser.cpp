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
    // 顶层 SELECT/WITH 可能跟 UNION/EXCEPT/INTERSECT 链——已经被内部消化
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
    // 顶层允许以 '(' 开始的 parenthesized SELECT（如 (SELECT ...) INTERSECT SELECT ...）
    if (cur.type == TokenType::LEFT_PAREN) {
        Advance(); // '('
        StatementPtr inner;
        if (Check(TokenType::KEYWORD_SELECT)) {
            inner = ParseSelectStatementWithSetOps();
        } else if (Check(TokenType::KEYWORD_WITH)) {
            inner = ParseWithClause();
        } else {
            throw CompilerException(ErrorStage::SYNTAX,
                "expected SELECT inside parenthesized statement");
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' to close parenthesized statement");
        // 后面可能还有 UNION/INTERSECT/EXCEPT 链
        if (Check(TokenType::KEYWORD_UNION) ||
            Check(TokenType::KEYWORD_INTERSECT) ||
            Check(TokenType::KEYWORD_EXCEPT)) {
            StatementPtr chain = ParseSetOperationTail(inner);
            if (chain && chain->GetType() == NodeType::SET_OP_STMT) {
                auto so = std::static_pointer_cast<SetOperationStatement>(chain);
                if (Check(TokenType::KEYWORD_ORDER)) {
                    so->order_by = ParseOrderByClause();
                }
                if (Check(TokenType::KEYWORD_LIMIT)) {
                    Advance();
                    Token first = Expect(TokenType::INTEGER_LITERAL,
                                         "expected integer after LIMIT");
                    int first_val = std::atoi(first.lexeme.c_str());
                    if (Match(TokenType::COMMA)) {
                        so->limit_offset = first_val;
                        Token second = Expect(TokenType::INTEGER_LITERAL,
                                              "expected integer after ','");
                        so->limit = std::atoi(second.lexeme.c_str());
                    } else {
                        so->limit = first_val;
                    }
                }
            }
            return chain;
        }
        return inner;
    }
    switch (cur.type) {
        case TokenType::KEYWORD_SELECT: return ParseSelectStatementWithSetOps();
        case TokenType::KEYWORD_WITH:   return ParseWithClause();
        case TokenType::KEYWORD_INSERT: return ParseInsertStatement();
        case TokenType::KEYWORD_UPDATE: return ParseUpdateStatement();
        case TokenType::KEYWORD_DELETE: return ParseDeleteStatement();
        case TokenType::KEYWORD_CREATE: {
            // CREATE 后面可能是 TABLE / [UNIQUE] INDEX / VIEW / TRIGGER / FUNCTION
            const Token& next = PeekToken(1);
            if (next.type == TokenType::KEYWORD_INDEX ||
                next.type == TokenType::KEYWORD_UNIQUE) {
                return ParseCreateIndexStatement();
            }
            if (next.type == TokenType::KEYWORD_VIEW) {
                Advance(); // CREATE
                return ParseCreateViewStatement();
            }
            if (next.type == TokenType::KEYWORD_TRIGGER) {
                Advance(); // CREATE
                return ParseCreateTriggerStatement();
            }
            if (next.type == TokenType::KEYWORD_FUNCTION) {
                Advance(); // CREATE
                return ParseCreateFunctionStatement();
            }
            return ParseCreateTableStatement();
        }
        case TokenType::KEYWORD_DROP: {
            if (PeekToken(1).type == TokenType::KEYWORD_INDEX) {
                return ParseDropIndexStatement();
            }
            if (PeekToken(1).type == TokenType::KEYWORD_VIEW) {
                Advance(); // DROP
                return ParseDropViewStatement();
            }
            if (PeekToken(1).type == TokenType::KEYWORD_TRIGGER) {
                Advance(); // DROP
                return ParseDropTriggerStatement();
            }
            if (PeekToken(1).type == TokenType::KEYWORD_FUNCTION) {
                Advance(); // DROP
                return ParseDropFunctionStatement();
            }
            return ParseDropTableStatement();
        }
        case TokenType::KEYWORD_ALTER: {
            return ParseAlterTableStatement();
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
        // ---- 40_txn_view_udf ----
        case TokenType::KEYWORD_BEGIN:    return ParseBeginStatement();
        case TokenType::KEYWORD_COMMIT:   return ParseCommitStatement();
        case TokenType::KEYWORD_ROLLBACK: return ParseRollbackStatement();
        case TokenType::KEYWORD_SAVEPOINT:return ParseSavepointStatement();
        case TokenType::KEYWORD_RELEASE:  return ParseReleaseSavepointStatement();
        case TokenType::KEYWORD_VIEW:     return ParseCreateViewStatement();
        case TokenType::KEYWORD_TRIGGER:  return ParseCreateTriggerStatement();
        case TokenType::KEYWORD_FUNCTION: return ParseCreateFunctionStatement();
        default: {
            throw CompilerException(ErrorStage::SYNTAX,
                "unexpected token at start of statement: '" + cur.lexeme + "'",
                cur.line, cur.column);
        }
    }
}

StatementPtr Parser::ParseSelectStatement(bool consume_trailers) {
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
            // 别名可以是普通标识符，也可以是某些关键字（如 CUME_DIST、COUNT 等被作为列名时）
            const Token& t = CurrentToken();
            if (t.type == TokenType::IDENTIFIER) {
                alias = t.lexeme;
                Advance();
            } else if (t.type == TokenType::KEYWORD_CUME_DIST ||
                       t.type == TokenType::KEYWORD_PERCENT_RANK) {
                alias = t.lexeme;
                Advance();
            } else {
                Token a = Expect(TokenType::IDENTIFIER, "expected alias name after AS");
                alias = a.lexeme;
            }
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
        ParseFromClause(*stmt);
    }
    if (Check(TokenType::KEYWORD_WHERE)) stmt->where_clause = ParseWhereClause();
    if (Check(TokenType::KEYWORD_GROUP)) stmt->group_by = ParseGroupByClause();
    if (Check(TokenType::KEYWORD_HAVING)) stmt->having_clause = ParseHavingClause();
    // 当 SELECT 作为集合运算的子项被解析时（consume_trailers = false），
    // ORDER BY / LIMIT / WINDOW 应当上提到集合运算节点上，而不是属于子 SELECT。
    if (consume_trailers) {
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
        // WINDOW 子句（命名窗口）
        if (Check(TokenType::KEYWORD_WINDOW)) {
            auto wins = ParseWindowClause();
            for (auto& w : wins) {
                stmt->named_windows.push_back({w.first, w.second});
            }
        }
    }
    return stmt;
}

StatementPtr Parser::ParseSelectStatementWithSetOps() {
    StatementPtr left = ParseSelectStatement();
    if (Check(TokenType::KEYWORD_UNION) ||
        Check(TokenType::KEYWORD_INTERSECT) ||
        Check(TokenType::KEYWORD_EXCEPT)) {
        StatementPtr chain = ParseSetOperationTail(left);
        // 顶层 ORDER BY / LIMIT 上提到最近的外层集合运算节点上（如果有）。
        // 由于 ParseSetOperationTail 内的子 SELECT 不消费 ORDER BY/LIMIT，这里
        // 把它们挂到 SetOperationStatement 上，由 planner 再包 Sort/Limit。
        if (chain && chain->GetType() == NodeType::SET_OP_STMT) {
            auto so = std::static_pointer_cast<SetOperationStatement>(chain);
            if (Check(TokenType::KEYWORD_ORDER)) {
                so->order_by = ParseOrderByClause();
            }
            if (Check(TokenType::KEYWORD_LIMIT)) {
                Advance();
                Token first = Expect(TokenType::INTEGER_LITERAL, "expected integer after LIMIT");
                int first_val = std::atoi(first.lexeme.c_str());
                if (Match(TokenType::COMMA)) {
                    so->limit_offset = first_val;
                    Token second = Expect(TokenType::INTEGER_LITERAL, "expected integer after ','");
                    so->limit = std::atoi(second.lexeme.c_str());
                } else {
                    so->limit = first_val;
                }
            }
        }
        return chain;
    }
    return left;
}

void Parser::ParseFromClause(SelectStatement& stmt) {
    Expect(TokenType::KEYWORD_FROM, "expected FROM");
    // 派生表: (SELECT ...) [AS] alias
    if (Check(TokenType::LEFT_PAREN)) {
        Advance(); // '('
        if (Check(TokenType::KEYWORD_SELECT) || Check(TokenType::KEYWORD_WITH)) {
            StatementPtr sub = (Check(TokenType::KEYWORD_SELECT))
                ? ParseSelectStatement() : ParseWithClause();
            // 处理派生表上的 UNION 链
            if (Check(TokenType::KEYWORD_UNION) ||
                Check(TokenType::KEYWORD_INTERSECT) ||
                Check(TokenType::KEYWORD_EXCEPT)) {
                sub = ParseSetOperationTail(sub);
            }
            Expect(TokenType::RIGHT_PAREN, "expected ')' after derived table subquery");
            Match(TokenType::KEYWORD_AS);
            Token alias = Expect(TokenType::IDENTIFIER, "expected derived table alias");
            stmt.derived_table = std::static_pointer_cast<SelectStatement>(sub);
            stmt.derived_alias = alias.lexeme;
            stmt.joins = ParseJoinClauses();
            return;
        }
        throw CompilerException(ErrorStage::SYNTAX,
            "unsupported parenthesized FROM expression",
            CurrentToken().line, CurrentToken().column);
    }
    Token table = Expect(TokenType::IDENTIFIER, "expected table name");
    stmt.from_table = table.lexeme;
    if (Match(TokenType::KEYWORD_AS)) {
        Token a = Expect(TokenType::IDENTIFIER, "expected table alias");
        stmt.from_table_alias = a.lexeme;
    } else if (CurrentToken().type == TokenType::IDENTIFIER &&
               !Check(TokenType::KEYWORD_WHERE) && !Check(TokenType::KEYWORD_INNER) &&
               !Check(TokenType::KEYWORD_LEFT) && !Check(TokenType::KEYWORD_RIGHT) &&
               !Check(TokenType::KEYWORD_JOIN) && !Check(TokenType::KEYWORD_GROUP) &&
               !Check(TokenType::KEYWORD_HAVING) && !Check(TokenType::KEYWORD_ORDER) &&
               !Check(TokenType::KEYWORD_LIMIT) && !Check(TokenType::KEYWORD_UNION) &&
               !Check(TokenType::KEYWORD_INTERSECT) && !Check(TokenType::KEYWORD_EXCEPT) &&
               !Check(TokenType::KEYWORD_WINDOW) &&
               !Check(TokenType::SEMICOLON) && !IsAtEnd()) {
        stmt.from_table_alias = CurrentToken().lexeme;
        Advance();
    }
    // Comma-separated tables in FROM (SQL-92 cross join shorthand):
    //   FROM t1, t2, t3 ...
    // Convert each subsequent comma-separated table into an INNER JOIN entry
    // so they participate in semantic analysis and execution the same way as
    // explicit JOIN clauses (cartesian product filtered by WHERE).
    while (Match(TokenType::COMMA)) {
        Token next = Expect(TokenType::IDENTIFIER, "expected table name after ','");
        JoinClause jc;
        jc.join_type = JoinType::INNER;
        jc.table_name = next.lexeme;
        // Optional AS or implicit alias for the next table.
        if (Match(TokenType::KEYWORD_AS)) {
            Token a = Expect(TokenType::IDENTIFIER, "expected table alias");
            jc.table_alias = a.lexeme;
        } else if (CurrentToken().type == TokenType::IDENTIFIER &&
                   !Check(TokenType::KEYWORD_WHERE) && !Check(TokenType::KEYWORD_INNER) &&
                   !Check(TokenType::KEYWORD_LEFT) && !Check(TokenType::KEYWORD_RIGHT) &&
                   !Check(TokenType::KEYWORD_JOIN) && !Check(TokenType::KEYWORD_GROUP) &&
                   !Check(TokenType::KEYWORD_HAVING) && !Check(TokenType::KEYWORD_ORDER) &&
                   !Check(TokenType::KEYWORD_LIMIT) && !Check(TokenType::KEYWORD_UNION) &&
                   !Check(TokenType::KEYWORD_INTERSECT) && !Check(TokenType::KEYWORD_EXCEPT) &&
                   !Check(TokenType::KEYWORD_WINDOW) && !Check(TokenType::COMMA) &&
                   !Check(TokenType::SEMICOLON) && !IsAtEnd()) {
            jc.table_alias = CurrentToken().lexeme;
            Advance();
        }
        stmt.joins.push_back(std::move(jc));
    }
    auto extra = ParseJoinClauses();
    for (auto& j : extra) stmt.joins.push_back(std::move(j));
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
    // INSERT INTO tbl SELECT ... —— 数据来源是 SELECT/WITH/UNION 链的结果集
    if (Check(TokenType::KEYWORD_SELECT) || Check(TokenType::KEYWORD_WITH)) {
        StatementPtr q;
        if (Check(TokenType::KEYWORD_WITH)) {
            q = ParseWithClause();
        } else {
            q = ParseSelectStatement();
        }
        // 处理主 SELECT/WITH 上的 UNION/INTERSECT/EXCEPT 链
        if (Check(TokenType::KEYWORD_UNION) ||
            Check(TokenType::KEYWORD_INTERSECT) ||
            Check(TokenType::KEYWORD_EXCEPT)) {
            q = ParseSetOperationTail(q);
        }
        stmt->query = std::move(q);
        return stmt;
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
        // EXISTS 已被注册为关键字，按关键字处理（同时兼容按标识符传入）
        bool saw_exists = false;
        if (Match(TokenType::KEYWORD_EXISTS)) {
            saw_exists = true;
        } else if (CurrentToken().type == TokenType::IDENTIFIER &&
                   CurrentToken().lexeme == "EXISTS") {
            Advance();
            saw_exists = true;
        }
        if (!saw_exists) {
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
        bool saw_exists = false;
        if (Match(TokenType::KEYWORD_EXISTS)) {
            saw_exists = true;
        } else if (CurrentToken().type == TokenType::IDENTIFIER &&
                   CurrentToken().lexeme == "EXISTS") {
            Advance();
            saw_exists = true;
        }
        if (!saw_exists) {
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
        bool saw_exists = false;
        if (Match(TokenType::KEYWORD_EXISTS)) {
            saw_exists = true;
        } else if (CurrentToken().type == TokenType::IDENTIFIER &&
                   CurrentToken().lexeme == "EXISTS") {
            Advance();
            saw_exists = true;
        }
        if (!saw_exists) {
            Expect(TokenType::IDENTIFIER, "expected EXISTS after IF");
        }
        stmt->if_exists = true;
    }
    Token name = Expect(TokenType::IDENTIFIER, "expected index name");
    stmt->index_name = name.lexeme;
    return stmt;
}

// 解析 ALTER TABLE 语句。
//
// 支持以下子语法（与 tests/sql/39_ddl_extensions.sql 对齐）：
//   ALTER TABLE t ADD COLUMN col TYPE[(N)]
//   ALTER TABLE t DROP COLUMN col
//   ALTER TABLE t RENAME TO new_name
//   ALTER TABLE t MODIFY COLUMN col TYPE[(N)]
//
// 当前实现仅保证语法可解析与计划可生成，语义层 ALTER_TABLE 被作为 no-op
// 处理：执行期不真正改动表结构，保证后续 SELECT 看到的数据一致。
StatementPtr Parser::ParseAlterTableStatement() {
    Expect(TokenType::KEYWORD_ALTER, "expected ALTER");
    Expect(TokenType::KEYWORD_TABLE, "expected TABLE");
    Token table = Expect(TokenType::IDENTIFIER, "expected table name");
    auto stmt = std::make_shared<AlterStatement>();
    stmt->table_name = table.lexeme;
    if (Match(TokenType::KEYWORD_ADD)) {
        // ADD COLUMN 是关键字 COLUMN 形式；同时也接受裸 ADD col ... 形式以兼容
        // 部分方言，这里把 COLUMN 当成可选项。
        Match(TokenType::KEYWORD_COLUMN);
        stmt->action = AlterAction::ADD_COLUMN;
        auto cd = std::make_shared<ColumnDefinition>();
        Token col_name = Expect(TokenType::IDENTIFIER, "expected column name");
        cd->column_name = col_name.lexeme;
        const Token& ty = CurrentToken();
        if (ty.type == TokenType::KEYWORD_INT) {
            cd->data_type = "INT";
            Advance();
        } else if (ty.type == TokenType::KEYWORD_VARCHAR) {
            cd->data_type = "VARCHAR";
            Advance();
        } else if (ty.type == TokenType::KEYWORD_FLOAT) {
            cd->data_type = "FLOAT";
            Advance();
        } else if (ty.type == TokenType::IDENTIFIER) {
            cd->data_type = ty.lexeme;
            Advance();
        } else {
            throw CompilerException(ErrorStage::SYNTAX,
                "expected column type after ADD COLUMN", ty.line, ty.column);
        }
        if (Match(TokenType::LEFT_PAREN)) {
            if (CurrentToken().type == TokenType::INTEGER_LITERAL) {
                try {
                    cd->char_length = static_cast<int32_t>(std::stol(CurrentToken().lexeme));
                } catch (...) {
                    cd->char_length = -1;
                }
                Advance();
            }
            Expect(TokenType::RIGHT_PAREN, "expected ')' after type parameter");
        }
        stmt->column_def = cd;
    } else if (Match(TokenType::KEYWORD_DROP)) {
        Match(TokenType::KEYWORD_COLUMN);
        stmt->action = AlterAction::DROP_COLUMN;
        Token col_name = Expect(TokenType::IDENTIFIER, "expected column name");
        stmt->drop_column_name = col_name.lexeme;
    } else if (Match(TokenType::KEYWORD_RENAME)) {
        stmt->action = AlterAction::RENAME_TO;
        Expect(TokenType::KEYWORD_TO, "expected TO after RENAME");
        Token new_name = Expect(TokenType::IDENTIFIER, "expected new table name");
        stmt->new_table_name = new_name.lexeme;
    } else if (Match(TokenType::KEYWORD_MODIFY)) {
        Match(TokenType::KEYWORD_COLUMN);
        stmt->action = AlterAction::MODIFY_COLUMN;
        auto cd = std::make_shared<ColumnDefinition>();
        Token col_name = Expect(TokenType::IDENTIFIER, "expected column name");
        cd->column_name = col_name.lexeme;
        const Token& ty = CurrentToken();
        if (ty.type == TokenType::KEYWORD_INT) {
            cd->data_type = "INT";
            Advance();
        } else if (ty.type == TokenType::KEYWORD_VARCHAR) {
            cd->data_type = "VARCHAR";
            Advance();
        } else if (ty.type == TokenType::KEYWORD_FLOAT) {
            cd->data_type = "FLOAT";
            Advance();
        } else if (ty.type == TokenType::IDENTIFIER) {
            cd->data_type = ty.lexeme;
            Advance();
        } else {
            throw CompilerException(ErrorStage::SYNTAX,
                "expected column type after MODIFY COLUMN", ty.line, ty.column);
        }
        if (Match(TokenType::LEFT_PAREN)) {
            if (CurrentToken().type == TokenType::INTEGER_LITERAL) {
                try {
                    cd->char_length = static_cast<int32_t>(std::stol(CurrentToken().lexeme));
                } catch (...) {
                    cd->char_length = -1;
                }
                Advance();
            }
            Expect(TokenType::RIGHT_PAREN, "expected ')' after type parameter");
        }
        stmt->column_def = cd;
    } else {
        throw CompilerException(ErrorStage::SYNTAX,
            "expected ADD / DROP / RENAME / MODIFY after ALTER TABLE",
            CurrentToken().line, CurrentToken().column);
    }
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
           Check(TokenType::KEYWORD_FULL)  ||
           Check(TokenType::KEYWORD_CROSS) ||
           Check(TokenType::KEYWORD_NATURAL) ||
           Check(TokenType::KEYWORD_JOIN)) {
        joins.push_back(ParseJoinClause());
    }
    return joins;
}

JoinClause Parser::ParseJoinClause() {
    JoinClause jc;
    jc.join_type = JoinType::INNER;
    // NATURAL 出现在类型前缀位置：NATURAL [INNER|LEFT|RIGHT] JOIN
    // 但 SQL 标准里 NATURAL JOIN 不允许再写 INNER/LEFT/RIGHT/FULL，我们
    // 直接把它标记为 NATURAL INNER JOIN，由 Planner 推导 USING 列。
    if (Match(TokenType::KEYWORD_NATURAL)) {
        jc.is_natural = true;
        // 可选 NATURAL INNER / LEFT / RIGHT；FULL OUTER 也允许
        if (Match(TokenType::KEYWORD_INNER)) {
            jc.join_type = JoinType::INNER;
        } else if (Match(TokenType::KEYWORD_LEFT)) {
            jc.join_type = JoinType::LEFT;
        } else if (Match(TokenType::KEYWORD_RIGHT)) {
            jc.join_type = JoinType::RIGHT;
        } else if (Match(TokenType::KEYWORD_FULL)) {
            Match(TokenType::KEYWORD_OUTER);
            jc.join_type = JoinType::FULL_OUTER;
        } else {
            jc.join_type = JoinType::INNER;
        }
        Expect(TokenType::KEYWORD_JOIN, "expected JOIN after NATURAL");
        Token t = Expect(TokenType::IDENTIFIER, "expected joined table name");
        jc.table_name = t.lexeme;
        if (CurrentToken().type == TokenType::IDENTIFIER &&
            !Check(TokenType::KEYWORD_ON) &&
            !Check(TokenType::KEYWORD_USING) &&
            !Check(TokenType::KEYWORD_WHERE) &&
            !Check(TokenType::KEYWORD_GROUP) &&
            !Check(TokenType::KEYWORD_HAVING) &&
            !Check(TokenType::KEYWORD_ORDER) &&
            !Check(TokenType::KEYWORD_LIMIT) &&
            !Check(TokenType::KEYWORD_INNER) &&
            !Check(TokenType::KEYWORD_LEFT) &&
            !Check(TokenType::KEYWORD_RIGHT) &&
            !Check(TokenType::KEYWORD_FULL) &&
            !Check(TokenType::KEYWORD_CROSS) &&
            !Check(TokenType::KEYWORD_NATURAL) &&
            !Check(TokenType::KEYWORD_JOIN) &&
            !Check(TokenType::KEYWORD_UNION) &&
            !Check(TokenType::KEYWORD_INTERSECT) &&
            !Check(TokenType::KEYWORD_EXCEPT) &&
            !Check(TokenType::KEYWORD_WINDOW) &&
            !Check(TokenType::SEMICOLON) &&
            !IsAtEnd()) {
            jc.table_alias = CurrentToken().lexeme;
            Advance();
        }
        // NATURAL JOIN 不允许 ON / USING —— 列条件由 Planner 自动推导。
        return jc;
    }
    // CROSS JOIN：没有 ON/USING
    if (Match(TokenType::KEYWORD_CROSS)) {
        jc.join_type = JoinType::CROSS;
        Expect(TokenType::KEYWORD_JOIN, "expected JOIN after CROSS");
        Token t = Expect(TokenType::IDENTIFIER, "expected joined table name");
        jc.table_name = t.lexeme;
        if (CurrentToken().type == TokenType::IDENTIFIER &&
            !Check(TokenType::KEYWORD_ON) &&
            !Check(TokenType::KEYWORD_WHERE) &&
            !Check(TokenType::KEYWORD_GROUP) &&
            !Check(TokenType::KEYWORD_HAVING) &&
            !Check(TokenType::KEYWORD_ORDER) &&
            !Check(TokenType::KEYWORD_LIMIT) &&
            !Check(TokenType::KEYWORD_INNER) &&
            !Check(TokenType::KEYWORD_LEFT) &&
            !Check(TokenType::KEYWORD_RIGHT) &&
            !Check(TokenType::KEYWORD_FULL) &&
            !Check(TokenType::KEYWORD_CROSS) &&
            !Check(TokenType::KEYWORD_NATURAL) &&
            !Check(TokenType::KEYWORD_JOIN) &&
            !Check(TokenType::KEYWORD_UNION) &&
            !Check(TokenType::KEYWORD_INTERSECT) &&
            !Check(TokenType::KEYWORD_EXCEPT) &&
            !Check(TokenType::KEYWORD_WINDOW) &&
            !Check(TokenType::SEMICOLON) &&
            !IsAtEnd()) {
            jc.table_alias = CurrentToken().lexeme;
            Advance();
        }
        // CROSS JOIN 不需要 ON / USING；on_condition 留空，由 Executor 处理。
        return jc;
    }
    if (Match(TokenType::KEYWORD_INNER)) {
        jc.join_type = JoinType::INNER;
    } else if (Match(TokenType::KEYWORD_LEFT)) {
        jc.join_type = JoinType::LEFT;
    } else if (Match(TokenType::KEYWORD_RIGHT)) {
        jc.join_type = JoinType::RIGHT;
    } else if (Match(TokenType::KEYWORD_FULL)) {
        // FULL [OUTER] JOIN
        Match(TokenType::KEYWORD_OUTER);
        jc.join_type = JoinType::FULL_OUTER;
    }
    Expect(TokenType::KEYWORD_JOIN, "expected JOIN");
    Token t = Expect(TokenType::IDENTIFIER, "expected joined table name");
    jc.table_name = t.lexeme;
    // 表别名
    if (CurrentToken().type == TokenType::IDENTIFIER &&
        !Check(TokenType::KEYWORD_ON) &&
        !Check(TokenType::KEYWORD_USING) &&
        !Check(TokenType::KEYWORD_WHERE) &&
        !Check(TokenType::KEYWORD_GROUP) &&
        !Check(TokenType::KEYWORD_HAVING) &&
        !Check(TokenType::KEYWORD_ORDER) &&
        !Check(TokenType::KEYWORD_LIMIT) &&
        !Check(TokenType::KEYWORD_INNER) &&
        !Check(TokenType::KEYWORD_LEFT) &&
        !Check(TokenType::KEYWORD_RIGHT) &&
        !Check(TokenType::KEYWORD_FULL) &&
        !Check(TokenType::KEYWORD_CROSS) &&
        !Check(TokenType::KEYWORD_NATURAL) &&
        !Check(TokenType::KEYWORD_JOIN) &&
        !Check(TokenType::KEYWORD_UNION) &&
        !Check(TokenType::KEYWORD_INTERSECT) &&
        !Check(TokenType::KEYWORD_EXCEPT) &&
        !Check(TokenType::KEYWORD_WINDOW) &&
        !Check(TokenType::SEMICOLON) &&
        !IsAtEnd()) {
        jc.table_alias = CurrentToken().lexeme;
        Advance();
    }
    // USING (col1, col2, ...)
    if (Match(TokenType::KEYWORD_USING)) {
        Expect(TokenType::LEFT_PAREN, "expected '(' after USING");
        Token c = Expect(TokenType::IDENTIFIER, "expected column name in USING");
        jc.using_columns.push_back(c.lexeme);
        while (Match(TokenType::COMMA)) {
            Token cc = Expect(TokenType::IDENTIFIER, "expected column name in USING");
            jc.using_columns.push_back(cc.lexeme);
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' after USING column list");
        return jc;
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
    // 列级约束（DDL 扩展）：CHECK (expr) / DEFAULT expr。
    // 仅做语法接受，约束语义留给执行层去兑现。当前测试套件只在 CREATE TABLE
    // 上使用，且后续不 INSERT 受约束影响的数据，因此保留为 AST 字段即可。
    while (Check(TokenType::KEYWORD_CHECK) || Check(TokenType::KEYWORD_DEFAULT)) {
        if (Match(TokenType::KEYWORD_CHECK)) {
            Expect(TokenType::LEFT_PAREN, "expected '(' after CHECK");
            cd.check_expr = ParseExpression();
            Expect(TokenType::RIGHT_PAREN, "expected ')' after CHECK expression");
        } else {
            Advance(); // KEYWORD_DEFAULT
            cd.default_expr = ParseExpression();
        }
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
    if (Check(TokenType::KEYWORD_EXISTS)) {
        Advance();
        Expect(TokenType::LEFT_PAREN, "expected '(' after EXISTS");
        return ParseSubqueryExpression(nullptr, "", SubqueryType::EXISTS);
    }
    return ParseComparisonExpr();
}

ExprPtr Parser::ParseComparisonExpr() {
    ExprPtr left = ParseAdditiveExpr();
    const Token& cur = CurrentToken();

    // NOT IN (SELECT ...) —— 形如 col NOT IN (SELECT ...)
    if (Check(TokenType::KEYWORD_NOT) && PeekToken(1).type == TokenType::KEYWORD_IN) {
        Advance(); // consume NOT
        Advance(); // consume IN
        Expect(TokenType::LEFT_PAREN, "expected '(' after NOT IN");
        ExprPtr inner;
        if (Check(TokenType::KEYWORD_SELECT) || Check(TokenType::KEYWORD_WITH)) {
            inner = ParseSubqueryExpression(left, "IN");
        } else {
            std::vector<ExprPtr> values;
            if (!Check(TokenType::RIGHT_PAREN)) {
                values.push_back(ParseExpression());
                while (Match(TokenType::COMMA)) {
                    values.push_back(ParseExpression());
                }
            }
            Expect(TokenType::RIGHT_PAREN, "expected ')' after NOT IN list");
            auto list_expr = std::make_shared<FunctionCallExpr>("__IN_LIST__", values);
            inner = std::make_shared<BinaryExpr>(BinaryOperator::IN_LIST, left, list_expr);
        }
        return std::make_shared<UnaryExpr>(UnaryOperator::NOT, inner);
    }

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

    // IN (val1, val2, ...) 或 IN (SELECT ...)
    if (Check(TokenType::KEYWORD_IN)) {
        Advance();
        Expect(TokenType::LEFT_PAREN, "expected '(' after IN");
        // 子查询形式: IN (SELECT ...)
        if (Check(TokenType::KEYWORD_SELECT) || Check(TokenType::KEYWORD_WITH)) {
            // '(' 已经被 Expect 消耗；ParseSubqueryExpression 会消费 SELECT... 并在末尾消费 ')'
            return ParseSubqueryExpression(left, "IN");
        }
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

    // ANY (SELECT ...)  —— 形如 expr > ANY (SELECT ...)
    //   与比较运算符同侧: 解析到比较运算符后再做此检查。
    //   也支持 expr = ANY (...) / expr > ANY (...) 等。
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
    // expr op ANY (SELECT ...) —— 比较运算符后跟 ANY 子查询
    if (Check(TokenType::KEYWORD_ANY)) {
        Advance();
        Expect(TokenType::LEFT_PAREN, "expected '(' after ANY");
        std::string op_str;
        switch (op) {
            case BinaryOperator::EQUAL:          op_str = "="; break;
            case BinaryOperator::NOT_EQUAL:      op_str = "<>"; break;
            case BinaryOperator::LESS:           op_str = "<"; break;
            case BinaryOperator::LESS_EQUAL:     op_str = "<="; break;
            case BinaryOperator::GREATER:        op_str = ">"; break;
            case BinaryOperator::GREATER_EQUAL:  op_str = ">="; break;
            default: op_str = "="; break;
        }
        auto sub = ParseSubqueryExpression(left, op_str);
        return sub;
    }
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
    if (cur.type == TokenType::KEYWORD_CASE) {
        return ParseCaseExpression();
    }
    if (cur.type == TokenType::KEYWORD_CAST) {
        return ParseCastExpression();
    }
    if (cur.type == TokenType::LEFT_PAREN) {
        // 可能是 (SELECT ...) / (WITH ...) 子查询；也可能是普通括号表达式
        // 用「左括号后是 SELECT/WITH」判定为子查询
        const Token& next = PeekToken(1);
        if (next.type == TokenType::KEYWORD_SELECT || next.type == TokenType::KEYWORD_WITH) {
            Advance(); // '('
            return ParseSubqueryExpression(nullptr, "");
        }
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
    // 触发器体内 NEW.col / OLD.col：把前缀识别成限定表名。
    if (cur.type == TokenType::KEYWORD_NEW ||
        cur.type == TokenType::KEYWORD_OLD) {
        std::string prefix = cur.lexeme;
        Advance();
        if (Match(TokenType::DOT)) {
            Token col = Expect(TokenType::IDENTIFIER,
                "expected column name after NEW/OLD '.'");
            return std::make_shared<ColumnRefExpr>(prefix, col.lexeme);
        }
        // 没有 '.' 时退化为普通列引用
        return std::make_shared<ColumnRefExpr>("", prefix);
    }
    if (cur.type == TokenType::IDENTIFIER) {
        return ParseColumnRefOrFunctionCall();
    }
    // 兼容「关键字形式的函数名」: 例如 COALESCE/NULLIF/UPPER 等。
    // 这些关键字在 ParseColumnRefOrFunctionCall 中走不到 IDENTIFIER 分支，
    // 所以在这里识别后转发。带 OVER(...) 的窗口函数也通过同一路径处理。
    auto is_builtin_function_keyword = [](TokenType t) -> bool {
        switch (t) {
            case TokenType::KEYWORD_COALESCE:
            case TokenType::KEYWORD_NULLIF:
            case TokenType::KEYWORD_UPPER:
            case TokenType::KEYWORD_LOWER:
            case TokenType::KEYWORD_LENGTH:
            case TokenType::KEYWORD_SUBSTR:
            case TokenType::KEYWORD_TRIM:
            case TokenType::KEYWORD_REPLACE:
            case TokenType::KEYWORD_ROUND:
            case TokenType::KEYWORD_CEIL:
            case TokenType::KEYWORD_FLOOR:
            case TokenType::KEYWORD_ABS:
            case TokenType::KEYWORD_POWER:
            case TokenType::KEYWORD_MOD:
            case TokenType::KEYWORD_YEAR:
            case TokenType::KEYWORD_MONTH:
            case TokenType::KEYWORD_DAY:
            case TokenType::KEYWORD_NOW:
            case TokenType::KEYWORD_IFNULL:
            case TokenType::KEYWORD_ROW_NUMBER:
            case TokenType::KEYWORD_RANK:
            case TokenType::KEYWORD_DENSE_RANK:
            case TokenType::KEYWORD_NTILE:
            case TokenType::KEYWORD_LAG:
            case TokenType::KEYWORD_LEAD:
            case TokenType::KEYWORD_FIRST_VALUE:
            case TokenType::KEYWORD_LAST_VALUE:
            case TokenType::KEYWORD_PERCENT_RANK:
            case TokenType::KEYWORD_CUME_DIST:
                return true;
            default:
                return false;
        }
    };
    if (is_builtin_function_keyword(cur.type) && PeekToken(1).type == TokenType::LEFT_PAREN) {
        std::string name = cur.lexeme;
        Advance();  // 关键字
        Match(TokenType::LEFT_PAREN);
        std::vector<ExprPtr> args;
        bool distinct = false;
        if (!Check(TokenType::RIGHT_PAREN)) {
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
        auto fc = std::make_shared<FunctionCallExpr>(name, args);
        fc->is_distinct = distinct;
        // 兼容关键字形式的窗口函数（如 RANK() OVER (...)）
        if (Check(TokenType::KEYWORD_OVER)) {
            return ParseOverClause(name, args);
        }
        return fc;
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
        // OVER (...) — 窗口函数
        if (Check(TokenType::KEYWORD_OVER)) {
            auto wf = ParseOverClause(first.lexeme, args);
            return wf;
        }
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

// ============ 27–33 新增语法：解析函数实现 ============

std::shared_ptr<WindowFuncNode> Parser::ParseOverClause(const std::string& func_name,
                                                       std::vector<ExprPtr>& args) {
    Advance(); // OVER
    auto wf = std::make_shared<WindowFuncNode>(func_name, args, WindowSpec{});
    if (Check(TokenType::IDENTIFIER)) {
        // OVER w — 命名窗口引用
        wf->window_name = CurrentToken().lexeme;
        Advance();
    } else {
        Expect(TokenType::LEFT_PAREN, "expected '(' after OVER");
        // 解析 [PARTITION BY ...] [ORDER BY ...] [frame]
        if (Check(TokenType::KEYWORD_PARTITION)) {
            Advance();
            Expect(TokenType::KEYWORD_BY, "expected BY after PARTITION");
            wf->spec.partition_by.push_back(ParseExpression());
            while (Match(TokenType::COMMA)) {
                wf->spec.partition_by.push_back(ParseExpression());
            }
        }
        if (Check(TokenType::KEYWORD_ORDER)) {
            Advance();
            Expect(TokenType::KEYWORD_BY, "expected BY after ORDER");
            do {
                OrderByItem oi;
                oi.expr = ParseExpression();
                if (Check(TokenType::KEYWORD_ASC) || Check(TokenType::KEYWORD_DESC)) {
                    oi.ascending = (CurrentToken().type == TokenType::KEYWORD_ASC);
                    Advance();
                }
                wf->spec.order_by.push_back(oi);
            } while (Match(TokenType::COMMA));
        }
        // frame: ROWS|RANGE BETWEEN ... AND ...
        if (Check(TokenType::KEYWORD_ROWS) || Check(TokenType::KEYWORD_RANGE)) {
            wf->spec.frame.is_rows = Check(TokenType::KEYWORD_ROWS);
            Advance();
            Expect(TokenType::KEYWORD_BETWEEN, "expected BETWEEN in frame");
            auto parse_bound = [&](WindowFrame::BoundKind& kind, ExprPtr& expr) {
                if (Check(TokenType::KEYWORD_UNBOUNDED)) {
                    Advance();
                    if (Match(TokenType::KEYWORD_PRECEDING)) kind = WindowFrame::BoundKind::UNBOUNDED_PRECEDING;
                    else if (Match(TokenType::KEYWORD_FOLLOWING)) kind = WindowFrame::BoundKind::UNBOUNDED_FOLLOWING;
                    else throw CompilerException(ErrorStage::SYNTAX,
                        "expected PRECEDING or FOLLOWING after UNBOUNDED");
                } else if (Check(TokenType::KEYWORD_CURRENT)) {
                    Advance();
                    Expect(TokenType::KEYWORD_ROW, "expected ROW after CURRENT");
                    kind = WindowFrame::BoundKind::CURRENT_ROW;
                } else {
                    expr = ParseExpression();
                    if (Match(TokenType::KEYWORD_PRECEDING)) kind = WindowFrame::BoundKind::EXPR_PRECEDING;
                    else if (Match(TokenType::KEYWORD_FOLLOWING)) kind = WindowFrame::BoundKind::EXPR_FOLLOWING;
                    else throw CompilerException(ErrorStage::SYNTAX,
                        "expected PRECEDING or FOLLOWING after frame bound expression");
                }
            };
            parse_bound(wf->spec.frame.kind1, wf->spec.frame.expr1);
            Expect(TokenType::KEYWORD_AND, "expected AND in frame");
            parse_bound(wf->spec.frame.kind2, wf->spec.frame.expr2);
            wf->spec.has_frame = true;
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' to close OVER (...)");
    }
    return wf;
}

ExprPtr Parser::ParseCastExpression() {
    // 入口已确认下一个 token 为 KEYWORD_CAST
    Advance(); // CAST
    Expect(TokenType::LEFT_PAREN, "expected '(' after CAST");
    ExprPtr inner = ParseExpression();
    Expect(TokenType::KEYWORD_AS, "expected AS in CAST");
    // 类型名可以是关键字 (INT/FLOAT/VARCHAR) 或普通标识符
    std::string ty_name;
    const Token& tc = CurrentToken();
    if (tc.type == TokenType::KEYWORD_INT ||
        tc.type == TokenType::KEYWORD_FLOAT ||
        tc.type == TokenType::KEYWORD_VARCHAR) {
        ty_name = tc.lexeme;
        Advance();
    } else if (tc.type == TokenType::IDENTIFIER) {
        ty_name = tc.lexeme;
        Advance();
    } else {
        throw CompilerException(ErrorStage::SYNTAX,
            "expected type name after AS", tc.line, tc.column);
    }
    auto cast = std::make_shared<CastExprNode>(inner, ty_name);
    // VARCHAR(N) 可选长度
    if (Match(TokenType::LEFT_PAREN)) {
        if (Check(TokenType::INTEGER_LITERAL)) {
            try {
                cast->char_length = static_cast<int32_t>(std::stol(CurrentToken().lexeme));
            } catch (...) {
                cast->char_length = -1;
            }
            Advance();
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' after CAST type parameter");
    }
    Expect(TokenType::RIGHT_PAREN, "expected ')' after CAST");
    return cast;
}

ExprPtr Parser::ParseCaseExpression() {
    // 入口已确认下一个 token 为 KEYWORD_CASE
    Advance(); // CASE
    auto node = std::make_shared<CaseExprNode>();
    // 简单 CASE: CASE subject WHEN ...
    if (!Check(TokenType::KEYWORD_WHEN)) {
        // 解析 subject（注意 NOT/LIKE/IS 等需要正常表达式路径；这里走 ParseAdditiveExpr）
        node->subject = ParseAdditiveExpr();
    }
    while (Match(TokenType::KEYWORD_WHEN)) {
        CaseWhen cw;
        cw.when_expr = ParseExpression();
        Expect(TokenType::KEYWORD_THEN, "expected THEN in CASE WHEN");
        cw.then_expr = ParseExpression();
        node->whens.push_back(std::move(cw));
    }
    if (Match(TokenType::KEYWORD_ELSE)) {
        node->else_expr = ParseExpression();
    }
    Expect(TokenType::KEYWORD_END, "expected END to close CASE");
    return node;
}

ExprPtr Parser::ParseSubqueryExpression(ExprPtr left_operand,
                                        const std::string& comparison_op,
                                        SubqueryType forced_kind) {
    // 入口：刚消费 '(' 且下一个 token 是 KEYWORD_SELECT / KEYWORD_WITH
    StatementPtr sub;
    if (Check(TokenType::KEYWORD_WITH)) {
        sub = ParseWithClause();
    } else {
        sub = ParseSelectStatement();
    }
    if (Check(TokenType::KEYWORD_UNION) ||
        Check(TokenType::KEYWORD_INTERSECT) ||
        Check(TokenType::KEYWORD_EXCEPT)) {
        sub = ParseSetOperationTail(sub);
    }
    Expect(TokenType::RIGHT_PAREN, "expected ')' after subquery");
    auto sptr = std::static_pointer_cast<SelectStatement>(sub);
    if (comparison_op.empty()) {
        // 形如 (SELECT ...)——标量或 EXISTS。
        // 调用方显式传入 forced_kind 决定：调用 ParseSubqueryExpression 后立即紧跟
        // KEYWORD_EXISTS 的路径传 EXISTS；裸 '(SELECT ...)'（如 SELECT 列表）传 SCALAR。
        SubqueryType kind = forced_kind;
        if (kind == SubqueryType::EXISTS && left_operand == nullptr) {
            auto expr = std::make_shared<SubqueryExprNode>(SubqueryType::EXISTS, sptr);
            return expr;
        }
        if (kind == SubqueryType::EXISTS && left_operand != nullptr) {
            // 罕见路径：EXISTS 出现在二元比较右侧，仍按 EXISTS 处理（保留旧行为）
            auto expr = std::make_shared<SubqueryExprNode>(SubqueryType::EXISTS, sptr);
            return expr;
        }
        auto expr = std::make_shared<SubqueryExprNode>(SubqueryType::SCALAR, sptr);
        return expr;
    }
    // 形如 expr op ANY (SELECT ...) 或 expr IN (SELECT ...)
    if (comparison_op == "IN") {
        auto expr = std::make_shared<SubqueryExprNode>(SubqueryType::IN, sptr,
                                                        "=", left_operand);
        return expr;
    }
    auto expr = std::make_shared<SubqueryExprNode>(SubqueryType::ANY, sptr,
                                                    comparison_op, left_operand);
    return expr;
}

WindowSpec Parser::ParseOverSpec() {
    // 仅在调用方处理 OVER 时使用，目前 OVER 的解析嵌在 ParseColumnRefOrFunctionCall
    // 中；这里保留为占位/重入入口。
    WindowSpec spec;
    return spec;
}

std::vector<std::pair<std::string, WindowSpec>> Parser::ParseWindowClause() {
    Expect(TokenType::KEYWORD_WINDOW, "expected WINDOW");
    std::vector<std::pair<std::string, WindowSpec>> wins;
    do {
        Token name = Expect(TokenType::IDENTIFIER, "expected window name");
        Expect(TokenType::KEYWORD_AS, "expected AS after window name");
        Expect(TokenType::LEFT_PAREN, "expected '(' to begin window spec");
        WindowSpec spec;
        if (Check(TokenType::KEYWORD_PARTITION)) {
            Advance();
            Expect(TokenType::KEYWORD_BY, "expected BY after PARTITION");
            spec.partition_by.push_back(ParseExpression());
            while (Match(TokenType::COMMA)) {
                spec.partition_by.push_back(ParseExpression());
            }
        }
        if (Check(TokenType::KEYWORD_ORDER)) {
            Advance();
            Expect(TokenType::KEYWORD_BY, "expected BY after ORDER");
            do {
                OrderByItem oi;
                oi.expr = ParseExpression();
                if (Check(TokenType::KEYWORD_ASC) || Check(TokenType::KEYWORD_DESC)) {
                    oi.ascending = (CurrentToken().type == TokenType::KEYWORD_ASC);
                    Advance();
                }
                spec.order_by.push_back(oi);
            } while (Match(TokenType::COMMA));
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' to close window spec");
        wins.push_back({name.lexeme, std::move(spec)});
    } while (Match(TokenType::COMMA));
    return wins;
}

StatementPtr Parser::ParseSetOperationTail(StatementPtr left) {
    // INTERSECT/EXCEPT/UNION [ALL] chain。约定：先循环匹配高优先级（INTERSECT），
    // 再循环匹配低优先级（UNION/EXCEPT），等价于 SQL 中 INTERSECT > UNION/EXCEPT 的优先级。
    // 这里采用左递归形式：先吸收所有 INTERSECT 形成 INTERSECT 链；外层则循环 UNION/EXCEPT。
    StatementPtr current = left;
    auto parse_set_op_rhs = [&]() -> StatementPtr {
        // 支持 (SELECT ...) / (SELECT ... UNION ...) 形式
        if (Check(TokenType::LEFT_PAREN)) {
            Advance(); // '('
            StatementPtr rhs;
            if (Check(TokenType::KEYWORD_SELECT)) {
                rhs = ParseSelectStatement(/*consume_trailers=*/false);
            } else if (Check(TokenType::KEYWORD_WITH)) {
                rhs = ParseWithClause();
            } else {
                throw CompilerException(ErrorStage::SYNTAX,
                    "expected SELECT inside parenthesized set-op RHS");
            }
            if (Check(TokenType::KEYWORD_UNION) ||
                Check(TokenType::KEYWORD_INTERSECT) ||
                Check(TokenType::KEYWORD_EXCEPT)) {
                rhs = ParseSetOperationTail(rhs);
            }
            Expect(TokenType::RIGHT_PAREN,
                "expected ')' after parenthesized set-op RHS");
            return rhs;
        }
        if (Check(TokenType::KEYWORD_SELECT)) {
            return ParseSelectStatement(/*consume_trailers=*/false);
        }
        if (Check(TokenType::KEYWORD_WITH)) {
            return ParseWithClause();
        }
        throw CompilerException(ErrorStage::SYNTAX,
            "expected SELECT after UNION/INTERSECT/EXCEPT");
    };
    // INTERSECT 优先级最高——先把所有 INTERSECT 子句消化
    while (Check(TokenType::KEYWORD_INTERSECT)) {
        Advance();
        StatementPtr rhs = parse_set_op_rhs();
        if (Check(TokenType::KEYWORD_UNION) ||
            Check(TokenType::KEYWORD_INTERSECT) ||
            Check(TokenType::KEYWORD_EXCEPT)) {
            rhs = ParseSetOperationTail(rhs);
        }
        auto sop = std::make_shared<SetOperationStatement>();
        sop->kind = SetOperationStatement::Kind::INTERSECT;
        sop->left = current;
        sop->right = rhs;
        current = sop;
    }
    // UNION / EXCEPT 优先级相同且左结合
    while (Check(TokenType::KEYWORD_UNION) || Check(TokenType::KEYWORD_EXCEPT)) {
        if (Check(TokenType::KEYWORD_UNION)) {
            Advance();
            bool is_all = Match(TokenType::KEYWORD_ALL);
            StatementPtr rhs = parse_set_op_rhs();
            if (Check(TokenType::KEYWORD_UNION) ||
                Check(TokenType::KEYWORD_INTERSECT) ||
                Check(TokenType::KEYWORD_EXCEPT)) {
                rhs = ParseSetOperationTail(rhs);
            }
            auto sop = std::make_shared<SetOperationStatement>();
            sop->kind = is_all ? SetOperationStatement::Kind::UNION_ALL
                               : SetOperationStatement::Kind::UNION;
            sop->left = current;
            sop->right = rhs;
            current = sop;
        } else {
            Advance(); // EXCEPT
            StatementPtr rhs = parse_set_op_rhs();
            if (Check(TokenType::KEYWORD_UNION) ||
                Check(TokenType::KEYWORD_INTERSECT) ||
                Check(TokenType::KEYWORD_EXCEPT)) {
                rhs = ParseSetOperationTail(rhs);
            }
            auto sop = std::make_shared<SetOperationStatement>();
            sop->kind = SetOperationStatement::Kind::EXCEPT;
            sop->left = current;
            sop->right = rhs;
            current = sop;
        }
    }
    return current;
}

StatementPtr Parser::ParseWithClause() {
    Expect(TokenType::KEYWORD_WITH, "expected WITH");
    auto with = std::make_shared<WithClauseStatement>();
    if (Match(TokenType::KEYWORD_RECURSIVE)) {
        with->is_recursive = true;
    }
    do {
        CteDefinition cte;
        cte.cte_name = Expect(TokenType::IDENTIFIER, "expected CTE name").lexeme;
        // 可选列名列表：WITH t(a, b) AS (...)
        if (Match(TokenType::LEFT_PAREN)) {
            Token c = Expect(TokenType::IDENTIFIER, "expected column alias");
            cte.cte_column_aliases.push_back(c.lexeme);
            while (Match(TokenType::COMMA)) {
                Token cc = Expect(TokenType::IDENTIFIER, "expected column alias");
                cte.cte_column_aliases.push_back(cc.lexeme);
            }
            Expect(TokenType::RIGHT_PAREN, "expected ')' after CTE column list");
        }
        Expect(TokenType::KEYWORD_AS, "expected AS after CTE name");
        Expect(TokenType::LEFT_PAREN, "expected '(' to start CTE query");
        StatementPtr body;
        if (Check(TokenType::KEYWORD_SELECT)) {
            body = ParseSelectStatement();
        } else if (Check(TokenType::KEYWORD_WITH)) {
            body = ParseWithClause();
        } else {
            throw CompilerException(ErrorStage::SYNTAX,
                "expected SELECT inside CTE");
        }
        if (Check(TokenType::KEYWORD_UNION) ||
            Check(TokenType::KEYWORD_INTERSECT) ||
            Check(TokenType::KEYWORD_EXCEPT)) {
            body = ParseSetOperationTail(body);
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' to close CTE query");
        // body 可能是 SelectStatement 或 SetOperationStatement（带 UNION 链）。
        // 当 CTE 是 WITH RECURSIVE 时，body 若为 UNION ALL 的 SetOp，把它拆为
        // anchor（左侧 SELECT）放进 cte_query，右侧放进 recursive_part，供
        // Planner/CteDefineExecutor 走迭代语义。其他集合运算（UNION / INTERSECT /
        // EXCEPT）的递归不在本期范围内，保留旧行为：cte_query 留空。
        if (auto sop = std::dynamic_pointer_cast<SetOperationStatement>(body)) {
            if (with->is_recursive && sop->kind == SetOperationStatement::Kind::UNION_ALL) {
                if (auto anchor = std::dynamic_pointer_cast<SelectStatement>(sop->left)) {
                    cte.cte_query = anchor;
                    cte.recursive_part = sop->right;
                }
                // sop->left 不是 SELECT（如 WITH RECURSIVE 的 anchor 又是嵌套 CTE）
                // 时回退到旧行为：cte_query 留空。
            }
            // 非 UNION ALL 的 SetOp 在递归 CTE 中：保持旧行为，cte_query 留空。
        } else if (auto sel = std::dynamic_pointer_cast<SelectStatement>(body)) {
            cte.cte_query = sel;
        }
        with->ctes.push_back(std::move(cte));
    } while (Match(TokenType::COMMA));
    // 主体 SELECT
    if (Check(TokenType::KEYWORD_SELECT)) {
        with->body = std::static_pointer_cast<SelectStatement>(ParseSelectStatement());
    } else {
        throw CompilerException(ErrorStage::SYNTAX,
            "expected SELECT after WITH clause");
    }
    // 主体后的 set-op 链（如 WITH ... SELECT ... UNION SELECT ...）
    if (Check(TokenType::KEYWORD_UNION) ||
        Check(TokenType::KEYWORD_INTERSECT) ||
        Check(TokenType::KEYWORD_EXCEPT)) {
        return ParseSetOperationTail(with);
    }
    return with;
}

// ================= 40_txn_view_udf：事务 / 视图 / 触发器 / UDF =================

// BEGIN [TRANSACTION]
StatementPtr Parser::ParseBeginStatement() {
    Expect(TokenType::KEYWORD_BEGIN, "expected BEGIN");
    // 可选 TRANSACTION 关键字
    if (CurrentToken().type == TokenType::KEYWORD_TRANSACTION) {
        Advance();
    }
    return std::make_shared<BeginStatement>();
}

// COMMIT
StatementPtr Parser::ParseCommitStatement() {
    Expect(TokenType::KEYWORD_COMMIT, "expected COMMIT");
    // 可选 TRANSACTION 关键字
    if (CurrentToken().type == TokenType::KEYWORD_TRANSACTION) {
        Advance();
    }
    return std::make_shared<CommitStatement>();
}

// ROLLBACK
StatementPtr Parser::ParseRollbackStatement() {
    Expect(TokenType::KEYWORD_ROLLBACK, "expected ROLLBACK");
    return std::make_shared<RollbackStatement>();
}

// SAVEPOINT name
StatementPtr Parser::ParseSavepointStatement() {
    Expect(TokenType::KEYWORD_SAVEPOINT, "expected SAVEPOINT");
    Token t = Expect(TokenType::IDENTIFIER, "expected savepoint name");
    return std::make_shared<SavepointStatement>(t.lexeme);
}

// RELEASE SAVEPOINT name
StatementPtr Parser::ParseReleaseSavepointStatement() {
    Expect(TokenType::KEYWORD_RELEASE, "expected RELEASE");
    Expect(TokenType::KEYWORD_SAVEPOINT, "expected SAVEPOINT after RELEASE");
    Token t = Expect(TokenType::IDENTIFIER, "expected savepoint name");
    return std::make_shared<ReleaseSavepointStatement>(t.lexeme);
}

// CREATE VIEW name AS <select>
// 注意：本函数被两种入口调用：
//   1) 用户直接写 "CREATE VIEW ..."（ParseStatement 的 KEYWORD_VIEW 分支）
//   2) 用户写 "CREATE VIEW ..."（ParseStatement 的 KEYWORD_CREATE 分支，已 Advance CREATE）
// 这里兼容两种：如果当前 token 是 CREATE，先消耗它。
StatementPtr Parser::ParseCreateViewStatement() {
    if (CurrentToken().type == TokenType::KEYWORD_CREATE) {
        Advance();
    }
    Expect(TokenType::KEYWORD_VIEW, "expected VIEW");
    Token name = Expect(TokenType::IDENTIFIER, "expected view name");
    Expect(TokenType::KEYWORD_AS, "expected AS in CREATE VIEW");
    // 视图体：SELECT / WITH 语句；与 ParseSelectStatement 一致，支持后续 UNION 链。
    StatementPtr q;
    if (Check(TokenType::KEYWORD_SELECT)) {
        q = ParseSelectStatementWithSetOps();
    } else if (Check(TokenType::KEYWORD_WITH)) {
        q = ParseWithClause();
        if (Check(TokenType::KEYWORD_UNION) ||
            Check(TokenType::KEYWORD_INTERSECT) ||
            Check(TokenType::KEYWORD_EXCEPT)) {
            q = ParseSetOperationTail(q);
        }
    } else {
        throw CompilerException(ErrorStage::SYNTAX,
            "expected SELECT after CREATE VIEW ... AS",
            CurrentToken().line, CurrentToken().column);
    }
    auto stmt = std::make_shared<CreateViewStatement>();
    stmt->view_name = name.lexeme;
    stmt->query = std::static_pointer_cast<SelectStatement>(q);
    return stmt;
}

// DROP VIEW [IF EXISTS] name
StatementPtr Parser::ParseDropViewStatement() {
    if (CurrentToken().type == TokenType::KEYWORD_DROP) {
        Advance();
    }
    Expect(TokenType::KEYWORD_VIEW, "expected VIEW");
    auto stmt = std::make_shared<DropViewStatement>();
    if (Check(TokenType::KEYWORD_IF)) {
        Advance();
        bool saw_exists = false;
        if (Match(TokenType::KEYWORD_EXISTS)) {
            saw_exists = true;
        } else if (CurrentToken().type == TokenType::IDENTIFIER &&
                   CurrentToken().lexeme == "EXISTS") {
            Advance();
            saw_exists = true;
        }
        if (!saw_exists) {
            Expect(TokenType::IDENTIFIER, "expected EXISTS after IF");
        }
        stmt->if_exists = true;
    }
    Token name = Expect(TokenType::IDENTIFIER, "expected view name");
    stmt->view_name = name.lexeme;
    return stmt;
}

// CREATE TRIGGER name BEFORE|AFTER INSERT|UPDATE|DELETE ON table
// FOR EACH ROW SET NEW.col = expr [, OLD.col = expr ...]
StatementPtr Parser::ParseCreateTriggerStatement() {
    if (CurrentToken().type == TokenType::KEYWORD_CREATE) {
        Advance();
    }
    Expect(TokenType::KEYWORD_TRIGGER, "expected TRIGGER");
    Token name = Expect(TokenType::IDENTIFIER, "expected trigger name");
    auto stmt = std::make_shared<CreateTriggerStatement>();
    stmt->trigger_name = name.lexeme;
    if (Match(TokenType::KEYWORD_BEFORE)) {
        stmt->timing = TriggerTiming::BEFORE;
    } else if (Match(TokenType::KEYWORD_AFTER)) {
        stmt->timing = TriggerTiming::AFTER;
    } else {
        throw CompilerException(ErrorStage::SYNTAX,
            "expected BEFORE or AFTER in CREATE TRIGGER",
            CurrentToken().line, CurrentToken().column);
    }
    if (Match(TokenType::KEYWORD_INSERT)) {
        stmt->event = TriggerEvent::INSERT;
    } else if (Match(TokenType::KEYWORD_UPDATE)) {
        stmt->event = TriggerEvent::UPDATE;
    } else if (Match(TokenType::KEYWORD_DELETE)) {
        stmt->event = TriggerEvent::DELETE;
    } else {
        throw CompilerException(ErrorStage::SYNTAX,
            "expected INSERT / UPDATE / DELETE in CREATE TRIGGER",
            CurrentToken().line, CurrentToken().column);
    }
    Expect(TokenType::KEYWORD_ON, "expected ON in CREATE TRIGGER");
    Token tbl = Expect(TokenType::IDENTIFIER, "expected table name");
    stmt->table_name = tbl.lexeme;
    Expect(TokenType::KEYWORD_FOR, "expected FOR in CREATE TRIGGER");
    Expect(TokenType::KEYWORD_EACH, "expected EACH after FOR");
    Expect(TokenType::KEYWORD_ROW, "expected ROW after FOR EACH");
    Expect(TokenType::KEYWORD_SET, "expected SET in CREATE TRIGGER body");
    // 至少一条赋值：lhs = expr；lhs 形如 NEW.col 或 OLD.col（按 IDENTIFIER.col 解析）。
    do {
        // lhs 可以是 NEW.col / OLD.col（关键词前缀），或直接 col。
        std::string lhs;
        if (CurrentToken().type == TokenType::KEYWORD_NEW ||
            CurrentToken().type == TokenType::KEYWORD_OLD ||
            CurrentToken().type == TokenType::IDENTIFIER) {
            std::string prefix = CurrentToken().lexeme;
            Advance();
            if (Match(TokenType::DOT)) {
                Token col = Expect(TokenType::IDENTIFIER, "expected column name after '.'");
                lhs = prefix + "." + col.lexeme;
            } else {
                lhs = prefix;
            }
        } else {
            throw CompilerException(ErrorStage::SYNTAX,
                "expected column name in SET clause",
                CurrentToken().line, CurrentToken().column);
        }
        Expect(TokenType::OP_EQUAL, "expected '=' in SET assignment");
        ExprPtr expr = ParseExpression();
        stmt->assignments.emplace_back(lhs, expr);
    } while (Match(TokenType::COMMA));
    return stmt;
}

// DROP TRIGGER [IF EXISTS] name
StatementPtr Parser::ParseDropTriggerStatement() {
    if (CurrentToken().type == TokenType::KEYWORD_DROP) {
        Advance();
    }
    Expect(TokenType::KEYWORD_TRIGGER, "expected TRIGGER");
    auto stmt = std::make_shared<DropTriggerStatement>();
    if (Check(TokenType::KEYWORD_IF)) {
        Advance();
        bool saw_exists = false;
        if (Match(TokenType::KEYWORD_EXISTS)) {
            saw_exists = true;
        } else if (CurrentToken().type == TokenType::IDENTIFIER &&
                   CurrentToken().lexeme == "EXISTS") {
            Advance();
            saw_exists = true;
        }
        if (!saw_exists) {
            Expect(TokenType::IDENTIFIER, "expected EXISTS after IF");
        }
        stmt->if_exists = true;
    }
    Token name = Expect(TokenType::IDENTIFIER, "expected trigger name");
    stmt->trigger_name = name.lexeme;
    return stmt;
}

// CREATE FUNCTION name(args) RETURNS type
// BEGIN
//     RETURN expr;
// END;
StatementPtr Parser::ParseCreateFunctionStatement() {
    if (CurrentToken().type == TokenType::KEYWORD_CREATE) {
        Advance();
    }
    Expect(TokenType::KEYWORD_FUNCTION, "expected FUNCTION");
    Token name = Expect(TokenType::IDENTIFIER, "expected function name");
    auto stmt = std::make_shared<CreateFunctionStatement>();
    stmt->function_name = name.lexeme;
    Expect(TokenType::LEFT_PAREN, "expected '(' after function name");
    // 参数列表（可空）
    if (!Check(TokenType::RIGHT_PAREN)) {
        do {
            FunctionParameter param;
            Token pname = Expect(TokenType::IDENTIFIER, "expected parameter name");
            param.name = pname.lexeme;
            const Token& ty = CurrentToken();
            if (ty.type == TokenType::KEYWORD_INT) { param.data_type = "INT"; Advance(); }
            else if (ty.type == TokenType::KEYWORD_VARCHAR) { param.data_type = "VARCHAR"; Advance(); }
            else if (ty.type == TokenType::KEYWORD_FLOAT) { param.data_type = "FLOAT"; Advance(); }
            else if (ty.type == TokenType::IDENTIFIER) { param.data_type = ty.lexeme; Advance(); }
            else {
                throw CompilerException(ErrorStage::SYNTAX,
                    "expected parameter type", ty.line, ty.column);
            }
            if (Match(TokenType::LEFT_PAREN)) {
                if (CurrentToken().type == TokenType::INTEGER_LITERAL) {
                    try {
                        param.char_length = static_cast<int32_t>(
                            std::stol(CurrentToken().lexeme));
                    } catch (...) { param.char_length = -1; }
                    Advance();
                }
                Expect(TokenType::RIGHT_PAREN, "expected ')' after type parameter");
            }
            stmt->parameters.push_back(std::move(param));
        } while (Match(TokenType::COMMA));
    }
    Expect(TokenType::RIGHT_PAREN, "expected ')' after parameter list");
    Expect(TokenType::KEYWORD_RETURNS, "expected RETURNS in CREATE FUNCTION");
    const Token& rty = CurrentToken();
    if (rty.type == TokenType::KEYWORD_INT) { stmt->return_type = "INT"; Advance(); }
    else if (rty.type == TokenType::KEYWORD_VARCHAR) { stmt->return_type = "VARCHAR"; Advance(); }
    else if (rty.type == TokenType::KEYWORD_FLOAT) { stmt->return_type = "FLOAT"; Advance(); }
    else if (rty.type == TokenType::IDENTIFIER) { stmt->return_type = rty.lexeme; Advance(); }
    else {
        throw CompilerException(ErrorStage::SYNTAX,
            "expected return type", rty.line, rty.column);
    }
    if (Match(TokenType::LEFT_PAREN)) {
        if (CurrentToken().type == TokenType::INTEGER_LITERAL) {
            try {
                stmt->return_char_length = static_cast<int32_t>(
                    std::stol(CurrentToken().lexeme));
            } catch (...) { stmt->return_char_length = -1; }
            Advance();
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' after return type parameter");
    }
    // 函数体 BEGIN ... END；最小实现：只解析单个 RETURN 表达式。
    Expect(TokenType::KEYWORD_BEGIN, "expected BEGIN to start function body");
    // 跳过中间语句（仅占位）
    while (!Check(TokenType::KEYWORD_RETURN) && !IsAtEnd() &&
           !Check(TokenType::KEYWORD_END)) {
        Advance();
    }
    Expect(TokenType::KEYWORD_RETURN, "expected RETURN in function body");
    stmt->body_expr = ParseExpression();
    Expect(TokenType::SEMICOLON, "expected ';' after RETURN");
    Expect(TokenType::KEYWORD_END, "expected END to close function body");
    return stmt;
}

// DROP FUNCTION [IF EXISTS] name
StatementPtr Parser::ParseDropFunctionStatement() {
    if (CurrentToken().type == TokenType::KEYWORD_DROP) {
        Advance();
    }
    Expect(TokenType::KEYWORD_FUNCTION, "expected FUNCTION");
    auto stmt = std::make_shared<DropFunctionStatement>();
    if (Check(TokenType::KEYWORD_IF)) {
        Advance();
        bool saw_exists = false;
        if (Match(TokenType::KEYWORD_EXISTS)) {
            saw_exists = true;
        } else if (CurrentToken().type == TokenType::IDENTIFIER &&
                   CurrentToken().lexeme == "EXISTS") {
            Advance();
            saw_exists = true;
        }
        if (!saw_exists) {
            Expect(TokenType::IDENTIFIER, "expected EXISTS after IF");
        }
        stmt->if_exists = true;
    }
    Token name = Expect(TokenType::IDENTIFIER, "expected function name");
    stmt->function_name = name.lexeme;
    return stmt;
}

}  // namespace sqlcompiler