#pragma once

#include "ast/AST.h"
#include "catalog/SystemCatalog.h"
#include "plan/Plan.h"

namespace sqlcompiler {

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

    // 访问路径选择：把 Filter -> SeqScan 改写成 IndexScan
    PlanNodePtr ChooseAccessPaths(PlanNodePtr plan);

    SystemCatalog* catalog_;

    // 列裁剪：去除Project中未被上层使用的列
    PlanNodePtr PruneColumns(PlanNodePtr plan);

    // 常量折叠：在编译期计算表达式中的常量子表达式
    ExprPtr FoldConstants(ExprPtr expr);

    // 判断表达式是否为常量表达式（不含列引用）
    bool IsConstantExpr(const ExprPtr& expr) const;
};

}  // namespace sqlcompiler
