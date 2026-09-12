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
// 60_funcs: OrderByItem 用于 FunctionCallExpr::within_group_order_by 字段，
// 也在 SelectStatement / WindowSpec 等处使用。前置声明以便在 FunctionCallExpr
// 引用该类型。
struct OrderByItem;

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
    CREATE_MATERIALIZED_VIEW_STMT,  // 60_view_trigger (Category 9)
    ALTER_MATERIALIZED_VIEW_STMT,  // 60_view_trigger (Category 9)
    CREATE_FUNCTION_STMT,
    DROP_FUNCTION_STMT,
    // ---- 59_procs (Category 8)：PROCEDURE / CALL ----
    CREATE_PROCEDURE_STMT,
    DROP_PROCEDURE_STMT,
    CALL_STMT,

    // ---- 53_ddl: DDL 扩展（FK / SCHEMA / SEQUENCE） ----
    CREATE_SCHEMA_STMT,      // CREATE SCHEMA name
    DROP_SCHEMA_STMT,        // DROP SCHEMA [IF EXISTS] name
    CREATE_SEQUENCE_STMT,    // CREATE SEQUENCE name [START n] [INCREMENT n]
    DROP_SEQUENCE_STMT,      // DROP SEQUENCE [IF EXISTS] name

    // ---- 54_dml: DML 扩展 ----
    MERGE_STMT,              // MERGE INTO target USING source ON cond ...

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
    NEXTVAL_EXPR,            // 53_ddl: NEXTVAL FOR sequence_name

    // ---- 47_udf_trigger_view: UDF 体内部使用的语句节点 ----
    DECLARE_VAR_STMT,        // DECLARE name TYPE
    SET_VAR_STMT,            // SET name = expr
    IF_STMT,                 // IF cond THEN ... [ELSEIF ...] [ELSE ...] END IF
    WHILE_STMT,              // WHILE cond DO ... END WHILE
    RETURN_STMT,             // RETURN [expr];
    // ---- 59_procs (Category 8)：过程语言扩展语句 ----
    LOOP_STMT,               // LOOP body END LOOP [label];
    REPEAT_STMT,             // REPEAT body UNTIL cond END REPEAT [label];
    CASE_STMT,               // 体内 CASE WHEN ... THEN ... ELSE ... END CASE
    LEAVE_STMT,              // LEAVE label;
    ITERATE_STMT,            // ITERATE label;
    SIGNAL_STMT,             // SIGNAL SQLSTATE '...' SET MESSAGE_TEXT = '...';
    DECLARE_HANDLER_STMT,    // DECLARE [type] HANDLER FOR cond stmt;
    DECLARE_CURSOR_STMT,     // DECLARE name CURSOR FOR select;
    CURSOR_OPEN_STMT,        // OPEN name;
    CURSOR_FETCH_STMT,       // FETCH name INTO var [, var ...];
    CURSOR_CLOSE_STMT,       // CLOSE name;
};

// ============ 基类 ============

// AST节点基类
class Node {
public:
    virtual ~Node() = default;
    virtual NodeType GetType() const = 0;
    virtual std::string ToString() const = 0;

