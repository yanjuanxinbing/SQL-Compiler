#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/AST.h"
#include "catalog/SystemCatalog.h"
#include "execution/Executor.h"
#include "storage_engine/Value.h"

namespace sqlcompiler {

// ============ 59_procs (Category 8)：过程语言扩展语义文档 ============
//
// 本执行器（UdfExecutor）被两个调用方共享：
//   - 函数调用：SELECT func(args); → ExpressionEvaluator 构造 UdfExecutor，
//     调用 Run() 拿到返回值。
//   - 过程调用：CALL proc(args); → CallExecutor 构造 UdfExecutor，
//     调用 RunProcedure() 执行 body。
//
// === LOOP / REPEAT / CASE（体内）===
//
// LOOP body END LOOP; —— 无限循环，由 LEAVE 退出；最外层解释循环有
// 10000 次硬性上限（与 WHILE 一致）。
//
// REPEAT body UNTIL cond END REPEAT; —— 至少执行 body 一次；每次循环末尾
// 评估 cond：truthy 即退出。cond 由 SQL 三值逻辑解释。
//
// CASE WHEN ... THEN stmts ... ELSE stmts END CASE;
//   - 简单 CASE（subject 非空）：当 subject = when_expr 为 TRUE 时执行
//     对应 body；否则继续。比较按 SQL 比较运算符 =，NULL/FALSE 视为不匹配。
//   - 搜索式 CASE（subject 空）：评估每个 when_expr 谓词，TRUE 即匹配。
// 没有匹配且无 ELSE 时不执行任何语句（不抛错）。
//
// === LEAVE / ITERATE（label）===
//
// 维护一个 label 栈：进入带 label 的 LOOP/WHILE/REPEAT 时压栈，
// 退出时弹栈。LEAVE label 弹出栈直到找到匹配 label，并设置
// frame.control = LEAVE；ITERATE label 类似，设置为 ITERATE。
//
// frame.control 在 ExecuteStatement 末尾被检查：LEAVE 让当前循环立即
// 跳出（return），ITERATE 让当前循环立即跳到下一次迭代。
//
// === SIGNAL SQLSTATE 'XXXXX' SET MESSAGE_TEXT = '...' ===
//
// 抛出一个特殊的 std::runtime_error：
//   "SIGNAL SQLSTATE 'XXXXX' MESSAGE 'msg'"。
// UdfExecutor::ExecuteStatement 在每条语句周围用 try/catch 包裹，
// 触发时遍历当前 frame 的 handler 栈，找到匹配则执行 body 并
// 继续；否则异常向上抛。
//
// === DECLARE ... HANDLER FOR ... ===
//
// 注册一个异常处理器到当前 frame 的 handlers_ 列表。
//   - type == CONTINUE：handler body 执行后继续执行触发语句之后。
//   - type == EXIT：未实现（执行器抛 "not supported"，保留语法可解析）。
//   - type == UNDO：未实现（同上）。
//
// condition 匹配规则：
//   - SQLEXCEPTION / SQLWARNING / NOT_FOUND：视为匹配任何 RuntimeError
//     （V1 简化：三类都捕获）。
//   - SQLSTATE 'XXXXX'：仅当异常的 SQLSTATE 等于此值时匹配。
//
// === DECLARE name CURSOR FOR <select> / OPEN / FETCH / CLOSE ===
//
// cursor state = { query_plan（SELECT 编译后的计划）, materialized_rows,
//                 current_index, is_open }。
//
// OPEN 把 query 编译为 PlanNodePtr，调用 ExecutionEngine::ExecuteSubplan
// 一次性收集所有 Tuple（V1 简化：materialize 全部结果）。
//
// FETCH cur INTO v1, v2, ...; —— 取当前行的各列依次赋给 v1, v2, ...
// 每次调用前移 cursor.current_index；超出行数时把 into_vars 设为 NULL。
// 当前 V1 没有 NOT FOUND 自动异常；用户需自行判 NULL。
//
// CLOSE cur; —— 把 is_open 置 false、释放 materialized_rows（V1 仅清空）。
//
// === OUT / INOUT 参数 ===
//
// V1 简化：procedure 参数若标记为 OUT 或 INOUT，UdfExecutor::RunProcedure
// 在结束时把 frame.locals 中参数名对应的值写入 ExecutionContext::out_args_
// 字典。INOUT 在调用方先求值再传入；OUT 调用前为 NULL。
//
// 由于 OUT 写入 out_args_，下游 SQL 必须通过专用 SELECT 函数（V1 暂未实现）
// 或持有表方式读取结果。当前 59_procs.sql 测试使用持有表方式：
//   procedure 把 OUT 值 INSERT 到某张表；调用方 SELECT 该表。

// 调用用户自定义函数 (UDF) 时使用的栈帧：
//   - locals 把"形参名 → 实参值"和"局部变量名 → 当前值"统一管理；
//   - done 标记在 RETURN 执行后被置位，调用方据此停止遍历 body_statements。
//   - control 用于 LEAVE / ITERATE 跳出当前循环，由 ExecuteStatement 在
//     循环入口压栈时检查，循环退出时清空。
//   - cursors / handlers 在 DECLARE 时填充；handler 匹配 SIGNAL / RUNTIME
//     异常时使用。
// 该结构只在 UdfExecutor 内部使用，不暴露给其他算子。
struct FunctionFrame {
    // 参数与局部变量统一存放在同一 map 中（命名空间不冲突时这是 OK 的，
    // 因为 SQL 的 DECLARE 与参数不允许同名，CREATE FUNCTION 在同一帧里也
    // 不允许 DECLARE 两次同名）。键 = 变量名（与 NEW.col / OLD.col 不同）。
    std::unordered_map<std::string, Value> locals;
    bool done = false;
    Value result = Value::MakeNull();

