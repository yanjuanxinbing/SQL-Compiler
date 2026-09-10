#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/AST.h"

namespace sqlcompiler {

// 逻辑执行计划节点类型
enum class PlanNodeType {
    SEQ_SCAN,      // 全表扫描
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
    TRUNCATE_TABLE // 清空表数据（保留表结构）
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
    CreateTableNode(std::string table_name, std::vector<ColumnDefinition> columns);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
    std::vector<ColumnDefinition> columns;
};

// 删表节点
class DropTableNode : public PlanNode {
public:
    explicit DropTableNode(std::string table_name);

    PlanNodeType GetType() const override;
    std::string ToString() const override;

    std::string table_name;
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
