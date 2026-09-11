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
    ALTER_TABLE_STMT,

    // ---- 40_txn_view_udf: 事务 / 视图 / 触发器 / UDF ----
    BEGIN_STMT,
    COMMIT_STMT,
    ROLLBACK_STMT,
    SAVEPOINT_STMT,
    RELEASE_SAVEPOINT_STMT,
    CREATE_VIEW_STMT,
    DROP_VIEW_STMT,
    CREATE_TRIGGER_STMT,
    DROP_TRIGGER_STMT,
    CREATE_FUNCTION_STMT,
    DROP_FUNCTION_STMT,

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
    // 列级 CHECK (expr) 约束：当前仅做语法接受度，语义层暂不强制校验。
    ExprPtr check_expr;
    // 列级 DEFAULT expr：当前仅做语法接受度，INSERT 时若显式未提供该列值
    // 由执行层决定是否替换为默认值；本任务下保留为占位即可。
    ExprPtr default_expr;
};

// JOIN 类型
enum class JoinType { INNER, LEFT, RIGHT, FULL_OUTER, CROSS };

// JOIN 子句
struct JoinClause {
    JoinType join_type;
    std::string table_name;
    std::string table_alias;
    ExprPtr on_condition;
    // USING (col1, col2, ...) —— 等价于 ON a.col = b.col AND ...，但输出
    // 阶段会把右表 USING 列去重。为简化起见，这里把 USING 翻译成 ON 条件；
    // 输出阶段的"右表 USING 列去重"在 Planner 生成的 ProjectNode 上体现。
    std::vector<std::string> using_columns;
    // NATURAL JOIN —— 等价于 USING(所有左右同名列)；标记后由 Planner 按表
    // 元数据自动推导 USING 列。
    bool is_natural = false;
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
    // INSERT INTO tbl SELECT ... —— query 非空时表示数据来源是 SELECT 的结果集，
    // 此时 values_list 必须为空。query 顶层可以是 SelectStatement、WithClauseStatement
    // 或 SetOperationStatement 之一。
    StatementPtr query;
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

// ALTER TABLE 语句：DDL 扩展语法。
//
// 支持四种动作（与测试用例 39_ddl_extensions 对齐）：
//   - ADD COLUMN col TYPE[(N)]         （新增列定义复用 ColumnDefinition）
//   - DROP COLUMN col                  （删除列）
//   - RENAME TO new_table_name         （表重命名）
//   - MODIFY COLUMN col TYPE[(N)]      （修改列类型）
//
// 当前实现只要求语法可解析并通过执行（语义可走 no-op 路径，测试期望
// ALTER 后原数据仍可被 SELECT）。
enum class AlterAction {
    ADD_COLUMN,
    DROP_COLUMN,
    RENAME_TO,
    MODIFY_COLUMN,
};
class AlterStatement : public Statement {
public:
    AlterStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    AlterAction action;
    // ADD COLUMN / MODIFY COLUMN 时使用的列定义；其他动作置空。
    std::shared_ptr<ColumnDefinition> column_def;
    // DROP COLUMN 时填写被删列名。
    std::string drop_column_name;
    // RENAME TO 时填写新表名。
    std::string new_table_name;
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

// ============ 40_txn_view_udf：事务 / 视图 / 触发器 / UDF 语句节点 ============

// BEGIN [TRANSACTION] —— 最小可用：no-op，开启事务计数。
class BeginStatement : public Statement {
public:
    BeginStatement();

    NodeType GetType() const override;
    std::string ToString() const override;
};

// COMMIT —— 最小可用：no-op，提交事务。
class CommitStatement : public Statement {
public:
    CommitStatement();

    NodeType GetType() const override;
    std::string ToString() const override;
};

// ROLLBACK —— 最小可用：no-op，回滚事务（当前 DML 仍立即生效，no-op 即可）。
class RollbackStatement : public Statement {
public:
    RollbackStatement();

    NodeType GetType() const override;
    std::string ToString() const override;
};

// SAVEPOINT name —— 最小可用：no-op。
class SavepointStatement : public Statement {
public:
    SavepointStatement(std::string name);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string savepoint_name;
};

// RELEASE SAVEPOINT name —— 最小可用：no-op。
class ReleaseSavepointStatement : public Statement {
public:
    ReleaseSavepointStatement(std::string name);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string savepoint_name;
};

// CREATE VIEW name AS <select> —— 视图定义保存在 catalog 中。
class CreateViewStatement : public Statement {
public:
    CreateViewStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string view_name;
    // 视图定义的 SELECT 语句；解析后转交 catalog 保存为文本或 AST 副本。
    SelectStatementPtr query;
};

// DROP VIEW [IF EXISTS] name —— 从 catalog 移除视图。
class DropViewStatement : public Statement {
public:
    DropViewStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string view_name;
    bool if_exists = false;
};

// 触发器时机：BEFORE / AFTER
enum class TriggerTiming { BEFORE, AFTER };

// 触发器事件：INSERT / UPDATE / DELETE
enum class TriggerEvent { INSERT, UPDATE, DELETE };

// CREATE TRIGGER name BEFORE|AFTER INSERT|UPDATE|DELETE ON table
// FOR EACH ROW SET NEW.col = expr [, ...]
// 最小可用：解析整段语法并把触发器记入 catalog（no-op）。
class CreateTriggerStatement : public Statement {
public:
    CreateTriggerStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string trigger_name;
    TriggerTiming timing = TriggerTiming::BEFORE;
    TriggerEvent event = TriggerEvent::INSERT;
    std::string table_name;
    // 形如 "SET NEW.col = expr [, OLD.col = expr ...]" 的赋值列表
    std::vector<std::pair<std::string, ExprPtr>> assignments;
};

// DROP TRIGGER [IF EXISTS] name —— 从 catalog 移除触发器。
class DropTriggerStatement : public Statement {
public:
    DropTriggerStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string trigger_name;
    bool if_exists = false;
};

// UDF 参数：name + type
struct FunctionParameter {
    std::string name;
    std::string data_type;
    int32_t char_length = -1;
};

// CREATE FUNCTION name(args) RETURNS type
// BEGIN
//     RETURN expr;
// END;
// 最小可用：体只接受单个 RETURN expr 语句；其余复合结构按占位处理。
class CreateFunctionStatement : public Statement {
public:
    CreateFunctionStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string function_name;
    std::vector<FunctionParameter> parameters;
    std::string return_type;
    int32_t return_char_length = -1;
    // 函数体的 RETURN 表达式；只支持单个表达式（与测试 40_txn_view_udf 对齐）。
    ExprPtr body_expr;
};

// DROP FUNCTION [IF EXISTS] name
class DropFunctionStatement : public Statement {
public:
    DropFunctionStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string function_name;
    bool if_exists = false;
};

}  // namespace sqlcompiler
