#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/AST.h"
#include "storage_engine/Value.h"

namespace sqlcompiler {

// 逻辑执行计划节点类型
enum class PlanNodeType {
    SEQ_SCAN,      // 全表扫描
    INDEX_SCAN,    // 走 B+Tree 索引的区间扫描（含等值点查）
    FILTER,        // 条件过滤（对应 WHERE / HAVING）
    PROJECT,       // 投影（对应 SELECT 列表）
    JOIN,          // 连接
    SORT,          // 排序（对应 ORDER BY）
    LIMIT,         // 限制返回行数
    AGGREGATE,     // 聚合（对应 GROUP BY / 聚合函数）
    INSERT,        // 插入
    UPDATE,        // 更新
    DELETE,        // 删除
    CREATE_TABLE,  // 建表
    DROP_TABLE,    // 删表
    TRUNCATE_TABLE, // 清空表数据（保留表结构）
    CREATE_INDEX,
    DROP_INDEX,
    ALTER_TABLE,   // ALTER TABLE 子句：当前仅接受语法，执行期按 no-op 处理
    SET_OP,        // UNION / INTERSECT / EXCEPT
    WINDOW,        // 窗口函数（OVER ...）—— 桩节点，由后续执行器填充
    SUBQUERY,      // 子查询：在父算子的表达式中被求值
    CTE_BIND,      // CTE 绑定：把一个已物化的 CTE 暴露为虚拟表
    CTE_DEFINE,    // CTE 定义：在执行期物化一份内部子计划并保存到 ExecutionContext

    // ---- 40_txn_view_udf ----
    NO_OP,         // BEGIN/COMMIT/ROLLBACK/SAVEPOINT/RELEASE/DropView 等纯副作用语句
    CREATE_VIEW,   // 视图已记入 catalog，no-op 执行（供后续 SELECT FROM view 查询）
    CREATE_TRIGGER,// 触发器已记入 catalog，no-op 执行
    CREATE_FUNCTION,// UDF 已记入 catalog，no-op 执行
    // ---- 59_procs (Category 8) ----
    CREATE_PROCEDURE, // 过程已记入 catalog，no-op 执行
    CALL,             // CALL proc(args) —— 复用 UdfExecutor 解释器
    VIEW_DEFINE,   // 视图定义：执行时把视图的 SELECT 翻译成子查询占位 SeqScanNode，
                   // ExecutionEngine 识别 alias 后改走子计划
    UPSERT,        // 43_upsert: ON DUPLICATE KEY UPDATE —— 主键冲突时改写已有行

    // ---- 48_acid_undo: 事务控制节点 ----
    BEGIN_TXN,         // BEGIN [TRANSACTION]
    COMMIT_TXN,        // COMMIT
    ROLLBACK_TXN,      // ROLLBACK
    SAVEPOINT,         // SAVEPOINT name
    ROLLBACK_TO_SP,    // ROLLBACK TO name
    RELEASE_SP,        // RELEASE SAVEPOINT name

    // ---- 46_meta: 元命令 ----
    EXPLAIN,       // EXPLAIN [ANALYZE] <statement> —— 把 inner 的计划树打印成文本
    SHOW,          // SHOW TABLES / SHOW COLUMNS / SHOW INDEX / SHOW CREATE TABLE

    // ---- 53_ddl: DDL 扩展（FK / SCHEMA / SEQUENCE） ----
    CREATE_SCHEMA,  // CREATE SCHEMA name —— 注册 schema 命名空间
    DROP_SCHEMA,    // DROP SCHEMA name —— 校验非空后移除
    CREATE_SEQUENCE,// CREATE SEQUENCE name [START n] [INCREMENT n] —— 注册序列
    DROP_SEQUENCE,  // DROP SEQUENCE name —— 释放序列

    // ---- 54_dml: DML 扩展（RETURNING / UPDATE-FROM / MERGE / REPLACE）----
    UPDATE_FROM,    // UPDATE ... FROM source ... —— 跨表更新；children[0] 是 JoinNode 子计划
    MERGE,          // MERGE INTO target USING source ON cond ...