    // 源码位置（Spec 1.3 要求语义错误携带 stage + line + column）。
    // 由 Parser 在节点构造时通过 Token::line / Token::column 填入；
    // -1 表示「来源未携带位置信息」（例如由执行期内部构造的临时节点）。
    // 所有继承自 Node / Statement / Expr 的具体节点都通过基类共享这两个字段，
    // 避免在数十种节点类上重复定义。
    int line = -1;
    int column = -1;
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
                          TIMESTAMP,  // 45_datetime: 'YYYY-MM-DD HH:MM:SS'
                          TIME,       // 52_data_types: 'HH:MM:SS'
                          JSON };     // 52_data_types: '{"k":1}'

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
    // ---- 60_funcs: 聚合修饰子句 ----
    // FILTER (WHERE cond)：仅当 cond 评估为 TRUE 时该聚合才把此行纳入计算。
    // 仅聚合函数（COUNT/SUM/AVG/MIN/MAX/STDDEV/...）有效；filter_expr 为 nullptr
    // 表示没有 FILTER 子句。FILTER 与 WITHIN GROUP 可同时出现。
    ExprPtr filter_expr;
    // WITHIN GROUP (ORDER BY expr)：有序集合（ordered-set）聚合使用的排序键。
    // 仅 PERCENTILE_CONT / PERCENTILE_DISC 实际使用；
    // STRING_AGG 也允许使用其作为拼接顺序键。
    // 为空时表示没有 WITHIN GROUP 子句。
    std::vector<OrderByItem> within_group_order_by;
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
    // 52_data_types: 列级 UNIQUE 约束。区别于 CREATE UNIQUE INDEX 的索引形式：
    // UNIQUE 约束在写入路径上做唯一性校验，索引仅加速查询。本实现把列级 /
    // 表级 UNIQUE 都转译为隐式唯一索引（与 CREATE UNIQUE INDEX 等价）。
    bool is_unique = false;
    // 52_data_types: AUTO_INCREMENT / SERIAL / IDENTITY 列。
    // 行为：当用户 INSERT 时显式 NULL（或 0）时，执行层自动填入下一个递增
    // 值；非 NULL 时按用户值落库。非首列位置亦可作为 AUTO_INCREMENT 列。
    bool is_auto_increment = false;
    // 列级 CHECK (expr) 约束：当前仅做语法接受度，语义层暂不强制校验。
    ExprPtr check_expr;
    // 列级 DEFAULT expr：当前仅做语法接受度，INSERT 时若显式未提供该列值
    // 由执行层决定是否替换为默认值；本任务下保留为占位即可。
    ExprPtr default_expr;
    // 53_ddl：列级 FOREIGN KEY REFERENCES parent(col) 引用。
    // 解析阶段记录；执行期把列级 / 表级 FK 合并，统一注册到 catalog 中。
    struct InlineForeignKey {
        std::string parent_table;
        std::string parent_col;
    };
    std::vector<InlineForeignKey> inline_foreign_keys;
    // 58_constraints: 列级 / 表级 CHECK 子句的可选命名。
    // 例如 `CONSTRAINT my_check CHECK (col > 0)`。
    // 仅在用户显式声明 CONSTRAINT name 时填写；空字符串表示匿名约束，
    // 错误消息回退到 "<table>.<column_or_index>" 形式。
    std::string constraint_name;
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
    // 55_query: LATERAL 标记。当 is_lateral 为 true 时，表示这是一个 LATERAL
    // 派生表 / 子查询形式的 join 节点，其 table_name 应理解为一个「派生表别名」，
    // 且内层 SELECT 可以引用外层 FROM 项的列。Planner 走 ApplyNode 路径
    // （对每条外层行重跑内层计划）。JoinClause 的 table_name 字段对 LATERAL
    // 路径复用：table_name = derived_alias。
    bool is_lateral = false;
    // LATERAL 子查询：当 is_lateral 为 true 时该 SelectStatementPtr 持有
    // 内层 SELECT（或 VALUES）。Planner 在该字段非空时把它编译为子计划挂到
    // ApplyNode.children[1] 上。
    SelectStatementPtr lateral_subquery;
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
    // ---- 60_funcs: GROUPING SETS / ROLLUP / CUBE ----
    // 当 grouping_sets 非空时使用它替代 group_by 进行分组。
    // 外层 vector 的每个元素是一条 grouping set（即一个分组键列表）；
    // ROLLUP(a, b) 展开为 [(a, b), (a), ()]；
    // CUBE(a, b)   展开为 [(a, b), (a), (b), ()]。
    // 运行时 Planner 会按 grouping_sets 把单条 AggregateNode 拆成
    // 多条 AggregateNode UNION ALL，并对未参与分组的维度补 NULL。
    std::vector<std::vector<ExprPtr>> grouping_sets;
    std::vector<OrderByItem> order_by;
    int limit = -1;                     // -1 表示不限制
    int limit_offset = 0;               // LIMIT offset, count 形式
    // 命名窗口（WINDOW 子句）：name → spec
    std::vector<std::pair<std::string, WindowSpec>> named_windows;
    // 派生表 FROM (SELECT ...) AS alias：当 derived_table 非空时优先使用。
    SelectStatementPtr derived_table;
    std::string derived_alias;
    // 55_query: (VALUES (a,b), (c,d)) AS t(id, name) —— VALUES 行构造器作为
    // FROM 派生表。当 values_rows 非空时，Planner 走 ValuesNode 路径，
    // 与 derived_table 互斥（同一 FROM 项只能有一种来源）。values_column_aliases
    // 提供列名（可选）；空时由 Planner 自动命名 col0/col1/...。
    std::vector<std::vector<ExprPtr>> values_rows;
    std::vector<std::string> values_column_aliases;
    // 55_query: SELECT FOR UPDATE / FOR SHARE / FOR NO KEY UPDATE / FOR KEY SHARE
    // 锁提示（parse-only hint）。当前实现为单写引擎，实际不做任何加锁；语义层
    // 不强制执行。0 = NONE, 1 = UPDATE, 2 = SHARE, 3 = NO_KEY_UPDATE, 4 = KEY_SHARE。
    int for_update_kind = 0;
    // 55_query: OFFSET n [ROW|ROWS] 标准形式：parser 在 OFFSET 关键字位置
    // 同步设置 limit_offset；保留字段以便未来区分"末尾 OFFSET"与"LIMIT 逗号形式"。
    int standard_offset = 0;            // OFFSET n ROWS 形式（与 limit_offset 同义）
    // 55_query: 标记当前 SELECT 自身是否作为 LATERAL 子查询使用。true 时
    // ExpressionEvaluator::CollectInnerTableNames 跳过 from_table，避免
    // 内层 FROM 与外层表名同名时把 outer 引用错解析为 inner。
    bool is_lateral = false;
};

