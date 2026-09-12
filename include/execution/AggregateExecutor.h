#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/AST.h"
#include "execution/Executor.h"
#include "storage_engine/Tuple.h"

namespace sqlcompiler {

// 聚合算子：对子算子的 Tuple 按 group_by_exprs 分组，并对每组计算
// aggregate_exprs 中的所有表达式（聚合函数 COUNT/SUM/AVG/MIN/MAX
// 在此处被识别并求值；非聚合表达式按组内任一 Tuple 求值）。
// 每个分组输出一条 Tuple，对应 SELECT 列表中的一项。
// 对应逻辑计划中的 AggregateNode（GROUP BY ... HAVING ...）。
class AggregateExecutor : public Executor {
public:
    AggregateExecutor(ExecutionContext* context, ExecutorPtr child,
                      std::vector<ExprPtr> group_by_exprs,
                      std::vector<ExprPtr> aggregate_exprs,
                      std::unordered_map<std::string, size_t> column_index_map);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    ExecutorPtr child_;
    std::vector<ExprPtr> group_by_exprs_;
    std::vector<ExprPtr> aggregate_exprs_;
    std::unordered_map<std::string, size_t> column_index_map_;

    struct AggregateState {
        int64_t count = 0;
        bool    count_has_non_null = false;
        int64_t count_non_null = 0;
        double  sum_int = 0.0;
        double  sum_float = 0.0;
        bool    any_numeric = false;
        Value   min_val;
        Value   max_val;
        bool    min_max_init = false;
        // 仅 COUNT(DISTINCT col) / SUM(DISTINCT col) 有效：按组收集到的去重集合。
        std::unordered_set<std::string> distinct_values;
        // ---- 60_funcs: STDDEV/VARIANCE（Welford 单遍累加器）----
        double  welford_mean = 0.0;
        double  welford_m2   = 0.0;
        // ---- 60_funcs: MEDIAN/PERCENTILE_CONT/PERCENTILE_DISC（有序集合聚合）----
        // 直接收集到的样本值（不参与 SUM/AVG 等路径，独立于上述字段）。
        std::vector<Value> collected_values;
        // ---- 60_funcs: PERCENTILE_* 专用 (fraction_p, sort_key) 样本对 ----
        // 因为 percent_rank 的样本值与分位点 p 分属两个不同评估路径，
        // 放在同一个 vector 会导致索引含义混淆。这里单独存放一个并行 vector。
        std::vector<Value> percentile_p_samples;
        std::vector<Value> percentile_sort_keys;
        // ---- 60_funcs: STRING_AGG（值 + 可选 WITHIN GROUP 排序键）----
        // 每条记录为 (sort_key_value, string_value)；sort_key 为 NULL 时
        // 表示未指定排序键，按插入顺序输出。
        std::vector<std::pair<Value, std::string>> string_agg_entries;
        // STRING_AGG 分隔符：执行期首次见到该聚合调用时确定。
        std::string string_agg_delim;
        // 是否已记录分隔符
        bool string_agg_delim_set = false;
        // 是否需要 WITHIN GROUP 排序输出
        bool string_agg_has_order = false;
    };

    struct Group {
        std::vector<Value> key_values;
        std::vector<AggregateState> agg_states;   // 与 aggregate_exprs 等长
        Tuple  sample_tuple;                       // 用于求值非聚合子表达式
    };

    std::vector<Group> groups_;
    size_t cursor_;

    // 评估 aggregate_exprs[i]，将其中聚合函数调用替换为已计算的状态值
    Value EvalAggregateExpr(const ExprPtr& expr, const Tuple& sample,
                            const std::vector<AggregateState>& states,
                            size_t idx) const;
};

}  // namespace sqlcompiler