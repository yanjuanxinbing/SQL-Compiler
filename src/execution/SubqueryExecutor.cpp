#include "execution/SubqueryExecutor.h"

namespace sqlcompiler {

SubqueryExecutor::SubqueryExecutor(ExecutionContext* context, SubqueryNode* node)
    : Executor(context), node_(node) {
}

void SubqueryExecutor::Init() {
    // no-op：子查询的真实执行由 ExpressionEvaluator 触发，
    // 这里只占位以满足 SubqueryNode 在计划树中出现时的 BuildExecutor 分支。
}

bool SubqueryExecutor::Next(Tuple* /*tuple*/) {
    // 不主动产出元组：SubqueryNode 应被外层 Project/Filter 在表达式中消费。
    return false;
}

}  // namespace sqlcompiler