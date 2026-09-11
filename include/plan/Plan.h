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
    SET_OP,        // UNION / INTERSECT / EXCEPT
    WINDOW,        // 窗口函数（OVER ...）—— 桩节点，由后续执行器填充
    SUBQUERY,      // 子查询：在父算子的表达式中被求值
    CTE_BIND,      // CTE 绑定：把一个已物化的 CTE 暴露为虚拟表
    CTE_DEFINE,    // CTE 定义：在执行期物化一份内部子计划并保存到 ExecutionContext
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

}  // namespace sqlcompiler
