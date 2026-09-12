#pragma once

#include "ast/AST.h"
#include "execution/Executor.h"
#include "plan/Plan.h"

namespace sqlcompiler {

// 59_procs (Category 8): CALL proc(args) 的执行器。
//
// 调用流程：
//   1) 解析 catalog 中 procedure 的参数列表；
//   2) 对每个实参 EvaluateExpr 求值（空 column_index_map 让 ColumnRefExpr
//      走 outer_bind 路径，但 procedure 实参通常都是字面量）；
//   3) 构造 UdfExecutor 并 RunProcedure。
//   4) 过程内部的 OUT / INOUT 参数会被 UdfExecutor 写回 ExecutionContext
//      的 out_args_。
//   5) Next() 始终返回 false——CALL 不产出结果集。
class CallExecutor : public Executor {
public:
    CallExecutor(ExecutionContext* context, CallNode* node);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    CallNode* node_;
    bool executed_ = false;
};

}  // namespace sqlcompiler
