// 词法分析模块测试：覆盖关键字、标识符、字面量、运算符、注释、
// 字符串转义、行列号追踪与非法输入报错。
#include "test_framework.h"

#include "common/Error.h"
#include "lexer/Lexer.h"
#include "lexer/Token.h"

using sqlcompiler::Lexer;
using sqlcompiler::Token;
using sqlcompiler::TokenType;

namespace {

// 检查 tokens[idx] 的种别码与词素
void ExpectToken(const std::vector<Token>& tokens, size_t idx, TokenType type,
                 const char* lexeme) {
    CHECK(idx < tokens.size());
    if (idx >= tokens.size()) return;
    CHECK_EQ(tokens[idx].type, type);
    CHECK_EQ(tokens[idx].lexeme, std::string(lexeme));
}

std::vector<Token> TokenizeOrDie(const std::string& sql) {
    Lexer lexer(sql);
    return lexer.Tokenize();
}

// ---- 基础语句 ----

void TestSimpleSelect() {
    auto tokens = TokenizeOrDie("SELECT * FROM student;");
    ExpectToken(tokens, 0, TokenType::KEYWORD_SELECT, "SELECT");
    ExpectToken(tokens, 1, TokenType::OP_STAR, "*");
    ExpectToken(tokens, 2, TokenType::KEYWORD_FROM, "FROM");
    ExpectToken(tokens, 3, TokenType::IDENTIFIER, "student");
    ExpectToken(tokens, 4, TokenType::SEMICOLON, ";");
    CHECK_EQ(tokens.size(), static_cast<size_t>(6));
    CHECK_EQ(tokens.back().type, TokenType::END_OF_FILE);
}

void TestKeywordCaseInsensitive() {
    auto tokens = TokenizeOrDie("select * from t where id = 1;");
    CHECK_EQ(tokens[0].type, TokenType::KEYWORD_SELECT);
    CHECK_EQ(tokens[2].type, TokenType::KEYWORD_FROM);
    CHECK_EQ(tokens[4].type, TokenType::KEYWORD_WHERE);
    // 大小写混合
    auto mixed = TokenizeOrDie("SeLeCt a From t;");
    CHECK_EQ(mixed[0].type, TokenType::KEYWORD_SELECT);
    CHECK_EQ(mixed[2].type, TokenType::KEYWORD_FROM);
}

void TestAllStatementKeywords() {
    auto tokens = TokenizeOrDie(
        "INSERT INTO t VALUES (1);"
        " UPDATE t SET a = 1;"
        " DELETE FROM t;"
        " CREATE TABLE t (id INT);"
        " DROP TABLE t;");
    CHECK_EQ(tokens[0].type, TokenType::KEYWORD_INSERT);
    CHECK_EQ(tokens[1].type, TokenType::KEYWORD_INTO);
    CHECK_EQ(tokens[3].type, TokenType::KEYWORD_VALUES);
    bool has_update = false, has_set = false, has_delete = false;
    bool has_create = false, has_drop = false;
    for (const auto& tk : tokens) {
        if (tk.type == TokenType::KEYWORD_UPDATE) has_update = true;
        if (tk.type == TokenType::KEYWORD_SET) has_set = true;
        if (tk.type == TokenType::KEYWORD_DELETE) has_delete = true;
        if (tk.type == TokenType::KEYWORD_CREATE) has_create = true;
        if (tk.type == TokenType::KEYWORD_DROP) has_drop = true;
    }
    CHECK(has_update);
    CHECK(has_set);
    CHECK(has_delete);
    CHECK(has_create);
    CHECK(has_drop);
}

void TestQueryClauseKeywords() {
    auto tokens = TokenizeOrDie(
        "SELECT DISTINCT a FROM t"
        " INNER JOIN u ON t.id = u.id"
        " WHERE a IS NOT NULL"
        " GROUP BY a HAVING COUNT(*) > 1"
        " ORDER BY a ASC LIMIT 5;");
    // 逐个确认子句关键字均被识别（IS 未注册会被当作普通标识符）
    std::vector<TokenType> types;
    for (const auto& tk : tokens) types.push_back(tk.type);
    auto contains = [&](TokenType t) {
        for (TokenType x : types) if (x == t) return true;
        return false;
    };
    CHECK(contains(TokenType::KEYWORD_DISTINCT));
    CHECK(contains(TokenType::KEYWORD_INNER));
    CHECK(contains(TokenType::KEYWORD_JOIN));
    CHECK(contains(TokenType::KEYWORD_ON));
    CHECK(contains(TokenType::KEYWORD_GROUP));
    CHECK(contains(TokenType::KEYWORD_BY));
    CHECK(contains(TokenType::KEYWORD_HAVING));
    CHECK(contains(TokenType::KEYWORD_ORDER));
    CHECK(contains(TokenType::KEYWORD_LIMIT));
    CHECK(contains(TokenType::KEYWORD_NULL));
    CHECK(contains(TokenType::KEYWORD_NOT));
}

void TestTypeKeywords() {
    auto tokens = TokenizeOrDie("CREATE TABLE t (a INT, b VARCHAR, c FLOAT, PRIMARY KEY(a));");
    std::vector<TokenType> types;
    for (const auto& tk : tokens) types.push_back(tk.type);
    auto contains = [&](TokenType t) {
        for (TokenType x : types) if (x == t) return true;
        return false;
    };
    CHECK(contains(TokenType::KEYWORD_INT));
    CHECK(contains(TokenType::KEYWORD_VARCHAR));
    CHECK(contains(TokenType::KEYWORD_FLOAT));
    CHECK(contains(TokenType::KEYWORD_PRIMARY));
    CHECK(contains(TokenType::KEYWORD_KEY));
}

// ---- 字面量与运算符 ----

void TestNumericLiterals() {
    auto tokens = TokenizeOrDie("SELECT 42, 3.14, 0, 100 FROM t;");
    ExpectToken(tokens, 1, TokenType::INTEGER_LITERAL, "42");
    ExpectToken(tokens, 3, TokenType::FLOAT_LITERAL, "3.14");
    ExpectToken(tokens, 5, TokenType::INTEGER_LITERAL, "0");
    ExpectToken(tokens, 7, TokenType::INTEGER_LITERAL, "100");
}

void TestStringLiterals() {
    auto tokens = TokenizeOrDie("SELECT 'hello', 'world' FROM t;");
    ExpectToken(tokens, 1, TokenType::STRING_LITERAL, "hello");
    ExpectToken(tokens, 3, TokenType::STRING_LITERAL, "world");
}

void TestStringEscapes() {
    // SQL 输入: 'a\'b\n'  ->  词素应为 a'b(换行)
    auto tokens = TokenizeOrDie("'a\\'b\\n'");
    CHECK_EQ(tokens[0].type, TokenType::STRING_LITERAL);
    CHECK_EQ(tokens[0].lexeme, std::string("a'b\n"));
    // 反斜杠本身：'x\\y' -> x\y
    auto tokens2 = TokenizeOrDie("'x\\\\y'");
    CHECK_EQ(tokens2[0].lexeme, std::string("x\\y"));
    // 制表符：'a\tb' -> a(TAB)b
    auto tokens3 = TokenizeOrDie("'a\\tb'");
    CHECK_EQ(tokens3[0].lexeme, std::string("a\tb"));
}

void TestAllOperators() {
    auto tokens = TokenizeOrDie("a != b <> c <= d >= e < f > g = h + i - j * k / l");
    ExpectToken(tokens, 1, TokenType::OP_NOT_EQUAL, "!=");
    ExpectToken(tokens, 4, TokenType::OP_NOT_EQUAL, "<>");
    ExpectToken(tokens, 7, TokenType::OP_LESS_EQUAL, "<=");
    ExpectToken(tokens, 10, TokenType::OP_GREATER_EQUAL, ">=");
    ExpectToken(tokens, 13, TokenType::OP_LESS, "<");
    ExpectToken(tokens, 16, TokenType::OP_GREATER, ">");
    ExpectToken(tokens, 19, TokenType::OP_EQUAL, "=");
    ExpectToken(tokens, 22, TokenType::OP_PLUS, "+");
    ExpectToken(tokens, 25, TokenType::OP_MINUS, "-");
    ExpectToken(tokens, 28, TokenType::OP_STAR, "*");
    ExpectToken(tokens, 31, TokenType::OP_SLASH, "/");
}

void TestPunctuations() {
    auto tokens = TokenizeOrDie("f(a.b), (c),;");
    ExpectToken(tokens, 0, TokenType::IDENTIFIER, "f");
    ExpectToken(tokens, 1, TokenType::LEFT_PAREN, "(");
    ExpectToken(tokens, 2, TokenType::IDENTIFIER, "a");
    ExpectToken(tokens, 3, TokenType::DOT, ".");
    ExpectToken(tokens, 4, TokenType::IDENTIFIER, "b");
    ExpectToken(tokens, 5, TokenType::RIGHT_PAREN, ")");
    ExpectToken(tokens, 6, TokenType::COMMA, ",");
}

// ---- 注释与空白 ----

void TestLineComment() {
    auto tokens = TokenizeOrDie("SELECT 1 -- 这是注释，应被忽略\n, 2 FROM t;");
    ExpectToken(tokens, 0, TokenType::KEYWORD_SELECT, "SELECT");
    ExpectToken(tokens, 1, TokenType::INTEGER_LITERAL, "1");
    ExpectToken(tokens, 2, TokenType::COMMA, ",");
    ExpectToken(tokens, 3, TokenType::INTEGER_LITERAL, "2");
    ExpectToken(tokens, 4, TokenType::KEYWORD_FROM, "FROM");
}

void TestBlockComment() {
    auto tokens = TokenizeOrDie("SELECT /* 多行\n注释\n测试 */ 1 FROM t;");
    CHECK_EQ(tokens[0].type, TokenType::KEYWORD_SELECT);
    CHECK_EQ(tokens[1].type, TokenType::INTEGER_LITERAL);
    CHECK_EQ(tokens[2].type, TokenType::KEYWORD_FROM);
}

void TestWhitespaceTolerance() {
    auto tokens = TokenizeOrDie("  \t\r\n  SELECT   1  ;  \n ");
    CHECK_EQ(tokens[0].type, TokenType::KEYWORD_SELECT);
    CHECK_EQ(tokens[1].type, TokenType::INTEGER_LITERAL);
    CHECK_EQ(tokens[2].type, TokenType::SEMICOLON);
}

// ---- 行列号追踪 ----

void TestLineColumnTracking() {
    // "SELECT" 在第 1 行；"ab" 在第 2 行第 2 列（1 个空格缩进）
    auto tokens = TokenizeOrDie("SELECT\n ab");
    CHECK_EQ(tokens[0].line, 1);
    CHECK_EQ(tokens[0].column, 1);
    CHECK_EQ(tokens[1].type, TokenType::IDENTIFIER);
    CHECK_EQ(tokens[1].line, 2);
    CHECK_EQ(tokens[1].column, 2);
}

void TestColumnAdvancesWithinLine() {
    auto tokens = TokenizeOrDie("SELECT a, b FROM t");
    // "b" 位于 "SELECT a, " 之后：1+6+1+1+1+1 = 列 11
    CHECK_EQ(tokens[4].lexeme, std::string("b"));
    CHECK_EQ(tokens[4].column, 11);
}

// ---- 非法输入 ----

void TestIllegalCharacter() {
    try {
        TokenizeOrDie("SELECT # FROM t;");
        CHECK(false);  // 不应到达这里
    } catch (const sqlcompiler::CompilerException& e) {
        CHECK_EQ(e.GetStage(), sqlcompiler::ErrorStage::LEXICAL);
        std::string msg = e.what();
        CHECK(msg.find("unexpected character") != std::string::npos);
    }
}

void TestUnterminatedString() {
    try {
        TokenizeOrDie("SELECT 'abc FROM t;");
        CHECK(false);  // 不应到达这里
    } catch (const sqlcompiler::CompilerException& e) {
        CHECK_EQ(e.GetStage(), sqlcompiler::ErrorStage::LEXICAL);
        std::string msg = e.what();
        CHECK(msg.find("unterminated string literal") != std::string::npos);
    }
}

// ---- 边界情况 ----

void TestEmptyInput() {
    auto tokens = TokenizeOrDie("");
    CHECK_EQ(tokens.size(), static_cast<size_t>(1));
    CHECK_EQ(tokens[0].type, TokenType::END_OF_FILE);
}

void TestOnlyCommentInput() {
    auto tokens = TokenizeOrDie("-- 只有注释\n/* 还是注释 */");
    CHECK_EQ(tokens.size(), static_cast<size_t>(1));
    CHECK_EQ(tokens[0].type, TokenType::END_OF_FILE);
}

void TestIdentifierWithUnderscore() {
    auto tokens = TokenizeOrDie("_private_col2 FROM t");
    CHECK_EQ(tokens[0].type, TokenType::IDENTIFIER);
    CHECK_EQ(tokens[0].lexeme, std::string("_private_col2"));
}

}  // namespace

