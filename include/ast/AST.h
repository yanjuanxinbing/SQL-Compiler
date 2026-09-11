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
    ROLLBACK_TO_STMT,       // 48_acid_undo: ROLLBACK TO name
    SAVEPOINT_STMT,
    RELEASE_SAVEPOINT_STMT,
    CREATE_VIEW_STMT,
    DROP_VIEW_STMT,
    CREATE_TRIGGER_STMT,
    DROP_TRIGGER_STMT,
    CREATE_FUNCTION_STMT,
    DROP_FUNCTION_STMT,

    // ---- 46_meta: 元命令（EXPLAIN / SHOW）----
    EXPLAIN_STMT,
    SHOW_STMT,

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
    UPSERT_VALUES_REF_EXPR,  // 43_upsert: ON DUPLICATE KEY UPDATE 中的 VALUES(col)
    LIKE_EXPR,               // 44_pattern_match: LIKE/ILIKE/REGEXP/RLIKE (+可选 ESCAPE)
    SET_OP_STMT,
    EXTRACT_EXPR,            // 45_datetime: EXTRACT(field FROM source)
    INTERVAL_EXPR,           // 45_datetime: INTERVAL <n> <unit>

    // ---- 47_udf_trigger_view: UDF 体内部使用的语句节点 ----
    DECLARE_VAR_STMT,        // DECLARE name TYPE
    SET_VAR_STMT,            // SET name = expr
    IF_STMT,                 // IF cond THEN ... [ELSEIF ...] [ELSE ...] END IF
    WHILE_STMT,              // WHILE cond DO ... END WHILE
    RETURN_STMT,             // RETURN [expr];
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

enum class LiteralType { INTEGER, FLOAT, STRING, NULL_VALUE, BOOLEAN,
                          DATE,       // 45_datetime: 'YYYY-MM-DD'
                          TIMESTAMP };// 45_datetime: 'YYYY-MM-DD HH:MM:SS'

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
    IS_NOT_NULL,
    // 45_datetime: <date_or_ts> ± INTERVAL <n> <unit>
    INTERVAL_ADD,
    INTERVAL_SUB
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
    // ---- 43_upsert: MySQL 风格 ON DUPLICATE KEY UPDATE ----
    // 当 has_on_duplicate 为 true 且 values_list 非空时（query 路径暂不支持
    // upsert，见 UpsertExecutor 注释），执行器对每条候选行先按 PRIMARY KEY
    // 探测：命中已有行则按 upsert_assignments 改写该行，未命中则按常规 INSERT。
    bool has_on_duplicate = false;
    // SET 子句：col = expr[, ...]。expr 中可出现 UpsertValuesRefExpr
    // （VALUES(col) 形式）引用本次候选行的列值；其他列引用视为目标表的现有行。
    std::vector<std::pair<std::string, ExprPtr>> upsert_assignments;
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

// ============ 43_upsert：ON DUPLICATE KEY UPDATE 中的 VALUES(col) ============
//
// 在 MySQL 风格 upsert 的赋值右侧，`VALUES(col)` 引用本次候选行的 col 值
// （即如果 INSERT 成功原本应该写入 col 的值），而不是表中现有行同名列。
// 这与赋值右侧出现的普通列引用（解析为表中现有行）形成对比，需要一个独立
// 的 AST 节点让 ExpressionEvaluator 在执行期能区分二者。
class UpsertValuesRefExpr : public Expr {
public:
    explicit UpsertValuesRefExpr(std::string column_name);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string column_name;
};

