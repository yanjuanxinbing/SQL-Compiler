#include "optimizer/Optimizer.h"

#include "optimizer/IndexAccessPath.h"

namespace sqlcompiler {

Optimizer::Optimizer(SystemCatalog* catalog) : catalog_(catalog) {
}

PlanNodePtr Optimizer::Optimize(PlanNodePtr plan) {
    plan = PushDownPredicates(plan);
    plan = ChooseAccessPaths(plan);
    plan = PruneColumns(plan);
    return plan;
}

PlanNodePtr Optimizer::ChooseAccessPaths(PlanNodePtr plan) {
    if (!plan || catalog_ == nullptr) return plan;
    for (auto& c : plan->children) {
        c = ChooseAccessPaths(c);
    }
    // 只处理 Filter 直接盖在 SeqScan 上的形态。JOIN 下的扫描暂不改写：
    // 连接谓词涉及两侧的列，不能当作常量区间。
    if (plan->GetType() != PlanNodeType::FILTER) return plan;
    if (plan->children.size() != 1) return plan;
    if (plan->children[0]->GetType() != PlanNodeType::SEQ_SCAN) return plan;

    auto filter = std::static_pointer_cast<FilterNode>(plan);
    auto scan = std::static_pointer_cast<SeqScanNode>(plan->children[0]);
    PlanNodePtr rewritten = TryRewriteWithIndex(catalog_, scan->table_name,
                                                scan->table_alias,
                                                filter->predicate);
    // 改写不成立时保持原计划：宁可慢，也不能因为改写出错而少返回行
    return rewritten ? rewritten : plan;
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