// FOR UPDATE 锁提示枚举值：
constexpr int kForUpdateNone        = 0;
constexpr int kForUpdateUpdate      = 1;
constexpr int kForUpdateShare       = 2;
constexpr int kForUpdateNoKeyUpdate = 3;
constexpr int kForUpdateKeyShare    = 4;

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
    // ---- 54_dml: RETURNING 子句（PG 风格）----
    // INSERT 成功后，对新写入的行计算 returning_exprs 并作为结果集返回。
    // returning_aliases 与 returning_exprs 平行（可能为空串）。语义：
    // INSERT RETURNING 发出"新行"——即落盘后的列值。
    std::vector<ExprPtr> returning_exprs;
    std::vector<std::string> returning_aliases;
    // ---- 54_dml: REPLACE INTO 标记 ----
    // true 时按 MySQL 语义处理：候选行若与已有行在 PRIMARY KEY / UNIQUE 上冲突，
    // 先删除已有行，再插入新行（等价于 INSERT … ON DUPLICATE KEY UPDATE 删旧 + 插新）。
    // 解析阶段：用户写 REPLACE INTO t VALUES (...) 时置位；REPLACE 共享 INSERT
    // 的列名列表与 VALUES 数据。当前 REPLACE 不支持 INSERT ... SELECT 数据源。
    bool is_replace = false;
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
    // ---- 54_dml: UPDATE ... FROM source [, source2 ...] ----
    // 当 from_sources 非空时，按 PG/Oracle 语义把 target 与 from_sources
    // 做连接（连接条件由 where_clause 描述，from_sources 可以是基表或派生表）。
    // 简化实现：对每个 (target, source) 组合评估 SET 赋值并写回 target 行；
    // LHS 必须是 target 表的列；RHS 可同时引用 target 与 source。
    std::vector<JoinClause> from_sources;
    // target 的别名（UPDATE t AS t SET ... FROM s AS ss WHERE t.id = ss.tid）。
    std::string table_alias;
    // ---- 54_dml: RETURNING 子句 ----
    // UPDATE 写回后，按 returning_exprs 对"更新后的行"求值并以结果集形式返回。
    std::vector<ExprPtr> returning_exprs;
    std::vector<std::string> returning_aliases;
};

// DELETE 语句
class DeleteStatement : public Statement {
public:
    DeleteStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    ExprPtr where_clause;  // 可为空
    // ---- 54_dml: RETURNING 子句 ----
    // DELETE 删除前，先对"即将删除的行"按 returning_exprs 求值并以结果集
    // 形式返回；DELETE RETURNING 发出的是"旧行"（删除前的内容）。
    std::vector<ExprPtr> returning_exprs;
    std::vector<std::string> returning_aliases;
};