// ============ 44_pattern_match：LIKE / ILIKE / REGEXP / RLIKE ============
//
// 统一承载四种模式匹配运算符的可选 ESCAPE 子句参数。
//   - kind == LIKE     ：SQL 标准 LIKE，'%' '_' 通配符，has_escape 为 true 时
//                         以 escape_char（默认 '\'）取消其特殊含义。
//   - kind == ILIKE    ：PostgreSQL 大小写不敏感 LIKE（其余语义同 LIKE）。
//   - kind == REGEXP   ：POSIX ERE 风格正则子串匹配；has_escape 仅控制
//                         错误信息字符串（保留以与 LIKE 同形）。
//   - kind == RLIKE    ：REGEXP 的同义别名。
//
// 该节点独立于 BinaryOperator::LIKE，是为了在不破坏既有 LIKE 路径的前提
// 下扩展 ESCAPE 子句、ILIKE/REGEXP/RLIKE 新运算符。旧 `s LIKE 'A%'` 仍走
// BinaryExpr(LIKE) + MatchLikePattern（保留 '\' 作为默认转义字符）。
class LikeExprNode : public Expr {
public:
    enum class Kind { LIKE, ILIKE, REGEXP, RLIKE };

    LikeExprNode(Kind kind,
                 ExprPtr operand,
                 ExprPtr pattern,
                 char escape_char,
                 bool has_escape);

    NodeType GetType() const override;
    std::string ToString() const override;

    Kind kind;
    ExprPtr operand;
    ExprPtr pattern;
    // 转义字符：仅当 has_escape 为 true 时使用。LIKE/ILIKE 下由 SQL 的
    // `ESCAPE 'x'` 子句设置；REGEXP/RLIKE 下不使用（但保留字段以保持节点
    // 形状统一）。
    char escape_char = '\\';
    // 是否显式通过 `ESCAPE <char>` 指定了转义字符；false 时 LIKE/ILIKE
    // 仍默认使用 '\\' 作为转义字符。
    bool has_escape = false;
};

// ============ 45_datetime：EXTRACT / INTERVAL 表达式 ============

// EXTRACT(field FROM source)
//   - field 是 IntervalUnit 之一（YEAR/MONTH/DAY/HOUR/MINUTE/SECOND）
//   - source 是任意可解析为日期/时间字符串的表达式（DATE/TIMESTAMP 列、
//     VARCHAR 字面量、列引用均可）
// 返回 INT（SECOND 字段下保留小数秒，本实现简化为 INT）。
class ExtractExprNode : public Expr {
public:
    ExtractExprNode(int field, ExprPtr source);

    NodeType GetType() const override;
    std::string ToString() const override;

    // 使用 IntervalUnit 枚举的整型值，避免引入对 DateTime.h 的头依赖。
    int field;       // IntervalUnit: YEAR/MONTH/DAY/HOUR/MINUTE/SECOND
    ExprPtr source;
};

// INTERVAL <n> <unit>
//   - n: int64 偏移量（可为正负；语义层要求 INTERVAL 出现在 +/- 之后，
//        因此符号由相邻运算符决定，本节点本身存绝对值）。
//   - unit: IntervalUnit 之一。
// 该节点仅作为 BinaryExpr(INTERVAL_ADD/SUB) 的右操作数出现。
class IntervalExprNode : public Expr {
public:
    IntervalExprNode(int64_t count, int unit);

    NodeType GetType() const override;
    std::string ToString() const override;

