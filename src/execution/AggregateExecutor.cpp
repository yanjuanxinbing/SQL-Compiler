#include "execution/AggregateExecutor.h"

#include "execution/ExpressionEvaluator.h"

#include <cctype>
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
    return u == "COUNT" || u == "SUM" || u == "AVG" ||
           u == "MIN" || u == "MAX";
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
        default:
            return false;
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
    if (!child_) return;
    child_->Init();

    ExpressionEvaluator eval(column_index_map_);

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

            // Determine the aggregate function and target column
            std::string fname;
            ExprPtr agg_arg;
            bool is_distinct = false;
            if (expr->GetType() == NodeType::FUNCTION_CALL_EXPR) {
                auto f = std::static_pointer_cast<FunctionCallExpr>(expr);
                fname = Upper(f->function_name);
                is_distinct = f->is_distinct;
                if (!f->arguments.empty()) {
                    agg_arg = f->arguments[0];
                }
            } else {
                continue;
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

            // For SUM/AVG/MIN/MAX, evaluate arg
            if (!agg_arg) continue;
            Value v = eval.Evaluate(agg_arg, t);
            if (v.IsNull()) continue;

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
}

Value AggregateExecutor::EvalAggregateExpr(const ExprPtr& expr, const Tuple& sample,
                                            const std::vector<AggregateState>& states,
                                            size_t idx) const {
    ExpressionEvaluator eval(column_index_map_);
    if (!expr) return Value::MakeNull();
    if (expr->GetType() != NodeType::FUNCTION_CALL_EXPR) {
        return eval.Evaluate(expr, sample);
    }
    auto f = std::static_pointer_cast<FunctionCallExpr>(expr);
    std::string fname = Upper(f->function_name);
    if (fname != "COUNT" && fname != "SUM" && fname != "AVG" &&
        fname != "MIN" && fname != "MAX") {
        return eval.Evaluate(expr, sample);
    }
    const auto& st = states[idx];
    if (fname == "COUNT") {
        if (f->arguments.empty()) {
            return Value::MakeInt(static_cast<int32_t>(st.count));
        }
        // COUNT(DISTINCT col) 走 distinct 集合大小；其余为非 NULL 计数
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
        double c = static_cast<double>(st.count);
        if (c == 0.0) return Value::MakeNull();
        return Value::MakeFloat(s / c);
    }
    if (fname == "MIN") {
        return st.min_max_init ? st.min_val : Value::MakeNull();
    }
    if (fname == "MAX") {
        return st.min_max_init ? st.max_val : Value::MakeNull();
    }
    return Value::MakeNull();
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