// 53_ddl：FOREIGN KEY 约束定义。
//
// 支持的来源形式：
//   - 列级：pid INT REFERENCES parent(id)
//   - 表级：FOREIGN KEY (pid) REFERENCES parent(id) [ON DELETE CASCADE/SET NULL/RESTRICT]
//           FOREIGN KEY (a, b) REFERENCES parent(p1, p2)
//
// 仅本子句的目标 side 需要显式记录子列名；父列在 child_cols 与 parent_cols 长度相等时
// 一一对应。on_delete / on_update 默认 RESTRICT（与 SQL 标准一致）。
struct ForeignKeyDef {
    std::vector<std::string> child_cols;
    std::string parent_table;
    std::vector<std::string> parent_cols;
    // 0=RESTRICT, 1=CASCADE, 2=SET NULL, 3=NO ACTION, 4=SET DEFAULT
    // （为简化运行时分支，把 SET DEFAULT 视为 RESTRICT。）
    int on_delete_action = 0;
    int on_update_action = 0;
};

// 58_constraints: 表级 CHECK 约束定义。
//
// 来源形式：
//   - 表级：CHECK (price > cost)
//   - 表级命名：CONSTRAINT dates_ok CHECK (start_d < end_d)
//
// 仅出现在表约束段（PRIMARY KEY(...) / UNIQUE(...) / FOREIGN KEY ... 之外），
// 由 CreateTableStatement::table_checks 收集。
// constraint_name 为空表示匿名 CHECK；错误消息回退到 "<table>.check_<index>"
// 形式以便定位。
struct TableCheckDef {
    std::string constraint_name;
    ExprPtr expr;
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
    // 52_data_types: 表级 UNIQUE 约束。每个内层 vector 表示一条
    // UNIQUE(col1, col2, ...) 子句涉及的列名集合。执行层 (CreateTableExecutor)
    // 把它转译为隐式 UNIQUE INDEX（与显式 CREATE UNIQUE INDEX 等价）。
    std::vector<std::vector<std::string>> unique_constraints;
    // CREATE TABLE IF NOT EXISTS 标记：true 时若表已存在则静默成功，不报错。
    bool if_not_exists = false;
    // 53_ddl：表级 FOREIGN KEY 约束列表。
    std::vector<ForeignKeyDef> foreign_keys;
    // 58_constraints: 表级 CHECK 约束列表（每条带可选 constraint_name）。
    // 表级 CHECK 可以引用多个列（例如 CHECK (price > cost)），
    // 写入路径在每行落盘前对每条表级 CHECK 求值；NULL 结果按 SQL 标准
    // 三值逻辑视为通过，仅当明确为 FALSE 时拒绝。
    std::vector<TableCheckDef> table_checks;
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
// 支持五种动作（与测试用例 39_ddl_extensions / 53_ddl 对齐）：
//   - ADD COLUMN col TYPE[(N)]         （新增列定义复用 ColumnDefinition）
//   - DROP COLUMN col                  （删除列）
//   - RENAME TO new_table_name         （表重命名）
//   - MODIFY COLUMN col TYPE[(N)]      （修改列类型）
//   - RENAME COLUMN old TO new         （53_ddl：列重命名，仅修改目录 schema）
//
// 当前实现只要求语法可解析并通过执行（语义可走 no-op 路径，测试期望
// ALTER 后原数据仍可被 SELECT）。
enum class AlterAction {
    ADD_COLUMN,
    DROP_COLUMN,
    RENAME_TO,
    MODIFY_COLUMN,
    RENAME_COLUMN,  // 53_ddl
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
    // 53_ddl：RENAME COLUMN 时填写被改名列名与新列名。
    std::string rename_column_old_name;
    std::string rename_column_new_name;
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
    // ---- 60_funcs: RESPECT / IGNORE NULLS 修饰 ----
    // true = IGNORE NULLS（FIRST_VALUE/LAST_VALUE/NTH_VALUE/LAG/LEAD 在
    //                    帧内 / 偏移方向上跳过 NULL 返回值）；
    // false = RESPECT NULLS（默认；与"未指定"等价）。
    bool ignore_nulls = false;
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
    // ---- 60_funcs: IGNORE NULLS / RESPECT NULLS 修饰 ----
    // 仅 FIRST_VALUE / LAST_VALUE / NTH_VALUE / LAG / LEAD 等位置敏感窗口函数有效。
    // 默认 false（RESPECT NULLS）：按 SQL 标准，RESPECT NULLS 与"未指定"等价。
    // WindowSpec.ignore_nulls 在 OVER (...) 内部携带同一修饰，让聚合 / 排名窗口
    // 函数也能表达统一的 NULL 处理意图（实际计算路径忽略该标记）。
    bool ignore_nulls = false;
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

// ============ 44_pattern_match / 56_pattern：模式匹配 ============
//
// 统一承载五种模式匹配运算符的可选 ESCAPE 子句参数。
//   - kind == LIKE        ：SQL 标准 LIKE，'%' '_' 通配符，has_escape 为 true 时
//                            以 escape_char（默认 '\'）取消其特殊含义。
//   - kind == ILIKE       ：PostgreSQL 大小写不敏感 LIKE（其余语义同 LIKE）。
//   - kind == REGEXP      ：POSIX ERE 风格正则子串匹配；has_escape 仅控制
//                            错误信息字符串（保留以与 LIKE 同形）。
//   - kind == RLIKE       ：REGEXP 的同义别名。
//   - kind == SIMILAR_TO  ：SQL:1999 SIMILAR TO，pattern 同时支持 LIKE 风格
//                            通配符（%/_）与 ERE 元字符（. * + ? | () [] ^ $ {}）。
//                            执行器把 pattern 翻译成 POSIX ERE 后再调用 std::regex。
//                            ESCAPE 子句仅用于取消 SQL 通配符（%/ _）的特殊含义；
//                            ERE 元字符照常作为元字符解释（与 SQL 标准一致）。
//
// 该节点独立于 BinaryOperator::LIKE，是为了在不破坏既有 LIKE 路径的前提
// 下扩展 ESCAPE 子句、ILIKE/REGEXP/RLIKE/SIMILAR TO 新运算符。旧
// `s LIKE 'A%'` 仍走 BinaryExpr(LIKE) + MatchLikePattern（保留 '\' 作为
// 默认转义字符）。
class LikeExprNode : public Expr {
public:
    enum class Kind { LIKE, ILIKE, REGEXP, RLIKE, SIMILAR_TO };

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
    // 转义字符：仅当 has_escape 为 true 时使用。LIKE/ILIKE/SIMILAR TO 下
    // 由 SQL 的 `ESCAPE 'x'` 子句设置；REGEXP/RLIKE 下不使用（但保留字段
    // 以保持节点形状统一）。
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
    // 60_view_trigger: CREATE OR REPLACE VIEW —— true 时若视图已存在则覆盖。
    bool is_or_replace = false;
    // 60_view_trigger: WITH [CASCADED|LOCAL] CHECK OPTION
    // true 表示 INSERT/UPDATE 通过该视图写入时，必须满足视图的 WHERE 条件。
    bool with_check_option = false;
    // true=CASCADED, false=LOCAL。PG 默认 LOCAL；本实现与 PG 对齐。
    bool check_option_cascaded = false;
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

// 60_view_trigger (Category 9):
// CREATE MATERIALIZED VIEW name AS <select>
//
// 物化视图：把 SELECT 的结果一次性物化到一张真实存在的表（"backing table"，
// 默认名 "__mv_<view_name>"）；后续 SELECT FROM <name> 直接扫描该表，无需
// 重新执行原 SELECT。ALTER MATERIALIZED VIEW ... REFRESH 重新执行 SELECT
// 并覆盖 backing table 的内容。
class MaterializedViewStatement : public Statement {
public:
    MaterializedViewStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string view_name;
    SelectStatementPtr query;
    // CREATE MATERIALIZED VIEW IF NOT EXISTS
    bool if_not_exists = false;
};

// ALTER MATERIALIZED VIEW name REFRESH
// 60_view_trigger (Category 9)：触发对 backing table 的 truncate + 重新物化。
class AlterMaterializedViewStatement : public Statement {
public:
    AlterMaterializedViewStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string view_name;
};

// 触发器时机：BEFORE / AFTER
enum class TriggerTiming { BEFORE, AFTER };

// 触发器事件：INSERT / UPDATE / DELETE
enum class TriggerEvent { INSERT, UPDATE, DELETE };

// 触发器粒度：ROW（每行触发）/ STATEMENT（每条语句触发一次）。
// 60_view_trigger (Category 9)：新增 STATEMENT 粒度。
enum class TriggerGranularity { ROW, STATEMENT };

// CREATE TRIGGER name BEFORE|AFTER INSERT|UPDATE|DELETE ON table
// FOR EACH ROW|STATEMENT SET NEW.col = expr [, ...]
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
    // 60_view_trigger: FOR EACH ROW（默认） / FOR EACH STATEMENT。
    // true = ROW, false = STATEMENT。
    bool for_each_row = true;
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
    // 59_procs (Category 8): 参数模式。函数/UDF 参数总是 IN（默认 0）；
    // 过程参数可以是 IN (0) / OUT (1) / INOUT (2)。
    int mode = 0;  // 0=IN, 1=OUT, 2=INOUT
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

// DECLARE name TYPE[(N)] [DEFAULT expr] —— 在当前函数帧中声明一个局部变量。
// 变量类型只支持 INT / FLOAT / VARCHAR 三种，其它类型在 parser 阶段就报错。
// DEFAULT expr 可空；为空时初始为 NULL，为 expr 时初始为 expr 求值结果。
class DeclareVarStatement : public Statement {
public:
    DeclareVarStatement(std::string var_name, std::string data_type,
                        int32_t char_length = -1);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string var_name;
    std::string data_type;
    int32_t char_length = -1;
    // 59_procs (Category 8): 可选 DEFAULT expr（NULL 表示未指定）。
    ExprPtr default_expr;
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

// ============ 59_procs (Category 8)：过程语言扩展语句节点 ============
//
// 与 47_udf_trigger_view 的 IF/WHILE/RETURN 一样，这些节点仅在
// CreateFunctionStatement::body_statements / CreateProcedureStatement::body_statements
// 中出现，由 UdfExecutor 在调用期间解释执行。它们不参与 Planner / Catalog
// 的注册路径。

// LOOP body END LOOP [label]; —— 无限循环，必须用 LEAVE 退出。
// label 可空：有 label 时由 UdfExecutor 维护 label 栈；LEAVE / ITERATE
// 在 LOOP 入口处压栈，出口处弹栈，从而支持多层嵌套。
class LoopStatement : public Statement {
public:
    LoopStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::vector<StatementPtr> body;
    // 可空。空时意味着隐式 "$$_loop_n"，仍能通过 LEAVE n 来离开最近的
    // 循环。推荐用户写显式 label 以保持可读性。
    std::string label;
};

// REPEAT body UNTIL cond END REPEAT [label]; —— do-while 循环：先执行
// body 一次，再评估 cond；若为 TRUE 则退出；否则继续循环。
class RepeatStatement : public Statement {
public:
    RepeatStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::vector<StatementPtr> body;
    ExprPtr until_expr;
    std::string label;  // 可空
};

// CASE WHEN cond THEN stmts [WHEN ...] [ELSE stmts] END CASE;
// 体内 CASE：每个 WHEN 分支是条件 + 语句列表，匹配时执行其语句列表；
// ELSE 分支（可空）在没有 WHEN 匹配时执行。语义上与表达式 CASE
// (CaseExprNode) 不同：体内 CASE 不返回值，只决定执行哪个语句序列。
class CaseStatement : public Statement {
public:
    CaseStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    // 可空：NULL 表示搜索式 CASE（每个 when_condition 都是独立谓词）。
    ExprPtr subject;
    struct WhenClause {
        // 简单 CASE：与 subject 比较的右值；搜索式 CASE：谓词表达式。
        ExprPtr when_expr;
        std::vector<StatementPtr> body;
    };
    std::vector<WhenClause> whens;
    std::vector<StatementPtr> else_body;  // 可空
};

// LEAVE label; —— 离开最近的同名 label 循环。
// 由 UdfExecutor 在执行 LOOP/WHILE/REPEAT 时维护 label 栈；
// LEAVE 弹出直到匹配 label，然后设置 frame.control = LEAVE。
class LeaveStatement : public Statement {
public:
    explicit LeaveStatement(std::string label);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string label;
};

// ITERATE label; —— 跳到最近的同名 label 循环开头。
// UdfExecutor 看到 ITERATE 时同样设置 frame.control = ITERATE；
// 解释循环看到此标记后立刻"重新进入"下一轮迭代，而不是逐条执行完 body。
class IterateStatement : public Statement {
public:
    explicit IterateStatement(std::string label);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string label;
};

// SIGNAL SQLSTATE 'XXXXX' SET MESSAGE_TEXT = '...'; —— 抛自定义异常。
// UdfExecutor 在执行 SIGNAL 时构造一个 RuntimeError 异常向上抛；
// 上层若遇到匹配的 DECLARE ... HANDLER 则捕获并执行其 body；
// 否则异常继续向上传播，最终被 ExecutionEngine 当作 ERROR 输出。
class SignalStatement : public Statement {
public:
    SignalStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    // SQLSTATE 是 5 字符的字符串，如 '22012'（除零）。存储时保留外层引号。
    std::string sqlstate;
    std::string message_text;
};

// DECLARE {EXIT|CONTINUE|UNDO} HANDLER FOR cond stmt;
// 在函数/过程帧的 handler 栈上注册一个 handler。
// 条件 cond_kind 决定如何匹配：
//   - SQLEXCEPTION 匹配任意运行时错误
//   - SQLWARNING   匹配 SQLWARNING 类错误（V1 与 SQLEXCEPTION 合并处理）
//   - NOT_FOUND    匹配 NOT FOUND 类错误（V1 同上）
//   - SQLSTATE     匹配 SQLSTATE 字符串相等的错误
// type == CONTINUE 时执行 handler body 后继续；EXIT/UNDO 解析期接受，
// 但 V1 仅 CONTINUE 真正生效（执行器看到 EXIT/UNDO 时抛"未实现"）。
class DeclareHandlerStatement : public Statement {
public:
    DeclareHandlerStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    enum class Type { CONTINUE, EXIT, UNDO };
    Type type = Type::CONTINUE;