    int64_t count;
    int     unit;   // IntervalUnit
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

// ROLLBACK TO name —— Phase A：回滚到指定保存点。
class RollbackToStatement : public Statement {
public:
    RollbackToStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string savepoint_name;
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
//     <stmt-list>
// END;
// 函数体是一个语句序列（DECLARE / SET / IF / WHILE / RETURN 等）。
// 调用时由 UdfExecutor 按顺序解释执行；遇到 RETURN 终止并返回值。
class CreateFunctionStatement : public Statement {
public:
    CreateFunctionStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string function_name;
    std::vector<FunctionParameter> parameters;
    std::string return_type;
    int32_t return_char_length = -1;
    // 函数体：有序语句集合。最简形式可以是单个 RETURN expr；
    // 复合形式含 DECLARE/SET/IF/WHILE/RETURN 等。空 body 视为无 RETURN。
    std::vector<StatementPtr> body_statements;
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

// ============ 47_udf_trigger_view：UDF 函数体内的语句节点 ============
//
// 这些节点仅在 CreateFunctionStatement::body_statements 中出现，
// 由 UdfExecutor 在调用期间解释执行。它们不参与 Planner / Catalog 的注册路径。

// DECLARE name TYPE[(N)] —— 在当前函数帧中声明一个局部变量。变量类型
// 只支持 INT / FLOAT / VARCHAR 三种，其它类型在 parser 阶段就报错。
class DeclareVarStatement : public Statement {
public:
    DeclareVarStatement(std::string var_name, std::string data_type,
                        int32_t char_length = -1);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string var_name;
    std::string data_type;
    int32_t char_length = -1;
};

// SET name = expr —— 把 expr 的结果赋给当前函数帧中的 name。
// 当 lhs 是 NEW.col / OLD.col 时（即出现在触发器体中），expr 右侧
// 的 OLD/NEW 引用保持原有的限定语义。
class SetVarStatement : public Statement {
public:
    SetVarStatement(std::string target, ExprPtr expr);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string target;     // "name" 或 "NEW.col" 或 "OLD.col"
    ExprPtr expr;
};

// IF cond THEN stmts [ELSEIF cond THEN stmts] ... [ELSE stmts] END IF
// elseif_clauses 按出现顺序展开为一系列 (condition, body) 对；
// else_body 是最终的兜底分支（可空）。
class IfStatement : public Statement {
public:
    IfStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    ExprPtr condition;
    std::vector<StatementPtr> then_body;
    struct ElseIfClause {
        ExprPtr condition;
        std::vector<StatementPtr> body;
    };
    std::vector<ElseIfClause> elseif_clauses;
    std::vector<StatementPtr> else_body;  // 可为空
};

// WHILE cond DO stmts END WHILE —— 条件为 TRUE 时循环执行 body。
class WhileStatement : public Statement {
public:
    WhileStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    ExprPtr condition;
    std::vector<StatementPtr> body;
};

// RETURN [expr]; —— expr 可空（NULL 返回）。UdfExecutor 看到 RETURN 时
// 终止执行并把 expr 的值（或 NULL）作为函数返回值上抛给调用者。
class ReturnStatement : public Statement {
public:
    explicit ReturnStatement(ExprPtr expr = nullptr);

    NodeType GetType() const override;
    std::string ToString() const override;

    ExprPtr expr;  // 可空
};

// ============ 46_meta：元命令（EXPLAIN / SHOW）============
//
// EXPLAIN 是对任意 DDL/DML 语句的"包装"：它并不自己实现执行，而是在执行阶段
// 把 inner 语句跑一次 Planner 生成 PlanNodePtr，再用 PrettyPrint 把计划树写
// 成文本。SHOW 则是内省命令，作用于 catalog，结果以单列结果集形式呈现。
//
// EXPLAIN ANALYZE 暂不支持：当 analyze==true 时执行器输出 "EXPLAIN ANALYZE not
// supported" 而不打印计划树；这是任务文档允许的 scope-cut。

// EXPLAIN [ANALYZE] <statement>
//
// 解析阶段：识别 EXPLAIN 关键字后消耗可选 ANALYZE，再调用 ParseStatement() 取得
// inner（SELECT/INSERT/UPDATE/DELETE/DDL 等均可）。analyze 当前仅记录，
// 执行器若见 analyze==true 直接打印"EXPLAIN ANALYZE not supported"提示。
class ExplainStatement : public Statement {
public:
    ExplainStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    bool analyze = false;
    StatementPtr inner;
};

// SHOW 语句：当前支持 4 种子命令。
//   - TABLES       —— 列出 catalog 中的所有基本表（不含视图/索引）。
//   - COLUMNS      —— 列出指定表的列定义；FROM tbl 是必需语法（SHOW COLUMNS FROM t）。
//   - INDEX        —— 列出指定表的所有索引（含主键索引）。
//   - CREATE_TABLE —— 复现该表的 CREATE TABLE SQL。
//
// target_table 仅在 COLUMNS/INDEX/CREATE_TABLE 时使用；TABLES 时为空。
class ShowStatement : public Statement {
public:
    enum class Kind {
        TABLES,
        COLUMNS,
        INDEX,
        CREATE_TABLE,
    };

    ShowStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    Kind kind = Kind::TABLES;
    std::string target_table;
};

}  // namespace sqlcompiler
