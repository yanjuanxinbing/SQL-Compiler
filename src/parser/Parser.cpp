#include <cstdio>
#include <iostream>
#include "parser/Parser.h"

#include "common/DateTime.h"
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

// ================= 工具：把 Token 的位置写到 AST 节点上 =================
//
// Spec 1.3 要求语义错误携带源码位置。AST 节点在 Node 基类上已经有 line / column
// 字段（默认 -1），由 Parser 在节点构造后立刻填入，以便 SemanticAnalyzer 在
// 任何报错位置直接读取。
//
// 用法：`stmt->line = cur.line; stmt->column = cur.column;`
// 下面的辅助函数封装该模式，避免每个 make_shared 调用点都要写两行重复代码。

namespace {

// 把 `t` 的位置写到 `node` 上。Node* 可以为空（空指针直接返回，不报错）。
void SetNodePos(sqlcompiler::Node* node, const sqlcompiler::Token& t) {
    if (node == nullptr) return;
    node->line = t.line;
    node->column = t.column;
}

// 重载：shared_ptr 版本，方便在 `auto stmt = std::make_shared<...>()` 后链式调用。
template <typename T>
void SetNodePos(const std::shared_ptr<T>& node, const sqlcompiler::Token& t) {
    SetNodePos(node.get(), t);
}

}  // namespace

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
    // ---- 46_meta: EXPLAIN 是语句前缀，先识别 ----
    if (cur.type == TokenType::KEYWORD_EXPLAIN) {
        return ParseExplainStatement();
    }
    // ---- 46_meta: SHOW 是独立语句 ----
    if (cur.type == TokenType::KEYWORD_SHOW) {
        return ParseShowStatement();
    }
    // ---- 46_meta: DESCRIBE / DESC 等价于 SHOW COLUMNS FROM table ----
    if (cur.type == TokenType::KEYWORD_DESCRIBE ||
        cur.type == TokenType::KEYWORD_DESC) {
        Advance(); // DESCRIBE / DESC
        Token t = Expect(TokenType::IDENTIFIER, "expected table name after DESCRIBE/DESC");
        auto stmt = std::make_shared<ShowStatement>();
        SetNodePos(stmt, cur);
        stmt->kind = ShowStatement::Kind::COLUMNS;
        stmt->target_table = t.lexeme;
        return stmt;
    }
    switch (cur.type) {
        case TokenType::KEYWORD_SELECT: return ParseSelectStatementWithSetOps();
        case TokenType::KEYWORD_WITH:   return ParseWithClause();
        case TokenType::KEYWORD_INSERT: return ParseInsertStatement();
        case TokenType::KEYWORD_REPLACE: {
            // 54_dml: REPLACE INTO ... —— MySQL 风格"删旧插新"。复用 ParseInsertStatement
            // 的解析路径，仅在解析前先把 REPLACE 消耗为 INSERT 行为，并标记 is_replace。
            // 实现上更简单：在 ParseInsertStatement 入口若看到 KEYWORD_REPLACE 则
            // 消耗之、把 stmt->is_replace 置位，并消耗 INTO。
            Advance();  // REPLACE
            Expect(TokenType::KEYWORD_INTO, "expected INTO after REPLACE");
            auto stmt = std::make_shared<InsertStatement>();
            SetNodePos(stmt, cur);
            stmt->is_replace = true;
            stmt->table_name = ParseTableNameAllowSchema();
            // 复制 ParseInsertStatement 余下的列名 / VALUES / ON DUPLICATE 解析。
            // 这里直接走 ParseInsertStatement 的"已消耗 INSERT INTO <table>"等价路径。
            // 简化：递归调用 ParseInsertStatement 后再覆盖。
            // 但 ParseInsertStatement 会从 KEYWORD_INSERT 重新开始；故改为手工
            // 复制剩余段。
            if (Match(TokenType::LEFT_PAREN)) {
                while (!Check(TokenType::RIGHT_PAREN) && !IsAtEnd()) {
                    Token col = Expect(TokenType::IDENTIFIER, "expected column name");
                    stmt->columns.push_back(col.lexeme);
                    if (!Match(TokenType::COMMA)) break;
                }
                Expect(TokenType::RIGHT_PAREN, "expected ')' after column list");
            }
            Expect(TokenType::KEYWORD_VALUES, "expected VALUES in REPLACE INTO");
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
            // RETURNING 子句（可选）
            if (Check(TokenType::KEYWORD_RETURNING)) {
                ParseReturningClause(stmt->returning_exprs, stmt->returning_aliases);
            }
            return stmt;
        }
        case TokenType::KEYWORD_UPDATE: return ParseUpdateStatement();
        case TokenType::KEYWORD_DELETE: return ParseDeleteStatement();
        case TokenType::KEYWORD_MERGE:  return ParseMergeStatement();
        case TokenType::KEYWORD_CREATE: {
            // CREATE 后面可能是 TABLE / [UNIQUE] INDEX / VIEW / TRIGGER / FUNCTION / SCHEMA / SEQUENCE
            const Token& next = PeekToken(1);
            if (next.type == TokenType::KEYWORD_INDEX ||
                next.type == TokenType::KEYWORD_UNIQUE) {
                return ParseCreateIndexStatement();
            }
            if (next.type == TokenType::KEYWORD_VIEW) {
                Advance(); // CREATE
                return ParseCreateViewStatement();
            }
            // 60_view_trigger: CREATE OR REPLACE VIEW —— OR 是关键字，
            // 看见 KEYWORD_OR 后再消耗并交给 ParseCreateViewStatement 处理。
            if (next.type == TokenType::KEYWORD_OR) {
                Advance(); // CREATE
                return ParseCreateViewStatement();
            }
            // 60_view_trigger: CREATE MATERIALIZED VIEW
            if (next.type == TokenType::KEYWORD_MATERIALIZED) {
                Advance(); // CREATE
                return ParseMaterializedViewStatement();
            }
            if (next.type == TokenType::KEYWORD_TRIGGER) {
                Advance(); // CREATE
                return ParseCreateTriggerStatement();
            }
            if (next.type == TokenType::KEYWORD_FUNCTION) {
                Advance(); // CREATE
                return ParseCreateFunctionStatement();
            }
            if (next.type == TokenType::KEYWORD_PROCEDURE) {
                Advance(); // CREATE
                return ParseCreateProcedureStatement();
            }
            if (next.type == TokenType::KEYWORD_SCHEMA) {
                Advance(); // CREATE
                return ParseCreateSchemaStatement();
            }
            if (next.type == TokenType::KEYWORD_SEQUENCE) {
                Advance(); // CREATE
                return ParseCreateSequenceStatement();
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
            if (PeekToken(1).type == TokenType::KEYWORD_PROCEDURE) {
                Advance(); // DROP
                return ParseDropProcedureStatement();
            }
            if (PeekToken(1).type == TokenType::KEYWORD_SCHEMA) {
                Advance(); // DROP
                return ParseDropSchemaStatement();
            }
            if (PeekToken(1).type == TokenType::KEYWORD_SEQUENCE) {
                Advance(); // DROP
                return ParseDropSequenceStatement();
            }
            return ParseDropTableStatement();
        }
        case TokenType::KEYWORD_ALTER: {
            // 60_view_trigger: ALTER MATERIALIZED VIEW name REFRESH
            if (PeekToken(1).type == TokenType::KEYWORD_MATERIALIZED) {
                return ParseAlterMaterializedViewStatement();
            }
            return ParseAlterTableStatement();
        }
        case TokenType::KEYWORD_TRUNCATE: {
            // TRUNCATE TABLE x：清空表中的所有数据，但保留表结构
            Advance(); // TRUNCATE
            Expect(TokenType::KEYWORD_TABLE, "expected TABLE after TRUNCATE");
            Token t = Expect(TokenType::IDENTIFIER, "expected table name");
            auto stmt = std::make_shared<TruncateTableStatement>();
            SetNodePos(stmt, cur);
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
        case TokenType::KEYWORD_PROCEDURE: return ParseCreateProcedureStatement();
        case TokenType::KEYWORD_CALL: return ParseCallStatement();
        default: {
            throw CompilerException(ErrorStage::SYNTAX,
                "unexpected token at start of statement: '" + cur.lexeme + "'",
                cur.line, cur.column);
        }
    }
}

StatementPtr Parser::ParseSelectStatement(bool consume_trailers) {
    Token select_tok = Expect(TokenType::KEYWORD_SELECT, "expected SELECT");
    auto stmt = std::make_shared<SelectStatement>();
    SetNodePos(stmt, select_tok);
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
    // 60_funcs: GROUPING SETS / ROLLUP / CUBE 展开后的 grouping sets
    // 由 ParseGroupByClause 暂存在 pending_grouping_sets_ 中，这里搬到
    // SelectStatement 上作为永久字段，并清空暂存。
    if (!pending_grouping_sets_.empty()) {
        stmt->grouping_sets = std::move(pending_grouping_sets_);
        pending_grouping_sets_.clear();
    }
    if (Check(TokenType::KEYWORD_HAVING)) stmt->having_clause = ParseHavingClause();
    // 当 SELECT 作为集合运算的子项被解析时（consume_trailers = false），
    // ORDER BY / LIMIT / WINDOW 应当上提到集合运算节点上，而不是属于子 SELECT。
    if (consume_trailers) {
        if (Check(TokenType::KEYWORD_ORDER)) stmt->order_by = ParseOrderByClause();
        // 55_query: OFFSET n [ROW|ROWS] 标准形式（SQL:2008）。可单独出现，
        // 也可与 FETCH FIRST 组合。同步设置 limit_offset。
        if (Check(TokenType::KEYWORD_OFFSET)) {
            Advance();  // OFFSET
            Token n = Expect(TokenType::INTEGER_LITERAL, "expected integer after OFFSET");
            stmt->limit_offset = std::atoi(n.lexeme.c_str());
            stmt->standard_offset = stmt->limit_offset;
            Match(TokenType::KEYWORD_ROW);
            Match(TokenType::KEYWORD_ROWS);  // ROW | ROWS 可选
        }
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
        // 55_query: FETCH {FIRST|NEXT} n [ROW|ROWS] [ONLY|WITH TIES] —— SQL:2008 标准
        // LIMIT。等价于 LIMIT n。WITH TIES 在 V1 接受但忽略（需要 ORDER BY tie-break）。
        if (Check(TokenType::KEYWORD_FETCH)) {
            Advance();  // FETCH
            if (Match(TokenType::KEYWORD_FIRST)) {
                // ok
            } else if (Match(TokenType::KEYWORD_NEXT)) {
                // ok
            } else {
                throw CompilerException(ErrorStage::SYNTAX,
                    "expected FIRST or NEXT after FETCH",
                    CurrentToken().line, CurrentToken().column);
            }
            Token n = Expect(TokenType::INTEGER_LITERAL,
                "expected integer after FETCH FIRST/NEXT");
            stmt->limit = std::atoi(n.lexeme.c_str());
            Match(TokenType::KEYWORD_ROW);
            Match(TokenType::KEYWORD_ROWS);  // ROW | ROWS 可选
            if (Match(TokenType::KEYWORD_WITH)) {
                Expect(TokenType::KEYWORD_TIES, "expected TIES after WITH");
                // WITH TIES 在 V1 接受但不强制 ORDER BY tie-break。
            } else {
                // 默认 ONLY（可显式写 ONLY 关键字）。
                Match(TokenType::KEYWORD_ONLY);
            }
        }
        // WINDOW 子句（命名窗口）
        if (Check(TokenType::KEYWORD_WINDOW)) {
            auto wins = ParseWindowClause();
            for (auto& w : wins) {
                stmt->named_windows.push_back({w.first, w.second});
            }
        }
        // 55_query: FOR UPDATE / FOR SHARE / FOR NO KEY UPDATE / FOR KEY SHARE
        // 单写引擎下为 parse-only hint：仅记录到 AST 字段，不做实际加锁。
        if (Check(TokenType::KEYWORD_FOR)) {
            Advance();  // FOR
            if (Match(TokenType::KEYWORD_NO)) {
                Expect(TokenType::KEYWORD_KEY, "expected KEY after FOR NO");
                Expect(TokenType::KEYWORD_UPDATE, "expected UPDATE after FOR NO KEY");
                stmt->for_update_kind = kForUpdateNoKeyUpdate;
            } else if (Match(TokenType::KEYWORD_KEY)) {
                Expect(TokenType::KEYWORD_SHARE, "expected SHARE after FOR KEY");
                stmt->for_update_kind = kForUpdateKeyShare;
            } else if (Match(TokenType::KEYWORD_UPDATE)) {
                stmt->for_update_kind = kForUpdateUpdate;
            } else if (Match(TokenType::KEYWORD_SHARE)) {
                stmt->for_update_kind = kForUpdateShare;
            } else {
                throw CompilerException(ErrorStage::SYNTAX,
                    "expected UPDATE / SHARE / NO KEY UPDATE / KEY SHARE after FOR",
                    CurrentToken().line, CurrentToken().column);
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
            // 55_query: 在 set-op 链尾部接受 OFFSET n [ROW|ROWS]。
            if (Check(TokenType::KEYWORD_OFFSET)) {
                Advance();
                Token n = Expect(TokenType::INTEGER_LITERAL, "expected integer after OFFSET");
                so->limit_offset = std::atoi(n.lexeme.c_str());
                Match(TokenType::KEYWORD_ROW);
                Match(TokenType::KEYWORD_ROWS);
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
            // 55_query: 在 set-op 链尾部接受 FETCH FIRST/NEXT n。
            if (Check(TokenType::KEYWORD_FETCH)) {
                Advance();
                if (!(Match(TokenType::KEYWORD_FIRST) || Match(TokenType::KEYWORD_NEXT))) {
                    throw CompilerException(ErrorStage::SYNTAX,
                        "expected FIRST or NEXT after FETCH",
                        CurrentToken().line, CurrentToken().column);
                }
                Token n = Expect(TokenType::INTEGER_LITERAL,
                    "expected integer after FETCH");
                so->limit = std::atoi(n.lexeme.c_str());
                Match(TokenType::KEYWORD_ROW);
                Match(TokenType::KEYWORD_ROWS);
                if (Match(TokenType::KEYWORD_WITH)) {
                    Expect(TokenType::KEYWORD_TIES, "expected TIES after WITH");
                } else {
                    Match(TokenType::KEYWORD_ONLY);
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
        // 55_query: VALUES row constructor as a top-level FROM clause:
        //   FROM (VALUES (1,'a'), (2,'b')) AS t(id, name)
        // 解析后填入 stmt.values_rows 与 stmt.values_column_aliases。
        if (Check(TokenType::KEYWORD_VALUES)) {
            Advance();  // VALUES
            std::vector<std::vector<ExprPtr>> rows;
            do {
                Expect(TokenType::LEFT_PAREN, "expected '(' to start VALUES row");
                std::vector<ExprPtr> row;
                while (!Check(TokenType::RIGHT_PAREN) && !IsAtEnd()) {
                    row.push_back(ParseExpression());
                    if (!Match(TokenType::COMMA)) break;
                }
                Expect(TokenType::RIGHT_PAREN, "expected ')' after VALUES row");
                rows.push_back(std::move(row));
            } while (Match(TokenType::COMMA));
            Expect(TokenType::RIGHT_PAREN, "expected ')' after VALUES list");
            Match(TokenType::KEYWORD_AS);
            Token alias = Expect(TokenType::IDENTIFIER, "expected derived table alias for VALUES");
            stmt.derived_alias = alias.lexeme;
            stmt.values_rows = std::move(rows);
            // 可选列名列表：t(id, name)
            if (Match(TokenType::LEFT_PAREN)) {
                Token cn = Expect(TokenType::IDENTIFIER, "expected column alias");
                stmt.values_column_aliases.push_back(cn.lexeme);
                while (Match(TokenType::COMMA)) {
                    Token cn2 = Expect(TokenType::IDENTIFIER, "expected column alias");
                    stmt.values_column_aliases.push_back(cn2.lexeme);
                }
                Expect(TokenType::RIGHT_PAREN, "expected ')' after column alias list");
            }
            stmt.joins = ParseJoinClauses();
            return;
        }
        throw CompilerException(ErrorStage::SYNTAX,
            "unsupported parenthesized FROM expression",
            CurrentToken().line, CurrentToken().column);
    }
    Token table = Expect(TokenType::IDENTIFIER, "expected table name");
    stmt.from_table = table.lexeme;
    // 53_ddl: schema.table 形式（FROM finance.txn）
    if (Check(TokenType::DOT)) {
        Advance();
        Token second = Expect(TokenType::IDENTIFIER, "expected table name after '.'");
        stmt.from_table += ".";
        stmt.from_table += second.lexeme;
    }
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
               !Check(TokenType::KEYWORD_WINDOW) && !Check(TokenType::KEYWORD_LATERAL) &&
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
        // 55_query: LATERAL (SELECT ...) AS alias —— 解析为带 lateral 标记的 JoinClause。
        bool is_lateral = false;
        if (Match(TokenType::KEYWORD_LATERAL)) {
            is_lateral = true;
        }
        if (is_lateral) {
            // LATERAL 仅支持 parenthesized SELECT 形式：LATERAL (SELECT ...) AS alias
            if (!Check(TokenType::LEFT_PAREN)) {
                throw CompilerException(ErrorStage::SYNTAX,
                    "LATERAL requires parenthesized SELECT (e.g. LATERAL (SELECT ...))",
                    CurrentToken().line, CurrentToken().column);
            }
            Advance();  // '('
            StatementPtr sub;
            if (Check(TokenType::KEYWORD_SELECT)) {
                sub = ParseSelectStatement();
            } else if (Check(TokenType::KEYWORD_WITH)) {
                sub = ParseWithClause();
            } else {
                throw CompilerException(ErrorStage::SYNTAX,
                    "expected SELECT inside LATERAL derived table",
                    CurrentToken().line, CurrentToken().column);
            }
            if (Check(TokenType::KEYWORD_UNION) ||
                Check(TokenType::KEYWORD_INTERSECT) ||
                Check(TokenType::KEYWORD_EXCEPT)) {
                sub = ParseSetOperationTail(sub);
            }
            Expect(TokenType::RIGHT_PAREN, "expected ')' after LATERAL derived table");
            Match(TokenType::KEYWORD_AS);
            Token alias = Expect(TokenType::IDENTIFIER,
                "expected derived table alias for LATERAL subquery");
            JoinClause jc;
            jc.join_type = JoinType::INNER;  // LATERAL 是相关子查询的 cross-apply
            jc.table_name = alias.lexeme;
            jc.table_alias = alias.lexeme;
            jc.is_lateral = true;
            // 标记内层 SELECT 为 LATERAL，让 ExpressionEvaluator 在内层
            // CollectInnerTableNames 时跳过 from_table，让 outer 引用走 outer_bind。
            if (jc.lateral_subquery) jc.lateral_subquery->is_lateral = true;
            jc.lateral_subquery = std::static_pointer_cast<SelectStatement>(sub);
            stmt.joins.push_back(std::move(jc));
            continue;
        }
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
                   !Check(TokenType::KEYWORD_WINDOW) && !Check(TokenType::KEYWORD_LATERAL) &&
                   !Check(TokenType::COMMA) &&
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
    Token insert_tok = Expect(TokenType::KEYWORD_INSERT, "expected INSERT");
    Expect(TokenType::KEYWORD_INTO, "expected INTO");
    std::string table_name = ParseTableNameAllowSchema();
    auto stmt = std::make_shared<InsertStatement>();
    SetNodePos(stmt, insert_tok);
    stmt->table_name = std::move(table_name);
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
    // ---- 43_upsert: 可选 ON DUPLICATE KEY UPDATE ----
    // MySQL 风格 upsert。仅当本 INSERT 由 VALUES 提供数据时才有意义（query 路径
    // 暂不支持）。语义：见 include/ast/AST.h 中 InsertStatement 的注释。
    if (Check(TokenType::KEYWORD_ON)) {
        Advance();
        Expect(TokenType::KEYWORD_DUPLICATE, "expected DUPLICATE after ON");
        Expect(TokenType::KEYWORD_KEY, "expected KEY after DUPLICATE");
        Expect(TokenType::KEYWORD_UPDATE, "expected UPDATE after ON DUPLICATE KEY");
        stmt->has_on_duplicate = true;
        stmt->upsert_assignments = ParseUpsertAssignments();
    }
    // ---- 54_dml: 可选 RETURNING 子句 ----
    if (Check(TokenType::KEYWORD_RETURNING)) {
        ParseReturningClause(stmt->returning_exprs, stmt->returning_aliases);
    }
    return stmt;
}

// 解析 ON DUPLICATE KEY UPDATE 末尾的 SET 子句：
//   col = expr [, col = expr ...]
// expr 中可出现 VALUES(col) 形式（在 ParsePrimaryExpr 中处理）。
std::vector<std::pair<std::string, ExprPtr>> Parser::ParseUpsertAssignments() {
    std::vector<std::pair<std::string, ExprPtr>> assigns;
    do {
        Token col = Expect(TokenType::IDENTIFIER, "expected column name in ON DUPLICATE KEY UPDATE");
        Expect(TokenType::OP_EQUAL, "expected '=' in upsert assignment");
        ExprPtr expr = ParseExpression();
        assigns.push_back({col.lexeme, std::move(expr)});
    } while (Match(TokenType::COMMA));
    return assigns;
}

StatementPtr Parser::ParseUpdateStatement() {
    Token update_tok = Expect(TokenType::KEYWORD_UPDATE, "expected UPDATE");
    std::string table_name = ParseTableNameAllowSchema();
    auto stmt = std::make_shared<UpdateStatement>();
    SetNodePos(stmt, update_tok);
    stmt->table_name = std::move(table_name);
    // 可选别名：UPDATE t AS t SET ... FROM s AS ss WHERE ...
    // 解析规则：当前 token 是 KEYWORD_AS 时消耗之；否则若 token 是 IDENTIFIER 且
    // 后面紧跟 SET / FROM / WHERE，也视为别名。
    if (Match(TokenType::KEYWORD_AS)) {
        Token a = Expect(TokenType::IDENTIFIER, "expected table alias after AS");
        stmt->table_alias = a.lexeme;
    } else if (CurrentToken().type == TokenType::IDENTIFIER &&
               !Check(TokenType::KEYWORD_SET) && !Check(TokenType::KEYWORD_FROM) &&
               !Check(TokenType::KEYWORD_WHERE) && !Check(TokenType::KEYWORD_RETURNING) &&
               !IsAtEnd()) {
        stmt->table_alias = CurrentToken().lexeme;
        Advance();
    }
    Expect(TokenType::KEYWORD_SET, "expected SET");
    do {
        Token col = Expect(TokenType::IDENTIFIER, "expected column name");
        Expect(TokenType::OP_EQUAL, "expected '=' in assignment");
        ExprPtr expr = ParseExpression();
        stmt->assignments.push_back({col.lexeme, expr});
    } while (Match(TokenType::COMMA));
    // ---- 54_dml: 可选 FROM source [, source2 ...] ----
    // 解析为 JoinClause 列表；连接条件仍由 WHERE 描述（PG/Oracle 风格）。
    // FROM 项可以是普通表名（带可选 AS / 隐式别名）或 (SELECT ...) AS alias。
    if (Check(TokenType::KEYWORD_FROM)) {
        Advance();  // FROM
        while (true) {
            if (Check(TokenType::LEFT_PAREN)) {
                Advance(); // '('
                StatementPtr sub;
                if (Check(TokenType::KEYWORD_SELECT)) {
                    sub = ParseSelectStatement();
                } else if (Check(TokenType::KEYWORD_WITH)) {
                    sub = ParseWithClause();
                } else {
                    throw CompilerException(ErrorStage::SYNTAX,
                        "expected SELECT in UPDATE FROM subquery");
                }
                if (Check(TokenType::KEYWORD_UNION) ||
                    Check(TokenType::KEYWORD_INTERSECT) ||
                    Check(TokenType::KEYWORD_EXCEPT)) {
                    sub = ParseSetOperationTail(sub);
                }
                Expect(TokenType::RIGHT_PAREN, "expected ')' after UPDATE FROM subquery");
                Match(TokenType::KEYWORD_AS);
                Token alias = Expect(TokenType::IDENTIFIER,
                                     "expected alias for UPDATE FROM subquery");
                JoinClause jc;
                jc.join_type = JoinType::INNER;
                jc.table_name = alias.lexeme;  // 把派生表别名存到 table_name 占位
                jc.table_alias = alias.lexeme;
                stmt->from_sources.push_back(std::move(jc));
                // 派生表的具体 SELECT AST 通过 sub 携带 —— 但 JoinClause 没有
                // 该字段。这里采取一种简化策略：仅支持 FROM 后跟普通表名或别名
                // 表，不支持嵌套 (SELECT ...) 派生表。Parser 在看到 '(' 时抛错
                // 以提示限制。
                (void)sub;
                // 真正可工作的路径：把 sub 作为 JoinClause 的"占位"信号 —— 但
                // Planner 还需要拿到 sub AST。我们把 sub 暂存到 from_sources 末
                // 端的 by-aux 字段。这里采用最小变通：派生表路径仅供 Planner
                // 通过 ModifyFromClause 重新解析时使用；当前 V1 直接禁止。
                throw CompilerException(ErrorStage::SYNTAX,
                    "UPDATE FROM with parenthesized subquery is not yet supported "
                    "(use FROM table_name instead)");
            }
            Token t = Expect(TokenType::IDENTIFIER, "expected table name in UPDATE FROM");
            JoinClause jc;
            jc.join_type = JoinType::INNER;
            jc.table_name = t.lexeme;
            if (Check(TokenType::DOT)) {
                Advance();
                Token second = Expect(TokenType::IDENTIFIER,
                                      "expected table name after '.'");
                jc.table_name += ".";
                jc.table_name += second.lexeme;
            }
            // AS 别名
            if (Match(TokenType::KEYWORD_AS)) {
                Token a = Expect(TokenType::IDENTIFIER, "expected alias after AS");
                jc.table_alias = a.lexeme;
            } else if (CurrentToken().type == TokenType::IDENTIFIER &&
                       !Check(TokenType::KEYWORD_WHERE) &&
                       !Check(TokenType::KEYWORD_RETURNING) &&
                       !Check(TokenType::COMMA) && !IsAtEnd()) {
                jc.table_alias = CurrentToken().lexeme;
                Advance();
            }
            stmt->from_sources.push_back(std::move(jc));
            if (!Match(TokenType::COMMA)) break;
        }
    }
    if (Check(TokenType::KEYWORD_WHERE)) {
        stmt->where_clause = ParseWhereClause();
    }
    // ---- 54_dml: 可选 RETURNING 子句 ----
    if (Check(TokenType::KEYWORD_RETURNING)) {
        ParseReturningClause(stmt->returning_exprs, stmt->returning_aliases);
    }
    return stmt;
}

StatementPtr Parser::ParseDeleteStatement() {
    Token delete_tok = Expect(TokenType::KEYWORD_DELETE, "expected DELETE");
    Expect(TokenType::KEYWORD_FROM, "expected FROM");
    std::string table_name = ParseTableNameAllowSchema();
    auto stmt = std::make_shared<DeleteStatement>();
    SetNodePos(stmt, delete_tok);
    stmt->table_name = std::move(table_name);
    if (Check(TokenType::KEYWORD_WHERE)) {
        stmt->where_clause = ParseWhereClause();
    }
    // ---- 54_dml: 可选 RETURNING 子句 ----
    if (Check(TokenType::KEYWORD_RETURNING)) {
        ParseReturningClause(stmt->returning_exprs, stmt->returning_aliases);
    }
    return stmt;
}

// ---- 54_dml: 共享 RETURNING 解析 ----
// 进入时当前 token 为 KEYWORD_RETURNING；离开时 RETURNING 已消耗。
// 语法：RETURNING expr [AS alias] [, expr [AS alias] ...]
void Parser::ParseReturningClause(std::vector<ExprPtr>& returning_exprs,
                                 std::vector<std::string>& returning_aliases) {
    Expect(TokenType::KEYWORD_RETURNING, "expected RETURNING");
    do {
        ExprPtr e = ParseExpression();
        std::string alias;
        if (Match(TokenType::KEYWORD_AS)) {
            Token a = Expect(TokenType::IDENTIFIER, "expected alias after AS");
            alias = a.lexeme;
        }
        returning_exprs.push_back(std::move(e));
        returning_aliases.push_back(std::move(alias));
    } while (Match(TokenType::COMMA));
}

// ---- 54_dml: MERGE INTO 解析 ----
// 语法：
//   MERGE INTO target [AS t_alias]
//   USING source [AS s_alias] ON <cond>
//   [WHEN MATCHED THEN UPDATE SET col = expr [, ...]]
//   [WHEN NOT MATCHED THEN INSERT (cols) VALUES (exprs)]
StatementPtr Parser::ParseMergeStatement() {
    Token merge_tok = Expect(TokenType::KEYWORD_MERGE, "expected MERGE");
    Expect(TokenType::KEYWORD_INTO, "expected INTO after MERGE");
    auto stmt = std::make_shared<MergeStatement>();
    SetNodePos(stmt, merge_tok);
    stmt->target_table = ParseTableNameAllowSchema();
    if (Match(TokenType::KEYWORD_AS)) {
        Token a = Expect(TokenType::IDENTIFIER, "expected target alias after AS");
        stmt->target_alias = a.lexeme;
    } else if (CurrentToken().type == TokenType::IDENTIFIER &&
               !Check(TokenType::KEYWORD_USING) &&
               !Check(TokenType::KEYWORD_ON)) {
        stmt->target_alias = CurrentToken().lexeme;
        Advance();
    }
    Expect(TokenType::KEYWORD_USING, "expected USING in MERGE");
    // source 可以是表名或派生表 (SELECT ...) AS alias。
    if (Check(TokenType::LEFT_PAREN)) {
        Advance();  // '('
        StatementPtr sub;
        if (Check(TokenType::KEYWORD_SELECT)) {
            sub = ParseSelectStatement();
        } else if (Check(TokenType::KEYWORD_WITH)) {
            sub = ParseWithClause();
        } else {
            throw CompilerException(ErrorStage::SYNTAX,
                "expected SELECT/WITH in MERGE USING subquery");
        }
        if (Check(TokenType::KEYWORD_UNION) ||
            Check(TokenType::KEYWORD_INTERSECT) ||
            Check(TokenType::KEYWORD_EXCEPT)) {
            sub = ParseSetOperationTail(sub);
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' after MERGE USING subquery");
        Match(TokenType::KEYWORD_AS);
        Token alias = Expect(TokenType::IDENTIFIER,
                             "expected alias for MERGE USING subquery");
        stmt->source_query = std::static_pointer_cast<SelectStatement>(sub);
        stmt->source_alias = alias.lexeme;
    } else {
        Token t = Expect(TokenType::IDENTIFIER, "expected source table name");
        stmt->source_table = t.lexeme;
        if (Match(TokenType::KEYWORD_AS)) {
            Token a = Expect(TokenType::IDENTIFIER, "expected source alias after AS");
            stmt->source_alias = a.lexeme;
        } else if (CurrentToken().type == TokenType::IDENTIFIER &&
                   !Check(TokenType::KEYWORD_ON)) {
            stmt->source_alias = CurrentToken().lexeme;
            Advance();
        }
    }
    Expect(TokenType::KEYWORD_ON, "expected ON in MERGE");
    stmt->on_condition = ParseExpression();
    // 循环解析 WHEN MATCHED / WHEN NOT MATCHED 分支。V1 范围：
    //   - 至多一条 WHEN MATCHED ... UPDATE SET ...
    //   - 至多一条 WHEN NOT MATCHED ... INSERT ...
    while (Check(TokenType::KEYWORD_WHEN)) {
        Advance();  // WHEN
        bool is_not_matched = false;
        if (Match(TokenType::KEYWORD_NOT)) {
            is_not_matched = true;
            Expect(TokenType::KEYWORD_MATCHED, "expected MATCHED after NOT");
        } else {
            Expect(TokenType::KEYWORD_MATCHED, "expected MATCHED in WHEN clause");
        }
        Expect(TokenType::KEYWORD_THEN, "expected THEN in WHEN clause");
        if (!is_not_matched) {
            // WHEN MATCHED THEN UPDATE SET ...
            if (stmt->has_matched_update) {
                throw CompilerException(ErrorStage::SYNTAX,
                    "MERGE supports at most one WHEN MATCHED clause (V1 scope)");
            }
            Expect(TokenType::KEYWORD_UPDATE, "expected UPDATE after WHEN MATCHED");
            Expect(TokenType::KEYWORD_SET, "expected SET after WHEN MATCHED UPDATE");
            do {
                Token col = Expect(TokenType::IDENTIFIER, "expected column name");
                Expect(TokenType::OP_EQUAL, "expected '=' in MERGE SET assignment");
                ExprPtr expr = ParseExpression();
                stmt->matched_assignments.push_back({col.lexeme, expr});
            } while (Match(TokenType::COMMA));
            stmt->has_matched_update = true;
        } else {
            // WHEN NOT MATCHED THEN INSERT (cols) VALUES (exprs)
            if (stmt->has_not_matched_insert) {
                throw CompilerException(ErrorStage::SYNTAX,
                    "MERGE supports at most one WHEN NOT MATCHED clause (V1 scope)");
            }
            Expect(TokenType::KEYWORD_INSERT, "expected INSERT after WHEN NOT MATCHED");
            Expect(TokenType::LEFT_PAREN, "expected '(' after MERGE INSERT");
            while (!Check(TokenType::RIGHT_PAREN) && !IsAtEnd()) {
                Token col = Expect(TokenType::IDENTIFIER, "expected column name");
                stmt->insert_columns.push_back(col.lexeme);
                if (!Match(TokenType::COMMA)) break;
            }
            Expect(TokenType::RIGHT_PAREN, "expected ')' after MERGE INSERT columns");
            Expect(TokenType::KEYWORD_VALUES, "expected VALUES after MERGE INSERT");
            Expect(TokenType::LEFT_PAREN, "expected '(' after MERGE VALUES");
            while (!Check(TokenType::RIGHT_PAREN) && !IsAtEnd()) {
                stmt->insert_values.push_back(ParseExpression());
                if (!Match(TokenType::COMMA)) break;
            }
            Expect(TokenType::RIGHT_PAREN, "expected ')' after MERGE VALUES");
            stmt->has_not_matched_insert = true;
        }
    }
    if (!stmt->has_matched_update && !stmt->has_not_matched_insert) {
        throw CompilerException(ErrorStage::SYNTAX,
            "MERGE requires at least one WHEN MATCHED or WHEN NOT MATCHED clause");
    }
    return stmt;
}

StatementPtr Parser::ParseCreateTableStatement() {
    Token create_tok = Expect(TokenType::KEYWORD_CREATE, "expected CREATE");
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
    std::string table_name = ParseTableNameAllowSchema();
    auto stmt = std::make_shared<CreateTableStatement>();
    SetNodePos(stmt, create_tok);
    stmt->table_name = std::move(table_name);
    stmt->if_not_exists = if_not_exists;
    Expect(TokenType::LEFT_PAREN, "expected '(' after table name");
    stmt->columns = ParseColumnDefinitions(*stmt);
    Expect(TokenType::RIGHT_PAREN, "expected ')' after column definitions");
    return stmt;
}

StatementPtr Parser::ParseDropTableStatement() {
    Token drop_tok = Expect(TokenType::KEYWORD_DROP, "expected DROP");
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
    std::string table_name = ParseTableNameAllowSchema();
    auto stmt = std::make_shared<DropTableStatement>();
    SetNodePos(stmt, drop_tok);
    stmt->table_name = std::move(table_name);
    stmt->if_exists = if_exists;
    return stmt;
}

StatementPtr Parser::ParseCreateIndexStatement() {
    Token create_tok = Expect(TokenType::KEYWORD_CREATE, "expected CREATE");
    auto stmt = std::make_shared<CreateIndexStatement>();
    SetNodePos(stmt, create_tok);
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
    Token drop_tok = Expect(TokenType::KEYWORD_DROP, "expected DROP");
    Expect(TokenType::KEYWORD_INDEX, "expected INDEX");
    auto stmt = std::make_shared<DropIndexStatement>();
    SetNodePos(stmt, drop_tok);
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
//   ALTER TABLE t RENAME COLUMN old TO new         （53_ddl）
//
// 当前实现仅保证语法可解析与计划可生成，语义层 ALTER_TABLE 被作为 no-op
// 处理：执行期不真正改动表结构，保证后续 SELECT 看到的数据一致。
StatementPtr Parser::ParseAlterTableStatement() {
    Token alter_tok = Expect(TokenType::KEYWORD_ALTER, "expected ALTER");
    Expect(TokenType::KEYWORD_TABLE, "expected TABLE");
    std::string table_name = ParseTableNameAllowSchema();
    auto stmt = std::make_shared<AlterStatement>();
    SetNodePos(stmt, alter_tok);
    stmt->table_name = std::move(table_name);
    auto parse_column_type = [](const Token& ty, ColumnDefinition* cd) {
        if (ty.type == TokenType::KEYWORD_INT)       { cd->data_type = "INT";       }
        else if (ty.type == TokenType::KEYWORD_VARCHAR)  { cd->data_type = "VARCHAR";  }
        else if (ty.type == TokenType::KEYWORD_FLOAT)    { cd->data_type = "FLOAT";    }
        else if (ty.type == TokenType::KEYWORD_DATE)     { cd->data_type = "DATE";     }
        else if (ty.type == TokenType::KEYWORD_TIMESTAMP){ cd->data_type = "TIMESTAMP";}
        else if (ty.type == TokenType::IDENTIFIER)       { cd->data_type = ty.lexeme;  }
        else {
            throw CompilerException(ErrorStage::SYNTAX,
                "expected column type", ty.line, ty.column);
        }
    };
    if (Match(TokenType::KEYWORD_ADD)) {
        // ADD COLUMN 是关键字 COLUMN 形式；同时也接受裸 ADD col ... 形式以兼容
        // 部分方言，这里把 COLUMN 当成可选项。
        Match(TokenType::KEYWORD_COLUMN);
        stmt->action = AlterAction::ADD_COLUMN;
        auto cd = std::make_shared<ColumnDefinition>();
        Token col_name = Expect(TokenType::IDENTIFIER, "expected column name");
        cd->column_name = col_name.lexeme;
        const Token& ty = CurrentToken();
        parse_column_type(ty, cd.get());
        Advance();
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
        // RENAME 后面可能跟 TO（表重命名）或 COLUMN（53_ddl：列重命名）。
        if (Match(TokenType::KEYWORD_COLUMN)) {
            stmt->action = AlterAction::RENAME_COLUMN;
            Token old_name = Expect(TokenType::IDENTIFIER, "expected old column name");
            Expect(TokenType::KEYWORD_TO, "expected TO after RENAME COLUMN");
            Token new_name = Expect(TokenType::IDENTIFIER, "expected new column name");
            stmt->rename_column_old_name = old_name.lexeme;
            stmt->rename_column_new_name = new_name.lexeme;
        } else {
            stmt->action = AlterAction::RENAME_TO;
            Expect(TokenType::KEYWORD_TO, "expected TO after RENAME");
            Token new_name = Expect(TokenType::IDENTIFIER, "expected new table name");
            stmt->new_table_name = new_name.lexeme;
        }
    } else if (Match(TokenType::KEYWORD_MODIFY)) {
        Match(TokenType::KEYWORD_COLUMN);
        stmt->action = AlterAction::MODIFY_COLUMN;
        auto cd = std::make_shared<ColumnDefinition>();
        Token col_name = Expect(TokenType::IDENTIFIER, "expected column name");
        cd->column_name = col_name.lexeme;
        const Token& ty = CurrentToken();
        parse_column_type(ty, cd.get());
        Advance();
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

    // 60_funcs: GROUPING SETS / ROLLUP / CUBE 扩展。
    //
    // GROUPING SETS (a, (b, c), ())     —— 多个分组键集合，逐一展开后 UNION ALL
    // ROLLUP (a, b, c)                  —— 等价于 GROUPING SETS ((a, b, c), (a, b), (a), ())
    // CUBE (a, b)                       —— 等价于 GROUPING SETS ((a, b), (a), (b), ())
    //
    // 语法上 ROLLUP/CUBE/GROUPING SETS 是 GROUP BY 之后的"分组规格"。
    // Parser 把所有形式归一为 SelectStatement::grouping_sets（每条 set 是一个
    // 分组键列表），普通 GROUP BY 走原有的 group_by 字段。
    auto parse_paren_group_list = [&]() -> std::vector<ExprPtr> {
        Expect(TokenType::LEFT_PAREN, "expected '(' to start grouping list");
        std::vector<ExprPtr> list;
        if (!Check(TokenType::RIGHT_PAREN)) {
            list.push_back(ParseExpression());
            while (Match(TokenType::COMMA)) {
                list.push_back(ParseExpression());
            }
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' to close grouping list");
        return list;
    };

    auto parse_rollup_cube = [&](bool is_rollup) -> std::vector<std::vector<ExprPtr>> {
        // ROLLUP / CUBE 只有一个形参列表 (a, b, c)，需展开为 grouping sets。
        std::vector<ExprPtr> cols = parse_paren_group_list();
        std::vector<std::vector<ExprPtr>> result;
        if (is_rollup) {
            // ROLLUP: 严格降序前缀 + 空集。cols = [a, b, c]
            //   result = [[a, b, c], [a, b], [a], []]
            for (size_t i = 0; i <= cols.size(); ++i) {
                std::vector<ExprPtr> prefix;
                for (size_t k = 0; k + i < cols.size(); ++k) {
                    prefix.push_back(cols[k]);
                }
                result.push_back(std::move(prefix));
            }
        } else {
            // CUBE: 2^n 个子集
            size_t n = cols.size();
            size_t total = (n >= 64) ? 0 : (size_t{1} << n);
            for (size_t mask = 0; mask < total; ++mask) {
                std::vector<ExprPtr> subset;
                for (size_t k = 0; k < n; ++k) {
                    if (mask & (size_t{1} << k)) subset.push_back(cols[k]);
                }
                result.push_back(std::move(subset));
            }
        }
        return result;
    };

    // GROUPING SETS (...)
    if (Check(TokenType::KEYWORD_GROUPING)) {
        Advance();
        Expect(TokenType::KEYWORD_SETS, "expected SETS after GROUPING");
        Expect(TokenType::LEFT_PAREN, "expected '(' after GROUPING SETS");
        std::vector<std::vector<ExprPtr>> sets;
        if (!Check(TokenType::RIGHT_PAREN)) {
            // 每个 set 形如 (a, b, c) 或 ()
            do {
                sets.push_back(parse_paren_group_list());
            } while (Match(TokenType::COMMA));
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' to close GROUPING SETS");
        // 把展开结果暂存到本 SelectStatement::grouping_sets。
        // 这里使用一个 sentinel：通过返回特殊的 vector + 让 caller 知道这是
        // GROUPING SETS。最简单的做法：调用方已经知道"GROUPING SETS"语义，
        // 我们直接返回 [] 让 caller 接收空 group_by；
        // 然后立刻把 sets 写入本 SelectStatement（外层 SelectStatement
        // 在 ParseGroupByClause 返回前被 ctor 拿到）。
        // 然而 std::vector<ExprPtr> 是返回值，无法携带 grouping_sets。
        // 为此采用一个全局上下文或附加成员。这里采用最简单方案：
        // 把 GROUPING SETS 的内容记入 SelectStatement::grouping_sets，
        // 然后返回空 group_by，让 caller 走 grouping_sets 路径。
        // 调用方通常通过 (group_by.empty() && !grouping_sets.empty()) 判定。
        // 由于 ParseGroupByClause 当前是 ParseSelectStatement 的私有成员，
        // 此处我们把 sets 存到 parser 的一个临时成员 pending_grouping_sets_ 上，
        // 让 ParseSelectStatement 消费。
        // —— 简化处理：直接存到一个静态 thread_local 容器（仅在解析时使用），
        // ParseSelectStatement 读取后清空。
        // 实际实现更简单：把 grouping_sets 缓存到 parser 实例成员。
        pending_grouping_sets_ = sets;
        return {};
    }

    // ROLLUP (...)
    if (Check(TokenType::KEYWORD_ROLLUP)) {
        Advance();
        pending_grouping_sets_ = parse_rollup_cube(true);
        return {};
    }

    // CUBE (...)
    if (Check(TokenType::KEYWORD_CUBE)) {
        Advance();
        pending_grouping_sets_ = parse_rollup_cube(false);
        return {};
    }

    // 普通 GROUP BY
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
    // 58_constraints: 表级 CHECK(expr) 或 CONSTRAINT name CHECK(expr)。
    // 入口 token 可能是 CHECK 或 CONSTRAINT；如为 CONSTRAINT，先吃掉
    // "CONSTRAINT name" 再走 CHECK (expr) 共用路径。表级 CHECK 可以引用
    // 任意列（与列级 CHECK 只看本列不同），所以挂到 CreateTableStatement 上。
    auto parse_table_check = [&]() {
        TableCheckDef tc;
        if (Match(TokenType::KEYWORD_CONSTRAINT)) {
            Token name = Expect(TokenType::IDENTIFIER,
                                "expected constraint name after CONSTRAINT");
            tc.constraint_name = name.lexeme;
        }
        Expect(TokenType::KEYWORD_CHECK, "expected CHECK after CONSTRAINT name");
        Expect(TokenType::LEFT_PAREN, "expected '(' after CHECK");
        tc.expr = ParseExpression();
        Expect(TokenType::RIGHT_PAREN, "expected ')' after CHECK expression");
        stmt.table_checks.push_back(std::move(tc));
    };
    // 52_data_types: 表级 UNIQUE(col1, col2, ...) — 与 PRIMARY KEY 同形。
    auto parse_table_unique = [&]() {
        Advance();  // UNIQUE
        Expect(TokenType::LEFT_PAREN, "expected '(' after UNIQUE");
        std::vector<std::string> uq_cols;
        Token c = Expect(TokenType::IDENTIFIER, "expected column name in UNIQUE");
        uq_cols.push_back(c.lexeme);
        while (Match(TokenType::COMMA)) {
            Token cc = Expect(TokenType::IDENTIFIER, "expected column name in UNIQUE");
            uq_cols.push_back(cc.lexeme);
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' after UNIQUE column list");
        stmt.unique_constraints.push_back(std::move(uq_cols));
    };

    // First element may be a column def or a table-level PK/UNIQUE/FOREIGN KEY/...
    // constraint. 表级 CHECK 不在此处解析（语法上仍是列级 CHECK），所以入口
    // 候选集合是 {column def, PRIMARY KEY, UNIQUE, FOREIGN, CHECK, CONSTRAINT}。
    if (Check(TokenType::KEYWORD_PRIMARY)) {
        parse_table_pk();
    } else if (Check(TokenType::KEYWORD_UNIQUE)) {
        parse_table_unique();
    } else if (Check(TokenType::KEYWORD_FOREIGN) ||
               Check(TokenType::KEYWORD_REFERENCES)) {
        // 表级 FOREIGN KEY 子句。当前 token 是 FOREIGN 或 REFERENCES 时直接交给
        // ParseTableLevelForeignKey 消耗完整段（FOREIGN KEY (cols) ...）。
        // 但有些方言允许裸 REFERENCES — 53_ddl 测试只用 FOREIGN KEY 形式。
        if (Check(TokenType::KEYWORD_FOREIGN)) {
            ParseTableLevelForeignKey(stmt);
        } else {
            // REFERENCES 形式（罕见）：退化为 "FOREIGN KEY (...) REFERENCES ..."
            // 这里把 token 改写为 FOREIGN 走同一路径。
            // 不增加新关键字分支：直接读出 (col) 然后按 FK 处理。
            Advance();  // REFERENCES
            Expect(TokenType::LEFT_PAREN, "expected '(' after REFERENCES");
            std::vector<std::string> child_cols;
            Token c = Expect(TokenType::IDENTIFIER, "expected column name");
            child_cols.push_back(c.lexeme);
            while (Match(TokenType::COMMA)) {
                Token cc = Expect(TokenType::IDENTIFIER, "expected column name");
                child_cols.push_back(cc.lexeme);
            }
            Expect(TokenType::RIGHT_PAREN, "expected ')' after column list");
            Expect(TokenType::IDENTIFIER, "expected parent table");
            std::string parent_table = CurrentToken().lexeme;
            Advance();
            Expect(TokenType::LEFT_PAREN, "expected '(' after parent table");
            std::vector<std::string> parent_cols;
            Token pc = Expect(TokenType::IDENTIFIER, "expected parent column name");
            parent_cols.push_back(pc.lexeme);
            while (Match(TokenType::COMMA)) {
                Token pcc = Expect(TokenType::IDENTIFIER, "expected parent column name");
                parent_cols.push_back(pcc.lexeme);
            }
            Expect(TokenType::RIGHT_PAREN, "expected ')' after parent column list");
            ForeignKeyDef fk;
            fk.child_cols = std::move(child_cols);
            fk.parent_table = std::move(parent_table);
            fk.parent_cols = std::move(parent_cols);
            stmt.foreign_keys.push_back(std::move(fk));
        }
    } else if (Check(TokenType::KEYWORD_CHECK) ||
               Check(TokenType::KEYWORD_CONSTRAINT)) {
        // 58_constraints: 表级 CHECK 子句 — 形式 `CHECK (expr)` 或
        // `CONSTRAINT name CHECK (expr)`。
        parse_table_check();
    } else {
        cols.push_back(ParseColumnDefinition());
    }
    while (Match(TokenType::COMMA)) {
        if (Check(TokenType::KEYWORD_PRIMARY)) {
            parse_table_pk();
        } else if (Check(TokenType::KEYWORD_UNIQUE)) {
            parse_table_unique();
        } else if (Check(TokenType::KEYWORD_FOREIGN)) {
            ParseTableLevelForeignKey(stmt);
        } else if (Check(TokenType::KEYWORD_CHECK) ||
                   Check(TokenType::KEYWORD_CONSTRAINT)) {
            parse_table_check();
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
    // 52_data_types: 新增的数据类型关键字。统一归一化到标准串名，语义层 /
    // 执行层只识别归一化后的名称。下表保留与历史 DataTypeId 一致的"内部串名"，
    // 例如 DECIMAL/NUMERIC 都映射到 "DECIMAL"、DOUBLE → "DOUBLE"、REAL → "REAL"、
    // SMALLINT/TINYINT → "INT"（运行期沿用 int32 表示）。
    if (ty.type == TokenType::KEYWORD_INT) {
        cd.data_type = "INT";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_VARCHAR) {
        cd.data_type = "VARCHAR";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_FLOAT) {
        cd.data_type = "FLOAT";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_DATE) {
        // 45_datetime: DATE 'YYYY-MM-DD'
        cd.data_type = "DATE";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_TIMESTAMP) {
        // 45_datetime: TIMESTAMP 'YYYY-MM-DD HH:MM:SS'
        cd.data_type = "TIMESTAMP";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_BOOLEAN ||
               ty.type == TokenType::KEYWORD_BOOL) {
        // BOOLEAN / BOOL — 归一化到 "BOOLEAN"。运行期按 INTEGER (0/1) 流转，
        // 但保留独立字符串名以便展示和落盘。
        cd.data_type = "BOOLEAN";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_CHAR) {
        cd.data_type = "CHAR";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_TEXT) {
        cd.data_type = "TEXT";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_DECIMAL ||
               ty.type == TokenType::KEYWORD_NUMERIC) {
        // DECIMAL / NUMERIC — 归一化到 "DECIMAL"，按精确十进制文本持久化。
        cd.data_type = "DECIMAL";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_DOUBLE) {
        cd.data_type = "DOUBLE";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_REAL) {
        cd.data_type = "REAL";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_SMALLINT) {
        // 16 位有符号：运行期使用 int32 表示。
        cd.data_type = "SMALLINT";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_TINYINT) {
        // 8 位无符号：运行期使用 int32 表示。
        cd.data_type = "TINYINT";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_TIME) {
        // TIME 'HH:MM:SS' — 按文本持久化。
        cd.data_type = "TIME";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_JSON) {
        cd.data_type = "JSON";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_UUID) {
        cd.data_type = "UUID";
        Advance();
    } else if (ty.type == TokenType::KEYWORD_SERIAL) {
        // SERIAL：PostgreSQL 风格列级 attribute，等价于
        // "INT PRIMARY KEY AUTO_INCREMENT NOT NULL"。本分支也作为"列类型"使用：
        //   id SERIAL, name VARCHAR
        // 此时 id 列的 data_type 仍写为 INT，同时把 PK / NOT NULL / AUTO_INCREMENT
        // 三个标志位置上。
        cd.data_type = "INT";
        cd.is_primary_key = true;
        cd.is_not_null = true;
        cd.is_auto_increment = true;
        Advance();
    } else if (ty.type == TokenType::IDENTIFIER) {
        cd.data_type = ty.lexeme;
        Advance();
    } else {
        throw CompilerException(ErrorStage::SYNTAX,
            "expected column type", ty.line, ty.column);
    }
    // 可选类型参数：VARCHAR(N) / CHAR(N) / DECIMAL(P,S) 等
    if (Match(TokenType::LEFT_PAREN)) {
        // 记录长度/精度上限，供 INSERT/UPDATE 时做约束校验
        if (CurrentToken().type == TokenType::INTEGER_LITERAL) {
            try {
                cd.char_length = static_cast<int32_t>(std::stol(CurrentToken().lexeme));
            } catch (...) {
                cd.char_length = -1;
            }
            Advance();
        }
        // DECIMAL(P, S) — 第二个数字是 scale；这里简单丢弃，因为运行期按
        // 文本存储并不强制 scale 截断；保留 char_length 作 VARCHAR(N) 上限
        // 校验用，DECIMAL 列上不参与长度校验。
        if (Match(TokenType::COMMA)) {
            if (CurrentToken().type == TokenType::INTEGER_LITERAL) {
                Advance();  // 跳过 scale
            }
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' after type parameter");
    }
    // 52_data_types: AUTO_INCREMENT / SERIAL / IDENTITY 列标记。
    //   - AUTO_INCREMENT：作为列级 attribute 出现在类型之后、PRIMARY KEY 之前
    //     或之后均可；本解析器在类型后立即识别，再在尾部 "skip_auto_inc" 再次
    //     跳过——容忍两种写法。
    //   - SERIAL：PostgreSQL 风格的列级 attribute，等价于
    //     "INT PRIMARY KEY AUTO_INCREMENT"，因此需要同时设置 is_primary_key
    //     与 is_auto_increment（若用户已显式声明 PK 则不重复设置）。
    //   - IDENTITY：同义别名。
    auto skip_auto_inc = [&]() {
        if (CurrentToken().type == TokenType::KEYWORD_AUTO_INCREMENT) {
            cd.is_auto_increment = true;
            Advance();
            return;
        }
        if (CurrentToken().type == TokenType::KEYWORD_SERIAL) {
            cd.is_auto_increment = true;
            if (!cd.is_primary_key) cd.is_primary_key = true;
            if (!cd.is_not_null) cd.is_not_null = true;
            Advance();
            return;
        }
        if (CurrentToken().type == TokenType::KEYWORD_IDENTITY) {
            cd.is_auto_increment = true;
            Advance();
            return;
        }
        if (CurrentToken().type == TokenType::IDENTIFIER &&
            CurrentToken().lexeme == "AUTO_INCREMENT") {
            cd.is_auto_increment = true;
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
    // 52_data_types: 列级 UNIQUE — 区别于 CREATE UNIQUE INDEX。
    // 落到 Catalog 后由 CreateTableExecutor 转译为隐式唯一索引；AST 阶段
    // 只标记 is_unique。
    if (Check(TokenType::KEYWORD_UNIQUE)) {
        cd.is_unique = true;
        Advance();
    }
    // 列级约束（DDL 扩展）：CHECK (expr) / DEFAULT expr。
    // 仅做语法接受，约束语义留给执行层去兑现。当前测试套件只在 CREATE TABLE
    // 上使用，且后续不 INSERT 受约束影响的数据，因此保留为 AST 字段即可。
    // 58_constraints: 列级 CHECK 可由可选 `CONSTRAINT name` 前缀命名，命名
    // 会同步进错误消息，方便用户定位"是哪个 CHECK 失败了"。
    while (Check(TokenType::KEYWORD_CHECK) || Check(TokenType::KEYWORD_DEFAULT)) {
        if (Match(TokenType::KEYWORD_CHECK)) {
            Expect(TokenType::LEFT_PAREN, "expected '(' after CHECK");
            cd.check_expr = ParseExpression();
            Expect(TokenType::RIGHT_PAREN, "expected ')' after CHECK expression");
        } else if (Match(TokenType::KEYWORD_CONSTRAINT)) {
            // `CONSTRAINT name CHECK (...)` 列级形式——把 name 挂到当前列。
            Token name = Expect(TokenType::IDENTIFIER,
                                "expected constraint name after CONSTRAINT");
            cd.constraint_name = name.lexeme;
            if (Match(TokenType::KEYWORD_CHECK)) {
                Expect(TokenType::LEFT_PAREN, "expected '(' after CHECK");
                cd.check_expr = ParseExpression();
                Expect(TokenType::RIGHT_PAREN,
                       "expected ')' after CHECK expression");
            } else {
                throw CompilerException(ErrorStage::SYNTAX,
                    "only CONSTRAINT name CHECK is supported as a named "
                    "column constraint",
                    CurrentToken().line, CurrentToken().column);
            }
        } else {
            Advance(); // KEYWORD_DEFAULT
            cd.default_expr = ParseExpression();
        }
    }
    // 53_ddl: 列级 REFERENCES parent(col) —— 解析为单列 FK。
    // 允许多个，例如 `pid INT REFERENCES parent(id) REFERENCES alt(id)`，
    // 但 53_ddl 测试只用到一次；本路径简单累加即可。
    while (Check(TokenType::KEYWORD_REFERENCES)) {
        Advance();
        Token parent = Expect(TokenType::IDENTIFIER, "expected parent table name");
        Expect(TokenType::LEFT_PAREN, "expected '(' after parent table");
        Token parent_col = Expect(TokenType::IDENTIFIER, "expected parent column name");
        Expect(TokenType::RIGHT_PAREN, "expected ')' after parent column");
        ColumnDefinition::InlineForeignKey inline_fk;
        inline_fk.parent_table = parent.lexeme;
        inline_fk.parent_col = parent_col.lexeme;
        cd.inline_foreign_keys.push_back(std::move(inline_fk));
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

    // LIKE <pattern> [ESCAPE 'x']  —— 旧路径仅在没有 ESCAPE 子句时使用，
    // 走 BinaryExpr + BinaryOperator::LIKE（默认 '\' 转义）；当显式带
    // ESCAPE 子句或使用 ILIKE / REGEXP / RLIKE 时，构建 LikeExprNode。
    if (Check(TokenType::KEYWORD_LIKE) || Check(TokenType::KEYWORD_ILIKE) ||
        Check(TokenType::KEYWORD_REGEXP) || Check(TokenType::KEYWORD_RLIKE)) {
        LikeExprNode::Kind kind;
        switch (CurrentToken().type) {
            case TokenType::KEYWORD_LIKE:   kind = LikeExprNode::Kind::LIKE;   break;
            case TokenType::KEYWORD_ILIKE:  kind = LikeExprNode::Kind::ILIKE;  break;
            case TokenType::KEYWORD_REGEXP: kind = LikeExprNode::Kind::REGEXP; break;
            case TokenType::KEYWORD_RLIKE:  kind = LikeExprNode::Kind::RLIKE;  break;
            default:                        kind = LikeExprNode::Kind::LIKE;   break;
        }
        Advance();
        ExprPtr pattern = ParseAdditiveExpr();
        // 可选 ESCAPE 'x' 子句：仅对 LIKE/ILIKE 真正生效；REGEXP/RLIKE 接受
        // 但忽略（节点上 has_escape 保持 false），保持语法宽容。
        char esc = '\\';
        bool has_esc = false;
        if (Check(TokenType::KEYWORD_ESCAPE)) {
            Advance();
            const Token& lit = Expect(TokenType::STRING_LITERAL,
                "parse error: ESCAPE must be a single character");
            if (lit.lexeme.size() != 1) {
                throw CompilerException(ErrorStage::SYNTAX,
                    "parse error: ESCAPE must be a single character",
                    lit.line, lit.column);
            }
            esc = lit.lexeme[0];
            has_esc = true;
        }
        if (kind == LikeExprNode::Kind::LIKE && !has_esc) {
            // 旧路径：没有显式 ESCAPE 时复用 BinaryExpr(LIKE) 以保持原
            // MatchLikePattern 默认 '\' 转义行为，避免触碰既有测试。
            return std::make_shared<BinaryExpr>(BinaryOperator::LIKE, left, pattern);
        }
        return std::make_shared<LikeExprNode>(kind, left, pattern, esc, has_esc);
    }

    // SIMILAR TO <pattern> [ESCAPE 'x']  —— SQL:1999 风格正则匹配。
    // 两关键字运算符：当前 token 必须是 SIMILAR，紧随其后必须为 TO；执行器
    // 把 SQL 模式（%/ _ 通配符 + ERE 元字符）翻译成 POSIX ERE 后再编译。
    if (Check(TokenType::KEYWORD_SIMILAR)) {
        Advance();
        Expect(TokenType::KEYWORD_TO, "parse error: expected TO after SIMILAR");
        ExprPtr pattern = ParseAdditiveExpr();
        char esc = '\\';
        bool has_esc = false;
        if (Check(TokenType::KEYWORD_ESCAPE)) {
            Advance();
            const Token& lit = Expect(TokenType::STRING_LITERAL,
                "parse error: ESCAPE must be a single character");
            if (lit.lexeme.size() != 1) {
                throw CompilerException(ErrorStage::SYNTAX,
                    "parse error: ESCAPE must be a single character",
                    lit.line, lit.column);
            }
            esc = lit.lexeme[0];
            has_esc = true;
        }
        return std::make_shared<LikeExprNode>(
            LikeExprNode::Kind::SIMILAR_TO, left, pattern, esc, has_esc);
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
        // 45_datetime: <date_or_ts> ± INTERVAL <n> <unit>
        // 在看到 +/- 之后若紧跟 INTERVAL 关键字，则把右侧整体解析为
        // IntervalExprNode 并构造 INTERVAL_ADD / INTERVAL_SUB 二元表达式。
        // 保留原有"右侧走 ParseMultiplicativeExpr"分支以保证普通算术不受影响。
        if ((Check(TokenType::OP_PLUS) || Check(TokenType::OP_MINUS)) &&
            PeekToken(1).type == TokenType::KEYWORD_INTERVAL) {
            TokenType op_tok = CurrentToken().type;
            Advance();  // '+' / '-'
            Advance();  // INTERVAL
            ExprPtr interval_expr = ParseIntervalExpression();
            BinaryOperator op = (op_tok == TokenType::OP_PLUS)
                                    ? BinaryOperator::INTERVAL_ADD
                                    : BinaryOperator::INTERVAL_SUB;
            left = std::make_shared<BinaryExpr>(op, left, interval_expr);
            continue;
        }
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
    // 45_datetime: DATE 'YYYY-MM-DD'
    if (cur.type == TokenType::KEYWORD_DATE) {
        Advance();  // DATE
        Token lit = Expect(TokenType::STRING_LITERAL,
                           "expected string literal after DATE");
        return std::make_shared<LiteralExpr>(LiteralType::DATE, lit.lexeme);
    }
    // 45_datetime: TIMESTAMP 'YYYY-MM-DD HH:MM:SS'
    if (cur.type == TokenType::KEYWORD_TIMESTAMP) {
        Advance();  // TIMESTAMP
        Token lit = Expect(TokenType::STRING_LITERAL,
                           "expected string literal after TIMESTAMP");
        return std::make_shared<LiteralExpr>(LiteralType::TIMESTAMP, lit.lexeme);
    }
    // 52_data_types: TIME 'HH:MM:SS'
    if (cur.type == TokenType::KEYWORD_TIME) {
        Advance();  // TIME
        Token lit = Expect(TokenType::STRING_LITERAL,
                           "expected string literal after TIME");
        return std::make_shared<LiteralExpr>(LiteralType::TIME, lit.lexeme);
    }
    // 52_data_types: TRUE / FALSE 关键字 → BOOLEAN 字面量。
    // 必须先于 IDENTIFIER 分支（避免被误识别为列引用）；KEYWORD_TRUE/FALSE
    // 不会被 LookupKeyword 转成 IDENTIFIER，因此这里按关键字处理即可。
    if (cur.type == TokenType::KEYWORD_TRUE) {
        Advance();
        return std::make_shared<LiteralExpr>(LiteralType::BOOLEAN, "TRUE");
    }
    if (cur.type == TokenType::KEYWORD_FALSE) {
        Advance();
        return std::make_shared<LiteralExpr>(LiteralType::BOOLEAN, "FALSE");
    }
    // 45_datetime: EXTRACT(field FROM source)
    if (cur.type == TokenType::KEYWORD_EXTRACT) {
        return ParseExtractExpression();
    }
    // 43_upsert: VALUES(col) —— 仅在 ON DUPLICATE KEY UPDATE 的赋值右侧出现。
    // 解析后语义层会校验「VALUES 引用只能出现在 Upsert 上下文」。当前位置必须在
    // KEYWORD_VALUES 关键字 + LEFT_PAREN + 列名 + RIGHT_PAREN 形式。
    if (cur.type == TokenType::KEYWORD_VALUES) {
        Advance();
        Expect(TokenType::LEFT_PAREN, "expected '(' after VALUES");
        Token col = Expect(TokenType::IDENTIFIER, "expected column name in VALUES()");
        Expect(TokenType::RIGHT_PAREN, "expected ')' after VALUES(column)");
        return std::make_shared<UpsertValuesRefExpr>(col.lexeme);
    }
    // 53_ddl: NEXTVAL FOR sequence_name —— 解析为 NextvalExpr。
    // 必须在 ParseColumnRefOrFunctionCall 之前，避免被当成函数调用解析。
    if (cur.type == TokenType::KEYWORD_NEXTVAL) {
        Advance();
        Expect(TokenType::KEYWORD_FOR, "expected FOR after NEXTVAL");
        Token seq = Expect(TokenType::IDENTIFIER, "expected sequence name after NEXTVAL FOR");
        return std::make_shared<NextvalExpr>(seq.lexeme);
    }
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
        // 60_funcs: 关键字形式函数（FIRST_VALUE / LAST_VALUE / 等）也支持
        // IGNORE NULLS / RESPECT NULLS 修饰（位于参数列表末尾、')' 之前）。
        bool ignore_nulls = false;
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
        if (Check(TokenType::KEYWORD_IGNORE)) {
            Advance();
            if (!Check(TokenType::KEYWORD_NULL) && !Check(TokenType::KEYWORD_NULLS)) {
                throw CompilerException(ErrorStage::SYNTAX,
                    "expected NULL/NULLS after IGNORE",
                    CurrentToken().line, CurrentToken().column);
            }
            Advance();
            ignore_nulls = true;
        } else if (Check(TokenType::KEYWORD_RESPECT)) {
            Advance();
            if (!Check(TokenType::KEYWORD_NULL) && !Check(TokenType::KEYWORD_NULLS)) {
                throw CompilerException(ErrorStage::SYNTAX,
                    "expected NULL/NULLS after RESPECT",
                    CurrentToken().line, CurrentToken().column);
            }
            Advance();
            ignore_nulls = false;
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' after function arguments");
        auto fc = std::make_shared<FunctionCallExpr>(name, args);
        fc->is_distinct = distinct;
        // 兼容关键字形式的窗口函数（如 RANK() OVER (...)）
        if (Check(TokenType::KEYWORD_OVER)) {
            auto wf = ParseOverClause(name, args);
            // 60_funcs: 透传 IGNORE/RESPECT NULLS 标记
            wf->ignore_nulls = ignore_nulls;
            wf->spec.ignore_nulls = ignore_nulls;
            return wf;
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
        // 60_funcs: 位置敏感窗口函数（FIRST_VALUE/LAST_VALUE/NTH_VALUE/LAG/LEAD）
        // 可在参数列表内携带 IGNORE NULLS / RESPECT NULLS 修饰。
        bool ignore_nulls = false;
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
        // 60_funcs: 消费可能出现的 IGNORE NULLS / RESPECT NULLS 修饰
        // （位于参数列表末尾、')' 之前）。
        // 接受 KEYWORD_NULL（标准单数）与 KEYWORD_NULLS（标准复数）。
        if (Check(TokenType::KEYWORD_IGNORE)) {
            Advance();
            if (!Check(TokenType::KEYWORD_NULL) && !Check(TokenType::KEYWORD_NULLS)) {
                throw CompilerException(ErrorStage::SYNTAX,
                    "expected NULL/NULLS after IGNORE",
                    CurrentToken().line, CurrentToken().column);
            }
            Advance();
            ignore_nulls = true;
        } else if (Check(TokenType::KEYWORD_RESPECT)) {
            Advance();
            if (!Check(TokenType::KEYWORD_NULL) && !Check(TokenType::KEYWORD_NULLS)) {
                throw CompilerException(ErrorStage::SYNTAX,
                    "expected NULL/NULLS after RESPECT",
                    CurrentToken().line, CurrentToken().column);
            }
            Advance();
            ignore_nulls = false;
        }
        Expect(TokenType::RIGHT_PAREN, "expected ')' after function arguments");
        auto fc = std::make_shared<FunctionCallExpr>(first.lexeme, args);
        fc->is_distinct = distinct;

        // 60_funcs: FILTER (WHERE cond) —— 聚合修饰子句。
        // 仅对聚合函数有效，但 parser 这里不强制校验；执行期对非聚合忽略即可。
        if (Check(TokenType::KEYWORD_FILTER)) {
            Advance();
            Expect(TokenType::LEFT_PAREN, "expected '(' after FILTER");
            Expect(TokenType::KEYWORD_WHERE, "expected WHERE inside FILTER");
            fc->filter_expr = ParseExpression();
            Expect(TokenType::RIGHT_PAREN, "expected ')' after FILTER predicate");
        }

        // 60_funcs: WITHIN GROUP (ORDER BY expr [ASC|DESC]) —— 有序集合聚合修饰。
        // 可与 FILTER 同时出现（先 FILTER 后 WITHIN GROUP）。
        if (Check(TokenType::KEYWORD_WITHIN)) {
            Advance();
            Expect(TokenType::KEYWORD_GROUP, "expected GROUP after WITHIN");
            Expect(TokenType::LEFT_PAREN, "expected '(' after WITHIN GROUP");
            Expect(TokenType::KEYWORD_ORDER, "expected ORDER BY inside WITHIN GROUP");
            Expect(TokenType::KEYWORD_BY, "expected BY inside WITHIN GROUP");
            OrderByItem ob;
            ob.expr = ParseExpression();
            if (Check(TokenType::KEYWORD_ASC)) {
                Advance();
                ob.ascending = true;
            } else if (Check(TokenType::KEYWORD_DESC)) {
                Advance();
                ob.ascending = false;
            }
            fc->within_group_order_by.push_back(ob);
            while (Match(TokenType::COMMA)) {
                OrderByItem ob2;
                ob2.expr = ParseExpression();
                if (Check(TokenType::KEYWORD_ASC)) {
                    Advance();
                    ob2.ascending = true;
                } else if (Check(TokenType::KEYWORD_DESC)) {
                    Advance();
                    ob2.ascending = false;
                }
                fc->within_group_order_by.push_back(ob2);
            }
            Expect(TokenType::RIGHT_PAREN, "expected ')' to close WITHIN GROUP");
        }

        // 60_funcs: 也支持窗口函数标准的"arg-list 之外"修饰：
        //   func(args) IGNORE NULLS OVER (...) / RESPECT NULLS OVER (...)
        // 若在参数列表内已识别 IGNORE NULLS，则后续不再重复处理。
        if (!ignore_nulls) {
            if (Check(TokenType::KEYWORD_IGNORE)) {
                Advance();
                if (!Check(TokenType::KEYWORD_NULL) && !Check(TokenType::KEYWORD_NULLS)) {
                    throw CompilerException(ErrorStage::SYNTAX,
                        "expected NULL/NULLS after IGNORE",
                        CurrentToken().line, CurrentToken().column);
                }
                Advance();
                ignore_nulls = true;
            } else if (Check(TokenType::KEYWORD_RESPECT)) {
                Advance();
                if (!Check(TokenType::KEYWORD_NULL) && !Check(TokenType::KEYWORD_NULLS)) {
                    throw CompilerException(ErrorStage::SYNTAX,
                        "expected NULL/NULLS after RESPECT",
                        CurrentToken().line, CurrentToken().column);
                }
                Advance();
                ignore_nulls = false;
            }
        }

        // OVER (...) — 窗口函数
        if (Check(TokenType::KEYWORD_OVER)) {
            auto wf = ParseOverClause(first.lexeme, args);
            // 60_funcs: 透传 IGNORE/RESPECT NULLS 标记。
            // 同时设置 WindowFuncNode 与 WindowSpec 两个字段，保证执行期任意入口都能拿到。
            wf->ignore_nulls = ignore_nulls;
            wf->spec.ignore_nulls = ignore_nulls;
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
        auto cr = std::make_shared<ColumnRefExpr>(first.lexeme, second.lexeme);
        SetNodePos(cr, first);
        return cr;
    }
    auto cr = std::make_shared<ColumnRefExpr>("", first.lexeme);
    SetNodePos(cr, first);
    return cr;
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

// ================= 45_datetime：DATE/TIMESTAMP/INTERVAL/EXTRACT =================

// EXTRACT(field FROM source)
//
//   field ∈ { YEAR, MONTH, DAY, HOUR, MINUTE, SECOND }
//   source 是任意可解析为日期/时间字符串的表达式（列、VARCHAR 字面量等）。
//
// 进入此函数时当前 token 必为 KEYWORD_EXTRACT。
ExprPtr Parser::ParseExtractExpression() {
    Advance();  // EXTRACT
    Expect(TokenType::LEFT_PAREN, "expected '(' after EXTRACT");
    IntervalUnit unit;
    const Token& ftok = CurrentToken();
    bool matched = false;
    switch (ftok.type) {
        case TokenType::KEYWORD_YEAR:   unit = IntervalUnit::YEAR;   matched = true; break;
        case TokenType::KEYWORD_MONTH:  unit = IntervalUnit::MONTH;  matched = true; break;
        case TokenType::KEYWORD_DAY:    unit = IntervalUnit::DAY;    matched = true; break;
        case TokenType::KEYWORD_HOUR:   unit = IntervalUnit::HOUR;   matched = true; break;
        case TokenType::KEYWORD_MINUTE: unit = IntervalUnit::MINUTE; matched = true; break;
        case TokenType::KEYWORD_SECOND: unit = IntervalUnit::SECOND; matched = true; break;
        default: break;
    }
    if (!matched) {
        throw CompilerException(ErrorStage::SYNTAX,
            std::string("expected YEAR/MONTH/DAY/HOUR/MINUTE/SECOND in EXTRACT (got '")
            + ftok.lexeme + "')",
            ftok.line, ftok.column);
    }
    Advance();  // field keyword
    Expect(TokenType::KEYWORD_FROM, "expected FROM in EXTRACT");
    ExprPtr source = ParseExpression();
    Expect(TokenType::RIGHT_PAREN, "expected ')' after EXTRACT source");
    return std::make_shared<ExtractExprNode>(static_cast<int>(unit), source);
}

// INTERVAL <n> <unit>
//
//   n 可以是 INTEGER_LITERAL 或 STRING_LITERAL（数字串）。
//   unit ∈ { YEAR, MONTH, DAY, HOUR, MINUTE, SECOND }。
//
// 进入此函数时 INTERVAL 已被调用方消费。
ExprPtr Parser::ParseIntervalExpression() {
    int64_t count = 0;
    const Token& nt = CurrentToken();
    if (nt.type == TokenType::INTEGER_LITERAL) {
        count = static_cast<int64_t>(std::atoll(nt.lexeme.c_str()));
        Advance();
    } else if (nt.type == TokenType::STRING_LITERAL) {
        try {
            count = static_cast<int64_t>(std::atoll(nt.lexeme.c_str()));
        } catch (...) {
            count = 0;
        }
        Advance();
    } else if (nt.type == TokenType::OP_MINUS) {
        // 允许 `-` 前缀（罕见）：仅用于让负数 INTERVAL 与 SUB 算符正确组合。
        Advance();
        const Token& next = CurrentToken();
        int64_t v = 0;
        if (next.type == TokenType::INTEGER_LITERAL) {
            v = static_cast<int64_t>(std::atoll(next.lexeme.c_str()));
        } else if (next.type == TokenType::STRING_LITERAL) {
            v = static_cast<int64_t>(std::atoll(next.lexeme.c_str()));
        } else {
            throw CompilerException(ErrorStage::SYNTAX,
                "expected number after INTERVAL -", next.line, next.column);
        }
        count = -v;
        Advance();
    } else {
        throw CompilerException(ErrorStage::SYNTAX,
            "expected number after INTERVAL (got '" + nt.lexeme + "')",
            nt.line, nt.column);
    }
    IntervalUnit unit;
    const Token& ut = CurrentToken();
    bool matched = false;
    switch (ut.type) {
        case TokenType::KEYWORD_YEAR:   unit = IntervalUnit::YEAR;   matched = true; break;
        case TokenType::KEYWORD_MONTH:  unit = IntervalUnit::MONTH;  matched = true; break;
        case TokenType::KEYWORD_DAY:    unit = IntervalUnit::DAY;    matched = true; break;
        case TokenType::KEYWORD_HOUR:   unit = IntervalUnit::HOUR;   matched = true; break;
        case TokenType::KEYWORD_MINUTE: unit = IntervalUnit::MINUTE; matched = true; break;
        case TokenType::KEYWORD_SECOND: unit = IntervalUnit::SECOND; matched = true; break;
        default: break;
    }
    if (!matched) {
        throw CompilerException(ErrorStage::SYNTAX,
            std::string("expected YEAR/MONTH/DAY/HOUR/MINUTE/SECOND after INTERVAL count (got '")
            + ut.lexeme + "')",
            ut.line, ut.column);
    }
    Advance();  // unit
    return std::make_shared<IntervalExprNode>(count, static_cast<int>(unit));
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

// ROLLBACK [TO name]
StatementPtr Parser::ParseRollbackStatement() {
    Expect(TokenType::KEYWORD_ROLLBACK, "expected ROLLBACK");
    // 可选 TO name —— 若有则升级为 ROLLBACK TO 语句。
    if (Match(TokenType::KEYWORD_TO)) {
        Token t = Expect(TokenType::IDENTIFIER, "expected savepoint name");
        auto stmt = std::make_shared<RollbackToStatement>();
        stmt->savepoint_name = t.lexeme;
        return stmt;
    }
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
// 注意：本函数被多种入口调用：
//   1) 用户直接写 "CREATE VIEW ..."（ParseStatement 的 KEYWORD_VIEW 分支）
//   2) 用户写 "CREATE VIEW ..."（ParseStatement 的 KEYWORD_CREATE 分支，已 Advance CREATE）
//   3) 60_view_trigger: 用户写 "CREATE OR REPLACE VIEW ..."（dispatch 已 Advance CREATE）
//
// 这里兼容所有入口：如果当前 token 是 CREATE，先消耗它；否则已是 OR/VIEW。
//
// 60_view_trigger (Category 9) 扩展：
//   - 可选的 OR REPLACE：CREATE [OR REPLACE] VIEW name AS ...
//   - 可选的 WITH [CASCADED|LOCAL] CHECK OPTION：CREATE VIEW ... AS ... WITH CHECK OPTION
StatementPtr Parser::ParseCreateViewStatement() {
    if (CurrentToken().type == TokenType::KEYWORD_CREATE) {
        Advance();
    }
    auto stmt = std::make_shared<CreateViewStatement>();
    // CREATE OR REPLACE VIEW —— CREATE 已被消耗后，current token 应是 OR。
    if (Check(TokenType::KEYWORD_OR)) {
        Advance();
        if (!Match(TokenType::KEYWORD_REPLACE)) {
            throw CompilerException(ErrorStage::SYNTAX,
                "expected REPLACE after OR in CREATE VIEW",
                CurrentToken().line, CurrentToken().column);
        }
        stmt->is_or_replace = true;
    }
    Expect(TokenType::KEYWORD_VIEW, "expected VIEW");
    Token name = Expect(TokenType::IDENTIFIER, "expected view name");
    stmt->view_name = name.lexeme;
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
    stmt->query = std::static_pointer_cast<SelectStatement>(q);
    // 可选 WITH [CASCADED|LOCAL] CHECK OPTION
    if (Match(TokenType::KEYWORD_WITH)) {
        // 形如 "WITH CASCADED CHECK OPTION" / "WITH LOCAL CHECK OPTION" / "WITH CHECK OPTION"
        bool cascaded = false;
        bool local = false;
        if (Match(TokenType::KEYWORD_CASCADED)) {
            cascaded = true;
        } else if (Match(TokenType::KEYWORD_LOCAL)) {
            local = true;
        }
        Expect(TokenType::KEYWORD_CHECK, "expected CHECK after WITH [CASCADED|LOCAL]");
        Expect(TokenType::KEYWORD_OPTION, "expected OPTION after CHECK");
        stmt->with_check_option = true;
        // PG 默认 LOCAL；显式 CASCADED 时 cascaded=true。
        stmt->check_option_cascaded = cascaded && !local;
    }
    return stmt;
}

// 60_view_trigger (Category 9): CREATE MATERIALIZED VIEW name AS <select>
// 进栈时 CREATE 已被消耗。语法：
//   CREATE MATERIALIZED VIEW [IF NOT EXISTS] name AS <select>
StatementPtr Parser::ParseMaterializedViewStatement() {
    // 当前 token 是 MATERIALIZED；消耗后期待 VIEW。
    Expect(TokenType::KEYWORD_MATERIALIZED, "expected MATERIALIZED");
    Expect(TokenType::KEYWORD_VIEW, "expected VIEW");
    auto stmt = std::make_shared<MaterializedViewStatement>();
    if (Match(TokenType::KEYWORD_IF)) {
        Expect(TokenType::KEYWORD_NOT, "expected NOT after IF");
        Expect(TokenType::KEYWORD_EXISTS, "expected EXISTS after IF NOT");
        stmt->if_not_exists = true;
    }
    Token name = Expect(TokenType::IDENTIFIER, "expected materialized view name");
    stmt->view_name = name.lexeme;
    Expect(TokenType::KEYWORD_AS, "expected AS in CREATE MATERIALIZED VIEW");
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
            "expected SELECT after CREATE MATERIALIZED VIEW ... AS",
            CurrentToken().line, CurrentToken().column);
    }
    stmt->query = std::static_pointer_cast<SelectStatement>(q);
    return stmt;
}

// 60_view_trigger (Category 9): ALTER MATERIALIZED VIEW name REFRESH
StatementPtr Parser::ParseAlterMaterializedViewStatement() {
    Expect(TokenType::KEYWORD_ALTER, "expected ALTER");
    Expect(TokenType::KEYWORD_MATERIALIZED, "expected MATERIALIZED");
    Expect(TokenType::KEYWORD_VIEW, "expected VIEW");
    Token name = Expect(TokenType::IDENTIFIER, "expected materialized view name");
    Expect(TokenType::KEYWORD_REFRESH, "expected REFRESH in ALTER MATERIALIZED VIEW");
    auto stmt = std::make_shared<AlterMaterializedViewStatement>();
    stmt->view_name = name.lexeme;
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
    // 60_view_trigger: FOR EACH ROW（默认）/ FOR EACH STATEMENT。
    if (Match(TokenType::KEYWORD_ROW)) {
        stmt->for_each_row = true;
    } else if (Match(TokenType::KEYWORD_STATEMENT)) {
        stmt->for_each_row = false;
    } else {
        throw CompilerException(ErrorStage::SYNTAX,
            "expected ROW or STATEMENT after FOR EACH",
            CurrentToken().line, CurrentToken().column);
    }
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
//     <stmts...>
// END;
//
// 函数体是有序语句集合，由 ParseFunctionBodyStatement 反复调用直到 KEYWORD_END。
// 每条语句末尾必须有 ';'（main.cpp 的 HasCompleteStatement 已经知道 BEGIN/END 块内的
// ';' 不算语句终止，因此这里在 BEGIN...END 内仍然逐条 StatementPtr 解析）。
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
    // 函数体 BEGIN ... END。允许体为空（无语句，但要求至少一条 RETURN 由语义层
    // 强制）。每条语句以 ';' 结尾，分号缺失会抛语法错。
    Expect(TokenType::KEYWORD_BEGIN, "expected BEGIN to start function body");
    stmt->body_statements = ParseFunctionBodyUntil(TokenType::KEYWORD_END);
    Expect(TokenType::KEYWORD_END, "expected END to close function body");
    return stmt;
}

// 解析一段语句直到遇到 end_token（不含 end_token 本身）。
// 这是 BEGIN...END / IF...END IF / WHILE...END WHILE 块共用的"序列解析器"。
//
// 块内允许出现空语句 / 多余的分号（例如 IF 体内 `RETURN 'positive';;` 或
// 嵌套 END 后残留的 `;`）。每次迭代开头先把连续 '; ' 都吃掉，再看是否到达
// end_token；若到达则退出；否则解析一条语句。
std::vector<StatementPtr> Parser::ParseFunctionBodyUntil(TokenType end_token) {
    std::vector<StatementPtr> stmts;
    while (!IsAtEnd()) {
        // 跳过块内的空语句（连续分号）
        while (Match(TokenType::SEMICOLON)) {}
        if (IsAtEnd() || Check(end_token)) break;
        // END 关键字必须留给外层结束块；块内不允许裸 "END" 语句
        if (Check(TokenType::KEYWORD_END)) {
            const Token& cur = CurrentToken();
            throw CompilerException(ErrorStage::SYNTAX,
                "unexpected END inside function body", cur.line, cur.column);
        }
        StatementPtr s = ParseFunctionBodyStatement();
        if (s) stmts.push_back(std::move(s));
    }
    return stmts;
}

// 解析 UDF 函数体内的一条语句：
//   RETURN [expr];
//   DECLARE name TYPE;
//   SET name = expr;     （lhs 可为 NEW.col / OLD.col）
//   IF cond THEN stmts [ELSEIF cond THEN stmts]... [ELSE stmts] END IF;
//   WHILE cond DO stmts END WHILE;
//
// 每条语句以 ';' 结尾（位于 ParseStatement 的入口，';' 已被 Parse() 顶层消耗，
// 但函数体内仍要每条手动消费，避免误切到下一条顶层 SQL）。
StatementPtr Parser::ParseFunctionBodyStatement() {
    const Token& cur = CurrentToken();
    if (cur.type == TokenType::KEYWORD_RETURN) {
        Advance();  // RETURN
        ExprPtr ret_expr = nullptr;
        if (!Check(TokenType::SEMICOLON) && !IsAtEnd()) {
            ret_expr = ParseExpression();
        }
        Match(TokenType::SEMICOLON);
        return std::make_shared<ReturnStatement>(ret_expr);
    }
    if (cur.type == TokenType::KEYWORD_DECLARE) {
        // 59_procs (Category 8): 先 peek 决定是 HANDLER / CURSOR / 普通变量。
        //   DECLARE [CONTINUE|EXIT|UNDO] HANDLER FOR ...
        //   DECLARE name CURSOR FOR ...   ← 第二个 token 是 CURSOR（不是 name）
        //   DECLARE name TYPE [DEFAULT expr];
        if (PeekToken(1).type == TokenType::KEYWORD_HANDLER ||
            PeekToken(1).type == TokenType::KEYWORD_CONTINUE ||
            PeekToken(1).type == TokenType::KEYWORD_EXIT ||
            PeekToken(1).type == TokenType::KEYWORD_UNDO) {
            return ParseDeclareHandlerStatement();
        }
        // CURSOR 形式需要先解析 name，再 peek-1 看 CURSOR 关键字。
        // 这里为简化，要求用户写 `DECLARE CURSOR name FOR select` 形式
        // （与 HANDLER 的关键字先行风格保持一致）；用户写 DECLARE name CURSOR
        // 时本分支不识别，按普通变量继续解析，下方会抛"类型不支持"。
        if (PeekToken(1).type == TokenType::KEYWORD_CURSOR) {
            return ParseDeclareCursorStatement();
        }
        Advance();  // DECLARE
        Token vname = Expect(TokenType::IDENTIFIER, "expected variable name after DECLARE");
        const Token& ty = CurrentToken();
        std::string data_type;
        int32_t char_length = -1;
        if (ty.type == TokenType::KEYWORD_INT) { data_type = "INT"; Advance(); }
        else if (ty.type == TokenType::KEYWORD_VARCHAR) { data_type = "VARCHAR"; Advance(); }
        else if (ty.type == TokenType::KEYWORD_FLOAT) { data_type = "FLOAT"; Advance(); }
        else {
            const Token& bad = CurrentToken();
            throw CompilerException(ErrorStage::SYNTAX,
                "DECLARE supports only INT / FLOAT / VARCHAR types",
                bad.line, bad.column);
        }
        if (Match(TokenType::LEFT_PAREN)) {
            if (CurrentToken().type == TokenType::INTEGER_LITERAL) {
                try {
                    char_length = static_cast<int32_t>(std::stol(CurrentToken().lexeme));
                } catch (...) { char_length = -1; }
                Advance();
            }
            Expect(TokenType::RIGHT_PAREN, "expected ')' after type parameter");
        }
        auto stmt = std::make_shared<DeclareVarStatement>(vname.lexeme, data_type, char_length);
        // 59_procs (Category 8): DEFAULT expr —— 局部变量初始值。
        if (Match(TokenType::KEYWORD_DEFAULT)) {
            stmt->default_expr = ParseExpression();
        }
        Match(TokenType::SEMICOLON);
        return stmt;
    }
    if (cur.type == TokenType::KEYWORD_SET) {
        Advance();  // SET
        // 解析 lhs：可能是 name / NEW.col / OLD.col。
        std::string target;
        if (CurrentToken().type == TokenType::KEYWORD_NEW ||
            CurrentToken().type == TokenType::KEYWORD_OLD ||
            CurrentToken().type == TokenType::IDENTIFIER) {
            std::string prefix = CurrentToken().lexeme;
            Advance();
            if (Match(TokenType::DOT)) {
                Token col = Expect(TokenType::IDENTIFIER,
                    "expected column name after NEW/OLD '.'");
                target = prefix + "." + col.lexeme;
            } else {
                target = prefix;
            }
        } else {
            const Token& bad = CurrentToken();
            throw CompilerException(ErrorStage::SYNTAX,
                "expected variable name after SET",
                bad.line, bad.column);
        }
        Expect(TokenType::OP_EQUAL, "expected '=' in SET assignment");
        ExprPtr rhs = ParseExpression();
        Match(TokenType::SEMICOLON);
        return std::make_shared<SetVarStatement>(target, rhs);
    }
    if (cur.type == TokenType::KEYWORD_IF) {
        Advance();  // IF
        ExprPtr cond = ParseExpression();
        Expect(TokenType::KEYWORD_THEN, "expected THEN in IF");
        auto stmt = std::make_shared<IfStatement>();
        stmt->condition = cond;
        // THEN 体：解析直到 ELSE / ELSEIF / END。
        auto parse_branch_until = [&](TokenType t1, TokenType t2, TokenType t3) {
            std::vector<StatementPtr> out;
            while (!IsAtEnd()) {
                // 跳过空语句（连续分号）
                while (Match(TokenType::SEMICOLON)) {}
                if (IsAtEnd()) break;
                if (Check(t1) || Check(t2) || Check(t3)) break;
                if (Check(TokenType::KEYWORD_END)) {
                    const Token& cur2 = CurrentToken();
                    throw CompilerException(ErrorStage::SYNTAX,
                        "unexpected END inside IF branch", cur2.line, cur2.column);
                }
                StatementPtr s = ParseFunctionBodyStatement();
                if (s) out.push_back(std::move(s));
            }
            return out;
        };
        stmt->then_body = parse_branch_until(
            TokenType::KEYWORD_ELSEIF, TokenType::KEYWORD_ELSE, TokenType::KEYWORD_END);
        // 循环消费 ELSEIF
        while (Match(TokenType::KEYWORD_ELSEIF)) {
            IfStatement::ElseIfClause ec;
            ec.condition = ParseExpression();
            Expect(TokenType::KEYWORD_THEN, "expected THEN in ELSEIF");
            ec.body = parse_branch_until(
                TokenType::KEYWORD_ELSEIF, TokenType::KEYWORD_ELSE, TokenType::KEYWORD_END);
            stmt->elseif_clauses.push_back(std::move(ec));
        }
        if (Match(TokenType::KEYWORD_ELSE)) {
            stmt->else_body = parse_branch_until(
                TokenType::KEYWORD_END, TokenType::KEYWORD_END, TokenType::KEYWORD_END);
        }
        Expect(TokenType::KEYWORD_END, "expected END to close IF");
        Expect(TokenType::KEYWORD_IF, "expected IF after END in IF statement");
        return stmt;
    }
    if (cur.type == TokenType::KEYWORD_WHILE) {
        Advance();  // WHILE
        ExprPtr cond = ParseExpression();
        Expect(TokenType::KEYWORD_DO, "expected DO in WHILE");
        auto stmt = std::make_shared<WhileStatement>();
        stmt->condition = cond;
        stmt->body = ParseFunctionBodyUntil(TokenType::KEYWORD_END);
        Expect(TokenType::KEYWORD_END, "expected END to close WHILE");
        Expect(TokenType::KEYWORD_WHILE, "expected WHILE after END in WHILE statement");
        return stmt;
    }
    // ============ 59_procs (Category 8) 扩展 ============
    if (cur.type == TokenType::KEYWORD_LOOP) {
        return ParseLoopStatement();
    }
    if (cur.type == TokenType::KEYWORD_REPEAT) {
        return ParseRepeatStatement();
    }
    if (cur.type == TokenType::KEYWORD_LEAVE) {
        return ParseLeaveStatement();
    }
    if (cur.type == TokenType::KEYWORD_ITERATE) {
        return ParseIterateStatement();
    }
    if (cur.type == TokenType::KEYWORD_SIGNAL) {
        return ParseSignalStatement();
    }
    if (cur.type == TokenType::KEYWORD_CASE) {
        return ParseBodyCaseStatement();
    }
    // DECLARE 的 HANDLER / CURSOR 变体已在上方 DECLARE var 分支中 peek 处理。
    if (cur.type == TokenType::KEYWORD_OPEN) {
        return ParseCursorOpenStatement();
    }
    if (cur.type == TokenType::KEYWORD_FETCH) {
        return ParseCursorFetchStatement();
    }
    if (cur.type == TokenType::KEYWORD_CLOSE) {
        return ParseCursorCloseStatement();
    }
    // 59_procs (Category 8): procedure 体内部允许完整的 DML 语句
    // （INSERT / UPDATE / DELETE）。委托给顶层 statement 解析器。
    if (cur.type == TokenType::KEYWORD_INSERT) {
        return ParseInsertStatement();
    }
    if (cur.type == TokenType::KEYWORD_UPDATE) {
        return ParseUpdateStatement();
    }
    if (cur.type == TokenType::KEYWORD_DELETE) {
        return ParseDeleteStatement();
    }
    const Token& bad = CurrentToken();
    throw CompilerException(ErrorStage::SYNTAX,
        "unexpected token in function body: '" + bad.lexeme + "'",
        bad.line, bad.column);
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

// ============ 46_meta: EXPLAIN / SHOW ============

// EXPLAIN [ANALYZE] <statement>
//
// EXPLAIN 是语句前缀：识别后消耗可选 ANALYZE，再把当前 token 重新抛回
// ParseStatement() 递归解析。递归调用会跳过任何 EXPLAIN/SHOW/DESCRIBE 前缀
// 顶层分支（因为它们的当前 token 已经不是这些关键字），最终命中 SELECT /
// INSERT / DDL 等真实语句类型。analyze 当前仅记录；执行期若见 analyze==true
// 则打印 "EXPLAIN ANALYZE not supported" 而不输出计划树。
StatementPtr Parser::ParseExplainStatement() {
    Expect(TokenType::KEYWORD_EXPLAIN, "expected EXPLAIN");
    auto stmt = std::make_shared<ExplainStatement>();
    // 可选 ANALYZE：ANALYZE 在本词法器未注册为关键字，lexer 会将其识别为
    // 普通 IDENTIFIER。这里兼容两种情况：先把 ANALYZE 当作标识符匹配。
    if (CurrentToken().type == TokenType::IDENTIFIER &&
        CurrentToken().lexeme == "ANALYZE") {
        Advance();
        stmt->analyze = true;
    }
    // 可选 FORMAT TEXT|JSON|SEXPR：决定 EXPLAIN 的输出格式。
    // TEXT 是默认（与原行为一致，向后兼容）；JSON / SEXPR 走结构化路径。
    // 注意 TEXT/JSON 在 Lexer 是关键字 (KEYWORD_TEXT/KEYWORD_JSON)，SEXPR 不是
    // 关键字，仍走 IDENTIFIER 分支；这里统一接收关键字与标识符两种来源。
    if (CurrentToken().type == TokenType::IDENTIFIER &&
        CurrentToken().lexeme == "FORMAT") {
        Advance();
        std::string fmt_name;
        TokenType ct = CurrentToken().type;
        if (ct == TokenType::IDENTIFIER) {
            fmt_name = CurrentToken().lexeme;
            Advance();
        } else if (ct == TokenType::KEYWORD_TEXT) {
            fmt_name = "TEXT";
            Advance();
        } else if (ct == TokenType::KEYWORD_JSON) {
            fmt_name = "JSON";
            Advance();
        } else {
            throw CompilerException(ErrorStage::SYNTAX,
                "expected format name after FORMAT (got '" + CurrentToken().lexeme + "')",
                CurrentToken().line, CurrentToken().column);
        }
        if (fmt_name != "TEXT" && fmt_name != "JSON" && fmt_name != "SEXPR") {
            throw CompilerException(ErrorStage::SYNTAX,
                "EXPLAIN FORMAT must be one of TEXT, JSON, SEXPR (got '"
                + fmt_name + "')",
                CurrentToken().line, CurrentToken().column);
        }
        stmt->format = fmt_name;
    }
    // 解析内部语句。这里递归调用 ParseStatement() 而不是直接拼装：保持与
    // 顶层语句调度完全一致的优先级（含 WITH/SET OP/CTE/INSERT ... SELECT 等）。
    if (IsAtEnd()) {
        throw CompilerException(ErrorStage::SYNTAX,
            "expected statement after EXPLAIN");
    }
    stmt->inner = ParseStatement();
    if (!stmt->inner) {
        throw CompilerException(ErrorStage::SYNTAX,
            "expected statement after EXPLAIN");
    }
    return stmt;
}

// SHOW TABLES | SHOW COLUMNS FROM t | SHOW INDEX FROM t | SHOW CREATE TABLE t
//
// 仅这 4 种子命令形式；其他 SHOW 形式（SHOW DATABASES 等）按语法错误处理。
// 注意：本任务的 EXPLAIN 不覆盖 SHOW，SHOW 不内嵌任何子语句。
StatementPtr Parser::ParseShowStatement() {
    Expect(TokenType::KEYWORD_SHOW, "expected SHOW");
    auto stmt = std::make_shared<ShowStatement>();
    if (Match(TokenType::KEYWORD_TABLES)) {
        stmt->kind = ShowStatement::Kind::TABLES;
        return stmt;
    }
    if (Match(TokenType::KEYWORD_COLUMNS)) {
        Expect(TokenType::KEYWORD_FROM, "expected FROM after SHOW COLUMNS");
        Token t = Expect(TokenType::IDENTIFIER, "expected table name after FROM");
        stmt->kind = ShowStatement::Kind::COLUMNS;
        stmt->target_table = t.lexeme;
        return stmt;
    }
    if (Match(TokenType::KEYWORD_INDEX)) {
        Expect(TokenType::KEYWORD_FROM, "expected FROM after SHOW INDEX");
        Token t = Expect(TokenType::IDENTIFIER, "expected table name after FROM");
        stmt->kind = ShowStatement::Kind::INDEX;
        stmt->target_table = t.lexeme;
        return stmt;
    }
    if (Check(TokenType::KEYWORD_CREATE) &&
        PeekToken(1).type == TokenType::KEYWORD_TABLE) {
        Advance(); // CREATE
        Advance(); // TABLE
        Token t = Expect(TokenType::IDENTIFIER, "expected table name after SHOW CREATE TABLE");
        stmt->kind = ShowStatement::Kind::CREATE_TABLE;
        stmt->target_table = t.lexeme;
        return stmt;
    }
    throw CompilerException(ErrorStage::SYNTAX,
        "unsupported SHOW form; expected TABLES, COLUMNS FROM tbl, "
        "INDEX FROM tbl, or CREATE TABLE tbl");
}

// ================= 53_ddl: SCHEMA / SEQUENCE / NEXTVAL =================

// ParseTableLevelForeignKey —— 当前 token 已是 FOREIGN KEY。
// 语法：FOREIGN KEY (child_cols) REFERENCES parent_table (parent_cols)
//       [ON DELETE {CASCADE|RESTRICT|SET NULL|NO ACTION|SET DEFAULT}]
//       [ON UPDATE {CASCADE|RESTRICT|SET NULL|NO ACTION|SET DEFAULT}]
//
// 解析后的 ForeignKeyDef 直接挂到 stmt.foreign_keys 上；执行期由
// CreateTableExecutor 转写到 catalog 的 fk_constraints。
void Parser::ParseTableLevelForeignKey(CreateTableStatement& stmt) {
    Advance();  // FOREIGN
    Expect(TokenType::KEYWORD_KEY, "expected KEY after FOREIGN");
    Expect(TokenType::LEFT_PAREN, "expected '(' after FOREIGN KEY");
    std::vector<std::string> child_cols;
    Token c = Expect(TokenType::IDENTIFIER, "expected child column name");
    child_cols.push_back(c.lexeme);
    while (Match(TokenType::COMMA)) {
        Token cc = Expect(TokenType::IDENTIFIER, "expected child column name");
        child_cols.push_back(cc.lexeme);
    }
    Expect(TokenType::RIGHT_PAREN, "expected ')' after FOREIGN KEY column list");
    Expect(TokenType::KEYWORD_REFERENCES,
           "expected REFERENCES after FOREIGN KEY column list");
    Token parent = Expect(TokenType::IDENTIFIER, "expected parent table name");
    Expect(TokenType::LEFT_PAREN, "expected '(' after parent table");
    std::vector<std::string> parent_cols;
    Token pc = Expect(TokenType::IDENTIFIER, "expected parent column name");
    parent_cols.push_back(pc.lexeme);
    while (Match(TokenType::COMMA)) {
        Token pcc = Expect(TokenType::IDENTIFIER, "expected parent column name");
        parent_cols.push_back(pcc.lexeme);
    }
    Expect(TokenType::RIGHT_PAREN, "expected ')' after parent column list");
    ForeignKeyDef fk;
    fk.child_cols = std::move(child_cols);
    fk.parent_table = parent.lexeme;
    fk.parent_cols = std::move(parent_cols);
    fk.on_delete_action = 0;  // 0=RESTRICT
    fk.on_update_action = 0;
    // 可选 ON DELETE / ON UPDATE 子句
    auto parse_action = [](const std::string& word) -> int {
        if (word == "CASCADE") return 1;
        if (word == "RESTRICT") return 0;
        if (word == "SET" || word == "SET_NULL" || word == "SET NULL") return 2;
        if (word == "NO" || word == "NO_ACTION") return 3;
        if (word == "SET_DEFAULT") return 4;
        return 0;
    };
    while (Check(TokenType::KEYWORD_ON)) {
        Advance();  // ON
        if (Match(TokenType::KEYWORD_DELETE)) {
            // ON DELETE action
            int act = 0;
            if (Match(TokenType::KEYWORD_CASCADE)) act = 1;
            else if (Match(TokenType::KEYWORD_RESTRICT)) act = 0;
            else if (Check(TokenType::KEYWORD_SET)) {
                Advance();
                if (Match(TokenType::KEYWORD_NULL)) act = 2;
                else if (Match(TokenType::KEYWORD_DEFAULT)) act = 4;
                else throw CompilerException(ErrorStage::SYNTAX,
                    "expected NULL or DEFAULT after SET in ON DELETE",
                    CurrentToken().line, CurrentToken().column);
            } else if (Check(TokenType::IDENTIFIER) &&
                       CurrentToken().lexeme == "NO") {
                Advance();
                Expect(TokenType::KEYWORD_ACTION, "expected ACTION after NO in ON DELETE");
                act = 3;
            } else {
                throw CompilerException(ErrorStage::SYNTAX,
                    "expected CASCADE/RESTRICT/SET NULL/NO ACTION after ON DELETE",
                    CurrentToken().line, CurrentToken().column);
            }
            fk.on_delete_action = act;
        } else if (Match(TokenType::KEYWORD_UPDATE)) {
            int act = 0;
            if (Match(TokenType::KEYWORD_CASCADE)) act = 1;
            else if (Match(TokenType::KEYWORD_RESTRICT)) act = 0;
            else if (Check(TokenType::KEYWORD_SET)) {
                Advance();
                if (Match(TokenType::KEYWORD_NULL)) act = 2;
                else if (Match(TokenType::KEYWORD_DEFAULT)) act = 4;
                else throw CompilerException(ErrorStage::SYNTAX,
                    "expected NULL or DEFAULT after SET in ON UPDATE",
                    CurrentToken().line, CurrentToken().column);
            } else if (Check(TokenType::IDENTIFIER) &&
                       CurrentToken().lexeme == "NO") {
                Advance();
                Expect(TokenType::KEYWORD_ACTION, "expected ACTION after NO in ON UPDATE");
                act = 3;
            } else {
                throw CompilerException(ErrorStage::SYNTAX,
                    "expected CASCADE/RESTRICT/SET NULL/NO ACTION after ON UPDATE",
                    CurrentToken().line, CurrentToken().column);
            }
            fk.on_update_action = act;
        } else {
            throw CompilerException(ErrorStage::SYNTAX,
                "expected DELETE or UPDATE after ON",
                CurrentToken().line, CurrentToken().column);
        }
    }
    (void)parse_action;
    stmt.foreign_keys.push_back(std::move(fk));
}

StatementPtr Parser::ParseCreateSchemaStatement() {
    Expect(TokenType::KEYWORD_SCHEMA, "expected SCHEMA");
    auto stmt = std::make_shared<CreateSchemaStatement>();
    if (Check(TokenType::KEYWORD_IF)) {
        Advance();
        Expect(TokenType::KEYWORD_NOT, "expected NOT after IF");
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
        stmt->if_not_exists = true;
    }
    Token name = Expect(TokenType::IDENTIFIER, "expected schema name");
    stmt->schema_name = name.lexeme;
    return stmt;
}

StatementPtr Parser::ParseDropSchemaStatement() {
    Expect(TokenType::KEYWORD_SCHEMA, "expected SCHEMA");
    auto stmt = std::make_shared<DropSchemaStatement>();
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
    Token name = Expect(TokenType::IDENTIFIER, "expected schema name");
    stmt->schema_name = name.lexeme;
    return stmt;
}

StatementPtr Parser::ParseCreateSequenceStatement() {
    Expect(TokenType::KEYWORD_SEQUENCE, "expected SEQUENCE");
    auto stmt = std::make_shared<CreateSequenceStatement>();
    if (Check(TokenType::KEYWORD_IF)) {
        Advance();
        Expect(TokenType::KEYWORD_NOT, "expected NOT after IF");
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
        stmt->if_not_exists = true;
    }
    Token name = Expect(TokenType::IDENTIFIER, "expected sequence name");
    stmt->sequence_name = name.lexeme;
    // 可选 START n（START 不是关键字，按标识符处理）
    if (Check(TokenType::IDENTIFIER) && CurrentToken().lexeme == "START") {
        Advance();
        Token v = Expect(TokenType::INTEGER_LITERAL, "expected integer after START");
        stmt->start_value = std::atoll(v.lexeme.c_str());
    }
    // 可选 INCREMENT n（INCREMENT 不是关键字，按标识符处理）
    if (Check(TokenType::IDENTIFIER) && CurrentToken().lexeme == "INCREMENT") {
        Advance();
        Token v = Expect(TokenType::INTEGER_LITERAL, "expected integer after INCREMENT");
        stmt->increment = std::atoll(v.lexeme.c_str());
        if (stmt->increment == 0) stmt->increment = 1;
    }
    return stmt;
}

StatementPtr Parser::ParseDropSequenceStatement() {
    Expect(TokenType::KEYWORD_SEQUENCE, "expected SEQUENCE");
    auto stmt = std::make_shared<DropSequenceStatement>();
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
    Token name = Expect(TokenType::IDENTIFIER, "expected sequence name");
    stmt->sequence_name = name.lexeme;
    return stmt;
}

// 53_ddl: ParseTableNameAllowSchema —— 读取表名并接受可选的 schema 前缀。
// 用法：CREATE TABLE foo.bar (...)、DROP TABLE foo.bar、SELECT ... FROM foo.bar
// 等位置。schema 与 table 之间必须用 '.'（DOT 符号）。返回 "schema.table" 或
// 仅 "table"。schema 部分不限制关键字，单纯按标识符串处理；执行期由 catalog
// 校验 schema 是否存在。
std::string Parser::ParseTableNameAllowSchema() {
    Token first = Expect(TokenType::IDENTIFIER, "expected table name");
    std::string name = first.lexeme;
    if (Check(TokenType::DOT)) {
        Advance();
        Token second = Expect(TokenType::IDENTIFIER, "expected table name after '.'");
        name += ".";
        name += second.lexeme;
    }
    return name;
}

// ============ 59_procs (Category 8)：过程语言扩展 ============

// CREATE PROCEDURE name(args) BEGIN body END
// 形参支持 [IN] / OUT / INOUT 三种模式；缺省为 IN。
StatementPtr Parser::ParseCreateProcedureStatement() {
    if (CurrentToken().type == TokenType::KEYWORD_CREATE) {
        Advance();
    }
    Expect(TokenType::KEYWORD_PROCEDURE, "expected PROCEDURE");
    Token name = Expect(TokenType::IDENTIFIER, "expected procedure name");
    auto stmt = std::make_shared<CreateProcedureStatement>();
    stmt->procedure_name = name.lexeme;
    Expect(TokenType::LEFT_PAREN, "expected '(' after procedure name");
    if (!Check(TokenType::RIGHT_PAREN)) {
        do {
            FunctionParameter param;
            int mode = 0;
            if (Match(TokenType::KEYWORD_IN)) {
                mode = 0;
            } else if (Match(TokenType::KEYWORD_OUT)) {
                mode = 1;
            } else if (Match(TokenType::KEYWORD_INOUT)) {
                mode = 2;
            }
            Token pname = Expect(TokenType::IDENTIFIER,
                                 "expected parameter name");
            param.name = pname.lexeme;
            param.mode = mode;
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
    Expect(TokenType::KEYWORD_BEGIN, "expected BEGIN to start procedure body");
    stmt->body_statements = ParseFunctionBodyUntil(TokenType::KEYWORD_END);
    Expect(TokenType::KEYWORD_END, "expected END to close procedure body");
    return stmt;
}

StatementPtr Parser::ParseDropProcedureStatement() {
    if (CurrentToken().type == TokenType::KEYWORD_DROP) {
        Advance();
    }
    Expect(TokenType::KEYWORD_PROCEDURE, "expected PROCEDURE");
    auto stmt = std::make_shared<DropProcedureStatement>();
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
    Token name = Expect(TokenType::IDENTIFIER, "expected procedure name");
    stmt->procedure_name = name.lexeme;
    return stmt;
}

// CALL name(arg1, arg2, ...);
StatementPtr Parser::ParseCallStatement() {
    Expect(TokenType::KEYWORD_CALL, "expected CALL");
    Token name = Expect(TokenType::IDENTIFIER, "expected procedure name");
    auto stmt = std::make_shared<CallStatement>();
    stmt->procedure_name = name.lexeme;
    Expect(TokenType::LEFT_PAREN, "expected '(' after procedure name");
    if (!Check(TokenType::RIGHT_PAREN)) {
        do {
            stmt->arguments.push_back(ParseExpression());
        } while (Match(TokenType::COMMA));
    }
    Expect(TokenType::RIGHT_PAREN, "expected ')' after argument list");
    Match(TokenType::SEMICOLON);
    return stmt;
}

// [label:] LOOP body END LOOP [label];
StatementPtr Parser::ParseLoopStatement() {
    Expect(TokenType::KEYWORD_LOOP, "expected LOOP");
    auto stmt = std::make_shared<LoopStatement>();
    stmt->body = ParseFunctionBodyUntil(TokenType::KEYWORD_END);
    Expect(TokenType::KEYWORD_END, "expected END to close LOOP");
    Expect(TokenType::KEYWORD_LOOP, "expected LOOP after END in LOOP statement");
    // 可选结束 label：END LOOP [label];
    if (CurrentToken().type == TokenType::IDENTIFIER) {
        stmt->label = CurrentToken().lexeme;
        Advance();
        Match(TokenType::SEMICOLON);
    } else {
        Match(TokenType::SEMICOLON);
    }
    return stmt;
}

// REPEAT body UNTIL cond END REPEAT [label];
StatementPtr Parser::ParseRepeatStatement() {
    Expect(TokenType::KEYWORD_REPEAT, "expected REPEAT");
    auto stmt = std::make_shared<RepeatStatement>();
    stmt->body = ParseFunctionBodyUntil(TokenType::KEYWORD_UNTIL);
    Expect(TokenType::KEYWORD_UNTIL, "expected UNTIL in REPEAT");
    stmt->until_expr = ParseExpression();
    Expect(TokenType::KEYWORD_END, "expected END to close REPEAT");
    Expect(TokenType::KEYWORD_REPEAT, "expected REPEAT after END in REPEAT statement");
    if (CurrentToken().type == TokenType::IDENTIFIER) {
        stmt->label = CurrentToken().lexeme;
        Advance();
        Match(TokenType::SEMICOLON);
    } else {
        Match(TokenType::SEMICOLON);
    }
    return stmt;
}

// CASE [subject] WHEN cond THEN stmts ... [ELSE stmts] END CASE;
// 体内 CASE（不同于表达式 CASE）。若首 token 之后是 WHEN 视为搜索式；
// 否则读 subject（解析到一个 IF/UNARY/LITERAL 之类），再次决策。
StatementPtr Parser::ParseBodyCaseStatement() {
    Expect(TokenType::KEYWORD_CASE, "expected CASE");
    auto stmt = std::make_shared<CaseStatement>();
    // 简单 CASE 必须先有"主体表达式"再是 WHEN：
    //   CASE x WHEN 1 THEN ... ELSE ... END CASE;
    // 搜索式 CASE 立即是 WHEN：
    //   CASE WHEN x > 0 THEN ... ELSE ... END CASE;
    // 解析规则：尝试先解析一个表达式（仅当下一 token 不是 WHEN 时）。
    if (!Check(TokenType::KEYWORD_WHEN)) {
        stmt->subject = ParseExpression();
    }
    // 循环 WHEN clause
    while (Match(TokenType::KEYWORD_WHEN)) {
        CaseStatement::WhenClause wc;
        wc.when_expr = ParseExpression();
        Expect(TokenType::KEYWORD_THEN, "expected THEN in CASE WHEN");
        // CASE WHEN body 解析：循环到下一个 WHEN / ELSE / END 才停止。
        std::vector<StatementPtr> body;
        while (!IsAtEnd()) {
            while (Match(TokenType::SEMICOLON)) {}
            if (IsAtEnd()) break;
            if (Check(TokenType::KEYWORD_WHEN) ||
                Check(TokenType::KEYWORD_ELSE) ||
                Check(TokenType::KEYWORD_END)) {
                break;
            }
            if (Check(TokenType::KEYWORD_CASE)) {
                // END CASE 之后误留的 CASE（罕见）；视为结束
                break;
            }
            StatementPtr s = ParseFunctionBodyStatement();
            if (s) body.push_back(std::move(s));
        }
        wc.body = std::move(body);
        stmt->whens.push_back(std::move(wc));
    }
    if (Match(TokenType::KEYWORD_ELSE)) {
        stmt->else_body = ParseFunctionBodyUntil(TokenType::KEYWORD_END);
    }
    Expect(TokenType::KEYWORD_END, "expected END to close CASE");
    Expect(TokenType::KEYWORD_CASE, "expected CASE after END in CASE statement");
    Match(TokenType::SEMICOLON);
    return stmt;
}

StatementPtr Parser::ParseLeaveStatement() {
    Expect(TokenType::KEYWORD_LEAVE, "expected LEAVE");
    // label 可空：空 label 让执行器跳出最近的循环（任意 label）。
    std::string label;
    if (CurrentToken().type == TokenType::IDENTIFIER) {
        label = CurrentToken().lexeme;
        Advance();
    }
    Match(TokenType::SEMICOLON);
    return std::make_shared<LeaveStatement>(label);
}

StatementPtr Parser::ParseIterateStatement() {
    Expect(TokenType::KEYWORD_ITERATE, "expected ITERATE");
    // label 可空。
    std::string label;
    if (CurrentToken().type == TokenType::IDENTIFIER) {
        label = CurrentToken().lexeme;
        Advance();
    }
    Match(TokenType::SEMICOLON);
    return std::make_shared<IterateStatement>(label);
}

// SIGNAL SQLSTATE 'XXXXX' SET MESSAGE_TEXT = 'msg';
StatementPtr Parser::ParseSignalStatement() {
    Expect(TokenType::KEYWORD_SIGNAL, "expected SIGNAL");
    Expect(TokenType::KEYWORD_SQLSTATE, "expected SQLSTATE after SIGNAL");
    Token st = Expect(TokenType::STRING_LITERAL,
                      "expected string literal SQLSTATE");
    if (st.lexeme.size() != 5) {
        throw CompilerException(ErrorStage::SYNTAX,
            "SQLSTATE must be a 5-character code (got '" + st.lexeme + "')",
            st.line, st.column);
    }
    Expect(TokenType::KEYWORD_SET, "expected SET after SQLSTATE");
    Expect(TokenType::KEYWORD_MESSAGE_TEXT, "expected MESSAGE_TEXT after SET");
    Expect(TokenType::OP_EQUAL, "expected '=' after MESSAGE_TEXT");
    Token msg = Expect(TokenType::STRING_LITERAL,
                       "expected string literal MESSAGE_TEXT");
    Match(TokenType::SEMICOLON);
    auto stmt = std::make_shared<SignalStatement>();
    stmt->sqlstate = st.lexeme;
    stmt->message_text = msg.lexeme;
    return stmt;
}

// DECLARE {EXIT|CONTINUE|UNDO} HANDLER FOR cond stmt;
// condition: SQLEXCEPTION | SQLWARNING | NOT FOUND | SQLSTATE 'XXXXX'
StatementPtr Parser::ParseDeclareHandlerStatement() {
    Expect(TokenType::KEYWORD_DECLARE, "expected DECLARE");
    auto stmt = std::make_shared<DeclareHandlerStatement>();
    if (Match(TokenType::KEYWORD_CONTINUE)) {
        stmt->type = DeclareHandlerStatement::Type::CONTINUE;
    } else if (Match(TokenType::KEYWORD_EXIT)) {
        stmt->type = DeclareHandlerStatement::Type::EXIT;
    } else if (Match(TokenType::KEYWORD_UNDO)) {
        stmt->type = DeclareHandlerStatement::Type::UNDO;
    } else {
        // 缺省视作 CONTINUE
        stmt->type = DeclareHandlerStatement::Type::CONTINUE;
    }
    Expect(TokenType::KEYWORD_HANDLER, "expected HANDLER after CONTINUE/EXIT/UNDO");
    Expect(TokenType::KEYWORD_FOR, "expected FOR after HANDLER");
    if (Match(TokenType::KEYWORD_SQLEXCEPTION)) {
        stmt->cond_kind = DeclareHandlerStatement::CondKind::SQLEXCEPTION;
    } else if (Match(TokenType::KEYWORD_SQLWARNING)) {
        stmt->cond_kind = DeclareHandlerStatement::CondKind::SQLWARNING;
    } else if (Match(TokenType::KEYWORD_NOT)) {
        Expect(TokenType::KEYWORD_FOUND, "expected FOUND after NOT");
        stmt->cond_kind = DeclareHandlerStatement::CondKind::NOT_FOUND;
    } else if (Match(TokenType::KEYWORD_SQLSTATE)) {
        Token st = Expect(TokenType::STRING_LITERAL,
                          "expected string literal SQLSTATE");
        if (st.lexeme.size() != 5) {
            throw CompilerException(ErrorStage::SYNTAX,
                "SQLSTATE must be a 5-character code (got '" + st.lexeme + "')",
                st.line, st.column);
        }
        stmt->cond_kind = DeclareHandlerStatement::CondKind::SQLSTATE;
        stmt->cond_sqlstate = st.lexeme;
    } else {
        throw CompilerException(ErrorStage::SYNTAX,
            "expected SQLEXCEPTION / SQLWARNING / NOT FOUND / SQLSTATE",
            CurrentToken().line, CurrentToken().column);
    }
    // handler body：当前 V1 限定为单条语句。
    stmt->body = ParseFunctionBodyStatement();
    return stmt;
}

// DECLARE name CURSOR FOR <select>;
StatementPtr Parser::ParseDeclareCursorStatement() {
    Expect(TokenType::KEYWORD_DECLARE, "expected DECLARE");
    Token name = Expect(TokenType::IDENTIFIER, "expected cursor name");
    Expect(TokenType::KEYWORD_CURSOR, "expected CURSOR after cursor name");
    Expect(TokenType::KEYWORD_FOR, "expected FOR after CURSOR");
    auto stmt_ptr = ParseSelectStatementWithSetOps();
    // V1 简化：cursor 仅接受单条 SELECT，不支持 UNION 等集合运算。
    auto sel = std::dynamic_pointer_cast<SelectStatement>(stmt_ptr);
    if (!sel) {
        throw CompilerException(ErrorStage::SYNTAX,
            "cursor query must be a plain SELECT (V1 limitation)");
    }
    Match(TokenType::SEMICOLON);
    return std::make_shared<DeclareCursorStatement>(name.lexeme, sel);
}

StatementPtr Parser::ParseCursorOpenStatement() {
    Expect(TokenType::KEYWORD_OPEN, "expected OPEN");
    Token name = Expect(TokenType::IDENTIFIER, "expected cursor name after OPEN");
    Match(TokenType::SEMICOLON);
    return std::make_shared<CursorOpenStatement>(name.lexeme);
}

StatementPtr Parser::ParseCursorFetchStatement() {
    Expect(TokenType::KEYWORD_FETCH, "expected FETCH");
    Token name = Expect(TokenType::IDENTIFIER, "expected cursor name after FETCH");
    Expect(TokenType::KEYWORD_INTO, "expected INTO after FETCH cursor");
    std::vector<std::string> into;
    do {
        Token v = Expect(TokenType::IDENTIFIER,
                         "expected variable name in FETCH INTO");
        into.push_back(v.lexeme);
    } while (Match(TokenType::COMMA));
    Match(TokenType::SEMICOLON);
    return std::make_shared<CursorFetchStatement>(name.lexeme, std::move(into));
}

StatementPtr Parser::ParseCursorCloseStatement() {
    Expect(TokenType::KEYWORD_CLOSE, "expected CLOSE");
    Token name = Expect(TokenType::IDENTIFIER, "expected cursor name after CLOSE");
    Match(TokenType::SEMICOLON);
    return std::make_shared<CursorCloseStatement>(name.lexeme);
}

}  // namespace sqlcompiler