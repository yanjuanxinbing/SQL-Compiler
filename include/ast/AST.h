#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sqlcompiler {

// 前向声明
class Expr;
using ExprPtr = std::shared_ptr<Expr>;

// AST节点类型标识
enum class NodeType {
    // ---- 语句 ----
    SELECT_STMT,
    INSERT_STMT,
    UPDATE_STMT,
    DELETE_STMT,
    CREATE_TABLE_STMT,
    DROP_TABLE_STMT,
    CREATE_INDEX_STMT,
    DROP_INDEX_STMT,
    TRUNCATE_TABLE_STMT,

    // ---- 表达式 ----
    BINARY_EXPR,
    UNARY_EXPR,
    LITERAL_EXPR,
    COLUMN_REF_EXPR,
    FUNCTION_CALL_EXPR,
};

// ============ 基类 ============

// AST节点基类
class Node {
public:
    virtual ~Node() = default;
    virtual NodeType GetType() const = 0;
    virtual std::string ToString() const = 0;
};
using NodePtr = std::shared_ptr<Node>;

// 语句基类（SELECT / INSERT / UPDATE / DELETE / DDL）
class Statement : public Node {
public:
    ~Statement() override = default;
};
using StatementPtr = std::shared_ptr<Statement>;

// 表达式基类
class Expr : public Node {
public:
    ~Expr() override = default;
};

// ============ 表达式节点 ============

enum class LiteralType { INTEGER, FLOAT, STRING, NULL_VALUE, BOOLEAN };

// 字面量表达式，如 123 / 3.14 / 'abc' / NULL
class LiteralExpr : public Expr {
public:
    LiteralExpr(LiteralType literal_type, std::string value);

    NodeType GetType() const override;
    std::string ToString() const override;

    LiteralType literal_type;
    std::string value;
};

// 列引用表达式，如 t.column 或 column
class ColumnRefExpr : public Expr {
public:
    ColumnRefExpr(std::string table_name, std::string column_name);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;   // 可为空，表示未显式指定表名
    std::string column_name;
};

enum class BinaryOperator {
    ADD,
    SUB,
    MUL,
    DIV,
    CONCAT,
    EQUAL,
    NOT_EQUAL,
    LESS,
    LESS_EQUAL,
    GREATER,
    GREATER_EQUAL,
    AND,
    OR,
    LIKE,
    IN_LIST,
    BETWEEN,
    IS_NULL,
    IS_NOT_NULL
};

// 二元表达式，如 a = b / a AND b / a + b
class BinaryExpr : public Expr {
public:
    BinaryExpr(BinaryOperator op, ExprPtr left, ExprPtr right);

    NodeType GetType() const override;
    std::string ToString() const override;

    BinaryOperator op;
    ExprPtr left;
    ExprPtr right;
};

enum class UnaryOperator { NOT, NEGATE };

// 一元表达式，如 NOT a / -a
class UnaryExpr : public Expr {
public:
    UnaryExpr(UnaryOperator op, ExprPtr operand);

    NodeType GetType() const override;
    std::string ToString() const override;

    UnaryOperator op;
    ExprPtr operand;
};

// 函数调用表达式，如 COUNT(*) / SUM(a) / COUNT(DISTINCT col)
class FunctionCallExpr : public Expr {
public:
    FunctionCallExpr(std::string function_name, std::vector<ExprPtr> arguments);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string function_name;
    std::vector<ExprPtr> arguments;
    // 仅聚合函数有效：如 COUNT(DISTINCT col) / SUM(DISTINCT col)
    bool is_distinct = false;
};

// ============ 辅助结构 ============

// 列定义，用于CREATE TABLE
struct ColumnDefinition {
    std::string column_name;
    std::string data_type;   // INT / VARCHAR / FLOAT 等
    // 类型参数，如 VARCHAR(50) 中的 50；未显式声明时为 -1（不限长）
    int32_t char_length = -1;
    bool is_primary_key = false;
    bool is_not_null = false;
};

// JOIN 类型
enum class JoinType { INNER, LEFT, RIGHT };

// JOIN 子句
struct JoinClause {
    JoinType join_type;
    std::string table_name;
    std::string table_alias;
    ExprPtr on_condition;
};

// ORDER BY 单项
struct OrderByItem {
    ExprPtr expr;
    bool ascending = true;
};

// ============ 语句节点 ============

// SELECT 语句
class SelectStatement : public Statement {
public:
    SelectStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    bool is_distinct = false;
    std::vector<ExprPtr> select_list;   // 选择的列/表达式，*表示全部列
    std::vector<std::string> select_aliases;  // 与 select_list 平行的别名（可能为空）
    std::string from_table;
    std::string from_table_alias;       // 表别名（FROM t AS a）
    std::vector<JoinClause> joins;
    ExprPtr where_clause;               // 可为空
    std::vector<ExprPtr> group_by;
    ExprPtr having_clause;              // 可为空
    std::vector<OrderByItem> order_by;
    int limit = -1;                     // -1 表示不限制
    int limit_offset = 0;               // LIMIT offset, count 形式
};

// INSERT 语句
class InsertStatement : public Statement {
public:
    InsertStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    std::vector<std::string> columns;               // 可为空，表示按表定义顺序插入
    std::vector<std::vector<ExprPtr>> values_list;   // 支持多行 VALUES
};

// UPDATE 语句
class UpdateStatement : public Statement {
public:
    UpdateStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    std::vector<std::pair<std::string, ExprPtr>> assignments;  // SET col = expr
    ExprPtr where_clause;  // 可为空
};

// DELETE 语句
class DeleteStatement : public Statement {
public:
    DeleteStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    ExprPtr where_clause;  // 可为空
};

// CREATE TABLE 语句
class CreateTableStatement : public Statement {
public:
    CreateTableStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    std::vector<ColumnDefinition> columns;
    // 表级主键约束：每个内层 vector 表示一条 PRIMARY KEY(col, ...) 子句涉及的列名。
    // 列内 `is_primary_key` 同时会被置位，便于既有执行路径（InsertExecutor/Schema）
    // 直接基于单列 is_primary_key 做校验/自增。
    std::vector<std::vector<std::string>> primary_keys;
    // CREATE TABLE IF NOT EXISTS 标记：true 时若表已存在则静默成功，不报错。
    bool if_not_exists = false;
};

// DROP TABLE 语句
class DropTableStatement : public Statement {
public:
    DropTableStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    // DROP TABLE IF EXISTS：表不存在时静默成功，便于幂等脚本
    bool if_exists = false;
};

// CREATE [UNIQUE] INDEX <name> ON <table>(col, ...)
class CreateIndexStatement : public Statement {
public:
    CreateIndexStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string index_name;
    std::string table_name;
    std::vector<std::string> key_columns;
    bool is_unique = false;
};

// DROP INDEX [IF EXISTS] <name>
class DropIndexStatement : public Statement {
public:
    DropIndexStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string index_name;
    bool if_exists = false;
};

// TRUNCATE TABLE 语句：清空表中所有数据，但保留表结构
class TruncateTableStatement : public Statement {
public:
    TruncateTableStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
};

}  // namespace sqlcompiler
