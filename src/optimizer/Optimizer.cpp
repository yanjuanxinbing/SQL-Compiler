#include "optimizer/Optimizer.h"

namespace sqlcompiler {

Optimizer::Optimizer() {
    // TODO: 如有需要可补充初始化逻辑（如注册优化规则列表）
}

PlanNodePtr Optimizer::Optimize(PlanNodePtr plan) {
    // TODO: 依次应用各优化规则，例如：
    // plan = PushDownPredicates(plan);
    // plan = PruneColumns(plan);
    return plan;
}

PlanNodePtr Optimizer::PushDownPredicates(PlanNodePtr plan) {
    // TODO: 递归遍历计划树，将FilterNode尽量下推到SeqScanNode/JoinNode附近
    return plan;
}

PlanNodePtr Optimizer::PruneColumns(PlanNodePtr plan) {
    // TODO: 递归遍历计划树，分析ProjectNode实际需要的列，
    // 消除多余的中间列计算
    return plan;
}

ExprPtr Optimizer::FoldConstants(ExprPtr expr) {
    // TODO: 递归遍历表达式树，若子表达式均为常量则直接计算出结果，
    // 替换为对应的LiteralExpr
    return expr;
}

bool Optimizer::IsConstantExpr(const ExprPtr& expr) const {
    // TODO: 判断表达式是否不包含ColumnRefExpr（即是否为常量表达式）
    return false;
}

}  // namespace sqlcompiler
