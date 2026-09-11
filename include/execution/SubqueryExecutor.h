#pragma once

#include "execution/Executor.h"
#include "plan/Plan.h"

namespace sqlcompiler {

// SubqueryExecutor：在父算子的表达式求值器无法直接拿到 ExecutionContext 时，
// 作为 SubqueryNode 的执行体——把子计划跑完并把结果收集到 std::vector<Tuple>，
// 由 ExpressionEvaluator::EvaluateSubquery 借助 ExecutionEngine 间接驱动。
// 实际上由于 ExpressionEvaluator 已直接用 ExecutionEngine 跑子计划，
// 这个执行器只在 SubqueryNode 出现在「主计划」树根时才有用（例如
// SELECT (SELECT ...) ... — Project 包裹的子查询）。当前实现以行为单位
// 调用 children[0] 一次并返回 false，让 Project 自行完成。
class SubqueryExecutor : public Executor {
public:
    SubqueryExecutor(ExecutionContext* context, SubqueryNode* node);
    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    SubqueryNode* node_;
};

}  // namespace sqlcompiler