    // ---- 55_query: 查询/表达式扩展 ----
    VALUES,         // (VALUES (1,2), (3,4)) 作为 FROM 派生表：逐行发射字面量元组
    APPLY,          // LATERAL/CROSS APPLY：对每条外层行跑一次右子计划并发出拼接行

    // ---- 60_view_trigger (Category 9): VIEW / TRIGGER 扩展 ----
    CREATE_MATERIALIZED_VIEW,  // 物化视图：建 backing table + 物化数据
    ALTER_MATERIALIZED_VIEW,   // 物化视图 REFRESH：truncate + 重新执行 SELECT
};

// 执行计划节点基类，采用树形结构，子节点为输入
class PlanNode {
public:
    virtual ~PlanNode() = default;
    virtual PlanNodeType GetType() const = 0;
    virtual std::string ToString() const = 0;

    std::vector<std::shared_ptr<PlanNode>> children;
};
using PlanNodePtr = std::shared_ptr<PlanNode>;

// 全表扫描节点：table_alias 用于限定列引用（如 FROM user u -> 列 u.id）
class SeqScanNode : public PlanNode {
public:
    SeqScanNode(std::string table_name, std::string table_alias = "");

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    std::string table_alias;
};

// 过滤节点
class FilterNode : public PlanNode {
public:
    explicit FilterNode(ExprPtr predicate);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    ExprPtr predicate;
};

// 索引扫描节点：沿 B+Tree 的叶子链扫描 [low_key, high_key] 区间，再用 RID 回表。
//
// 边界用 std::vector<Value> 而不是表达式：Optimizer 只在谓词能完全求值成常量时
// 才改写成索引扫描，把「表达式求值」这件事挡在计划生成阶段之外，执行期就不必
// 再考虑边界值随行变化的情况。
class IndexScanNode : public PlanNode {
public:
    IndexScanNode(std::string table_name, std::string index_name,
                  std::string table_alias = "");

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    std::string index_name;
    std::string table_alias;

    std::vector<Value> low_key;      // 空表示 -inf
    std::vector<Value> high_key;     // 空表示 +inf
    bool low_inclusive = true;
    bool high_inclusive = true;

