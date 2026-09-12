#include "execution/AggregateExecutor.h"

#include "execution/ExpressionEvaluator.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace sqlcompiler {

namespace {

std::string Upper(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) r.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return r;
}

bool IsAggregateFunc(const std::string& name) {
    std::string u = Upper(name);
    // 60_funcs: 把 STDDEV / VARIANCE / MEDIAN / STRING_AGG / PERCENTILE_*
    // 视为聚合函数，AggregateExecutor 在分组阶段为它们维护独立的状态。
    return u == "COUNT" || u == "SUM" || u == "AVG" ||
           u == "MIN" || u == "MAX" ||
           u == "STDDEV" || u == "STDDEV_POP" || u == "STDDEV_SAMP" ||
           u == "VARIANCE" || u == "VAR_POP" || u == "VAR_SAMP" ||
           u == "MEDIAN" ||
           u == "STRING_AGG" || u == "GROUP_CONCAT" ||
           u == "PERCENTILE_CONT" || u == "PERCENTILE_DISC";
}

// Walk an expression tree to detect any aggregate function calls
bool ContainsAggregate(const ExprPtr& expr) {
    if (!expr) return false;
    switch (expr->GetType()) {
        case NodeType::LITERAL_EXPR:
            return false;
        case NodeType::COLUMN_REF_EXPR:
            return false;
        case NodeType::BINARY_EXPR: {
            auto b = std::static_pointer_cast<BinaryExpr>(expr);
            return ContainsAggregate(b->left) || ContainsAggregate(b->right);
        }
        case NodeType::UNARY_EXPR: {
            auto u = std::static_pointer_cast<UnaryExpr>(expr);
            return ContainsAggregate(u->operand);
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            auto f = std::static_pointer_cast<FunctionCallExpr>(expr);
            if (IsAggregateFunc(f->function_name)) return true;
            for (auto& a : f->arguments) {
                if (ContainsAggregate(a)) return true;
            }
            return false;
        }
        case NodeType::CASE_EXPR: {
            auto c = std::static_pointer_cast<CaseExprNode>(expr);
            if (c->subject && ContainsAggregate(c->subject)) return true;
            for (const auto& w : c->whens) {
                if (ContainsAggregate(w.when_expr)) return true;
                if (ContainsAggregate(w.then_expr)) return true;
            }
            if (c->else_expr && ContainsAggregate(c->else_expr)) return true;
            return false;
        }
        case NodeType::CAST_EXPR: {
            auto c = std::static_pointer_cast<CastExprNode>(expr);
            return ContainsAggregate(c->expr);
        }
        case NodeType::WINDOW_FUNC_EXPR: {
            auto wf = std::static_pointer_cast<WindowFuncNode>(expr);
            // 窗口函数包装聚合（MAX(MAX(salary)) OVER ...）时，
            // 让 AggregateExecutor 在分组阶段计算内层聚合。
            if (IsAggregateFunc(wf->function_name)) return true;
            for (auto& a : wf->arguments) {
                if (ContainsAggregate(a)) return true;
            }
            return false;
        }
        default:
            return false;
    }
}