int main() {
    testfw::Run("词法: 简单SELECT语句", TestSimpleSelect);
    testfw::Run("词法: 关键字大小写不敏感", TestKeywordCaseInsensitive);
    testfw::Run("词法: 五类语句关键字", TestAllStatementKeywords);
    testfw::Run("词法: 查询子句关键字", TestQueryClauseKeywords);
    testfw::Run("词法: 数据类型关键字", TestTypeKeywords);
    testfw::Run("词法: 数值字面量", TestNumericLiterals);
    testfw::Run("词法: 字符串字面量", TestStringLiterals);
    testfw::Run("词法: 字符串转义序列", TestStringEscapes);
    testfw::Run("词法: 全部运算符", TestAllOperators);
    testfw::Run("词法: 分隔符与点号", TestPunctuations);
    testfw::Run("词法: 单行注释", TestLineComment);
    testfw::Run("词法: 块注释", TestBlockComment);
    testfw::Run("词法: 空白容忍", TestWhitespaceTolerance);
    testfw::Run("词法: 行号追踪", TestLineColumnTracking);
    testfw::Run("词法: 列号推进", TestColumnAdvancesWithinLine);
    testfw::Run("词法: 非法字符报错", TestIllegalCharacter);
    testfw::Run("词法: 未闭合字符串报错", TestUnterminatedString);
    testfw::Run("词法: 空输入", TestEmptyInput);
    testfw::Run("词法: 纯注释输入", TestOnlyCommentInput);
    testfw::Run("词法: 下划线标识符", TestIdentifierWithUnderscore);
    return testfw::Summary("lexer_test");
}
