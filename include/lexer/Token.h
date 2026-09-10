#pragma once

#include <string>

namespace sqlcompiler {

// 词法单元类型
enum class TokenType {
    // ---- 关键字 ----
    KEYWORD_SELECT,
    KEYWORD_FROM,
    KEYWORD_WHERE,
    KEYWORD_INSERT,
    KEYWORD_INTO,
    KEYWORD_VALUES,
    KEYWORD_UPDATE,
    KEYWORD_SET,
    KEYWORD_DELETE,
    KEYWORD_CREATE,
    KEYWORD_TABLE,
    KEYWORD_INDEX,
    KEYWORD_UNIQUE,
    KEYWORD_DROP,
    KEYWORD_AND,
    KEYWORD_OR,
    KEYWORD_NOT,
    KEYWORD_NULL,
    KEYWORD_ORDER,
    KEYWORD_BY,
    KEYWORD_GROUP,
    KEYWORD_HAVING,
    KEYWORD_JOIN,
    KEYWORD_INNER,
    KEYWORD_LEFT,
    KEYWORD_RIGHT,
    KEYWORD_ON,
    KEYWORD_AS,
    KEYWORD_DISTINCT,
    KEYWORD_LIMIT,
    KEYWORD_INT,
    KEYWORD_VARCHAR,
    KEYWORD_FLOAT,
    KEYWORD_PRIMARY,
    KEYWORD_KEY,
    KEYWORD_IS,
    KEYWORD_LIKE,
    KEYWORD_IN,
    KEYWORD_BETWEEN,
    KEYWORD_ASC,
    KEYWORD_DESC,
    KEYWORD_IF,
    KEYWORD_TRUNCATE,

    // ---- 标识符与字面量 ----
    IDENTIFIER,
    INTEGER_LITERAL,
    FLOAT_LITERAL,
    STRING_LITERAL,

    // ---- 运算符与符号 ----
    OP_EQUAL,          // =
    OP_NOT_EQUAL,      // != 或 <>
    OP_LESS,           // <
    OP_LESS_EQUAL,     // <=
    OP_GREATER,        // >
    OP_GREATER_EQUAL,  // >=
    OP_PLUS,           // +
    OP_MINUS,          // -
    OP_STAR,           // *
    OP_SLASH,          // /
    OP_CONCAT,         // || 字符串连接
    LEFT_PAREN,        // (
    RIGHT_PAREN,       // )
    COMMA,             // ,
    SEMICOLON,         // ;
    DOT,               // .
    BACKTICK,          // ` 用于中文等特殊标识符

    END_OF_FILE,
    UNKNOWN
};

// 将TokenType转换为可读字符串，便于调试与报错
std::string TokenTypeToString(TokenType type);

// 词法单元
struct Token {
    TokenType type;
    std::string lexeme;  // 原始文本
    int line;
    int column;

    Token();
    Token(TokenType type, const std::string& lexeme, int line, int column);

    std::string ToString() const;
};

}  // namespace sqlcompiler