    // 59_procs: 控制流状态。每个循环语句执行前压栈、执行后弹栈；
    // 循环内部 LEAVE/ITERATE 时设置；ExecuteStatement 在每条语句后
    // 检查 control 并据此决定是否中断循环。
    enum class Control { NONE, LEAVE, ITERATE };
    std::vector<std::pair<std::string, Control*>> label_stack;  // (label, &ctl)
    Control control = Control::NONE;

    // 59_procs: 异常处理器。当前 frame 注册的 DECLARE HANDLER 全部压栈，
    // SIGNAL/异常时按 LIFO 顺序遍历，找到第一个匹配的 handler 后执行。
    struct HandlerEntry {
        DeclareHandlerStatement::Type type;
        DeclareHandlerStatement::CondKind cond_kind;
        std::string cond_sqlstate;  // 仅 SQLSTATE 条件时使用
        StatementPtr body;
    };
    std::vector<HandlerEntry> handlers;

    // 59_procs: cursor 表。每个 cursor 是 (query AST, 物化行缓冲, 当前
    // 索引, 是否 OPEN)。DECLARE 时填充 query；OPEN 时再 plan + 物化。
    struct CursorEntry {
        SelectStatementPtr query;        // 原始 AST
        PlanNodePtr plan;                // 计划（OPEN 时填入）
        std::vector<Tuple> rows;         // OPEN 时一次性物化
        size_t index = 0;                // FETCH 推进的位置
        bool is_open = false;
    };
    std::unordered_map<std::string, CursorEntry> cursors;
};

// UDF 执行器：调用 ExpressionEvaluator::EvaluateFunctionCall 时构造一个
// UdfExecutor 实例，调用 Run() 即可在 body_statements 上跑一遍；
// 过程调用则由 CallStatement 走 RunProcedure()。
//
// 重要约定：
//   - body_statements 中的 DECLARE 必须在 SET 之前引用该变量。
//     我们不在 parser / semantic 强制这一点，但执行期会先创建条目为 NULL，
//     再在 SET 时更新。
//   - IF / WHILE / LOOP 的条件按 SQL 三值逻辑解释：TRUE 真值；FALSE/NULL 视为假。
//   - RETURN expr 求值后立即终止（设置 frame.done 并保存 result）。
//   - 若 body_statements 走完没有 RETURN，函数抛 RUNTIME 异常
//     "runtime error: function did not return a value"；过程则视为成功返回。
class UdfExecutor {
public:
    // 函数调用入口。
    UdfExecutor(SystemCatalog* catalog,
                ExecutionContext* context,
                const SystemCatalog::FunctionDefinition& fn,
                std::unordered_map<std::string, Value> arg_bind);

    // 过程调用入口：仅依赖 catalog + context，不绑定任何 FunctionDefinition。
    explicit UdfExecutor(SystemCatalog* catalog,
                         ExecutionContext* context);

    // 过程调用入口：parameters 可携带 mode（IN/OUT/INOUT），执行结束后
    // 把 mode != IN 的形参写入 context->out_args_。
    // 返回过程中是否正常完成（false 表示 SIGNAL 等导致异常向上抛）。
    bool RunProcedure(const SystemCatalog::ProcedureDefinition& proc,
                      std::unordered_map<std::string, Value> arg_bind);

    // 执行 fn.body_statements，返回值。执行期间抛出 CompilerException 时
    // 会原样上抛。
    Value Run();

private:
    // 逐条语句解释执行。遇到 RETURN 则提前终止。
    // SIGNAL / RUNTIME 异常被 catch 后遍历 frame.handlers_ 找匹配 handler；
    // 找到则执行 body 并继续；否则重新抛出。
    void ExecuteStatement(const StatementPtr& stmt, FunctionFrame& frame);
    // 表达式求值辅助：locals 注入到 outer_bind。
    Value EvaluateExpr(const ExprPtr& expr, const FunctionFrame& frame,
                       const Tuple& tuple) const;

    // 59_procs: 检查一条 SIGNAL/RuntimeError 是否匹配某个 handler，
    // 匹配则返回 true 并填充 out 字段。
    static bool MatchesHandler(const DeclareHandlerStatement::CondKind kind,
                               const std::string& cond_sqlstate,
                               const std::string& err_sqlstate);
    // 抛出一个携带 SQLSTATE/MESSAGE 的 RuntimeError，由 SIGNAL 或
    // 通用错误转换得到。
    [[noreturn]] static void RaiseRuntimeError(const std::string& sqlstate,
                                               const std::string& message);

    SystemCatalog* catalog_;
    ExecutionContext* context_;
    // 函数定义引用（Run() 用）；RunProcedure 时不入此字段。
    const SystemCatalog::FunctionDefinition* fn_ = nullptr;
    // 形参名 → 实参值；构造时拷贝。
    std::unordered_map<std::string, Value> initial_args_;
    // 当前正在运行的 procedure 参数表（RunProcedure 用）。
    std::vector<FunctionParameter> proc_params_;
};

}  // namespace sqlcompiler
