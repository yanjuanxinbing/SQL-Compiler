#include "execution/CallExecutor.h"

#include "common/Error.h"
#include "execution/ExpressionEvaluator.h"
#include "execution/UdfExecutor.h"

namespace sqlcompiler {

CallExecutor::CallExecutor(ExecutionContext* context, CallNode* node)
    : Executor(context), node_(node) {
}

void CallExecutor::Init() {
    if (executed_) return;
    executed_ = true;
    if (!context_ || !node_) return;
    SystemCatalog* catalog = context_->GetCatalog();
    if (catalog == nullptr) {
        throw CompilerException(ErrorStage::RUNTIME,
            "CALL: no catalog available");
    }
    const SystemCatalog::ProcedureDefinition* proc =
        catalog->LookupProcedure(node_->procedure_name);
    if (proc == nullptr) {
        throw CompilerException(ErrorStage::RUNTIME,
            "runtime error: procedure '" + node_->procedure_name +
            "' does not exist");
    }
    if (proc->parameters.size() != node_->arguments.size()) {
        throw CompilerException(ErrorStage::RUNTIME,
            "runtime error: procedure '" + node_->procedure_name +
            "' expects " + std::to_string(proc->parameters.size()) +
            " argument(s), got " + std::to_string(node_->arguments.size()));
    }
    // 对每个实参 EvaluateExpr 求值（空 column_index_map）。
    std::unordered_map<std::string, size_t> empty_cmap;
    ExpressionEvaluator eval(empty_cmap, context_, /*outer_bind=*/nullptr);
    std::unordered_map<std::string, Value> arg_bind;
    for (size_t i = 0; i < node_->arguments.size(); ++i) {
        Value v = eval.Evaluate(node_->arguments[i], Tuple());
        arg_bind[proc->parameters[i].name] = std::move(v);
    }
    // 走 UdfExecutor 解释器执行 body。
    UdfExecutor exec(catalog, context_);
    exec.RunProcedure(*proc, std::move(arg_bind));
}

bool CallExecutor::Next(Tuple* /*tuple*/) {
    return false;
}

}  // namespace sqlcompiler
