#include "execution/FilterExecutor.h"

#include "execution/ExpressionEvaluator.h"

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
        ExpressionEvaluator eval(column_index_map_, context_, nullptr);
        Value v = eval.Evaluate(predicate_, tmp);
        // IsTruthy() 正确处理 NULL / INTEGER / FLOAT / VARCHAR 四种类型的
        // 谓词结果；旧实现直接 v.AsInt() != 0 对 VARCHAR / FLOAT 会读到
        // 无关字段（int_val_），是把 "true" 之类的字符串当作 0 而漏过行的根因。
        if (v.IsTruthy()) {
            if (tuple) *tuple = tmp;
            return true;
        }
    }
    return false;
}

}  // namespace sqlcompiler