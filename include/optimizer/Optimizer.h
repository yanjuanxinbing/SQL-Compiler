#pragma once

#include "ast/AST.h"
#include "plan/Plan.h"

namespace sqlcompiler {

// 查询优化器：对逻辑执行计划进行等价改写以提升执行效率
class Optimizer {
public:
    Optimizer();

    // 优化入口：输入原始逻辑计划，返回优化后的逻辑计划
    PlanNodePtr Optimize(PlanNodePtr plan);

private:
    // 谓词下推：将Filter尽可能下推到靠近数据源的位置
    PlanNodePtr PushDownPredicates(PlanNodePtr plan);

    // 列裁剪：去除Project中未被上层使用的列
    PlanNodePtr PruneColumns(PlanNodePtr plan);

    // 常量折叠：在编译期计算表达式中的常量子表达式
    ExprPtr FoldConstants(ExprPtr expr);

    // 判断表达式是否为常量表达式（不含列引用）
    bool IsConstantExpr(const ExprPtr& expr) const;
};

}  // namespace sqlcompiler
