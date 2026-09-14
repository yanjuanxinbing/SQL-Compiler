#include "execution/CallExecutor.h"

#include "common/Error.h"
#include "execution/ExpressionEvaluator.h"
#include "execution/UdfExecutor.h"

namespace sqlcompiler {

namespace {

// 71_proc_out_params：判断表达式是否为 "@<name>" 形式的 ColumnRefExpr。
// 若 expr 为非空 ColumnRefExpr 且 column_name 以 '@' 开头，返回 session var 名（去掉前缀）。
// 否则返回空字符串 —— 调用方应据此判断是否需要报错。
std::string ExtractSessionVarName(const ExprPtr& expr) {
    if (!expr || expr->GetType() != NodeType::COLUMN_REF_EXPR) return "";
    const auto& cr = static_cast<const ColumnRefExpr&>(*expr);
    if (cr.table_name.empty() && !cr.column_name.empty() &&
        cr.column_name.front() == '@') {
        return std::string(cr.column_name.begin() + 1, cr.column_name.end());
    }
    return "";
}

}  // namespace

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
    // 71_proc_out_params：先按形参 mode 校验实参是否为 @var，并构造绑定表。
    // OUT 形参位置上必须传 @var（MySQL 语义），IN/INOUT 接受任何表达式。
    // 绑定表形参名 → session var 名，传给 UdfExecutor::RunProcedure 用于把
    // 形参最终值复制到 session_vars_。
    std::unordered_map<std::string, std::string> out_arg_session_map;
    for (size_t i = 0; i < proc->parameters.size(); ++i) {
        const auto& p = proc->parameters[i];
        if (p.mode == 0) continue;  // IN：跳过校验
        std::string sv = ExtractSessionVarName(node_->arguments[i]);
        if (sv.empty()) {
            throw CompilerException(ErrorStage::RUNTIME,
                "runtime error: procedure '" + node_->procedure_name +
                "' argument " + std::to_string(i + 1) + " ('" + p.name +
                "') is " + (p.mode == 1 ? "OUT" : "INOUT") +
                " and must be a session variable (@name)");
        }
        out_arg_session_map[p.name] = std::move(sv);
    }
    // 对每个实参 EvaluateExpr 求值（空 column_index_map）。
    std::unordered_map<std::string, size_t> empty_cmap;
    ExpressionEvaluator eval(empty_cmap, context_, /*outer_bind=*/nullptr);
    std::unordered_map<std::string, Value> arg_bind;
    for (size_t i = 0; i < node_->arguments.size(); ++i) {
        const auto& p = proc->parameters[i];
        // OUT 形参按 MySQL 语义：忽略调用方传入的初值，统一以 NULL 启动。
        // 这样 procedure 体内直接 SELECT total 不会读到调用方无意传入的值。
        if (p.mode == 1) {
            arg_bind[p.name] = Value::MakeNull();
            continue;
        }
        Value v = eval.Evaluate(node_->arguments[i], Tuple());
        arg_bind[p.name] = std::move(v);
    }
    // 走 UdfExecutor 解释器执行 body。
    UdfExecutor exec(catalog, context_);
    exec.RunProcedure(*proc, std::move(arg_bind),
                      std::move(out_arg_session_map));
}

bool CallExecutor::Next(Tuple* /*tuple*/) {
    return false;
}

}  // namespace sqlcompiler