#include "execution/PreAggScanExecutor.h"

#include <cctype>
#include <string>

#include "execution/ExpressionEvaluator.h"

namespace sqlcompiler {

namespace {

std::string Upper(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) r.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return r;
}

bool IsBareCountSum(const ExprPtr& e) {
    if (!e || e->GetType() != NodeType::FUNCTION_CALL_EXPR) return false;
    auto f = std::static_pointer_cast<FunctionCallExpr>(e);
    std::string u = Upper(f->function_name);
    if (u != "COUNT" && u != "SUM") return false;
    if (f->is_distinct) return false;
    return f->arguments.size() <= 1;
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

PreAggScanExecutor::PreAggScanExecutor(
    ExecutionContext* context, ExecutorPtr child,
    std::vector<ExprPtr> group_by_exprs,
    std::vector<ExprPtr> aggregate_exprs,
    std::unordered_map<std::string, size_t> column_index_map)
    : Executor(context), child_(std::move(child)),
      group_by_exprs_(std::move(group_by_exprs)),
      aggregate_exprs_(std::move(aggregate_exprs)),
      column_index_map_(std::move(column_index_map)), cursor_(0) {
}

void PreAggScanExecutor::Init() {
    cursor_ = 0;
    groups_.clear();
    group_index_.clear();
    if (!child_) {
        // 无输入子节点：与 AggregateExecutor 一致，无 GROUP BY 时仍发射一行初始状态。
        if (group_by_exprs_.empty()) {
            Group g;
            g.sample = Tuple();
            g.states.resize(aggregate_exprs_.size());
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

        Group* grp = nullptr;
        if (group_by_exprs_.empty()) {
            // 无 GROUP BY：单组，避免哈希键为空串的额外开销。
            if (groups_.empty()) {
                Group g;
                g.key_values.clear();
                g.sample = t;
                g.states.resize(aggregate_exprs_.size());
                groups_.push_back(std::move(g));
            }
            grp = &groups_[0];
        } else {
            auto it = group_index_.find(key_str);
            if (it == group_index_.end()) {
                Group g;
                g.key_values = std::move(key);
                g.sample = t;
                g.states.resize(aggregate_exprs_.size());
                group_index_.emplace(key_str, groups_.size());
                groups_.push_back(std::move(g));
                grp = &groups_.back();
            } else {
                grp = &groups_[it->second];
            }
        }
        // 组内样本行更新为最新一行（与 AggregateExecutor 行为一致：普通表达式
        // 在输出阶段用组内最近一行求值；GROUP BY 列在组内恒定，取哪行都一致）。
        grp->sample = t;

        // 逐项更新裸 COUNT/SUM 状态。
        for (size_t i = 0; i < aggregate_exprs_.size(); ++i) {
            const auto& expr = aggregate_exprs_[i];
            if (!IsBareCountSum(expr)) continue;  // 普通表达式：仅输出阶段求值
            auto f = std::static_pointer_cast<FunctionCallExpr>(expr);
            std::string fname = Upper(f->function_name);
            auto& st = grp->states[i];
            ++st.count;
            if (f->arguments.empty()) continue;  // COUNT(*)
            Value v = eval.Evaluate(f->arguments[0], t);
            if (v.IsNull()) continue;
            ++st.count_non_null;
            st.any_numeric = true;
            if (fname == "SUM") {
                if (v.GetType() == ValueType::INTEGER) {
                    st.sum += static_cast<double>(v.AsInt());
                } else if (v.GetType() == ValueType::FLOAT) {
                    st.sum += v.AsFloat();
                }
                // 其它类型（如 VARCHAR）：与 AggregateExecutor 一致地只计非 NULL 数，
                // 不参与数值累计（any_numeric 已置位，SUM 输出 0.0）。
            }
        }
    }

    // 无 GROUP BY 且输入为空（空表 / Filter 后 0 行 / 恒假 WHERE）：仍发射一行，
    // 聚合值用初始状态（COUNT=0，SUM 因 any_numeric=false 输出 NULL），
    // 与 AggregateExecutor 语义一致。
    if (group_by_exprs_.empty() && groups_.empty()) {
        Group g;
        g.key_values.clear();
        g.sample = Tuple();
        g.states.resize(aggregate_exprs_.size());
        groups_.push_back(std::move(g));
    }
}

bool PreAggScanExecutor::Next(Tuple* tuple) {
    if (cursor_ >= groups_.size()) return false;
    const Group& g = groups_[cursor_];
    ExpressionEvaluator eval(column_index_map_, context_, nullptr);
    std::vector<Value> out_values;
    out_values.reserve(aggregate_exprs_.size());
    for (size_t i = 0; i < aggregate_exprs_.size(); ++i) {
        const auto& expr = aggregate_exprs_[i];
        if (IsBareCountSum(expr)) {
            auto f = std::static_pointer_cast<FunctionCallExpr>(expr);
            std::string fname = Upper(f->function_name);
            const auto& st = g.states[i];
            if (fname == "COUNT") {
                if (f->arguments.empty()) {
                    out_values.push_back(Value::MakeInt(static_cast<int32_t>(st.count)));
                } else {
                    out_values.push_back(Value::MakeInt(static_cast<int32_t>(st.count_non_null)));
                }
            } else {  // SUM
                if (!st.any_numeric) {
                    out_values.push_back(Value::MakeNull());
                } else {
                    out_values.push_back(Value::MakeFloat(st.sum));
                }
            }
        } else {
            // 普通（非聚合）表达式：在组内样本行上求值（典型为 GROUP BY 列）。
            out_values.push_back(eval.Evaluate(expr, g.sample));
        }
    }
    if (tuple) *tuple = Tuple(std::move(out_values));
    ++cursor_;
    return true;
}

}  // namespace sqlcompiler