    enum class CondKind { SQLEXCEPTION, SQLWARNING, NOT_FOUND, SQLSTATE };
    CondKind cond_kind = CondKind::SQLEXCEPTION;
    // 仅 CondKind == SQLSTATE 时使用；为 'XXXXX' 形式的 SQLSTATE。
    std::string cond_sqlstate;

    // handler 主体：当前 V1 限定为单条语句。
    StatementPtr body;
};

// DECLARE name CURSOR FOR <select>; —— 在当前帧中注册一个 cursor。
// UdfExecutor 看到此语句时把 cursor_name → query 存入 frame.cursors，
// OPEN 时再调用 Planner/ExecutionEngine 把 query 转成执行计划并收集
// 全部结果行。
class DeclareCursorStatement : public Statement {
public:
    DeclareCursorStatement(std::string cursor_name, SelectStatementPtr query);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string cursor_name;
    SelectStatementPtr query;
};

// OPEN name; —— 启动 cursor：把 SELECT 计划跑一遍收集全部行，
// 把结果作为游标当前迭代器。
class CursorOpenStatement : public Statement {
public:
    explicit CursorOpenStatement(std::string cursor_name);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string cursor_name;
};

// FETCH name INTO var [, var ...]; —— 把 cursor 当前行的各列值依次
// 赋给 into_vars。行耗尽时 INTO 变量被置 NULL（NOT FOUND handler 可捕获）。
class CursorFetchStatement : public Statement {
public:
    CursorFetchStatement(std::string cursor_name, std::vector<std::string> into_vars);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string cursor_name;
    std::vector<std::string> into_vars;
};

// CLOSE name; —— 释放 cursor 持有的元组缓冲。允许再次 OPEN。
class CursorCloseStatement : public Statement {
public:
    explicit CursorCloseStatement(std::string cursor_name);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string cursor_name;
};

// ============ 59_procs (Category 8)：PROCEDURE 与 CALL 语句节点 ============

// CREATE PROCEDURE name(args) BEGIN body END —— 过程与函数形态相似，
// 但过程没有返回值，可以拥有 OUT 参数。
class CreateProcedureStatement : public Statement {
public:
    CreateProcedureStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string procedure_name;
    std::vector<FunctionParameter> parameters;
    std::vector<StatementPtr> body_statements;
};

// DROP PROCEDURE [IF EXISTS] name
class DropProcedureStatement : public Statement {
public:
    DropProcedureStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string procedure_name;
    bool if_exists = false;
};

// CALL name(arg1, arg2, ...); —— 在新函数帧里执行 procedure 的 body。
// 实参按位置映射到形参；OUT 参数在 procedure 返回后回写到 ExecutionContext
// 的 out_args_ 字典。
class CallStatement : public Statement {
public:
    CallStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string procedure_name;
    std::vector<ExprPtr> arguments;
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
    // format ∈ { "TEXT", "JSON", "SEXPR" }。默认 TEXT 与原行为一致。
    std::string format = "TEXT";
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

// ============ 54_dml：MERGE 语句 ============
//
// MERGE INTO target [AS t_alias]
// USING source [AS s_alias] ON <cond>
// WHEN MATCHED THEN UPDATE SET col = expr [, ...]
// WHEN NOT MATCHED THEN INSERT (cols) VALUES (exprs)
//
// 简化语义（V1）：
//   - 仅支持恰好一条 WHEN MATCHED UPDATE + 一条 WHEN NOT MATCHED INSERT；
//     多 MATCHED 链 / 多 NOT MATCHED 链不在 V1 范围内（任务文档 scope-cut）。
//   - source 可以是普通表名或派生表 (sub_query) AS alias。
//   - target_alias / source_alias 允许为空（无别名时回退到表名）。
//
// MERGE 语义：
//   对 source 的每行：
//     1) 用 ON 条件与 target 做匹配；
//     2) 命中则执行 UPDATE SET（带 CHECK / UNIQUE / FK / WAL 全套约束）；
//     3) 未命中则执行 INSERT VALUES（同样走完整约束）；
//   当前实现按 source 行枚举顺序执行；不保证确定性顺序，调用方应让 ON 条件
//   形成唯一匹配（PG/Oracle 同样要求）。
class MergeStatement : public Statement {
public:
    MergeStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string target_table;
    std::string target_alias;
    // source 可以是表名 + 可选别名，或者派生表（source_query 非空时优先）。
    std::string source_table;
    std::string source_alias;
    // 派生表数据源：USING (SELECT ...) AS alias 形式。
    SelectStatementPtr source_query;
    // ON <expr>：target 与 source 之间的连接条件。
    ExprPtr on_condition;