// 找到表达式中嵌套的聚合函数调用节点（深度优先）。
// 用于处理 `COALESCE(SUM(o.amount), 0)` 这类用标量函数包装聚合的情况：标量函数外壳
// 由 ExpressionEvaluator 求值，内层 SUM/COUNT/... 的状态由 AggregateExecutor 维护。
// 也支持 MAX(MAX(salary)) OVER (...) 这种窗口函数包装聚合的情形。
ExprPtr FindAggregateCall(const ExprPtr& expr) {
    if (!expr) return nullptr;
    switch (expr->GetType()) {
        case NodeType::FUNCTION_CALL_EXPR: {
            auto f = std::static_pointer_cast<FunctionCallExpr>(expr);
            if (IsAggregateFunc(f->function_name)) return expr;
            for (auto& a : f->arguments) {
                auto sub = FindAggregateCall(a);
                if (sub) return sub;
            }
            return nullptr;
        }
        case NodeType::BINARY_EXPR: {
            auto b = std::static_pointer_cast<BinaryExpr>(expr);
            auto sub = FindAggregateCall(b->left);
            if (sub) return sub;
            return FindAggregateCall(b->right);
        }
        case NodeType::UNARY_EXPR:
            return FindAggregateCall(std::static_pointer_cast<UnaryExpr>(expr)->operand);
        case NodeType::CASE_EXPR: {
            auto c = std::static_pointer_cast<CaseExprNode>(expr);
            if (c->subject) {
                auto sub = FindAggregateCall(c->subject);
                if (sub) return sub;
            }
            for (const auto& w : c->whens) {
                auto sub = FindAggregateCall(w.when_expr);
                if (sub) return sub;
                sub = FindAggregateCall(w.then_expr);
                if (sub) return sub;
            }
            if (c->else_expr) return FindAggregateCall(c->else_expr);
            return nullptr;
        }
        case NodeType::CAST_EXPR: {
            auto c = std::static_pointer_cast<CastExprNode>(expr);
            return FindAggregateCall(c->expr);
        }
        case NodeType::WINDOW_FUNC_EXPR: {
            auto wf = std::static_pointer_cast<WindowFuncNode>(expr);
            // 若是 MAX(...)/MIN(...)/SUM(...)/AVG(...)/COUNT(...) 等窗口聚合：
            //   - 若首个参数本身是聚合调用（如 MAX(MAX(salary))），剥到内层聚合；
            //   - 否则把窗口函数的 (function_name, arguments[0]) 包装成 FunctionCallExpr，
            //     让下游 state-update 路径直接按标量聚合函数处理。
            if (IsAggregateFunc(wf->function_name)) {
                if (!wf->arguments.empty()) {
                    auto sub = FindAggregateCall(wf->arguments[0]);
                    if (sub) return sub;
                }
                ExprPtr agg_arg = wf->arguments.empty() ? nullptr : wf->arguments[0];
                std::vector<ExprPtr> args;
                if (agg_arg) args.push_back(agg_arg);
                return std::make_shared<FunctionCallExpr>(wf->function_name, args);
            }
            for (auto& a : wf->arguments) {
                auto sub = FindAggregateCall(a);
                if (sub) return sub;
            }
            return nullptr;
        }
        default:
            return nullptr;
    }
}

// 递归替换表达式中的聚合函数调用为 LiteralExpr(state_value)，
// 然后用 ExpressionEvaluator 求值替换后的表达式（处理 COALESCE / CASE / 算术 等）。
ExprPtr SubstituteAggregates(const ExprPtr& expr, const Value& state_value) {
    if (!expr) return nullptr;
    switch (expr->GetType()) {
        case NodeType::LITERAL_EXPR:
        case NodeType::COLUMN_REF_EXPR:
            return expr;
        case NodeType::BINARY_EXPR: {
            auto b = std::static_pointer_cast<BinaryExpr>(expr);
            return std::make_shared<BinaryExpr>(
                b->op,
                SubstituteAggregates(b->left, state_value),
                SubstituteAggregates(b->right, state_value));
        }
        case NodeType::UNARY_EXPR: {
            auto u = std::static_pointer_cast<UnaryExpr>(expr);
            return std::make_shared<UnaryExpr>(u->op,
                SubstituteAggregates(u->operand, state_value));
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            auto f = std::static_pointer_cast<FunctionCallExpr>(expr);
            if (IsAggregateFunc(f->function_name)) {
                // 用聚合状态值替换为字面量
                std::string lit;
                switch (state_value.GetType()) {
                    case ValueType::INTEGER:
                        lit = std::to_string(state_value.AsInt());
                        return std::make_shared<LiteralExpr>(LiteralType::INTEGER, lit);
                    case ValueType::FLOAT:
                        lit = std::to_string(state_value.AsFloat());
                        return std::make_shared<LiteralExpr>(LiteralType::FLOAT, lit);
                    case ValueType::VARCHAR:
                        lit = state_value.AsVarchar();
                        return std::make_shared<LiteralExpr>(LiteralType::STRING, lit);
                    default:
                        return std::make_shared<LiteralExpr>(LiteralType::NULL_VALUE, "NULL");
                }
            }
            std::vector<ExprPtr> new_args;
            new_args.reserve(f->arguments.size());
            for (auto& a : f->arguments) {
                new_args.push_back(SubstituteAggregates(a, state_value));
            }
            auto nf = std::make_shared<FunctionCallExpr>(f->function_name, new_args);
            nf->is_distinct = f->is_distinct;
            return nf;
        }
        default:
            return expr;
    }
}

