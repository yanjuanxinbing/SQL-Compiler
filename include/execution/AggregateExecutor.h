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