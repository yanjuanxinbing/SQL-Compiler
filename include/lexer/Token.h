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
    KEYWORD_FULL,
    KEYWORD_OUTER,
    KEYWORD_CROSS,
    KEYWORD_NATURAL,
    KEYWORD_USING,
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
    KEYWORD_ALTER,
    KEYWORD_ADD,
    KEYWORD_RENAME,
    KEYWORD_MODIFY,
    KEYWORD_CHECK,
    KEYWORD_DEFAULT,
    KEYWORD_TO,
    KEYWORD_COLUMN,
    KEYWORD_CASE,
    KEYWORD_WHEN,
    KEYWORD_THEN,
    KEYWORD_ELSE,
    KEYWORD_END,
    KEYWORD_CAST,
    KEYWORD_COALESCE,
    KEYWORD_NULLIF,
    KEYWORD_WITH,
    KEYWORD_RECURSIVE,
    KEYWORD_OVER,
    KEYWORD_PARTITION,
    KEYWORD_ROWS,
    KEYWORD_RANGE,
    KEYWORD_BETWEEN_KW,   // already used as BETWEEN in expressions; kept as keyword for window framing
    KEYWORD_UNBOUNDED,
    KEYWORD_PRECEDING,
    KEYWORD_FOLLOWING,
    KEYWORD_CURRENT,
    KEYWORD_ROW,
    KEYWORD_WINDOW,
    KEYWORD_ROW_NUMBER,
    KEYWORD_RANK,
    KEYWORD_DENSE_RANK,
    KEYWORD_NTILE,
    KEYWORD_LAG,
    KEYWORD_LEAD,
    KEYWORD_FIRST_VALUE,
    KEYWORD_LAST_VALUE,
    KEYWORD_PERCENT_RANK,
    KEYWORD_CUME_DIST,
    KEYWORD_UNION,
    KEYWORD_INTERSECT,
    KEYWORD_EXCEPT,
    KEYWORD_ANY,
    KEYWORD_ALL,
    KEYWORD_EXISTS,
    KEYWORD_UPPER,
    KEYWORD_LOWER,
    KEYWORD_LENGTH,
    KEYWORD_SUBSTR,
    KEYWORD_TRIM,
    KEYWORD_REPLACE,
    KEYWORD_ROUND,
    KEYWORD_CEIL,
    KEYWORD_FLOOR,
    KEYWORD_ABS,
    KEYWORD_POWER,
    KEYWORD_MOD,
    KEYWORD_YEAR,
    KEYWORD_MONTH,
    KEYWORD_DAY,
    KEYWORD_NOW,
    KEYWORD_IFNULL,

    // ---- 40_txn_view_udf: 事务 / 视图 / 触发器 / 用户自定义函数 ----
    KEYWORD_BEGIN,        // BEGIN [TRANSACTION]
    KEYWORD_TRANSACTION,
    KEYWORD_COMMIT,
    KEYWORD_ROLLBACK,
    KEYWORD_SAVEPOINT,
    KEYWORD_RELEASE,
    KEYWORD_VIEW,
    KEYWORD_TRIGGER,
    KEYWORD_FUNCTION,
    KEYWORD_BEFORE,
    KEYWORD_AFTER,
    KEYWORD_FOR,
    KEYWORD_EACH,
    KEYWORD_NEW,          // 触发器 NEW.row
    KEYWORD_OLD,          // 触发器 OLD.row
    KEYWORD_RETURN,       // UDF 体
    KEYWORD_RETURNS,      // UDF 返回类型

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
