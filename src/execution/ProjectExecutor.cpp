#include "execution/ProjectExecutor.h"

#include "execution/ExpressionEvaluator.h"

namespace sqlcompiler {

ProjectExecutor::ProjectExecutor(ExecutionContext* context, ExecutorPtr child,
                                  std::vector<ExprPtr> select_list,
                                  std::unordered_map<std::string, size_t> column_index_map)
    : Executor(context),
      child_(std::move(child)),
      select_list_(std::move(select_list)),
      column_index_map_(std::move(column_index_map)),
      has_emitted_(false) {
}

void ProjectExecutor::Init() {
    has_emitted_ = false;
    if (child_) child_->Init();
}

bool ProjectExecutor::Next(Tuple* tuple) {
    // 无 FROM（SELECT 1 / SELECT 'label'）：发射一行常量然后结束
    if (!child_) {
        if (has_emitted_) return false;
        has_emitted_ = true;
        if (!tuple) return true;
        ExpressionEvaluator eval(column_index_map_);
        std::vector<Value> values;
        values.reserve(select_list_.size());
        for (const auto& e : select_list_) {
            if (e) values.push_back(eval.Evaluate(e, Tuple()));
            else values.push_back(Value::MakeNull());
        }
        *tuple = Tuple(std::move(values));
        return true;
    }
    Tuple in;
    if (!child_->Next(&in)) return false;
    if (!tuple) return true;
    // Handle STAR: pass-through
    if (select_list_.size() == 1 &&
        select_list_[0]->GetType() == NodeType::FUNCTION_CALL_EXPR) {
        auto fc = std::static_pointer_cast<FunctionCallExpr>(select_list_[0]);
        if (fc->function_name == "*" || fc->function_name == "STAR") {
            *tuple = in;
            return true;
        }
    }
    ExpressionEvaluator eval(column_index_map_);
    std::vector<Value> values;
    values.reserve(select_list_.size());
    for (const auto& e : select_list_) {
        values.push_back(eval.Evaluate(e, in));
    }
    *tuple = Tuple(std::move(values));
    return true;
}

}  // namespace sqlcompiler