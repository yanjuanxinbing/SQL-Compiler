#pragma once

#include <cstddef>
#include <limits>
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
// 注册一个异常处理器到当前 frame 的 handlers_ 列表。type 决定 handler body
// 执行完之后的 unwind 行为：
//   - CONTINUE：handler body 执行后继续执行触发语句之后的下一条语句。
//   - EXIT：    handler body 执行后离开 handler 声明所在 block（procedure
//               隐式 BEGIN 或最近的 IF/WHILE/LOOP/REPEAT/CASE body），
//               相当于在该 block 末尾做了一次 "LEAVE block"。
//   - UNDO：    与 EXIT 类似，但在 unwind 之前对该 block 进入时分配的
//               SAVEPOINT 做 ROLLBACK TO；若无显式事务则退化为 EXIT。
//
// condition 匹配规则：
//   - SQLEXCEPTION / SQLWARNING / NOT_FOUND：视为匹配任何 RuntimeError
//     （V1 简化：三类都捕获）。
//   - SQLSTATE 'XXXXX'：仅当异常的 SQLSTATE 等于此值时匹配。
//
// 优先级（与 MySQL/MariaDB 对齐）：
//   1) 更具体的 condition 胜出（SQLSTATE > class）。
//   2) 同 specificity 下，最近声明的 handler 胜出（LIFO）。
//   3) UNDO > EXIT > CONTINUE 优先级（仅在 block 退出语义上有区别）。
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
// 71_proc_out_params 后：procedure 输出参数通过两种机制回传给调用方：
//   1) context->out_args_ 字典（保留兼容，持有表形式仍可用）；
//   2) session_variables 表：CallExecutor 把 OUT/INOUT 实参的 @var 名
//      绑定到对应形参，RunProcedure 结束后把 frame.locals[param] 的最终值
//      同步到 ExecutionContext::session_vars_[sv_name]，调用方在 CALL
//      后立即 SELECT @var 即可读取。
// SET @var = expr 在 procedure 体内直接写 session_vars_，不经过 frame.locals；
// 即 SET 会话变量在 procedure 调用结束后对调用方可见。