    // 无法用索引消解的剩余谓词，回表拿到 Tuple 后再判一次。
    // 为空表示索引区间已经精确等价于原谓词。
    ExprPtr residual_predicate;
};

// 投影节点
class ProjectNode : public PlanNode {
public:
    ProjectNode(std::vector<ExprPtr> columns,
                std::vector<std::string> aliases = {},
                bool is_distinct = false);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::vector<ExprPtr> columns;
    std::vector<std::string> aliases;  // 与 columns 平行，可空
    bool is_distinct;
};

// 连接节点
class JoinNode : public PlanNode {
public:
    JoinNode(JoinType join_type, ExprPtr condition);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    JoinType join_type;
    ExprPtr condition;
};

// 排序节点
class SortNode : public PlanNode {
public:
    explicit SortNode(std::vector<OrderByItem> order_items);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::vector<OrderByItem> order_items;
};

// 限制行数节点：支持 LIMIT count 或 LIMIT offset, count 两种形式
class LimitNode : public PlanNode {
public:
    LimitNode(int limit_count, int offset = 0);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    int limit_count;
    int offset;
};

// 聚合节点
class AggregateNode : public PlanNode {
public:
    AggregateNode(std::vector<ExprPtr> group_by_exprs, std::vector<ExprPtr> aggregate_exprs,
                  std::vector<std::string> aliases = {});

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::vector<ExprPtr> group_by_exprs;
    std::vector<ExprPtr> aggregate_exprs;
    // 与 aggregate_exprs 平行的别名（来自 SELECT list 的 alias），用于 HAVING/ORDER BY
    // 通过别名引用对应的聚合输出位置。
    std::vector<std::string> aliases;
};

// 插入节点
class InsertNode : public PlanNode {
public:
    InsertNode(std::string table_name, std::vector<std::string> columns,
               std::vector<std::vector<ExprPtr>> values_list);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    std::vector<std::string> columns;
    std::vector<std::vector<ExprPtr>> values_list;
    // INSERT ... SELECT 时由 Planner 把 SELECT 转换为子计划。children[0] 同时指向
    // 该子计划（语义上"插入源"），children[0] 即 query_plan。执行器优先消费它，
    // values_list 与 query_plan 互斥（query_plan 非空时使用源计划，否则用 values_list）。
    PlanNodePtr query_plan;
    // ---- 54_dml: REPLACE INTO 标记 ----
    // true 时按 MySQL REPLACE 语义：候选行在 PK/UNIQUE 上冲突，先删旧行再插新行。
    // 当前实现走 UpsertExecutor 的"删除+插入"路径。
    bool is_replace = false;
    // ---- 54_dml: RETURNING 子句 ----
    // INSERT 成功后，对新行求值 returning_exprs 并以结果集形式返回。
    std::vector<ExprPtr> returning_exprs;
    std::vector<std::string> returning_aliases;
};

// 43_upsert: ON DUPLICATE KEY UPDATE 节点。
//
// 与 InsertNode 共用 values_list / columns（候选数据来自 VALUES），但额外携带
// upsert_assignments 用于冲突路径上的列改写。当前实现仅 PRIMARY KEY 冲突会触发
// 改写；UNIQUE INDEX 冲突尚未接入（见 UpsertExecutor 头部说明）。
class UpsertNode : public PlanNode {
public:
    UpsertNode(std::string table_name, std::vector<std::string> columns,
               std::vector<std::vector<ExprPtr>> values_list,
               std::vector<std::pair<std::string, ExprPtr>> upsert_assignments);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    std::vector<std::string> columns;
    std::vector<std::vector<ExprPtr>> values_list;
    // col = expr[, ...]；expr 内允许出现 UpsertValuesRefExpr 引用本次候选行的列。
    std::vector<std::pair<std::string, ExprPtr>> upsert_assignments;
    // ---- 54_dml: RETURNING 子句（与 InsertNode 共享语义） ----
    std::vector<ExprPtr> returning_exprs;
    std::vector<std::string> returning_aliases;
};

// 更新节点
class UpdateNode : public PlanNode {
public:
    UpdateNode(std::string table_name,
               std::vector<std::pair<std::string, ExprPtr>> assignments, ExprPtr predicate);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    std::vector<std::pair<std::string, ExprPtr>> assignments;
    ExprPtr predicate;  // 可为空
    // ---- 54_dml: UPDATE ... FROM source ----
    // 当 from_sources 非空时，由 Planner 构造一个 UpdateFromNode（见下）走 join 路径；
    // 当前 UpdateNode 仅承载 from_sources 为空的情况。target_alias 可为空。
    std::string target_alias;
    // ---- 54_dml: RETURNING 子句 ----
    std::vector<ExprPtr> returning_exprs;
    std::vector<std::string> returning_aliases;
};

// 54_dml: UPDATE ... FROM 节点。
//
// 与 UpdateNode 类似，但带一个 JoinNode 子计划：children[0] 是 Planner
// 把 target × from_sources 拼成的连接结果，每行包含 target 与 source 的所有列。
// 执行器对每行"joined tuple"评估 SET 赋值（按 target 列下标写回），并复用
// UPDATE 的约束 / 索引维护 / WAL 路径。
class UpdateFromNode : public PlanNode {
public:
    UpdateFromNode(std::string table_name,
                   std::vector<std::pair<std::string, ExprPtr>> assignments);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    // target 表的别名（UPDATE t AS t SET ... FROM s WHERE ...），空字符串表示无别名。
    std::string target_alias;
    std::vector<std::pair<std::string, ExprPtr>> assignments;
    // WHERE 谓词：在 UPDATE FROM 路径下，连接条件 + 过滤条件都在这里（PG/Oracle 风格）。
    ExprPtr where_clause;
    // ---- 54_dml: RETURNING 子句 ----
    std::vector<ExprPtr> returning_exprs;
    std::vector<std::string> returning_aliases;
};

// 删除节点
class DeleteNode : public PlanNode {
public:
    DeleteNode(std::string table_name, ExprPtr predicate);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    ExprPtr predicate;  // 可为空
    // ---- 54_dml: RETURNING 子句 ----
    std::vector<ExprPtr> returning_exprs;
    std::vector<std::string> returning_aliases;
};

// 54_dml: MERGE INTO 节点。
//
// target_table：被合并的目标表；source_table / source_alias：USING 子句的数据源；
// source_plan：Planner 把 source_table 或 (SELECT ...) AS alias 转成的子计划，
// 输出列顺序与 source_table 视图（或派生表 SELECT list）一致。
// on_condition：target × source 的连接条件，用于判定 MATCHED / NOT MATCHED。
// matched_assignments / not_matched_insert_*：分别承载 WHEN MATCHED / NOT MATCHED
// 分支的具体动作。MergeExecutor 枚举 source_plan 的每一行评估 on_condition 后
// 走对应分支；与现有 CHECK / UNIQUE / FK / WAL 路径完全共享。
class MergeNode : public PlanNode {
public:
    MergeNode(std::string target_table);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string target_table;
    std::string target_alias;
    std::string source_table;
    std::string source_alias;
    PlanNodePtr source_plan;
    ExprPtr on_condition;

