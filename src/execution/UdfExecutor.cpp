#include "execution/UdfExecutor.h"

#include "common/Error.h"
#include "execution/ExpressionEvaluator.h"

namespace sqlcompiler {

namespace {

// 解释表达式的真值。SQL 三值逻辑：仅当 IsTrue（非 NULL 且为 truthy）时返回 true；
// NULL 与 false / 0 都视为 false。
bool IsTruthyValue(const Value& v) {
    if (v.IsNull()) return false;
    switch (v.GetType()) {
        case ValueType::INTEGER: return v.AsInt() != 0;
        case ValueType::FLOAT:   return v.AsFloat() != 0.0;
        case ValueType::VARCHAR: return !v.AsVarchar().empty();
        case ValueType::NULL_TYPE: return false;
    }
    return false;
}

}  // namespace

UdfExecutor::UdfExecutor(SystemCatalog* catalog,
                         ExecutionContext* context,
                         const SystemCatalog::FunctionDefinition& fn,
                         std::unordered_map<std::string, Value> arg_bind)
    : catalog_(catalog), context_(context), fn_(fn),
      initial_args_(std::move(arg_bind)) {
}

Value UdfExecutor::EvaluateExpr(const ExprPtr& expr, const FunctionFrame& frame,
                                const Tuple& tuple) const {
    // 用空 column_index_map + 当前 locals 作 outer_bind。
    // column_index_map 为空意味着 ColumnRefExpr 走 outer_bind 路径。
    std::unordered_map<std::string, size_t> empty_cmap;
    ExpressionEvaluator inner(empty_cmap, context_, &frame.locals);
    return inner.Evaluate(expr, tuple);
}

Value UdfExecutor::Run() {
    FunctionFrame frame;
    // 把形参值拷到 locals 中，后续 SET 也能改写形参引用（同 SQL/PSM 行为）。
    for (const auto& kv : initial_args_) {
        frame.locals[kv.first] = kv.second;
    }
    for (const auto& s : fn_.body_statements) {
        if (!s) continue;
        if (frame.done) break;
        ExecuteStatement(s, frame);
    }
    if (!frame.done) {
        throw CompilerException(ErrorStage::RUNTIME,
            "runtime error: function did not return a value");
    }
    return frame.result;
}

void UdfExecutor::ExecuteStatement(const StatementPtr& stmt, FunctionFrame& frame) {
    switch (stmt->GetType()) {
        case NodeType::RETURN_STMT: {
            const auto& rs = static_cast<const ReturnStatement&>(*stmt);
            if (rs.expr) {
                Value v = EvaluateExpr(rs.expr, frame, Tuple());
                frame.result = v;
            } else {
                frame.result = Value::MakeNull();
            }
            frame.done = true;
            return;
        }
        case NodeType::DECLARE_VAR_STMT: {
            const auto& dv = static_cast<const DeclareVarStatement&>(*stmt);
            // 若重复声明，覆盖为 NULL（与标准 SQL 行为对齐）。
            frame.locals[dv.var_name] = Value::MakeNull();
            return;
        }
        case NodeType::SET_VAR_STMT: {
            const auto& sv = static_cast<const SetVarStatement&>(*stmt);
            // SET target = expr
            //   - 若 target 是 "name"，写入 frame.locals[name]。
            //   - 若 target 是 "NEW.col" / "OLD.col"，调用方应使用 TriggerExecutor，
            //     这里只做"frame 里已有 NEW.col/OLD.col"的就地更新（兼容 UDF 内
            //     出现 NEW.col 引用——理论上不会被触发，但保留容错）。
            Value v = EvaluateExpr(sv.expr, frame, Tuple());
            // 如果 target 是 NEW.col / OLD.col，键直接照搬到 frame.locals。
            // 触发器路径下 trigger 算子维护自己的 frame；这里只是写回同名键。
            frame.locals[sv.target] = v;
            return;
        }
        case NodeType::IF_STMT: {
            const auto& ifs = static_cast<const IfStatement&>(*stmt);
            Value cond = EvaluateExpr(ifs.condition, frame, Tuple());
            if (IsTruthyValue(cond)) {
                for (const auto& s : ifs.then_body) {
                    if (frame.done) return;
                    if (s) ExecuteStatement(s, frame);
                }
                return;
            }
            for (const auto& ec : ifs.elseif_clauses) {
                Value v = EvaluateExpr(ec.condition, frame, Tuple());
                if (IsTruthyValue(v)) {
                    for (const auto& s : ec.body) {
                        if (frame.done) return;
                        if (s) ExecuteStatement(s, frame);
                    }
                    return;
                }
            }
            for (const auto& s : ifs.else_body) {
                if (frame.done) return;
                if (s) ExecuteStatement(s, frame);
            }
            return;
        }
        case NodeType::WHILE_STMT: {
            const auto& ws = static_cast<const WhileStatement&>(*stmt);
            // 最多 10000 次迭代以避免无限循环；这是 UDF 内部的硬性安全网。
            const size_t kMaxIter = 10000;
            for (size_t iter = 0; iter < kMaxIter; ++iter) {
                Value cond = EvaluateExpr(ws.condition, frame, Tuple());
                if (!IsTruthyValue(cond)) break;
                for (const auto& s : ws.body) {
                    if (frame.done) return;
                    if (s) ExecuteStatement(s, frame);
                }
                if (frame.done) return;
            }
            return;
        }
        default:
            throw CompilerException(ErrorStage::RUNTIME,
                "unsupported statement inside function body");
    }
}

}  // namespace sqlcompiler