std::string GroupKeyOf(const std::vector<Value>& vs) {
    std::string k;
    for (const auto& v : vs) {
        k += v.ToString();
        k.push_back('\x1F');
    }
    return k;
}

}  // namespace

AggregateExecutor::AggregateExecutor(ExecutionContext* context, ExecutorPtr child,
                                     std::vector<ExprPtr> group_by_exprs,
                                     std::vector<ExprPtr> aggregate_exprs,
                                     std::unordered_map<std::string, size_t> column_index_map)
    : Executor(context), child_(std::move(child)),
      group_by_exprs_(std::move(group_by_exprs)),
      aggregate_exprs_(std::move(aggregate_exprs)),
      column_index_map_(std::move(column_index_map)), cursor_(0) {
}

void AggregateExecutor::Init() {
    cursor_ = 0;
    groups_.clear();
    if (!child_) {
        // 无输入子节点：保证无 GROUP BY 时仍发射一行（聚合初始值：COUNT=0, SUM=0, AVG/MIN/MAX=NULL）。
        if (group_by_exprs_.empty()) {
            Group g;
            g.key_values.clear();
            g.sample_tuple = Tuple();
            g.agg_states.resize(aggregate_exprs_.size());
            groups_.push_back(std::move(g));
        }
        return;
    }
    child_->Init();

    ExpressionEvaluator eval(column_index_map_, context_, nullptr);

    Tuple t;
    while (child_->Next(&t)) {
        std::vector<Value> key;
        key.reserve(group_by_exprs_.size());
        for (const auto& e : group_by_exprs_) {
            key.push_back(eval.Evaluate(e, t));
        }
        std::string key_str = GroupKeyOf(key);

        // Find or create the group
        Group* grp = nullptr;
        for (auto& g : groups_) {
            if (GroupKeyOf(g.key_values) == key_str) {
                grp = &g;
                break;
            }
        }
        if (!grp) {
            Group g;
            g.key_values = std::move(key);
            g.sample_tuple = t;
            g.agg_states.resize(aggregate_exprs_.size());
            groups_.push_back(std::move(g));
            grp = &groups_.back();
        }
        // Update sample tuple (in case a column ref evaluates differently for first row)
        if (grp->agg_states.empty() == false) {
            grp->sample_tuple = t;
        }

        // Update aggregates for each aggregate_expr
        for (size_t i = 0; i < aggregate_exprs_.size(); ++i) {
            const auto& expr = aggregate_exprs_[i];
            if (!expr) continue;
            if (!ContainsAggregate(expr)) continue;  // non-aggregate; only computed at output time

            // 找到嵌套的聚合函数调用（处理 COALESCE(SUM(x), 0) 这类外包标量函数的情况）
            ExprPtr agg_call = FindAggregateCall(expr);
            if (!agg_call) continue;
            auto f = std::static_pointer_cast<FunctionCallExpr>(agg_call);
            std::string fname = Upper(f->function_name);
            bool is_distinct = f->is_distinct;
            ExprPtr agg_arg = f->arguments.empty() ? nullptr : f->arguments[0];

            // 60_funcs: FILTER (WHERE cond) —— 仅当 cond 评估为 TRUE（非 0、非 NULL、
            // 非空布尔 false）时该行才参与此聚合。filter_expr 为 nullptr 时不过滤。
            if (f->filter_expr) {
                Value fv = eval.Evaluate(f->filter_expr, t);
                // 与 WHERE/HAVING 一致：INTEGER/FLOAT 非 0；BOOLEAN/字符串按惯例。
                bool pass = !fv.IsNull();
                if (pass) {
                    if (fv.GetType() == ValueType::INTEGER) pass = (fv.AsInt() != 0);
                    else if (fv.GetType() == ValueType::FLOAT) pass = (fv.AsFloat() != 0.0);
                }
                if (!pass) continue;
            }

            auto& st = grp->agg_states[i];
            ++st.count;

            if (fname == "COUNT") {
                if (fname == "COUNT" && agg_arg) {
                    Value v = eval.Evaluate(agg_arg, t);
                    if (!v.IsNull()) {
                        ++st.count_non_null;
                        if (is_distinct) {
                            // COUNT(DISTINCT col)：按值字符串去重
                            st.distinct_values.insert(v.ToString());
                        }
                    }
                }
                continue;
            }

            // ---- 60_funcs: STDDEV / VARIANCE / MEDIAN ----
            // 对所有这些聚合，统一要求一个参数；NULL 不计入计数。
            // STDDEV / VARIANCE 同时维护 Welford 累加器（mean / M2）以获得数值
            // 稳定的单遍方差估计；MEDIAN 仅依赖 collected_values，排序后取中位。
            if (fname == "STDDEV" || fname == "STDDEV_POP" || fname == "STDDEV_SAMP" ||
                fname == "VARIANCE" || fname == "VAR_POP" || fname == "VAR_SAMP" ||
                fname == "MEDIAN") {
                if (!agg_arg) continue;
                Value v = eval.Evaluate(agg_arg, t);
                if (v.IsNull()) continue;
                // MEDIAN 只需要有序的样本集合。
                if (fname == "MEDIAN") {
                    st.collected_values.push_back(v);
                    continue;
                }
                // STDDEV / VARIANCE: Welford 单遍累加
                double x = (v.GetType() == ValueType::FLOAT) ? v.AsFloat() :
                           (v.GetType() == ValueType::INTEGER) ? static_cast<double>(v.AsInt()) : 0.0;
                // M2 = 0 与 n=0 是初始化约定；新样本用递推更新。
                // 为正确处理"首次进入"分支，这里手动维护 n 而非 st.count（避免重复统计 FILTER）。
                double n = static_cast<double>(st.collected_values.size()) + 1.0;
                double delta = x - st.welford_mean;
                st.welford_mean += delta / n;
                double delta2 = x - st.welford_mean;
                st.welford_m2 += delta * delta2;
                st.collected_values.push_back(v);
                continue;
            }

            // ---- 60_funcs: PERCENTILE_CONT / PERCENTILE_DISC ----
            // 有序集合聚合：WITHIN GROUP (ORDER BY x) 提供排序键；参数 p 仅用于
            // 输出阶段定位分位数。Init 阶段把 (p, sort_key) 一起收集到专门字段，
            // 输出阶段按 sort_key 排序后计算百分位。
            if (fname == "PERCENTILE_CONT" || fname == "PERCENTILE_DISC") {
                if (f->within_group_order_by.empty()) continue;
                Value pv = agg_arg ? eval.Evaluate(agg_arg, t) : Value::MakeNull();
                Value sv = eval.Evaluate(f->within_group_order_by[0].expr, t);
                // 即使 p 或样本值为 NULL，也保留（输出阶段再处理）。
                st.percentile_p_samples.push_back(pv);
                st.percentile_sort_keys.push_back(sv);
                continue;
            }

            // ---- 60_funcs: STRING_AGG / GROUP_CONCAT ----
            // 双参数：(expr, delimiter)；可选 WITHIN GROUP (ORDER BY key) 控制顺序。
            // 非 NULL expr 会被收集，分隔符仅在首行记录（同一组内不应改变）。
            if (fname == "STRING_AGG" || fname == "GROUP_CONCAT") {
                if (f->arguments.size() < 2) continue;
                Value v  = eval.Evaluate(f->arguments[0], t);
                Value dv = eval.Evaluate(f->arguments[1], t);
                if (!st.string_agg_delim_set) {
                    st.string_agg_delim = dv.IsNull() ? std::string(",") : dv.ToString();
                    st.string_agg_delim_set = true;
                    st.string_agg_has_order = !f->within_group_order_by.empty();
                }
                if (v.IsNull()) continue;
                Value sort_key;
                if (!f->within_group_order_by.empty()) {
                    sort_key = eval.Evaluate(f->within_group_order_by[0].expr, t);
                } else {
                    sort_key = Value::MakeNull();
                }
                st.string_agg_entries.emplace_back(sort_key, v.ToString());
                continue;
            }

            // For SUM/AVG/MIN/MAX, evaluate arg
            if (!agg_arg) continue;
            Value v = eval.Evaluate(agg_arg, t);
            if (v.IsNull()) continue;
            // 记录非 NULL 个数，供 AVG 计算 SUM(non_null) / COUNT(non_null)。
            ++st.count_non_null;

            // SUM(DISTINCT col) / AVG(DISTINCT col)：只对组内首次出现的值累加
            if (is_distinct) {
                if (!st.distinct_values.insert(v.ToString()).second) {
                    // 已出现过，跳过累加；MIN/MAX 仍按全部值参与
                }
            }

            st.any_numeric = true;

            if (v.GetType() == ValueType::INTEGER) {
                st.sum_int += static_cast<double>(v.AsInt());
            } else if (v.GetType() == ValueType::FLOAT) {
                st.sum_float += v.AsFloat();
            }
            // MIN/MAX：所有非 NULL 值都参与（DISTINCT 不影响极值语义）
            if (!st.min_max_init) {
                st.min_val = v;
                st.max_val = v;
                st.min_max_init = true;
            } else {
                if (Value::Compare(v, st.min_val) < 0) st.min_val = v;
                if (Value::Compare(v, st.max_val) > 0) st.max_val = v;
            }
        }
    }

    // 无 GROUP BY 时即使输入为空（filter 后 0 行 / 空表 / 恒假 WHERE），仍需发射一行，
    // 聚合值用其初始状态（COUNT=0, SUM=0, AVG/MIN/MAX=NULL）。
    if (group_by_exprs_.empty() && groups_.empty()) {
        Group g;
        g.key_values.clear();
        g.sample_tuple = Tuple();
        g.agg_states.resize(aggregate_exprs_.size());
        groups_.push_back(std::move(g));
    }
}

