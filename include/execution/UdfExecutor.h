#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "ast/AST.h"
#include "catalog/SystemCatalog.h"
#include "execution/Executor.h"
#include "storage_engine/Value.h"

namespace sqlcompiler {

// 调用用户自定义函数 (UDF) 时使用的栈帧：
//   - locals 把"形参名 → 实参值"和"局部变量名 → 当前值"统一管理；
//   - done 标记在 RETURN 执行后被置位，调用方据此停止遍历 body_statements。
// 该结构只在 UdfExecutor 内部使用，不暴露给其他算子。
struct FunctionFrame {
    // 参数与局部变量统一存放在同一 map 中（命名空间不冲突时这是 OK 的，
    // 因为 SQL 的 DECLARE 与参数不允许同名，CREATE FUNCTION 在同一帧里也
    // 不允许 DECLARE 两次同名）。键 = 变量名（与 NEW.col / OLD.col 不同）。
    std::unordered_map<std::string, Value> locals;
    bool done = false;
    Value result = Value::MakeNull();
};

// UDF 执行器：调用 ExpressionEvaluator::EvaluateFunctionCall 时构造一个
// UdfExecutor 实例，调用 Run(...) 即可在 body_statements 上跑一遍。
//
// 重要约定：
//   - body_statements 中的 DECLARE 必须在 SET 之前引用该变量。
//     我们不在 parser / semantic 强制这一点，但执行期会先创建条目为 NULL，
//     再在 SET 时更新。
//   - IF / WHILE 的条件按 SQL 三值逻辑解释：TRUE 真值；FALSE/NULL 视为假。
//   - RETURN expr 求值后立即终止（设置 frame.done 并保存 result）。
//   - 若 body_statements 走完没有 RETURN，抛出 RUNTIME 异常
//     "runtime error: function did not return a value"。
class UdfExecutor {
public:
    // 形参名 → 实参值。
    UdfExecutor(SystemCatalog* catalog,
                ExecutionContext* context,
                const SystemCatalog::FunctionDefinition& fn,
                std::unordered_map<std::string, Value> arg_bind);

    // 执行 fn.body_statements，返回值。执行期间抛出 CompilerException 时
    // 会原样上抛。
    Value Run();

private:
    // 逐条语句解释执行。遇到 RETURN 则提前终止。
    void ExecuteStatement(const StatementPtr& stmt, FunctionFrame& frame);
    // 表达式求值辅助：locals 注入到 outer_bind。
    Value EvaluateExpr(const ExprPtr& expr, const FunctionFrame& frame,
                       const Tuple& tuple) const;

    SystemCatalog* catalog_;
    ExecutionContext* context_;
    const SystemCatalog::FunctionDefinition& fn_;
    std::unordered_map<std::string, Value> initial_args_;
};

}  // namespace sqlcompiler