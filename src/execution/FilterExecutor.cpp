#include "execution/FilterExecutor.h"

#include "execution/ExpressionEvaluator.h"

#include <cstdio>

namespace sqlcompiler {

FilterExecutor::FilterExecutor(ExecutionContext* context, ExecutorPtr child, ExprPtr predicate,
                               std::unordered_map<std::string, size_t> column_index_map)
    : Executor(context),
      child_(std::move(child)),
      predicate_(std::move(predicate)),
      column_index_map_(std::move(column_index_map)) {
}

void FilterExecutor::Init() {
    if (child_) child_->Init();
}

bool FilterExecutor::Next(Tuple* tuple) {
    if (!child_) return false;
    Tuple tmp;
    while (child_->Next(&tmp)) {
        ExpressionEvaluator eval(column_index_map_);
        Value v = eval.Evaluate(predicate_, tmp);
        fprintf(stderr, "[FLT] res=%d\n", v.IsNull() ? -1 : (int)v.AsInt());
        if (!v.IsNull() && v.AsInt() != 0) {
            if (tuple) *tuple = tmp;
            return true;
        }
    }
    return false;
}

}  // namespace sqlcompiler