    // WHEN MATCHED THEN UPDATE SET col = expr [, ...]
    bool has_matched_update = false;
    std::vector<std::pair<std::string, ExprPtr>> matched_assignments;

    // WHEN NOT MATCHED THEN INSERT (cols) VALUES (exprs)
    bool has_not_matched_insert = false;
    std::vector<std::string> insert_columns;
    std::vector<ExprPtr> insert_values;  // 单行 VALUES，与 insert_columns 平行
};

// ============ 53_ddl：SCHEMA / SEQUENCE 语句 ============

// CREATE SCHEMA name —— 记录 schema 存在性。DROP TABLE schema.tbl 等引用
// 形式以此校验 schema 已存在。
class CreateSchemaStatement : public Statement {
public:
    CreateSchemaStatement();
    explicit CreateSchemaStatement(std::string name);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string schema_name;
    // CREATE SCHEMA IF NOT EXISTS：schema 已存在时静默成功。
    bool if_not_exists = false;
};

// DROP SCHEMA [IF EXISTS] name —— 仅当 schema 内已无表时可成功；
// 非空时返回错误并保持 schema 存在。
class DropSchemaStatement : public Statement {
public:
    DropSchemaStatement();
    explicit DropSchemaStatement(std::string name);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string schema_name;
    bool if_exists = false;
};

// CREATE SEQUENCE name [START n] [INCREMENT n] —— 内存态计数。
// 缺省 START=1、INCREMENT=1。NEXTVAL FOR name 读取并原子推进。
class CreateSequenceStatement : public Statement {
public:
    CreateSequenceStatement();

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string sequence_name;
    int64_t start_value = 1;
    int64_t increment = 1;
    bool if_not_exists = false;
};

// DROP SEQUENCE [IF EXISTS] name —— 释放序列计数器。
class DropSequenceStatement : public Statement {
public:
    DropSequenceStatement();
    explicit DropSequenceStatement(std::string name);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string sequence_name;
    bool if_exists = false;
};

// NEXTVAL FOR sequence_name —— 求值期由 ExpressionEvaluator 在 sequence
// 字典里查找并原子推进；返回值是 INTEGER。若序列不存在，抛语义错误。
class NextvalExpr : public Expr {
public:
    explicit NextvalExpr(std::string sequence_name);

    NodeType GetType() const override;
    std::string ToString() const override;

    std::string sequence_name;
};

}  // namespace sqlcompiler