Value AggregateExecutor::EvalAggregateExpr(const ExprPtr& expr, const Tuple& sample,
                                            const std::vector<AggregateState>& states,
                                            size_t idx) const {
    ExpressionEvaluator eval(column_index_map_, context_, nullptr);
    if (!expr) return Value::MakeNull();
    const auto& st = states[idx];
    // 窗口函数包装聚合（MAX(MAX(salary)) OVER (PARTITION BY dept) ... GROUP BY dept）：
    // 分组阶段已经把内层聚合写入了 idx 对应的 state；把窗口函数值退化为内层聚合状态。
    // 这一简化的合理性：每条分组只有 1 行，外层窗口聚合在 PARTITION BY 列上的
    // 默认 frame 覆盖整个分区，对单行取 MAX/MIN/SUM/AVG/COUNT 即返回该行的值。
    if (expr->GetType() == NodeType::WINDOW_FUNC_EXPR) {
        auto wf = std::static_pointer_cast<WindowFuncNode>(expr);
        if (IsAggregateFunc(wf->function_name)) {
            // 外层窗口本身就是聚合（MAX/MIN/SUM/AVG/COUNT），直接用 state 输出。
            if (wf->function_name == "COUNT" || wf->function_name == "count") {
                if (wf->arguments.empty()) {
                    return Value::MakeInt(static_cast<int32_t>(st.count));
                }
                return Value::MakeInt(static_cast<int32_t>(st.count_non_null));
            }
            if (wf->function_name == "SUM" || wf->function_name == "sum") {
                if (!st.any_numeric) return Value::MakeNull();
                return Value::MakeFloat(st.sum_int + st.sum_float);
            }
            if (wf->function_name == "AVG" || wf->function_name == "avg") {
                if (st.count_non_null == 0) return Value::MakeNull();
                double s = st.sum_int + st.sum_float;
                return Value::MakeFloat(s / static_cast<double>(st.count_non_null));
            }
            if (wf->function_name == "MIN" || wf->function_name == "min") {
                return st.min_max_init ? st.min_val : Value::MakeNull();
            }
            if (wf->function_name == "MAX" || wf->function_name == "max") {
                return st.min_max_init ? st.max_val : Value::MakeNull();
            }
        }
        // 其它窗口函数（RANK / ROW_NUMBER ...）在 AggregateExecutor 内没有意义，
        // 走通用路径让 ExpressionEvaluator 求值（实际通常返回 NULL）。
        ExprPtr agg_call = FindAggregateCall(expr);
        if (agg_call) {
            Value agg_val = EvalAggregateExpr(agg_call, sample, states, idx);
            ExprPtr rewritten = SubstituteAggregates(expr, agg_val);
            return eval.Evaluate(rewritten, sample);
        }
        return eval.Evaluate(expr, sample);
    }
    // 处理「裸」聚合函数调用：直接返回状态值
    if (expr->GetType() == NodeType::FUNCTION_CALL_EXPR) {
        auto f = std::static_pointer_cast<FunctionCallExpr>(expr);
        std::string fname = Upper(f->function_name);
        if (fname == "COUNT" || fname == "SUM" || fname == "AVG" ||
            fname == "MIN" || fname == "MAX") {
            if (fname == "COUNT") {
                if (f->arguments.empty()) {
                    return Value::MakeInt(static_cast<int32_t>(st.count));
                }
                if (f->is_distinct) {
                    return Value::MakeInt(static_cast<int32_t>(st.distinct_values.size()));
                }
                return Value::MakeInt(static_cast<int32_t>(st.count_non_null));
            }
            if (!st.any_numeric && fname != "MIN" && fname != "MAX") {
                return Value::MakeNull();
            }
            if (fname == "SUM") {
                double s = st.sum_int + st.sum_float;
                return Value::MakeFloat(s);
            }
            if (fname == "AVG") {
                double s = st.sum_int + st.sum_float;
                double c = static_cast<double>(st.count_non_null);
                if (c == 0.0) return Value::MakeNull();
                return Value::MakeFloat(s / c);
            }
            if (fname == "MIN") {
                return st.min_max_init ? st.min_val : Value::MakeNull();
            }
            if (fname == "MAX") {
                return st.min_max_init ? st.max_val : Value::MakeNull();
            }
        }

        // ---- 60_funcs: STDDEV / VARIANCE ----
        //
        // ===== 语义文档 =====
        //   STDDEV(x)            ≡ STDDEV_SAMP(x)   —— 样本标准差（n-1 除数）
        //   STDDEV_SAMP(x)                          —— 样本标准差（n-1 除数）
        //   STDDEV_POP(x)                           —— 总体标准差（n 除数）
        //   VARIANCE(x)          ≡ VAR_SAMP(x)     —— 样本方差（n-1 除数）
        //   VAR_SAMP(x)                             —— 样本方差（n-1 除数）
        //   VAR_POP(x)                               —— 总体方差（n 除数）
        //
        // 与 PostgreSQL / Oracle 行为一致：缺省形式按"样本"口径计算（n-1 除数），
        // 与 _SAMP / _POP 后缀形式按 SQL:2003 标准明确区分。
        //
        // 算法选择：Welford 单遍数值稳定累加（在 Init 阶段同步更新 mean / M2），
        // 输出阶段用 M2 / (n-1)（样本）或 M2 / n（总体）得到方差，开方得标准差。
        // 边界：n == 0 返回 NULL；_SAMP 且 n < 2 返回 NULL（n-1 除法无定义）。
        // ===== 语义文档结束 =====
        //
        // 单点 / 零样本 / 单样本（含 _SAMP 且 n=1）：n<2 且用 n-1 除时无定义，返回 NULL。
        if (fname == "STDDEV" || fname == "STDDEV_POP" || fname == "STDDEV_SAMP" ||
            fname == "VARIANCE" || fname == "VAR_POP" || fname == "VAR_SAMP") {
            size_t n = st.collected_values.size();
            if (n == 0) return Value::MakeNull();
            bool is_samp = (fname == "STDDEV" || fname == "STDDEV_SAMP" ||
                            fname == "VARIANCE" || fname == "VAR_SAMP");
            if (is_samp && n < 2) return Value::MakeNull();
            // Welford: M2 / n - 1 (sample) 或 M2 / n (population)
            double variance = st.welford_m2 / static_cast<double>(is_samp ? (n - 1) : n);
            if (fname.find("STDDEV") != std::string::npos) {
                if (variance < 0.0) variance = 0.0;  // 数值误差保护
                return Value::MakeFloat(std::sqrt(variance));
            }
            return Value::MakeFloat(variance);
        }

        // ---- 60_funcs: MEDIAN ----
        // 把组内非 NULL 值收集到 collected_values，排序后取中间；
        // 偶数 n 时取两中位点的算术平均（与 PostgreSQL 一致）。
        if (fname == "MEDIAN") {
            if (st.collected_values.empty()) return Value::MakeNull();
            std::vector<Value> sorted = st.collected_values;
            std::sort(sorted.begin(), sorted.end(),
                [](const Value& a, const Value& b) { return Value::Compare(a, b) < 0; });
            size_t n = sorted.size();
            if (n % 2 == 1) return sorted[n / 2];
            const Value& lo = sorted[n / 2 - 1];
            const Value& hi = sorted[n / 2];
            // 取两者算术平均。类型统一为 FLOAT。
            double lv = (lo.GetType() == ValueType::FLOAT) ? lo.AsFloat() :
                        (lo.GetType() == ValueType::INTEGER) ? static_cast<double>(lo.AsInt()) : 0.0;
            double hv = (hi.GetType() == ValueType::FLOAT) ? hi.AsFloat() :
                        (hi.GetType() == ValueType::INTEGER) ? static_cast<double>(hi.AsInt()) : 0.0;
            return Value::MakeFloat((lv + hv) / 2.0);
        }

        // ---- 60_funcs: PERCENTILE_CONT / PERCENTILE_DISC ----
        // PERCENTILE_CONT(p) WITHIN GROUP (ORDER BY x)：把 sort_key 排序，
        // 按 p*(n-1) 线性插值（CONT）或取最接近 ceil(p*n) 的离散值（DISC）。
        // 这里"p"取自首次收集到的非 NULL 样本（同一组内 p 应一致）。
        if (fname == "PERCENTILE_CONT" || fname == "PERCENTILE_DISC") {
            if (st.percentile_sort_keys.empty()) return Value::MakeNull();
            // 提取 p（同一组内取首个非 NULL）
            double p = 0.0;
            bool p_set = false;
            for (const auto& pv : st.percentile_p_samples) {
                if (!pv.IsNull()) {
                    p = (pv.GetType() == ValueType::FLOAT) ? pv.AsFloat() :
                        (pv.GetType() == ValueType::INTEGER) ? static_cast<double>(pv.AsInt()) : 0.0;
                    p_set = true;
                    break;
                }
            }
            if (!p_set) return Value::MakeNull();
            // 排序 sort_key 与之配对的 p
            std::vector<Value> sorted_keys = st.percentile_sort_keys;
            std::sort(sorted_keys.begin(), sorted_keys.end(),
                [](const Value& a, const Value& b) { return Value::Compare(a, b) < 0; });
            // 去掉 NULL（NULL 排在最后；sorted 仍可能含 NULL）
            std::vector<Value> non_null;
            for (const auto& v : sorted_keys) if (!v.IsNull()) non_null.push_back(v);
            if (non_null.empty()) return Value::MakeNull();
            size_t n = non_null.size();
            if (fname == "PERCENTILE_DISC") {
                // 取 ceil(p*n) - 1（按 PG 语义）
                size_t idx = 0;
                double target = p * static_cast<double>(n);
                size_t t = static_cast<size_t>(std::ceil(target));
                if (t == 0) t = 1;
                if (t > n) t = n;
                idx = t - 1;
                return non_null[idx];
            }
            // PERCENTILE_CONT：按 p * (n-1) 浮点位置做线性插值
            double pos = p * static_cast<double>(n - 1);
            size_t lo_i = static_cast<size_t>(std::floor(pos));
            size_t hi_i = static_cast<size_t>(std::ceil(pos));
            if (lo_i == hi_i) return non_null[lo_i];
            double frac = pos - static_cast<double>(lo_i);
            const Value& lo = non_null[lo_i];
            const Value& hi = non_null[hi_i];
            double lv = (lo.GetType() == ValueType::FLOAT) ? lo.AsFloat() :
                        (lo.GetType() == ValueType::INTEGER) ? static_cast<double>(lo.AsInt()) : 0.0;
            double hv = (hi.GetType() == ValueType::FLOAT) ? hi.AsFloat() :
                        (hi.GetType() == ValueType::INTEGER) ? static_cast<double>(hi.AsInt()) : 0.0;
            return Value::MakeFloat(lv + (hv - lv) * frac);
        }

        // ---- 60_funcs: STRING_AGG / GROUP_CONCAT ----
        // 把非 NULL expr 值用 delim 连接；有 WITHIN GROUP 时按排序键排序后输出。
        if (fname == "STRING_AGG" || fname == "GROUP_CONCAT") {
            if (st.string_agg_entries.empty()) return Value::MakeNull();
            std::vector<std::pair<Value, std::string>> entries = st.string_agg_entries;
            if (st.string_agg_has_order) {
                std::sort(entries.begin(), entries.end(),
                    [](const std::pair<Value, std::string>& a,
                       const std::pair<Value, std::string>& b) {
                        return Value::Compare(a.first, b.first) < 0;
                    });
            }
            std::string out;
            for (size_t i = 0; i < entries.size(); ++i) {
                if (i > 0) out += st.string_agg_delim;
                out += entries[i].second;
            }
            return Value::MakeVarchar(out);
        }
    }
    // 标量函数包裹聚合的情况：先算出内层聚合的状态值，替换到表达式中再交给 ExpressionEvaluator 求值。
    // 例：COALESCE(SUM(o.amount), 0) → 先得 sum_state → 把 SUM(...) 替换为 literal(sum_state)
    //     再让 ExpressionEvaluator 走 COALESCE(literal, 0) → 非 NULL 时返回 sum_state。
    ExprPtr agg_call = FindAggregateCall(expr);
    if (agg_call) {
        Value agg_val = EvalAggregateExpr(agg_call, sample, states, idx);
        ExprPtr rewritten = SubstituteAggregates(expr, agg_val);
        return eval.Evaluate(rewritten, sample);
    }
    // 普通表达式：直接交给 ExpressionEvaluator
    return eval.Evaluate(expr, sample);
}

bool AggregateExecutor::Next(Tuple* tuple) {
    if (cursor_ >= groups_.size()) return false;
    const Group& g = groups_[cursor_];
    std::vector<Value> out_values;
    out_values.reserve(aggregate_exprs_.size());
    for (size_t i = 0; i < aggregate_exprs_.size(); ++i) {
        out_values.push_back(
            EvalAggregateExpr(aggregate_exprs_[i], g.sample_tuple, g.agg_states, i));
    }
    if (tuple) *tuple = Tuple(std::move(out_values));
    ++cursor_;
    return true;
}

}  // namespace sqlcompiler