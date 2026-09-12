#include "lexer/Lexer.h"

#include "common/Error.h"

#include <cctype>
#include <unordered_map>

namespace sqlcompiler {

namespace {

const std::unordered_map<std::string, TokenType>& KeywordTable() {
    static const std::unordered_map<std::string, TokenType> kKeywords = {
        {"SELECT",   TokenType::KEYWORD_SELECT},
        {"FROM",     TokenType::KEYWORD_FROM},
        {"WHERE",    TokenType::KEYWORD_WHERE},
        {"INSERT",   TokenType::KEYWORD_INSERT},
        {"INTO",     TokenType::KEYWORD_INTO},
        {"VALUES",   TokenType::KEYWORD_VALUES},
        {"UPDATE",   TokenType::KEYWORD_UPDATE},
        {"SET",      TokenType::KEYWORD_SET},
        {"DELETE",   TokenType::KEYWORD_DELETE},
        {"CREATE",   TokenType::KEYWORD_CREATE},
        {"TABLE",    TokenType::KEYWORD_TABLE},
        {"INDEX",    TokenType::KEYWORD_INDEX},
        {"UNIQUE",   TokenType::KEYWORD_UNIQUE},
        {"DROP",     TokenType::KEYWORD_DROP},
        {"AND",      TokenType::KEYWORD_AND},
        {"OR",       TokenType::KEYWORD_OR},
        {"NOT",      TokenType::KEYWORD_NOT},
        {"NULL",     TokenType::KEYWORD_NULL},
        {"ORDER",    TokenType::KEYWORD_ORDER},
        {"BY",       TokenType::KEYWORD_BY},
        {"GROUP",    TokenType::KEYWORD_GROUP},
        {"HAVING",   TokenType::KEYWORD_HAVING},
        {"JOIN",     TokenType::KEYWORD_JOIN},
        {"INNER",    TokenType::KEYWORD_INNER},
        {"LEFT",     TokenType::KEYWORD_LEFT},
        {"RIGHT",    TokenType::KEYWORD_RIGHT},
        {"FULL",     TokenType::KEYWORD_FULL},
        {"OUTER",    TokenType::KEYWORD_OUTER},
        {"CROSS",    TokenType::KEYWORD_CROSS},
        {"NATURAL",  TokenType::KEYWORD_NATURAL},
        {"USING",    TokenType::KEYWORD_USING},
        {"ON",       TokenType::KEYWORD_ON},
        {"AS",       TokenType::KEYWORD_AS},
        {"DISTINCT", TokenType::KEYWORD_DISTINCT},
        {"LIMIT",    TokenType::KEYWORD_LIMIT},
        {"INT",      TokenType::KEYWORD_INT},
        {"VARCHAR",  TokenType::KEYWORD_VARCHAR},
        {"FLOAT",    TokenType::KEYWORD_FLOAT},
        {"PRIMARY",  TokenType::KEYWORD_PRIMARY},
        {"KEY",      TokenType::KEYWORD_KEY},
        {"IS",       TokenType::KEYWORD_IS},
        {"LIKE",     TokenType::KEYWORD_LIKE},
        {"IN",       TokenType::KEYWORD_IN},
        {"BETWEEN",  TokenType::KEYWORD_BETWEEN},
        {"ASC",      TokenType::KEYWORD_ASC},
        {"DESC",     TokenType::KEYWORD_DESC},
        {"IF",       TokenType::KEYWORD_IF},
        {"DUPLICATE",TokenType::KEYWORD_DUPLICATE},
        {"TRUNCATE", TokenType::KEYWORD_TRUNCATE},
        {"ALTER",    TokenType::KEYWORD_ALTER},
        {"ADD",      TokenType::KEYWORD_ADD},
        {"RENAME",   TokenType::KEYWORD_RENAME},
        {"MODIFY",   TokenType::KEYWORD_MODIFY},
        {"CHECK",    TokenType::KEYWORD_CHECK},
        {"DEFAULT",  TokenType::KEYWORD_DEFAULT},
        {"TO",       TokenType::KEYWORD_TO},
        {"COLUMN",   TokenType::KEYWORD_COLUMN},
        {"CASE",     TokenType::KEYWORD_CASE},
        {"WHEN",     TokenType::KEYWORD_WHEN},
        {"THEN",     TokenType::KEYWORD_THEN},
        {"ELSE",     TokenType::KEYWORD_ELSE},
        {"END",      TokenType::KEYWORD_END},
        {"CAST",     TokenType::KEYWORD_CAST},
        {"COALESCE", TokenType::KEYWORD_COALESCE},
        {"NULLIF",   TokenType::KEYWORD_NULLIF},
        {"WITH",     TokenType::KEYWORD_WITH},
        {"RECURSIVE",TokenType::KEYWORD_RECURSIVE},
        {"OVER",     TokenType::KEYWORD_OVER},
        {"PARTITION",TokenType::KEYWORD_PARTITION},
        {"ROWS",     TokenType::KEYWORD_ROWS},
        {"RANGE",    TokenType::KEYWORD_RANGE},
        {"UNBOUNDED",TokenType::KEYWORD_UNBOUNDED},
        {"PRECEDING",TokenType::KEYWORD_PRECEDING},
        {"FOLLOWING",TokenType::KEYWORD_FOLLOWING},
        {"CURRENT",  TokenType::KEYWORD_CURRENT},
        {"ROW",      TokenType::KEYWORD_ROW},
        {"WINDOW",   TokenType::KEYWORD_WINDOW},
        {"ROW_NUMBER", TokenType::KEYWORD_ROW_NUMBER},
        {"RANK",     TokenType::KEYWORD_RANK},
        {"DENSE_RANK", TokenType::KEYWORD_DENSE_RANK},
        {"NTILE",    TokenType::KEYWORD_NTILE},
        {"LAG",      TokenType::KEYWORD_LAG},
        {"LEAD",     TokenType::KEYWORD_LEAD},
        {"FIRST_VALUE", TokenType::KEYWORD_FIRST_VALUE},
        {"LAST_VALUE",  TokenType::KEYWORD_LAST_VALUE},
        {"PERCENT_RANK",TokenType::KEYWORD_PERCENT_RANK},
        {"CUME_DIST",   TokenType::KEYWORD_CUME_DIST},
        {"UNION",    TokenType::KEYWORD_UNION},
        {"INTERSECT",TokenType::KEYWORD_INTERSECT},
        {"EXCEPT",   TokenType::KEYWORD_EXCEPT},
        {"ANY",      TokenType::KEYWORD_ANY},
        {"ALL",      TokenType::KEYWORD_ALL},
        {"EXISTS",   TokenType::KEYWORD_EXISTS},
        {"UPPER",    TokenType::KEYWORD_UPPER},
        {"LOWER",    TokenType::KEYWORD_LOWER},
        {"LENGTH",   TokenType::KEYWORD_LENGTH},
        {"SUBSTR",   TokenType::KEYWORD_SUBSTR},
        {"TRIM",     TokenType::KEYWORD_TRIM},
        {"REPLACE",  TokenType::KEYWORD_REPLACE},
        {"ROUND",    TokenType::KEYWORD_ROUND},
        {"CEIL",     TokenType::KEYWORD_CEIL},
        {"FLOOR",    TokenType::KEYWORD_FLOOR},
        {"ABS",      TokenType::KEYWORD_ABS},
        {"POWER",    TokenType::KEYWORD_POWER},
        {"MOD",      TokenType::KEYWORD_MOD},
        {"YEAR",     TokenType::KEYWORD_YEAR},
        {"MONTH",    TokenType::KEYWORD_MONTH},
        {"DAY",      TokenType::KEYWORD_DAY},
        {"NOW",      TokenType::KEYWORD_NOW},
        {"IFNULL",   TokenType::KEYWORD_IFNULL},
        // 45_datetime: EXTRACT 字段（HOUR/MINUTE/SECOND）也作为关键字注册，
        // 解析器在 EXTRACT 上下文中识别它们，普通位置仍允许作为标识符使用
        // （Parser 中通过 Check + PeekToken(1) 判定）。
        {"HOUR",     TokenType::KEYWORD_HOUR},
        {"MINUTE",   TokenType::KEYWORD_MINUTE},
        {"SECOND",   TokenType::KEYWORD_SECOND},
        // 40_txn_view_udf: 事务 / 视图 / 触发器 / UDF 关键字
        {"BEGIN",     TokenType::KEYWORD_BEGIN},
        {"TRANSACTION", TokenType::KEYWORD_TRANSACTION},
        {"COMMIT",    TokenType::KEYWORD_COMMIT},
        {"ROLLBACK",  TokenType::KEYWORD_ROLLBACK},
        {"SAVEPOINT", TokenType::KEYWORD_SAVEPOINT},
        {"RELEASE",   TokenType::KEYWORD_RELEASE},
        {"VIEW",      TokenType::KEYWORD_VIEW},
        {"TRIGGER",   TokenType::KEYWORD_TRIGGER},
        {"FUNCTION",  TokenType::KEYWORD_FUNCTION},
        {"BEFORE",    TokenType::KEYWORD_BEFORE},
        {"AFTER",     TokenType::KEYWORD_AFTER},
        {"FOR",       TokenType::KEYWORD_FOR},
        {"EACH",      TokenType::KEYWORD_EACH},
        {"NEW",       TokenType::KEYWORD_NEW},
        {"OLD",       TokenType::KEYWORD_OLD},
        {"RETURN",    TokenType::KEYWORD_RETURN},
        {"RETURNS",   TokenType::KEYWORD_RETURNS},
        // 47_udf_trigger_view: UDF 体内部使用的语句关键字
        {"DECLARE",   TokenType::KEYWORD_DECLARE},
        {"WHILE",     TokenType::KEYWORD_WHILE},
        {"DO",        TokenType::KEYWORD_DO},
        {"ELSEIF",    TokenType::KEYWORD_ELSEIF},
        // 44_pattern_match: 模式匹配扩展关键字
        {"ILIKE",     TokenType::KEYWORD_ILIKE},
        {"REGEXP",    TokenType::KEYWORD_REGEXP},
        {"RLIKE",     TokenType::KEYWORD_RLIKE},
        {"ESCAPE",    TokenType::KEYWORD_ESCAPE},
        // 56_pattern: SIMILAR TO 扩展关键字
        {"SIMILAR",   TokenType::KEYWORD_SIMILAR},
        // 45_datetime: DATE / TIMESTAMP / INTERVAL / EXTRACT
        {"DATE",      TokenType::KEYWORD_DATE},
        {"TIMESTAMP", TokenType::KEYWORD_TIMESTAMP},
        {"INTERVAL",  TokenType::KEYWORD_INTERVAL},
        {"EXTRACT",   TokenType::KEYWORD_EXTRACT},
        // 46_meta: 元命令关键字。DESC / INDEX / FROM / CREATE 复用上方已注册项。
        {"EXPLAIN",   TokenType::KEYWORD_EXPLAIN},
        {"SHOW",      TokenType::KEYWORD_SHOW},
        {"DESCRIBE",  TokenType::KEYWORD_DESCRIBE},
        {"TABLES",    TokenType::KEYWORD_TABLES},
        {"COLUMNS",   TokenType::KEYWORD_COLUMNS},
        // 52_data_types: 新数据类型关键字
        {"BOOLEAN",   TokenType::KEYWORD_BOOLEAN},
        {"BOOL",      TokenType::KEYWORD_BOOL},
        {"CHAR",      TokenType::KEYWORD_CHAR},
        {"TEXT",      TokenType::KEYWORD_TEXT},
        {"DECIMAL",   TokenType::KEYWORD_DECIMAL},
        {"NUMERIC",   TokenType::KEYWORD_NUMERIC},
        {"DOUBLE",    TokenType::KEYWORD_DOUBLE},
        {"REAL",      TokenType::KEYWORD_REAL},
        {"SMALLINT",  TokenType::KEYWORD_SMALLINT},
        {"TINYINT",   TokenType::KEYWORD_TINYINT},
        {"TIME",      TokenType::KEYWORD_TIME},
        {"JSON",      TokenType::KEYWORD_JSON},
        {"UUID",      TokenType::KEYWORD_UUID},
        {"AUTO_INCREMENT", TokenType::KEYWORD_AUTO_INCREMENT},
        {"SERIAL",    TokenType::KEYWORD_SERIAL},
        {"IDENTITY",  TokenType::KEYWORD_IDENTITY},
        {"TRUE",      TokenType::KEYWORD_TRUE},
        {"FALSE",     TokenType::KEYWORD_FALSE},
        // 53_ddl: DDL 扩展（FK / SCHEMA / SEQUENCE）
        {"SCHEMA",    TokenType::KEYWORD_SCHEMA},
        {"SEQUENCE",  TokenType::KEYWORD_SEQUENCE},
        {"NEXTVAL",   TokenType::KEYWORD_NEXTVAL},
        {"FOREIGN",   TokenType::KEYWORD_FOREIGN},
        {"REFERENCES",TokenType::KEYWORD_REFERENCES},
        {"CASCADE",   TokenType::KEYWORD_CASCADE},
        {"RESTRICT",  TokenType::KEYWORD_RESTRICT},
        {"ACTION",    TokenType::KEYWORD_ACTION},
        // 58_constraints: 命名约束前缀
        {"CONSTRAINT", TokenType::KEYWORD_CONSTRAINT},
        // 54_dml: DML 扩展（MERGE / RETURNING）
        {"MERGE",     TokenType::KEYWORD_MERGE},
        {"MATCHED",   TokenType::KEYWORD_MATCHED},
        {"RETURNING", TokenType::KEYWORD_RETURNING},
        // 55_query: 查询/表达式扩展关键字
        {"LATERAL", TokenType::KEYWORD_LATERAL},
        {"FETCH",   TokenType::KEYWORD_FETCH},
        {"OFFSET",  TokenType::KEYWORD_OFFSET},
        {"FIRST",   TokenType::KEYWORD_FIRST},
        {"NEXT",    TokenType::KEYWORD_NEXT},
        // ROWS / KEY 已在 KEYWORD_ROWS / KEYWORD_KEY 处声明，复用同一枚举值。
        {"ONLY",    TokenType::KEYWORD_ONLY},
        {"TIES",    TokenType::KEYWORD_TIES},
        {"SHARE",   TokenType::KEYWORD_SHARE},
        {"NO",      TokenType::KEYWORD_NO},
        // ---- 60_funcs: Category 6 函数 / 聚合扩展 ----
        // 聚合 / 标量函数名（STDDEV / VARIANCE / MEDIAN / STRING_AGG /
        // GROUP_CONCAT / PERCENTILE_CONT / PERCENTILE_DISC / GREATEST /
        // LEAST / RAND / RANDOM）保留为标识符，由 parser / executor 按
        // FunctionCallExpr.function_name 字段识别；不注册为关键字，
        // 否则 lexer 会把它们变成 KEYWORD_* 与 ParseColumnRefOrFunctionCall
        // 期望的 IDENTIFIER 冲突。
        {"FILTER",          TokenType::KEYWORD_FILTER},
        {"SETS",            TokenType::KEYWORD_SETS},
        {"ROLLUP",          TokenType::KEYWORD_ROLLUP},
        {"CUBE",            TokenType::KEYWORD_CUBE},
        {"GROUPING",        TokenType::KEYWORD_GROUPING},
        {"IGNORE",          TokenType::KEYWORD_IGNORE},
        {"RESPECT",         TokenType::KEYWORD_RESPECT},
        {"NULLS",           TokenType::KEYWORD_NULLS},
        {"WITHIN",          TokenType::KEYWORD_WITHIN},
        // ---- 59_procs (Category 8): 过程语言扩展 ----
        {"LOOP",            TokenType::KEYWORD_LOOP},
        {"REPEAT",          TokenType::KEYWORD_REPEAT},
        {"UNTIL",           TokenType::KEYWORD_UNTIL},
        {"OPEN",            TokenType::KEYWORD_OPEN},
        {"CLOSE",           TokenType::KEYWORD_CLOSE},
        {"LEAVE",           TokenType::KEYWORD_LEAVE},
        {"ITERATE",         TokenType::KEYWORD_ITERATE},
        {"SIGNAL",          TokenType::KEYWORD_SIGNAL},
        {"SQLSTATE",        TokenType::KEYWORD_SQLSTATE},
        {"MESSAGE_TEXT",    TokenType::KEYWORD_MESSAGE_TEXT},
        {"HANDLER",         TokenType::KEYWORD_HANDLER},
        {"CONTINUE",        TokenType::KEYWORD_CONTINUE},
        {"SQLEXCEPTION",    TokenType::KEYWORD_SQLEXCEPTION},
        {"SQLWARNING",      TokenType::KEYWORD_SQLWARNING},
        {"FOUND",           TokenType::KEYWORD_FOUND},
        {"CURSOR",          TokenType::KEYWORD_CURSOR},
        {"PROCEDURE",       TokenType::KEYWORD_PROCEDURE},
        {"CALL",            TokenType::KEYWORD_CALL},
        {"OUT",             TokenType::KEYWORD_OUT},
        {"INOUT",           TokenType::KEYWORD_INOUT},
        {"EXIT",            TokenType::KEYWORD_EXIT},
        {"UNDO",            TokenType::KEYWORD_UNDO},
        {"CONDITION",       TokenType::KEYWORD_CONDITION},
        // ---- 60_view_trigger (Category 9): VIEW / TRIGGER 扩展 ----
        {"MATERIALIZED",    TokenType::KEYWORD_MATERIALIZED},
        {"REFRESH",         TokenType::KEYWORD_REFRESH},
        {"STATEMENT",       TokenType::KEYWORD_STATEMENT},
        {"OPTION",          TokenType::KEYWORD_OPTION},
        {"CASCADED",        TokenType::KEYWORD_CASCADED},
        {"LOCAL",           TokenType::KEYWORD_LOCAL},
    };
    return kKeywords;
}

std::string ToUpper(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

bool IsIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool IsIdentPart(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool IsDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c));
}

}  // namespace

