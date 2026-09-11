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

class PlanNode;
using PlanNodePtr = std::shared_ptr<PlanNode>;

class SelectStatement;
using SelectStatementPtr = std::shared_ptr<SelectStatement>;

// 窗口规格结构在文件后部定义，但 SelectStatement 内部需要它，
// 故前置声明。完整定义见下方 "新增表达式节点" 区域。
struct WindowSpec;

// AST节点类型标识
enum class NodeType {
    // ---- 语句 ----
    SELECT_STMT,
    WITH_STMT,
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
    CASE_EXPR,
    CAST_EXPR,
    WINDOW_FUNC_EXPR,
    SUBQUERY_EXPR,
    SET_OP_STMT,
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
    // 命名窗口（WINDOW 子句）：name → spec
    std::vector<std::pair<std::string, WindowSpec>> named_windows;
    // 派生表 FROM (SELECT ...) AS alias：当 derived_table 非空时优先使用。
    SelectStatementPtr derived_table;
    std::string derived_alias;
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

// ============ 新增表达式节点（27–33 测试套件） ============

// CASE WHEN 表达式（同时支持简单 CASE 和搜索式 CASE）
//   - 简单 CASE：subject 不为空，每个 when_expr 是与 subject 比较的右侧
//   - 搜索式 CASE：subject 为空，每个 when_expr 是布尔谓词
struct CaseWhen {
    ExprPtr when_expr;     // 简单CASE: 被比较值；搜索式CASE: 谓词
    ExprPtr then_expr;     // THEN 后的结果表达式
};
class CaseExprNode : public Expr {
public:
    CaseExprNode();

    NodeType GetType() const override;
    std::string ToString() const override;

    ExprPtr subject;                       // 可空（搜索式 CASE）
    std::vector<CaseWhen> whens;
    ExprPtr else_expr;                     // 可空
};

// CAST(expr AS type) 表达式
class CastExprNode : public Expr {
public:
    CastExprNode(ExprPtr expr, std::string target_type);

    NodeType GetType() const override;
    std::string ToString() const override;

    ExprPtr expr;
    std::string target_type;   // "INT" / "FLOAT" / "VARCHAR"
    int32_t char_length = -1;  // VARCHAR(N) 中的 N
};

// 窗口函数 frame 描述（ROWS BETWEEN ... AND ...）
struct WindowFrame {
    enum class BoundKind {
        UNBOUNDED_PRECEDING,
        UNBOUNDED_FOLLOWING,
        EXPR_PRECEDING,
        EXPR_FOLLOWING,
        CURRENT_ROW,
    };
    BoundKind kind1;
    ExprPtr expr1;  // 可空
    BoundKind kind2;
    ExprPtr expr2;  // 可空
    // true=ROWS, false=RANGE
    bool is_rows = true;
};

// 窗口规格：PARTITION BY / ORDER BY / frame
struct WindowSpec {
    std::vector<ExprPtr> partition_by;
    std::vector<OrderByItem> order_by;
    bool has_frame = false;
    WindowFrame frame;
};

// OVER (...) 节点，承载在函数调用之上：window_func(name(args) OVER spec)
class WindowFuncNode : public Expr {
public:
    WindowFuncNode(std::string function_name,
                   std::vector<ExprPtr> arguments,
                   WindowSpec spec,
                   std::string window_name = "");

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string function_name;
    std::vector<ExprPtr> arguments;
    WindowSpec spec;
    std::string window_name;  // OVER w 引用命名窗口
};

// 子查询表达式：标量 / IN / EXISTS / ANY
enum class SubqueryType { SCALAR, EXISTS, IN, ANY };
class SubqueryExprNode : public Expr {
public:
    SubqueryExprNode(SubqueryType kind, SelectStatementPtr subquery,
                     std::string comparison_op = "",
                     ExprPtr outer_expr = nullptr);

    NodeType GetType() const override;
    std::string ToString() const override;

    SubqueryType kind;
    SelectStatementPtr subquery;
    // 仅 ANY/IN 有效：比较运算符（如 ">"、"<"），用于 expr op ANY (SELECT ...)
    std::string comparison_op;
    // 仅 ANY/IN 有效：与子查询比较的外部表达式
    ExprPtr outer_expr;
    // Planner 在递归下降阶段填充：把 subquery 转成的内部计划。
    // 求值期 ExpressionEvaluator 看到 subquery_plan 非空时用它直接驱动子查询，
    // 而无需重新 Parse/Plan 整段子查询。CTE 物化时也会被填充。
    PlanNodePtr subquery_plan;
};

// ============ WITH-CTE 与集合运算语句节点 ============

// 单条 CTE 定义
struct CteDefinition {
    std::string cte_name;
    std::vector<std::string> cte_column_aliases;  // 可空：WITH t(a,b) AS (...)
    SelectStatementPtr cte_query;
    // 递归 CTE 的"递归部分"。当 CTE 体是 `anchor UNION ALL recursive` 时，
    // cte_query 承载 anchor（左侧 SELECT），recursive_part 承载递归部分（右侧）。
    // 当前实现仅支持 UNION ALL 作为连接符；其他集合运算的递归不在范围内。
    StatementPtr recursive_part;
};

// 带 WITH 子句的 SELECT（也支持后续 SET OP 链）
class WithClauseStatement : public Statement {
public:
    WithClauseStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    bool is_recursive = false;
    std::vector<CteDefinition> ctes;
    SelectStatementPtr body;  // 主 SELECT（含可选的 UNION 链）
};

// 集合运算：UNION / UNION ALL / INTERSECT / EXCEPT
class SetOperationStatement : public Statement {
public:
    SetOperationStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    enum class Kind { UNION, UNION_ALL, INTERSECT, EXCEPT };
    Kind kind;
    StatementPtr left;   // SelectStatement 或 SetOperationStatement
    StatementPtr right;
    // 集合运算结果上的顶层 ORDER BY / LIMIT。
    // 内部 SELECT 上的 ORDER BY / LIMIT 在 SQL 标准里作用于子查询，但本实现的
    // parser 主动将 ORDER BY / LIMIT 上提到最近的集合运算节点上，因此把它们
    // 直接挂在这里、planner 再围绕 SetOpNode 加 Sort/Limit 节点即可。
    std::vector<OrderByItem> order_by;
    int limit = -1;
    int limit_offset = 0;
};

// SELECTStatement 与 INSERT/UPDATE/DELETE 等共享 Statement 基类。
// 集合运算节点与 WithClause 中需要将 SelectStatement 单独成指针，
// 故提供强类型别名 SelectStatementPtr（已在文件顶部声明）。

}  // namespace sqlcompiler
