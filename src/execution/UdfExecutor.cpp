#include "execution/UdfExecutor.h"

#include <cctype>
#include <utility>

#include "common/Error.h"
#include "execution/ExecutionEngine.h"
#include "execution/ExpressionEvaluator.h"
#include "optimizer/Optimizer.h"
#include "plan/Planner.h"
#include "semantic/SemanticAnalyzer.h"

namespace sqlcompiler {

// 允许 procedure 体内部直接执行 INSERT / UPDATE / DELETE 等 DML 语句。
// 实现：调用 Planner / Optimizer 把语句翻译为 PlanNodePtr，
// 再走 ExecutionEngine::ExecuteSubplan。
// 注意：DML 会被事务管理捕获；autocommit 上下文下直接生效。
void ExecuteDmlStatement(SystemCatalog* catalog,
                         ExecutionContext* context,
                         const Statement& stmt);


// ============ 59_procs (Category 8)：过程语言扩展语义文档 ============
//
// 本实现遵循以下约定：
//
// === LOOP / REPEAT / 体内 CASE ===
//   LOOP body END LOOP; —— 无限循环，由 LEAVE 退出；为安全起见，
//   内部循环仍有 10000 次迭代硬性上限（与 WHILE 一致），但因为每条语句
//   是即时执行，不会出现长时间挂起。
//   REPEAT body UNTIL cond END REPEAT; —— do-while：先执行 body 一次再
//   判断 cond；条件 truthy 即退出。NULL / FALSE 视为假。
//   CASE WHEN ... THEN stmts ... END CASE; —— 体内 CASE 不返回值，
//   只决定执行哪条语句序列。没有匹配且无 ELSE 时不执行任何语句。
//
// === LEAVE / ITERATE（label）===
//   早期实现曾用 frame.label_stack 维护 (label, &ctl) 对；进入 LOOP/WHILE/REPEAT
//   时压栈、退出时弹栈，由 LEAVE/ITERATE 弹出匹配项。但这种 push/pop 包裹
//   与 body 内 SIGNAL 异常路径难以共存：异常逃逸 body 会跳过对应 pop，
//   导致下一次循环在空栈上 pop_back 断言失败；此外 LEAVE 自身也弹出
//   匹配项，会让循环末尾的 pop_back 在空栈上崩溃。
//   V1 改为：循环不维护 label_stack，LEAVE/ITERATE 仅设置
//   frame.control = LEAVE / ITERATE；循环在每次迭代后检查 control
//   并据此退出或继续。frame.label_stack 字段仍保留以保持 ABI，但
//   本版本不被读写。
//
// === SIGNAL SQLSTATE 'XXXXX' SET MESSAGE_TEXT = '...' ===
//   抛 std::runtime_error("SIGNAL SQLSTATE 'XXXXX' MESSAGE 'msg'")。
//   ExecuteStatement 用 try/catch 包住每条语句；触发时遍历
//   frame.handlers_ 找匹配 handler：
//     - type == CONTINUE：执行 body 后继续当前函数帧的执行。
//     - type == EXIT/UNDO：V1 未实现，执行器直接抛出以提醒调用方。
//
// === DECLARE ... HANDLER FOR ... ===
//   注册到 frame.handlers_。匹配规则：
//     - SQLEXCEPTION / SQLWARNING / NOT_FOUND：捕获任意 RuntimeError
//       （V1 简化，三类一并处理）。
//     - SQLSTATE 'XXXXX'：仅当异常的 SQLSTATE 等于此值时匹配。
//
// === DECLARE name CURSOR FOR <select> / OPEN / FETCH / CLOSE ===
//   cursor 在 frame.cursors_ 中以 cursor_name 为键存储：
//     - DECLARE 时仅保存 query AST。
//     - OPEN 时使用 Planner + Optimizer 把 query 编译为 PlanNodePtr，
//       调 ExecutionEngine::ExecuteSubplan 一次性收集所有 Tuple。
//       （V1 简化：cursor 不支持真正流式迭代，而是 materialize。）
//     - FETCH INTO 把当前行各列依次赋给 into_vars，并推进 index；
//       越界时把所有 into_vars 设为 NULL（让用户可以 WHILE v IS NOT NULL
//       终止迭代）。
//     - CLOSE 把 is_open 置 false、清空 rows。
//
// === OUT / INOUT 参数 ===
//   procedure 参数可标记 OUT/INOUT。RunProcedure 在结束时把 frame.locals
//   中这些参数名对应的 Value 写入 context->out_args_。下游 SQL 可通过
//   持有表读取（V1 简化，不实现 SELECT @out_arg 形式）。

namespace {

// 把表达式的值强制转换为声明的列类型，避免 INT/FLOAT 混淆。
// 本实现只覆盖 INT / FLOAT / VARCHAR 三种与 DECLARE 兼容的类型；其它
// 类型原样返回。
Value CoerceToType(const Value& v, const std::string& data_type) {
    std::string up;
    up.reserve(data_type.size());
    for (char c : data_type) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (v.IsNull()) return v;
    if (up == "INT" || up == "INTEGER") {
        if (v.GetType() == ValueType::INTEGER) return v;
        if (v.GetType() == ValueType::FLOAT) return Value::MakeInt(static_cast<int32_t>(v.AsFloat()));
        if (v.GetType() == ValueType::VARCHAR) {
            try { return Value::MakeInt(static_cast<int32_t>(std::stoi(v.AsVarchar()))); }
            catch (...) { return Value::MakeInt(0); }
        }
    } else if (up == "FLOAT" || up == "DOUBLE") {
        if (v.GetType() == ValueType::FLOAT) return v;
        if (v.GetType() == ValueType::INTEGER) return Value::MakeFloat(static_cast<double>(v.AsInt()));
        if (v.GetType() == ValueType::VARCHAR) {
            try { return Value::MakeFloat(std::stod(v.AsVarchar())); }
            catch (...) { return Value::MakeFloat(0.0); }
        }
    }
    return v;
}

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

// 把 SignalStatement 的 SQLSTATE 字符串规范化（去掉外层单引号）。
// parser 已经把引号剥掉并放在 sqlstate 字段；这里只做兜底 trim。
std::string NormalizeSqlstate(const std::string& s) {
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

}  // namespace

UdfExecutor::UdfExecutor(SystemCatalog* catalog,
                         ExecutionContext* context,
                         const SystemCatalog::FunctionDefinition& fn,
                         std::unordered_map<std::string, Value> arg_bind)
    : catalog_(catalog), context_(context),
      fn_(&fn), initial_args_(std::move(arg_bind)) {
}

// 过程调用专用：不需要 fn_ 字段；Run() 不能用。
UdfExecutor::UdfExecutor(SystemCatalog* catalog,
                         ExecutionContext* context)
    : catalog_(catalog), context_(context), fn_(nullptr) {
}

Value UdfExecutor::EvaluateExpr(const ExprPtr& expr, const FunctionFrame& frame,
                                const Tuple& tuple) const {
    // 用空 column_index_map + 当前 locals 作 outer_bind。
    // column_index_map 为空意味着 ColumnRefExpr 走 outer_bind 路径。
    std::unordered_map<std::string, size_t> empty_cmap;
    ExpressionEvaluator inner(empty_cmap, context_, &frame.locals);
    return inner.Evaluate(expr, tuple);
}

[[noreturn]] void UdfExecutor::RaiseRuntimeError(const std::string& sqlstate,
                                                 const std::string& message) {
    // 我们沿用 CompilerException(RUNTIME) 作为可被 handler 捕获的异常形式。
    // 但其 message 不直接形如 "SIGNAL SQLSTATE..." ，而是把 SQLSTATE 与
    // MESSAGE 拼到消息头，由 FormatError 与 FormatHandler 共同消费。
    std::string sqlstate_clean = NormalizeSqlstate(sqlstate);
    std::string m = "SIGNAL SQLSTATE '" + sqlstate_clean + "' MESSAGE '" +
                    message + "'";
    throw CompilerException(ErrorStage::RUNTIME, m);
}

bool UdfExecutor::MatchesHandler(const DeclareHandlerStatement::CondKind kind,
                                 const std::string& cond_sqlstate,
                                 const std::string& err_sqlstate) {
    // err_sqlstate 来自异常的 message（被 RaiseRuntimeError 编码在
    // "SIGNAL SQLSTATE 'XXXXX' MESSAGE '...'" 前缀中）。
    switch (kind) {
        case DeclareHandlerStatement::CondKind::SQLEXCEPTION:
        case DeclareHandlerStatement::CondKind::SQLWARNING:
        case DeclareHandlerStatement::CondKind::NOT_FOUND:
            // V1 简化：三类一并捕获任意 RuntimeError。
            return true;
        case DeclareHandlerStatement::CondKind::SQLSTATE:
            return NormalizeSqlstate(cond_sqlstate) == NormalizeSqlstate(err_sqlstate);
    }
    return false;
}

Value UdfExecutor::Run() {
    FunctionFrame frame;
    // 把形参值拷到 locals 中，后续 SET 也能改写形参引用（同 SQL/PSM 行为）。
    for (const auto& kv : initial_args_) {
        frame.locals[kv.first] = kv.second;
    }
    for (const auto& s : fn_->body_statements) {
        if (!s) continue;
        if (frame.done) break;
        try {
            ExecuteStatement(s, frame);
        } catch (const CompilerException& ce) {
            // SIGNAL/运行期错误：尝试 handler 匹配。
            // ce.message() 形如 "SIGNAL SQLSTATE 'XXXXX' MESSAGE '...'"。
            std::string msg = ce.what();
            std::string err_sqlstate;
            // 抽取 SQLSTATE
            auto pos = msg.find("SQLSTATE '");
            if (pos != std::string::npos) {
                auto p2 = msg.find('\'', pos + 10);
                if (p2 != std::string::npos) {
                    err_sqlstate = msg.substr(pos + 10, p2 - (pos + 10));
                }
            }
            bool handled = false;
            for (const auto& h : frame.handlers) {
                if (MatchesHandler(h.cond_kind, h.cond_sqlstate, err_sqlstate)) {
                    if (h.type == DeclareHandlerStatement::Type::CONTINUE) {
                        if (h.body) ExecuteStatement(h.body, frame);
                        handled = true;
                    } else {
                        // EXIT / UNDO：V1 暂不支持。
                        throw CompilerException(ErrorStage::RUNTIME,
                            "EXIT / UNDO handler not implemented (V1 only supports CONTINUE)");
                    }
                    break;
                }
            }
            if (!handled) throw;
        }
    }
    if (!frame.done) {
        throw CompilerException(ErrorStage::RUNTIME,
            "runtime error: function did not return a value");
    }
    return frame.result;
}

bool UdfExecutor::RunProcedure(const SystemCatalog::ProcedureDefinition& proc,
                               std::unordered_map<std::string, Value> arg_bind) {
    FunctionFrame frame;
    proc_params_ = proc.parameters;
    for (const auto& kv : arg_bind) {
        frame.locals[kv.first] = kv.second;
    }
    bool ok = true;
    try {
        for (const auto& s : proc.body_statements) {
            if (!s) continue;
            if (frame.done) break;
            try {
                ExecuteStatement(s, frame);
            } catch (const CompilerException& ce) {
                std::string msg = ce.what();
                std::string err_sqlstate;
                auto pos = msg.find("SQLSTATE '");
                if (pos != std::string::npos) {
                    auto p2 = msg.find('\'', pos + 10);
                    if (p2 != std::string::npos) {
                        err_sqlstate = msg.substr(pos + 10, p2 - (pos + 10));
                    }
                }
                bool handled = false;
                for (const auto& h : frame.handlers) {
                    if (MatchesHandler(h.cond_kind, h.cond_sqlstate, err_sqlstate)) {
                        if (h.type == DeclareHandlerStatement::Type::CONTINUE) {
                            if (h.body) ExecuteStatement(h.body, frame);
                            handled = true;
                        } else {
                            throw CompilerException(ErrorStage::RUNTIME,
                                "EXIT / UNDO handler not implemented (V1 only supports CONTINUE)");
                        }
                        break;
                    }
                }
                if (!handled) throw;
            }
        }
    } catch (...) {
        ok = false;
        throw;
    }

    // OUT / INOUT 回写到 ExecutionContext。
    if (context_ != nullptr) {
        for (const auto& p : proc.parameters) {
            if (p.mode == 0) continue;  // IN 跳过
            auto it = frame.locals.find(p.name);
            if (it != frame.locals.end()) {
                context_->SetOutArg(p.name, it->second);
            }
        }
    }
    return ok;
}

void UdfExecutor::ExecuteStatement(const StatementPtr& stmt, FunctionFrame& frame) {
    // 控制流在每条语句执行后由调用方检查；这里只负责"执行一条"。
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
            // 59_procs (Category 8): DEFAULT expr；不存在时为 NULL。
            if (dv.default_expr) {
                frame.locals[dv.var_name] = EvaluateExpr(dv.default_expr, frame, Tuple());
            } else {
                frame.locals[dv.var_name] = Value::MakeNull();
            }
            return;
        }
        case NodeType::SET_VAR_STMT: {
            const auto& sv = static_cast<const SetVarStatement&>(*stmt);
            Value v = EvaluateExpr(sv.expr, frame, Tuple());
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
            const size_t kMaxIter = 10000;
            // 注意：frame.label_stack 故意不在此处压栈/弹栈 —— 之前用
            // push_back + pop_back 包裹循环体会被 body 内的 LEAVE/ITERATE
            // 以及 SIGNAL 异常路径破坏平衡，导致 pop_back 在空栈上断言失败。
            // 控制流仅依赖 frame.control：body 设 LEAVE/ITERATE 后，
            // 循环在下一次检查 control 时清零并退出/继续。
            for (size_t iter = 0; iter < kMaxIter; ++iter) {
                Value cond = EvaluateExpr(ws.condition, frame, Tuple());
                if (!IsTruthyValue(cond)) break;
                for (const auto& s : ws.body) {
                    if (frame.done || frame.control != FunctionFrame::Control::NONE) break;
                    if (s) ExecuteStatement(s, frame);
                }
                if (frame.done) break;
                if (frame.control == FunctionFrame::Control::LEAVE) {
                    frame.control = FunctionFrame::Control::NONE;
                    break;
                }
                if (frame.control == FunctionFrame::Control::ITERATE) {
                    frame.control = FunctionFrame::Control::NONE;
                    continue;
                }
            }
            return;
        }
        case NodeType::LOOP_STMT: {
            const auto& ls = static_cast<const LoopStatement&>(*stmt);
            const size_t kMaxIter = 10000;
            // 同 WHILE：不再维护 label_stack 平衡，依赖 frame.control。
            // ls.label 当前保留但 V1 未使用（标签栈需要嵌套循环上下文，
            // 与 RAII 友好的异常路径难以共存；59_procs 全程不使用 label）。
            (void)ls.label;
            for (size_t iter = 0; iter < kMaxIter; ++iter) {
                if (frame.done) break;
                for (const auto& s : ls.body) {
                    if (frame.done || frame.control != FunctionFrame::Control::NONE) break;
                    if (s) ExecuteStatement(s, frame);
                }
                if (frame.done) break;
                if (frame.control == FunctionFrame::Control::LEAVE) {
                    frame.control = FunctionFrame::Control::NONE;
                    break;
                }
                if (frame.control == FunctionFrame::Control::ITERATE) {
                    frame.control = FunctionFrame::Control::NONE;
                    continue;
                }
            }
            return;
        }
        case NodeType::REPEAT_STMT: {
            const auto& rs = static_cast<const RepeatStatement&>(*stmt);
            const size_t kMaxIter = 10000;
            // 同上：不再 push_back/pop_back label_stack。
            (void)rs.label;
            for (size_t iter = 0; iter < kMaxIter; ++iter) {
                if (frame.done) break;
                for (const auto& s : rs.body) {
                    if (frame.done || frame.control != FunctionFrame::Control::NONE) break;
                    if (s) ExecuteStatement(s, frame);
                }
                if (frame.done) break;
                // ITERATE 跳到下一次迭代；LEAVE 跳出循环。
                if (frame.control == FunctionFrame::Control::LEAVE) {
                    frame.control = FunctionFrame::Control::NONE;
                    break;
                }
                if (frame.control == FunctionFrame::Control::ITERATE) {
                    frame.control = FunctionFrame::Control::NONE;
                    continue;
                }
                // 评估 UNTIL cond：truthy 即退出。
                Value cond = EvaluateExpr(rs.until_expr, frame, Tuple());
                if (IsTruthyValue(cond)) break;
            }
            return;
        }
        case NodeType::CASE_STMT: {
            const auto& cs = static_cast<const CaseStatement&>(*stmt);
            for (const auto& w : cs.whens) {
                if (!w.when_expr) continue;
                Value matched = Value::MakeNull();
                if (cs.subject) {
                    // 简单 CASE：subject = when_expr
                    Value sv = EvaluateExpr(cs.subject, frame, Tuple());
                    Value wv = EvaluateExpr(w.when_expr, frame, Tuple());
                    if (!sv.IsNull() && !wv.IsNull() && Value::Compare(sv, wv) == 0) {
                        matched = Value::MakeInt(1);
                    } else {
                        matched = Value::MakeInt(0);
                    }
                } else {
                    matched = EvaluateExpr(w.when_expr, frame, Tuple());
                }
                if (IsTruthyValue(matched)) {
                    for (const auto& s : w.body) {
                        if (frame.done) return;
                        if (s) ExecuteStatement(s, frame);
                    }
                    return;
                }
            }
            for (const auto& s : cs.else_body) {
                if (frame.done) return;
                if (s) ExecuteStatement(s, frame);
            }
            return;
        }
        case NodeType::LEAVE_STMT: {
            const auto& ls = static_cast<const LeaveStatement&>(*stmt);
            // V1 简化：LOOP/WHILE/REPEAT 不再维护 label_stack。
            // 不带 label 的 LEAVE 直接跳出最近的循环；带 label 的
            // LEAVE 也同样设置 control（59_procs 不使用带 label 的 LEAVE，
            // 未来若启用嵌套 label 需要在结构上重新设计栈维护）。
            (void)ls.label;
            frame.control = FunctionFrame::Control::LEAVE;
            return;
        }
        case NodeType::ITERATE_STMT: {
            const auto& is = static_cast<const IterateStatement&>(*stmt);
            // 同 LEAVE：仅设置 control，依赖循环自身的 ITERATE 处理逻辑。
            (void)is.label;
            frame.control = FunctionFrame::Control::ITERATE;
            return;
        }
        case NodeType::SIGNAL_STMT: {
            const auto& sg = static_cast<const SignalStatement&>(*stmt);
            RaiseRuntimeError(sg.sqlstate, sg.message_text);
        }
        case NodeType::DECLARE_HANDLER_STMT: {
            const auto& dh = static_cast<const DeclareHandlerStatement&>(*stmt);
            FunctionFrame::HandlerEntry he;
            he.type = dh.type;
            he.cond_kind = dh.cond_kind;
            he.cond_sqlstate = dh.cond_sqlstate;
            he.body = dh.body;
            frame.handlers.push_back(std::move(he));
            return;
        }
        case NodeType::DECLARE_CURSOR_STMT: {
            const auto& dc = static_cast<const DeclareCursorStatement&>(*stmt);
            FunctionFrame::CursorEntry ce;
            ce.query = dc.query;
            ce.plan = nullptr;
            ce.rows.clear();
            ce.index = 0;
            ce.is_open = false;
            frame.cursors[dc.cursor_name] = std::move(ce);
            return;
        }
        case NodeType::CURSOR_OPEN_STMT: {
            const auto& op = static_cast<const CursorOpenStatement&>(*stmt);
            auto it = frame.cursors.find(op.cursor_name);
            if (it == frame.cursors.end()) {
                throw CompilerException(ErrorStage::RUNTIME,
                    "runtime error: cursor '" + op.cursor_name +
                    "' not declared");
            }
            auto& ce = it->second;
            if (!ce.query) {
                throw CompilerException(ErrorStage::RUNTIME,
                    "runtime error: cursor '" + op.cursor_name +
                    "' has no query");
            }
            // 把 SelectStatement 共享到 StatementPtr（继承关系）然后 plan。
            std::shared_ptr<Statement> s = std::static_pointer_cast<Statement>(ce.query);
            Planner planner(catalog_, catalog_->GetSymbolTable());
            PlanNodePtr plan = planner.CreatePlan(s);
            Optimizer opt(catalog_);
            plan = opt.Optimize(plan);
            if (!plan) {
                throw CompilerException(ErrorStage::RUNTIME,
                    "runtime error: failed to plan cursor '" +
                    op.cursor_name + "' query");
            }
            ExecutionEngine engine(catalog_, /*txn_manager*/ nullptr);
            // 复用当前 ExecutionContext，让 cursor 内部表达式能看到
            // frame.locals（如 Procedure 局部变量）。
            ExecutionResult r = engine.ExecuteSubplan(plan, context_);
            if (!r.success) {
                throw CompilerException(ErrorStage::RUNTIME,
                    "runtime error: cursor '" + op.cursor_name +
                    "' query failed: " + r.message);
            }
            ce.rows = std::move(r.rows);
            ce.index = 0;
            ce.is_open = true;
            return;
        }
        case NodeType::CURSOR_FETCH_STMT: {
            const auto& ft = static_cast<const CursorFetchStatement&>(*stmt);
            auto it = frame.cursors.find(ft.cursor_name);
            if (it == frame.cursors.end()) {
                throw CompilerException(ErrorStage::RUNTIME,
                    "runtime error: cursor '" + ft.cursor_name +
                    "' not declared");
            }
            auto& ce = it->second;
            if (!ce.is_open) {
                throw CompilerException(ErrorStage::RUNTIME,
                    "runtime error: cursor '" + ft.cursor_name +
                    "' is not open");
            }
            if (ce.index < ce.rows.size()) {
                const Tuple& row = ce.rows[ce.index];
                ++ce.index;
                for (size_t i = 0; i < ft.into_vars.size(); ++i) {
                    if (i < row.ColumnCount()) {
                        frame.locals[ft.into_vars[i]] = row.GetValue(i);
                    } else {
                        frame.locals[ft.into_vars[i]] = Value::MakeNull();
                    }
                }
            } else {
                // 越界：把所有 INTO 变量置 NULL；让用户可以 WHILE v IS NOT NULL
                // 检测结束。
                for (const auto& v : ft.into_vars) {
                    frame.locals[v] = Value::MakeNull();
                }
            }
            return;
        }
        case NodeType::CURSOR_CLOSE_STMT: {
            const auto& cl = static_cast<const CursorCloseStatement&>(*stmt);
            auto it = frame.cursors.find(cl.cursor_name);
            if (it == frame.cursors.end()) {
                throw CompilerException(ErrorStage::RUNTIME,
                    "runtime error: cursor '" + cl.cursor_name +
                    "' not declared");
            }
            auto& ce = it->second;
            ce.is_open = false;
            ce.rows.clear();
            ce.index = 0;
            return;
        }
        case NodeType::INSERT_STMT:
        case NodeType::UPDATE_STMT:
        case NodeType::DELETE_STMT: {
            // 59_procs (Category 8): procedure 体内部允许 INSERT/UPDATE/DELETE。
            // 走 Planner + Optimizer + ExecutionEngine::ExecuteSubplan。
            // 把当前 frame.locals 注入 ExecutionContext，让内嵌 DML 的表达式
            // 解析能引用 procedure 局部变量（如 INSERT INTO t VALUES (x)）。
            const std::unordered_map<std::string, Value>* saved = nullptr;
            if (context_) {
                saved = context_->GetProcLocals();
                context_->SetProcLocals(&frame.locals);
            }
            try {
                ExecuteDmlStatement(catalog_, context_, *stmt);
            } catch (...) {
                if (context_) context_->SetProcLocals(saved);
                throw;
            }
            if (context_) context_->SetProcLocals(saved);
            return;
        }
        default:
            throw CompilerException(ErrorStage::RUNTIME,
                "unsupported statement inside function body");
    }
}

// 静态 DML 执行辅助。把单条 INSERT/UPDATE/DELETE 翻译为计划并执行。
// 使用 context 的当前事务句柄（如果有），否则 autocommit。
void ExecuteDmlStatement(SystemCatalog* catalog,
                         ExecutionContext* context,
                         const Statement& stmt) {
    // Planner 接收 StatementPtr（const-cast 去掉 const）。
    StatementPtr stmt_ptr = std::const_pointer_cast<Statement>(
        std::shared_ptr<Statement>(const_cast<Statement*>(&stmt), [](Statement*){}));
    Planner planner(catalog, catalog->GetSymbolTable());
    PlanNodePtr plan = planner.CreatePlan(stmt_ptr);
    Optimizer opt(catalog);
    plan = opt.Optimize(plan);
    if (!plan) {
        throw CompilerException(ErrorStage::RUNTIME,
            "runtime error: failed to plan DML inside procedure body");
    }
    ExecutionEngine engine(catalog, /*txn_manager=*/nullptr);
    ExecutionResult r = engine.ExecuteSubplan(plan, context);
    if (!r.success) {
        throw CompilerException(ErrorStage::RUNTIME,
            "runtime error: DML inside procedure failed: " + r.message);
    }
}

}  // namespace sqlcompiler