// 调用用户自定义函数 (UDF) 时使用的栈帧：
//   - locals 把"形参名 → 实参值"和"局部变量名 → 当前值"统一管理；
//   - done 标记在 RETURN 执行后被置位，调用方据此停止遍历 body_statements。
//   - control 用于 LEAVE / ITERATE 跳出当前循环，由 ExecuteStatement 在
//     循环入口压栈时检查，循环退出时清空。
//   - cursors / handlers 在 DECLARE 时填充；handler 匹配 SIGNAL / RUNTIME
//     异常时使用。
//   - block_stack / pending_exit_block_id / pending_undo 用于 EXIT / UNDO
//     HANDLER 的块边界 unwind：block_stack 跟踪嵌套 BEGIN-block（含 procedure
//     隐式块、IF/WHILE/LOOP/REPEAT/CASE body），每个 block 拥有唯一 id；
//     异常匹配到 EXIT / UNDO handler 时把"目标 block id"写入
//     pending_exit_block_id，外层 block 在自己的 iteration 末尾检查并退出。
//     pending_undo 标记 UNDO handler 触发的回滚请求，block 退出前先
//     ROLLBACK TO SAVEPOINT 再 RELEASE SAVEPOINT。
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
    // 每个 handler 携带其声明所在 block 的 id（block_id >= 0）。当异常
    // 命中 EXIT/UNDO handler 时，把 block_id 写入 pending_exit_block_id；
    // 外层 block 在 iteration 末尾检查并按需退出 / 回滚。
    struct HandlerEntry {
        DeclareHandlerStatement::Type type;
        DeclareHandlerStatement::CondKind cond_kind;
        std::string cond_sqlstate;  // 仅 SQLSTATE 条件时使用
        StatementPtr body;
        // 声明所在 block 的 id。-1 = procedure/function 顶层隐式 block。
        // 退出时只 unwind 到该 id 为止，不影响外层 block。
        int block_id = -1;
    };
    std::vector<HandlerEntry> handlers;

    // 68_proc_handlers: 嵌套 block 栈。每个 block 对应一个 BEGIN...END
    // 边界：procedure/function 顶层（隐式 BEGIN）以及 IF/WHILE/LOOP/REPEAT/
    // CASE body（嵌套 BEGIN）。id 单调递增；savepoint_name 用于 UNDO 的
    // 块级 SAVEPOINT / ROLLBACK TO / RELEASE。仅当 block 进入时显式事务
    // 已激活才分配 savepoint（auto-commit 下 UNDO 退化为 EXIT）。
    struct BlockFrame {
        int id = -1;
        std::string label;
        std::string savepoint_name;
        bool savepoint_active = false;  // false 表示未分配（无 txn 时）
    };
    std::vector<BlockFrame> block_stack;
    int next_block_id_ = 0;
    // EXIT / UNDO handler 命中时写入 pending_exit_block_id；-1 表示无。
    // pending_undo == true 时，匹配到的 block 退出前先 ROLLBACK TO。
    int pending_exit_block_id = -1;
    bool pending_undo = false;
    // 标记 UNDO / EXIT 触发的"块级 unwind"是否已完成 unwind。某些路径
    // （如顶层 procedure body 末尾）需要检查：若已完成 unwind 即可跳过
    // 后续语句，但同时需要保留 done / control 等原有标志的语义。
    bool block_unwound = false;

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
    // 71_proc_out_params：可选 out_arg_session_map 把 OUT/INOUT 形参的最终
    // 值进一步同步到 session_vars_[sv_name]，让 CALL 后立即 SELECT @var 可见。
    // 返回过程中是否正常完成（false 表示 SIGNAL 等导致异常向上抛）。
    bool RunProcedure(
        const SystemCatalog::ProcedureDefinition& proc,
        std::unordered_map<std::string, Value> arg_bind,
        std::unordered_map<std::string, std::string> out_arg_session_map = {});

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
    // 68_proc_handlers: 在 frame.handlers 中按"specificity 优先 +
    // 最近声明优先 + UNDO>EXIT>CONTINUE"规则选最佳匹配。返回下标；
    // 若无匹配返回 (size_t)-1。
    static size_t SelectBestHandler(
        const std::vector<FunctionFrame::HandlerEntry>& handlers,
        const std::string& err_sqlstate);
    // 抛出一个携带 SQLSTATE/MESSAGE 的 RuntimeError，由 SIGNAL 或
    // 通用错误转换得到。
    [[noreturn]] static void RaiseRuntimeError(const std::string& sqlstate,
                                               const std::string& message);

    // ---- 68_proc_handlers: EXIT / UNDO HANDLER 块边界支撑 ----
    // 进入一个 BEGIN-block：分配唯一 id，若当前有显式事务则分配同名
    // SAVEPOINT（auto-commit 下不分配，UNDO 退化为 EXIT）。返回新 block 的 id。
    int EnterBlock(FunctionFrame& frame, const std::string& label);
    // block 正常结束（无 EXIT/UNDO 触发）：若分配过 savepoint 则 RELEASE。
    // 同时清除 frame.pending_exit_block_id / pending_undo，避免污染外层。
    void ExitBlockNormally(FunctionFrame& frame);
    // block 因 EXIT/UNDO 触发而退出：若 pending_undo 则先 ROLLBACK TO，
    // 再 RELEASE SAVEPOINT；调用方负责在调用后传播 pending_exit_block_id。
    void ExitBlockWithControl(FunctionFrame& frame, bool undo);
    // 在 body 退出前调用：根据 frame.pending_exit_block_id 决定是否
    // 触发本 block 的 unwind。返回 true 表示调用方应立即 return（pending
    // 标记指向外层 block，需向上传播）。具体行为：
    //   - 无 pending：pop 本 block，无 rollback（正常退出）。
    //   - pending 指向本 block：pop 本 block，undo 时 rollback，并清标记。
    //   - pending 指向外层 block：pop 本 block（无 rollback），保留标记
    //     供外层 block 处理，返回 true 让调用方向上 propagate。
    bool FinalizeBlockExit(FunctionFrame& frame);

    // 把一条 SQLSTATE 字符串规范化（去掉外层单引号）。
    static std::string NormalizeSqlstate(const std::string& s);

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
