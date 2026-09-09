#include "optimizer/Optimizer.h"

namespace sqlcompiler {

Optimizer::Optimizer() {
}

PlanNodePtr Optimizer::Optimize(PlanNodePtr plan) {
    plan = PushDownPredicates(plan);
    plan = PruneColumns(plan);
    return plan;
}

PlanNodePtr Optimizer::PushDownPredicates(PlanNodePtr plan) {
    if (!plan) return plan;
    // Recurse into children first
    for (auto& c : plan->children) {
        c = PushDownPredicates(c);
    }
    return plan;
}

PlanNodePtr Optimizer::PruneColumns(PlanNodePtr plan) {
    (void)plan;
    return plan;
}

ExprPtr Optimizer::FoldConstants(ExprPtr expr) {
    return expr;
}

bool Optimizer::IsConstantExpr(const ExprPtr& expr) const {
    (void)expr;
    return false;
}

}  // namespace sqlcompiler