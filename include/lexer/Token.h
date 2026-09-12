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
    KEYWORD_DUPLICATE,  // 43_upsert: ON DUPLICATE KEY UPDATE
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
    KEYWORD_HOUR,
    KEYWORD_MINUTE,
    KEYWORD_SECOND,

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
    KEYWORD_DECLARE,      // 47_udf_trigger_view: UDF 体内声明局部变量
    KEYWORD_WHILE,        // 47_udf_trigger_view: UDF 体内 while 循环
    KEYWORD_DO,           // 47_udf_trigger_view: WHILE cond DO ...
    KEYWORD_ELSEIF,       // 47_udf_trigger_view: UDF 体内 ELSEIF 分支

    // ---- 44_pattern_match: 模式匹配扩展 ----
    KEYWORD_ILIKE,        // ILIKE — PostgreSQL 大小写不敏感 LIKE
    KEYWORD_REGEXP,       // REGEXP — MySQL 风格 POSIX ERE 子串匹配
    KEYWORD_RLIKE,        // RLIKE — REGEXP 的同义别名
    KEYWORD_ESCAPE,       // ESCAPE — LIKE / ILIKE / SIMILAR TO 的转义字符指定符

    // ---- 56_pattern: SIMILAR TO 扩展 ----
    KEYWORD_SIMILAR,      // SIMILAR TO — SQL:1999 风格正则匹配（SQL pattern → ERE）

    // ---- 45_datetime: DATE / TIMESTAMP / INTERVAL / EXTRACT ----
    KEYWORD_DATE,         // DATE — DATE 'YYYY-MM-DD'
    KEYWORD_TIMESTAMP,    // TIMESTAMP 'YYYY-MM-DD HH:MM:SS'
    KEYWORD_INTERVAL,     // INTERVAL n unit
    KEYWORD_EXTRACT,      // EXTRACT(field FROM source)
    // HOUR / MINUTE / SECOND 同时作为 EXTRACT 字段；
    // YEAR / MONTH / DAY 已在上方 KEYWORD_YEAR/MONTH/DAY 定义。

    // ---- 46_meta: 元命令（EXPLAIN / SHOW / DESCRIBE / DESC） ----
    KEYWORD_EXPLAIN,      // EXPLAIN <statement>
    KEYWORD_SHOW,         // SHOW TABLES / SHOW COLUMNS / SHOW INDEX / SHOW CREATE TABLE
    KEYWORD_DESCRIBE,     // DESCRIBE <table>
    // DESC / INDEX 复用上方的 KEYWORD_DESC / KEYWORD_INDEX（已存在）。
    KEYWORD_TABLES,       // SHOW TABLES
    KEYWORD_COLUMNS,      // SHOW COLUMNS FROM <table>

    // ---- 52_data_types：扩展数据类型 ----
    KEYWORD_BOOLEAN,      // BOOLEAN / BOOL
    KEYWORD_BOOL,         // BOOL 同义词
    KEYWORD_CHAR,         // CHAR(n) 定长字符串
    KEYWORD_TEXT,         // TEXT 不限长 VARCHAR
    KEYWORD_DECIMAL,      // DECIMAL(p, s) / NUMERIC 同义别名
    KEYWORD_NUMERIC,      // NUMERIC 同义词
    KEYWORD_DOUBLE,       // DOUBLE 8 字节浮点
    KEYWORD_REAL,         // REAL 4 字节浮点
    KEYWORD_SMALLINT,     // SMALLINT 16 位整型
    KEYWORD_TINYINT,      // TINYINT 8 位整型
    KEYWORD_TIME,         // TIME 'HH:MM:SS'
    KEYWORD_JSON,         // JSON 文档类型
    KEYWORD_UUID,         // UUID 16 字节二进制类型
    KEYWORD_AUTO_INCREMENT,// AUTO_INCREMENT 自增列标记
    KEYWORD_SERIAL,       // SERIAL = INT PRIMARY KEY AUTO_INCREMENT
    KEYWORD_IDENTITY,     // IDENTITY 同 AUTO_INCREMENT
    KEYWORD_TRUE,         // TRUE 字面量
    KEYWORD_FALSE,        // FALSE 字面量

    // ---- 53_ddl: DDL 扩展（FK / SCHEMA / SEQUENCE） ----
    KEYWORD_SCHEMA,       // CREATE/DROP SCHEMA name
    KEYWORD_SEQUENCE,     // CREATE/DROP SEQUENCE name
    KEYWORD_NEXTVAL,      // NEXTVAL FOR name（作为函数调用前缀）
    KEYWORD_FOREIGN,      // FOREIGN KEY (col, ...) REFERENCES other(...)
    KEYWORD_REFERENCES,   // FOREIGN KEY (...) REFERENCES other(...)
    KEYWORD_CASCADE,      // ON DELETE/UPDATE CASCADE
    KEYWORD_RESTRICT,     // ON DELETE/UPDATE RESTRICT
    KEYWORD_ACTION,       // NO ACTION（保留用于 FK ON DELETE/UPDATE 子句）

    // ---- 58_constraints: 命名 CHECK 约束 ----
    KEYWORD_CONSTRAINT,   // CONSTRAINT name CHECK (...)

    // ---- 54_dml: DML 扩展（MERGE / RETURNING / UPDATE-FROM / REPLACE）----
    KEYWORD_MERGE,        // MERGE INTO target USING source ON cond ...
    KEYWORD_MATCHED,      // WHEN MATCHED / WHEN NOT MATCHED
    KEYWORD_RETURNING,    // RETURNING expr [, ...]

    // ---- 55_query: 查询/表达式扩展（LATERAL / VALUES / FETCH FIRST / FOR UPDATE）----
    KEYWORD_LATERAL,      // LATERAL derived-table prefix
    KEYWORD_FETCH,        // FETCH FIRST n / FETCH NEXT n
    KEYWORD_OFFSET,       // OFFSET n ROWS
    KEYWORD_FIRST,        // FIRST (after FETCH)
    KEYWORD_NEXT,         // NEXT (after FETCH)
    // ROWS 已在上方窗口函数定义区声明，这里复用同一枚举值。
    // KEY 已在上方 CREATE TABLE / 触发器定义区声明，这里复用同一枚举值。
    KEYWORD_ONLY,         // ONLY (after FETCH n)
    KEYWORD_TIES,         // WITH TIES (after FETCH n)
    KEYWORD_SHARE,        // FOR SHARE / FOR KEY SHARE
    KEYWORD_NO,           // FOR NO KEY UPDATE

    // ---- 60_funcs: Category 6 函数 / 聚合扩展 ----
    KEYWORD_STDDEV,           // STDDEV / STDDEV_SAMP / STDDEV_POP（按标识符处理，解析层/执行层识别）
    KEYWORD_VARIANCE,         // VARIANCE / VAR_SAMP / VAR_POP
    KEYWORD_MEDIAN,           // MEDIAN
    KEYWORD_STRING_AGG,       // STRING_AGG(expr, delim) [ORDER BY ...]
    KEYWORD_GROUP_CONCAT,     // MySQL 风格同义别名
    KEYWORD_PERCENTILE_CONT,  // PERCENTILE_CONT(p) WITHIN GROUP (ORDER BY x)
    KEYWORD_PERCENTILE_DISC,  // PERCENTILE_DISC(p) WITHIN GROUP (ORDER BY x)
    KEYWORD_GREATEST,         // GREATEST(a, b, ...)
    KEYWORD_LEAST,            // LEAST(a, b, ...)
    KEYWORD_RAND,             // RAND() / RANDOM()
    KEYWORD_RANDOM,           // RANDOM() 同义别名
    KEYWORD_FILTER,           // aggregate FILTER (WHERE cond)
    KEYWORD_SETS,             // GROUPING SETS
    KEYWORD_ROLLUP,           // ROLLUP
    KEYWORD_CUBE,             // CUBE
    KEYWORD_GROUPING,         // GROUPING(col) 函数
    KEYWORD_IGNORE,           // IGNORE NULLS（窗口函数修饰符）
    KEYWORD_RESPECT,          // RESPECT NULLS（窗口函数修饰符，默认）
    KEYWORD_NULLS,            // NULLS（与 IGNORE/RESPECT 配合的"复数"形式）
    KEYWORD_WITHIN,           // WITHIN GROUP (ORDER BY ...) —— 有序集合聚合修饰

    // ---- 59_procs (Category 8)：过程语言扩展 ----
    KEYWORD_LOOP,             // LOOP ... END LOOP
    KEYWORD_REPEAT,           // REPEAT ... UNTIL cond END REPEAT
    KEYWORD_UNTIL,            // UNTIL cond
    KEYWORD_OPEN,             // OPEN cursor
    KEYWORD_CLOSE,            // CLOSE cursor
    KEYWORD_LEAVE,            // LEAVE label
    KEYWORD_ITERATE,          // ITERATE label
    KEYWORD_SIGNAL,           // SIGNAL SQLSTATE '...' SET MESSAGE_TEXT = '...'
    KEYWORD_SQLSTATE,         // SQLSTATE 'XXXXX'
    KEYWORD_MESSAGE_TEXT,     // MESSAGE_TEXT (after SET in SIGNAL)
    KEYWORD_HANDLER,          // DECLARE ... HANDLER FOR ...
    KEYWORD_CONTINUE,         // CONTINUE (HANDLER type)
    KEYWORD_SQLEXCEPTION,     // SQLEXCEPTION condition
    KEYWORD_SQLWARNING,       // SQLWARNING condition
    KEYWORD_FOUND,            // NOT FOUND condition
    KEYWORD_CURSOR,           // DECLARE name CURSOR FOR select
    KEYWORD_PROCEDURE,        // CREATE PROCEDURE
    KEYWORD_CALL,             // CALL name(args)
    KEYWORD_OUT,              // OUT parameter mode
    KEYWORD_INOUT,            // INOUT parameter mode
    KEYWORD_EXIT,             // EXIT (HANDLER type)
    KEYWORD_UNDO,             // UNDO (HANDLER type)
    KEYWORD_CONDITION,        // CONDITION (DECLARE ... CONDITION FOR ...)

    // ---- 60_view_trigger (Category 9)：VIEW / TRIGGER 扩展 ----
    KEYWORD_MATERIALIZED,     // CREATE MATERIALIZED VIEW name AS <select>
    KEYWORD_REFRESH,          // ALTER MATERIALIZED VIEW name REFRESH
    KEYWORD_STATEMENT,        // FOR EACH STATEMENT (trigger granularity)
    KEYWORD_OPTION,           // WITH [CASCADED|LOCAL] CHECK OPTION
    KEYWORD_CASCADED,         // CASCADED in CHECK OPTION
    KEYWORD_LOCAL,            // LOCAL in CHECK OPTION

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