    bool has_matched_update = false;
    std::vector<std::pair<std::string, ExprPtr>> matched_assignments;

    bool has_not_matched_insert = false;
    std::vector<std::string> not_matched_columns;
    std::vector<ExprPtr> not_matched_values;
};

// 建表节点
class CreateTableNode : public PlanNode {
public:
    CreateTableNode(std::string table_name, std::vector<ColumnDefinition> columns,
                    std::vector<std::vector<std::string>> primary_keys = {},
                    std::vector<std::vector<std::string>> unique_constraints = {},
                    std::vector<ForeignKeyDef> foreign_keys = {},
                    std::vector<TableCheckDef> table_checks = {},
                    bool if_not_exists = false);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    std::vector<ColumnDefinition> columns;
    // 表级 PRIMARY KEY(a, b) 的分组信息，需原样传到 Catalog 才能按「组合唯一」校验
    std::vector<std::vector<std::string>> primary_keys;
    // 52_data_types: 表级 UNIQUE(col, ...) 约束。CreateTableExecutor 据此
    // 自动建立等价的隐式唯一索引。
    std::vector<std::vector<std::string>> unique_constraints;
    // 53_ddl: 表级 FOREIGN KEY 约束。CreateTableExecutor 把它们登记到
    // catalog.fk_constraints_，并在 INSERT/UPDATE/DELETE 路径上兑现约束。
    std::vector<ForeignKeyDef> foreign_keys;
    // 58_constraints: 表级 CHECK(expr) / CONSTRAINT name CHECK(expr)。
    // CreateTableExecutor 把它原样落到 catalog，约束校验在写入路径上做。
    std::vector<TableCheckDef> table_checks;
    // CREATE TABLE IF NOT EXISTS 标记
    bool if_not_exists = false;
};

// 删表节点
class DropTableNode : public PlanNode {
public:
    explicit DropTableNode(std::string table_name, bool if_exists = false);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    bool if_exists = false;
};

// 建索引节点
class CreateIndexNode : public PlanNode {
public:
    CreateIndexNode(std::string index_name, std::string table_name,
                    std::vector<std::string> key_columns, bool is_unique);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string index_name;
    std::string table_name;
    std::vector<std::string> key_columns;
    bool is_unique = false;
};

// 删索引节点
class DropIndexNode : public PlanNode {
public:
    DropIndexNode(std::string index_name, bool if_exists);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string index_name;
    bool if_exists = false;
};

// 清空表节点
class TruncateTableNode : public PlanNode {
public:
    explicit TruncateTableNode(std::string table_name);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
};

// ALTER TABLE 节点：承载 5 类动作（ADD/DROP/RENAME/MODIFY/RENAME_COLUMN）。
// 当前执行器以 no-op 处理（DDL 扩展语法的最小实现），保证后续
// SELECT 仍能访问原表。
class AlterTableNode : public PlanNode {
public:
    AlterTableNode(AlterAction action, std::string table_name);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    AlterAction action;
    std::string table_name;
    // ADD COLUMN / MODIFY COLUMN 时使用的列定义；其他动作置空。
    std::shared_ptr<ColumnDefinition> column_def;
    // DROP COLUMN 时填写被删列名。
    std::string drop_column_name;
    // RENAME TO 时填写新表名。
    std::string new_table_name;
    // 53_ddl: RENAME COLUMN 时填写被改名列名与新列名。
    std::string rename_column_old_name;
    std::string rename_column_new_name;
};

// 集合运算节点
class SetOpNode : public PlanNode {
public:
    enum class Kind { UNION, UNION_ALL, INTERSECT, EXCEPT };
    SetOpNode(Kind kind);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    Kind kind;
};

// 窗口函数节点：消费子算子的全部 Tuple，对 SELECT 列表中的 WindowFuncNode
// 求值并返回与 Project 相同形状的输出。
class WindowNode : public PlanNode {
public:
    WindowNode(std::vector<ExprPtr> select_list,
               std::vector<std::string> aliases,
               std::vector<std::pair<std::string, WindowSpec>> named_windows);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::vector<ExprPtr> select_list;
    std::vector<std::string> aliases;
    std::vector<std::pair<std::string, WindowSpec>> named_windows;
};

// 子查询节点：包装一个内部子计划，使其可在父算子的表达式树中被求值。
//
// kind 决定求值语义：
//   SCALAR  — 返回子计划第一行第一列（如果为空则 NULL）。相关子查询需每行重算。
//   EXISTS  — 子计划产生 ≥1 行即 TRUE。
//   IN      — 子计划单列时按 IN 集合判定；否则按行值匹配 outer_expr。
//   ANY     — outer_expr op ANY(...)：至少一个元素满足 op。
//
// outer_expr/comparison_op 仅 IN/ANY 语义使用。
class SubqueryNode : public PlanNode {
public:
    SubqueryNode(SubqueryType kind, ExprPtr outer_expr = nullptr,
                 std::string comparison_op = "");

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    SubqueryType kind;
    ExprPtr outer_expr;       // IN/ANY 语义下与子查询结果比较的外部表达式
    std::string comparison_op; // 仅 ANY：= / < / <= / > / >= / <>
};

// CTE 定义节点：执行时先把 children[0] 的子计划跑一遍，
// 把结果存入 ExecutionContext 的 cte_results_[name]。CTE_BIND 节点随后可按名引用。
class CteDefineNode : public PlanNode {
public:
    CteDefineNode(std::string cte_name, bool is_recursive = false);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string cte_name;
    bool is_recursive;
    // 递归 CTE 的"非递归部"（anchor）。可选；只有 is_recursive=true 时使用。
    // 普通 CTE 直接由 cte_plan 提供全部结果。
    PlanNodePtr anchor_child;
    // 普通 CTE 的物化计划（由 PlanWithClause 填入）。
    PlanNodePtr cte_plan;
};

// CTE 引用节点：把已物化的 CTE 结果作为一张虚拟表逐行发射。
class CteBindNode : public PlanNode {
public:
    explicit CteBindNode(std::string cte_name);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string cte_name;
    // 物化 CTE 时的列名（用于把结果列按名暴露给外层列引用解析）。
    std::vector<std::string> column_names;
};

// ============ 40_txn_view_udf：事务 / 视图 / 触发器 / UDF 节点 ============

// 无副作用 / 已记录到 catalog 的语句节点。
class NoOpNode : public PlanNode {
public:
    NoOpNode(std::string description);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string description;
};

// CREATE VIEW：no-op。视图的 SELECT 子句记入 catalog，
// SELECT FROM view 时由 Planner 查 catalog 并把视图查询替换为子计划。
class CreateViewNode : public PlanNode {
public:
    explicit CreateViewNode(std::string view_name);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string view_name;
};

// CREATE TRIGGER：no-op。
class CreateTriggerNode : public PlanNode {
public:
    explicit CreateTriggerNode(std::string trigger_name);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string trigger_name;
};

// CREATE FUNCTION：no-op。
class CreateFunctionNode : public PlanNode {
public:
    explicit CreateFunctionNode(std::string function_name);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string function_name;
};

// ============ 59_procs (Category 8)：CREATE PROCEDURE / CALL 节点 ============

// CREATE PROCEDURE：no-op。
class CreateProcedureNode : public PlanNode {
public:
    explicit CreateProcedureNode(std::string procedure_name);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string procedure_name;
};

// CALL proc(args) —— 在新函数帧里执行 procedure 的 body。
// 实参按位置映射到形参；OUT / INOUT 参数在 procedure 完成后由执行器
// 写回 ExecutionContext::out_args_。
class CallNode : public PlanNode {
public:
    CallNode(std::string procedure_name, std::vector<ExprPtr> arguments);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string procedure_name;
    std::vector<ExprPtr> arguments;
};

// DROP VIEW / DROP TRIGGER / DROP FUNCTION：no-op。
class DropObjectNode : public PlanNode {
public:
    enum class Kind { VIEW, TRIGGER, FUNCTION, PROCEDURE };
    DropObjectNode(Kind kind, std::string object_name, bool if_exists);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    Kind kind;
    std::string object_name;
    bool if_exists;
};

// VIEW_DEFINE：把视图的 SELECT 翻译为子查询占位 SeqScanNode。
// 等同于派生表占位：table_name == alias 且 children[0] 为子计划。
// 使用 ViewDefineNode 主要便于 ExecutionEngine / Planner 区分 catalog 视图
// 与派生表（FROM (SELECT ...) AS alias）。
class ViewDefineNode : public PlanNode {
public:
    ViewDefineNode(std::string view_name, std::string view_alias);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string view_name;
    std::string view_alias;
};

// ============ 46_meta: 元命令 ============

// EXPLAIN 节点：把 inner 的计划树渲染成文本。
//
// 内层计划由 Planner 把 ExplainStatement.inner 翻译为对应的 PlanNode 后挂在
// children[0] 上；ExplainExecutor 在 Init() 阶段调用 plan->ToString() 获取
// 文本，并把它包成一行结果集返回到上层。
class ExplainNode : public PlanNode {
public:
    ExplainNode(bool analyze);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    bool analyze = false;
};

// ============ 48_acid_undo: 事务控制节点 ============

class BeginTxnNode : public PlanNode {
public:
    BeginTxnNode();