Lexer::Lexer(const std::string& source) : source_(source), pos_(0), line_(1), column_(1) {
}

std::vector<Token> Lexer::Tokenize() {
    std::vector<Token> tokens;
    while (true) {
        Token tok = NextToken();
        tokens.push_back(tok);
        if (tok.type == TokenType::END_OF_FILE) {
            break;
        }
    }
    return tokens;
}

bool Lexer::IsAtEnd() const {
    return pos_ >= source_.size();
}

char Lexer::CurrentChar() const {
    if (pos_ >= source_.size()) return '\0';
    return source_[pos_];
}

char Lexer::PeekChar(int offset) const {
    size_t idx = pos_ + static_cast<size_t>(offset);
    if (idx >= source_.size()) return '\0';
    return source_[idx];
}

void Lexer::Advance() {
    if (pos_ >= source_.size()) return;
    if (source_[pos_] == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    ++pos_;
}

void Lexer::SkipWhitespaceAndComments() {
    while (!IsAtEnd()) {
        char c = CurrentChar();
        if (std::isspace(static_cast<unsigned char>(c))) {
            Advance();
            continue;
        }
        if (c == '-' && PeekChar() == '-') {
            while (!IsAtEnd() && CurrentChar() != '\n') {
                Advance();
            }
            continue;
        }
        if (c == '/' && PeekChar() == '*') {
            Advance(); // '/'
            Advance(); // '*'
            while (!IsAtEnd()) {
                if (CurrentChar() == '*' && PeekChar() == '/') {
                    Advance();
                    Advance();
                    break;
                }
                Advance();
            }
            continue;
        }
        break;
    }
}

TokenType Lexer::LookupKeyword(const std::string& text) const {
    std::string upper = ToUpper(text);
    auto& table = KeywordTable();
    auto it = table.find(upper);
    if (it != table.end()) {
        return it->second;
    }
    return TokenType::IDENTIFIER;
}

Token Lexer::ScanIdentifierOrKeyword() {
    int start_line = line_;
    int start_col = column_;
    size_t start_pos = pos_;
    while (!IsAtEnd() && IsIdentPart(CurrentChar())) {
        Advance();
    }
    std::string text = source_.substr(start_pos, pos_ - start_pos);
    TokenType type = LookupKeyword(text);
    return Token(type, text, start_line, start_col);
}

Token Lexer::ScanNumber() {
    int start_line = line_;
    int start_col = column_;
    size_t start_pos = pos_;
    while (!IsAtEnd() && IsDigit(CurrentChar())) {
        Advance();
    }
    bool is_float = false;
    if (!IsAtEnd() && CurrentChar() == '.' && IsDigit(PeekChar())) {
        is_float = true;
        Advance();
        while (!IsAtEnd() && IsDigit(CurrentChar())) {
            Advance();
        }
    }
    // Scientific notation: [e|E][+|-]?digits  (only when there's already a
    // fractional part OR an integer is immediately followed by e/E+digit,
    // so that plain identifiers like 'e' or 'e1' are still lexed as IDENTIFIER)
    if (!IsAtEnd() && (CurrentChar() == 'e' || CurrentChar() == 'E')) {
        bool has_sign = (PeekChar() == '+' || PeekChar() == '-');
        size_t after_exp = pos_ + 1 + (has_sign ? 1 : 0);
        bool exp_has_digit = (after_exp < source_.size() &&
                              IsDigit(source_[after_exp]));
        if (is_float || exp_has_digit) {
            is_float = true;
            Advance(); // 'e' or 'E'
            if (!IsAtEnd() && (CurrentChar() == '+' || CurrentChar() == '-')) {
                Advance();
            }
            while (!IsAtEnd() && IsDigit(CurrentChar())) {
                Advance();
            }
        }
    }
    std::string text = source_.substr(start_pos, pos_ - start_pos);
    TokenType type = is_float ? TokenType::FLOAT_LITERAL : TokenType::INTEGER_LITERAL;
    return Token(type, text, start_line, start_col);
}

Token Lexer::ScanString() {
    int start_line = line_;
    int start_col = column_;
    Advance(); // consume opening '
    std::string buf;
    while (!IsAtEnd()) {
        char c = CurrentChar();
        if (c == '\'') {
            // SQL 标准双单引号转义 ''
            if (PeekChar() == '\'') {
                buf.push_back('\'');
                Advance();
                Advance();
                continue;
            }
            // 闭合引号
            break;
        }
        if (c == '\\' && PeekChar() != '\0') {
            Advance();
            char esc = CurrentChar();
            switch (esc) {
                case 'n': buf.push_back('\n'); break;
                case 't': buf.push_back('\t'); break;
                case 'r': buf.push_back('\r'); break;
                case '\\': buf.push_back('\\'); break;
                case '\'': buf.push_back('\''); break;
                case '"': buf.push_back('"'); break;
                case '0': buf.push_back('\0'); break;
                // SQL 标准：未识别的 \<char> 保留两个字符 (\ 和原字符)，
                // 而非像 C 那样丢掉 \。这是为了让 44_pattern_match 的
                // `LIKE '\%' ESCAPE '\\'` 能正确得到模式 `\%`。
                default:
                    buf.push_back('\\');
                    buf.push_back(esc);
                    break;
            }
            Advance();
            continue;
        }
        buf.push_back(c);
        Advance();
    }
    if (IsAtEnd()) {
        throw CompilerException(ErrorStage::LEXICAL,
                                "unterminated string literal", start_line, start_col);
    }
    Advance(); // consume closing '
    return Token(TokenType::STRING_LITERAL, buf, start_line, start_col);
}

Token Lexer::ScanOperatorOrSymbol() {
    int start_line = line_;
    int start_col = column_;
    char c = CurrentChar();
    char n = PeekChar();
    // Backtick identifier (for ``...`` 形式的中文等特殊标识符)
    if (c == '`') {
        int start_line = line_;
        int start_col = column_;
        Advance(); // consume opening `
        std::string buf;
        while (!IsAtEnd() && CurrentChar() != '`') {
            buf.push_back(CurrentChar());
            Advance();
        }
        if (IsAtEnd()) {
            throw CompilerException(ErrorStage::LEXICAL,
                "unterminated backtick identifier", start_line, start_col);
        }
        Advance(); // consume closing `
        return Token(TokenType::IDENTIFIER, buf, start_line, start_col);
    }
    // Two-character operators first
    if (c == '!' && n == '=') {
        Advance(); Advance();
        return Token(TokenType::OP_NOT_EQUAL, "!=", start_line, start_col);
    }
    if (c == '<' && n == '=') {
        Advance(); Advance();
        return Token(TokenType::OP_LESS_EQUAL, "<=", start_line, start_col);
    }
    if (c == '>' && n == '=') {
        Advance(); Advance();
        return Token(TokenType::OP_GREATER_EQUAL, ">=", start_line, start_col);
    }
    if (c == '<' && n == '>') {
        Advance(); Advance();
        return Token(TokenType::OP_NOT_EQUAL, "<>", start_line, start_col);
    }
    if (c == '|' && n == '|') {
        Advance(); Advance();
        return Token(TokenType::OP_CONCAT, "||", start_line, start_col);
    }
    Advance();
    switch (c) {
        case '=': return Token(TokenType::OP_EQUAL, "=", start_line, start_col);
        case '<': return Token(TokenType::OP_LESS, "<", start_line, start_col);
        case '>': return Token(TokenType::OP_GREATER, ">", start_line, start_col);
        case '+': return Token(TokenType::OP_PLUS, "+", start_line, start_col);
        case '-': return Token(TokenType::OP_MINUS, "-", start_line, start_col);
        case '*': return Token(TokenType::OP_STAR, "*", start_line, start_col);
        case '/': return Token(TokenType::OP_SLASH, "/", start_line, start_col);
        case '(': return Token(TokenType::LEFT_PAREN, "(", start_line, start_col);
        case ')': return Token(TokenType::RIGHT_PAREN, ")", start_line, start_col);
        case ',': return Token(TokenType::COMMA, ",", start_line, start_col);
        case ';': return Token(TokenType::SEMICOLON, ";", start_line, start_col);
        case '.': return Token(TokenType::DOT, ".", start_line, start_col);
        default: {
            std::string bad(1, c);
            throw CompilerException(ErrorStage::LEXICAL,
                "unexpected character: '" + bad + "'", start_line, start_col);
        }
    }
}

Token Lexer::NextToken() {
    SkipWhitespaceAndComments();
    if (IsAtEnd()) {
        return Token(TokenType::END_OF_FILE, "", line_, column_);
    }
    char c = CurrentChar();
    if (IsIdentStart(c)) {
        return ScanIdentifierOrKeyword();
    }
    if (IsDigit(c)) {
        return ScanNumber();
    }
    if (c == '\'') {
        return ScanString();
    }
    Token tk = ScanOperatorOrSymbol();
    return tk;
}

Token Lexer::PeekToken() {
    size_t saved_pos = pos_;
    int saved_line = line_;
    int saved_col = column_;
    Token tok = NextToken();
    pos_ = saved_pos;
    line_ = saved_line;
    column_ = saved_col;
    return tok;
}

}  // namespace sqlcompiler