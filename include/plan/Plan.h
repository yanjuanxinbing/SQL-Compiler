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
};

// 删除节点
class DeleteNode : public PlanNode {
public:
    DeleteNode(std::string table_name, ExprPtr predicate);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    ExprPtr predicate;  // 可为空
};

// 建表节点
class CreateTableNode : public PlanNode {
public:
    CreateTableNode(std::string table_name, std::vector<ColumnDefinition> columns,
                    std::vector<std::vector<std::string>> primary_keys = {},
                    bool if_not_exists = false);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    std::vector<ColumnDefinition> columns;
    // 表级 PRIMARY KEY(a, b) 的分组信息，需原样传到 Catalog 才能按「组合唯一」校验
    std::vector<std::vector<std::string>> primary_keys;
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

// ALTER TABLE 节点：承载 4 类动作（ADD/DROP/RENAME/MODIFY）。
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

// DROP VIEW / DROP TRIGGER / DROP FUNCTION：no-op。
class DropObjectNode : public PlanNode {
public:
    enum class Kind { VIEW, TRIGGER, FUNCTION };
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

}  // namespace sqlcompiler