    PlanNodeType GetType() const override;
    std::string ToString() const override;
};

class CommitTxnNode : public PlanNode {
public:
    CommitTxnNode();

    PlanNodeType GetType() const override;
    std::string ToString() const override;
};

class RollbackTxnNode : public PlanNode {
public:
    RollbackTxnNode();

    PlanNodeType GetType() const override;
    std::string ToString() const override;
};

class SavepointNode : public PlanNode {
public:
    explicit SavepointNode(std::string name);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string savepoint_name;
};

class RollbackToSavepointNode : public PlanNode {
public:
    explicit RollbackToSavepointNode(std::string name);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string savepoint_name;
};

class ReleaseSavepointNode : public PlanNode {
public:
    explicit ReleaseSavepointNode(std::string name);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string savepoint_name;
};

// SHOW 节点：kind 决定执行器从 catalog 拉什么数据填充结果集。
// target_table 仅在 COLUMNS/INDEX/CREATE_TABLE 时使用。
class ShowNode : public PlanNode {
public:
    enum class Kind {
        TABLES,
        COLUMNS,
        INDEX,
        CREATE_TABLE,
    };

    ShowNode(Kind kind, std::string target_table);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    Kind kind;
    std::string target_table;
};

// ============ 55_query: VALUES / APPLY 计划节点 ============

// VALUES 节点：承载 (VALUES (a,b), (c,d)) AS t(id, name) 的字面量行。
// children 为空；执行器按 rows 顺序发射每个 Tuple。
class ValuesNode : public PlanNode {
public:
    ValuesNode(std::vector<std::vector<ExprPtr>> rows,
               std::vector<std::string> column_aliases,
               std::string derived_alias);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::vector<std::vector<ExprPtr>> rows;
    std::vector<std::string> column_aliases;  // 可空
    std::string derived_alias;                 // 派生表别名 t
};

// APPLY 节点（LATERAL）：对每条外层（左）行驱动一次右子计划，发出拼接行。
// is_left_outer == true 对应 LEFT OUTER APPLY（无匹配也补 NULL 行），
// 当前 V1 始终为 INNER CROSS APPLY：右子计划无输出时不发该外层行。
// lateral_alias 是 LATERAL 派生表的别名（如 `sub`），供 ExecutionEngine 把
// 右子计划的输出列以 `<alias>.<col>` 形式登记到 column_index_map。
// lateral_inner_tables 是右子查询的 from_table_alias 与 join 别名集合，
// 让右子计划 evaluator 识别「qualified ref 是内层表名还是外层引用」。
class ApplyNode : public PlanNode {
public:
    ApplyNode(bool is_left_outer, std::string lateral_alias,
              std::vector<std::string> lateral_inner_tables);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    bool is_left_outer = false;
    std::string lateral_alias;
    std::vector<std::string> lateral_inner_tables;
};

// ============ 53_ddl: SCHEMA / SEQUENCE 计划节点 ============

class CreateSchemaNode : public PlanNode {
public:
    CreateSchemaNode(std::string name, bool if_not_exists = false);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string schema_name;
    bool if_not_exists;
};

class DropSchemaNode : public PlanNode {
public:
    DropSchemaNode(std::string name, bool if_exists = false);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string schema_name;
    bool if_exists;
};

class CreateSequenceNode : public PlanNode {
public:
    CreateSequenceNode(std::string name, int64_t start_value = 1,
                       int64_t increment = 1, bool if_not_exists = false);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string sequence_name;
    int64_t start_value;
    int64_t increment;
    bool if_not_exists;
};

class DropSequenceNode : public PlanNode {
public:
    DropSequenceNode(std::string name, bool if_exists = false);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string sequence_name;
    bool if_exists;
};

// ============ 60_view_trigger (Category 9)：物化视图节点 ============

// CREATE MATERIALIZED VIEW name AS <select>
// 执行流程：
//   1. 把 SELECT 的输出列定型为 ColumnDefinition；
//   2. catalog 内建一张 backing table（表名 "__mv_<view_name>"）；
//   3. 立即执行 SELECT 的子计划，把每行写入 backing table。
// 子计划挂在 children[0] 上，由 MaterializedViewExecutor 在 Init 阶段
// 收集全部行后批量 InsertTuple。
class CreateMaterializedViewNode : public PlanNode {
public:
    CreateMaterializedViewNode(std::string view_name,
                               std::vector<ColumnDefinition> columns,
                               bool if_not_exists = false);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string view_name;
    std::vector<ColumnDefinition> columns;
    bool if_not_exists = false;
};

// ALTER MATERIALIZED VIEW name REFRESH
// 截断 backing table 并重新执行 SELECT。
// 子计划同 CREATE_MATERIALIZED_VIEW —— 复用 PlanSelect 的输出。
class AlterMaterializedViewNode : public PlanNode {
public:
    AlterMaterializedViewNode(std::string view_name);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string view_name;
};

}  // namespace sqlcompiler
