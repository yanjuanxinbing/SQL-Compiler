#pragma once

#include <string>
#include <utility>
#include <vector>

#include "ast/AST.h"
#include "catalog/SystemCatalog.h"
#include "plan/Plan.h"

namespace sqlcompiler {

// 列裁剪传播上下文：在自顶向下的递归中累积上层需要的列。
// 区分「限定 / 未限定」是因为 JOIN 时需要按表分派列引用，
// 而未限定列可能属于多张表的同名列，需保守地下放到所有候选表。
struct PruneColumnsCtx {
    SystemCatalog* catalog = nullptr;
    // 限定列引用（如 t.id）。JOIN 左右分派时按表名/别名归类。
    std::vector<std::pair<std::string, std::string>> required_qualified;
    // 未限定列引用。无法判定属于哪张表时下放到所有候选表。
    std::vector<std::string> required_unqualified;
};

// 查询优化器：对逻辑执行计划进行等价改写以提升执行效率
class Optimizer {
public:
    // catalog 可为空：此时不做基于索引的访问路径选择，其余优化照常。
    // 让它可空是为了保证优化器在没有目录的场景（如单元测试）仍可独立使用。
    explicit Optimizer(SystemCatalog* catalog = nullptr);

    // 优化入口：输入原始逻辑计划，返回优化后的逻辑计划
    PlanNodePtr Optimize(PlanNodePtr plan);

private:
    // 谓词下推：将Filter尽可能下推到靠近数据源的位置
    PlanNodePtr PushDownPredicates(PlanNodePtr plan);
    PlanNodePtr PushDownFilterOverScan(PlanNodePtr plan);
    PlanNodePtr PushDownFilterOverJoin(PlanNodePtr plan);

    // 访问路径选择：把 Filter -> SeqScan 改写成 IndexScan
    PlanNodePtr ChooseAccessPaths(PlanNodePtr plan);

    // 常量折叠：在编译期求值表达式的常量子表达式，并把化简后的
    // TRUE/FALSE 谓词应用到 FilterNode（TRUE → 移除 Filter；
    // FALSE → 保留，FilterExecutor 自然返回空集）。
    // 本函数对 PlanNodePtr 做整体改写（含 children 替换），返回新根。
    PlanNodePtr FoldConstants(PlanNodePtr plan);

    SystemCatalog* catalog_;

    // 列裁剪：去除Project中未被上层使用的列
    PlanNodePtr PruneColumns(PlanNodePtr plan);

    // 列裁剪递归助手：自顶向下传播「上层用到的列」，在叶子节点上写入 read_columns
    void PruneNode(PlanNode* node, const PruneColumnsCtx& ctx);

    // 常量折叠：在编译期计算表达式中的常量子表达式
    ExprPtr FoldConstants(ExprPtr expr);

    // 判断表达式是否为常量表达式（不含列引用、聚合、子查询、窗口函数、NEXTVAL 等）
    bool IsConstantExpr(const ExprPtr& expr) const;
};

}  // namespace sqlcompiler
