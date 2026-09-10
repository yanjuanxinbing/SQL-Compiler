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
    DROP_INDEX
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
    AggregateNode(std::vector<ExprPtr> group_by_exprs, std::vector<ExprPtr> aggregate_exprs);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::vector<ExprPtr> group_by_exprs;
    std::vector<ExprPtr> aggregate_exprs;
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

}  // namespace sqlcompiler
