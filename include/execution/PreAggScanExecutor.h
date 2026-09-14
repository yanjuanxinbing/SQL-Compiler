#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ast/AST.h"
#include "execution/Executor.h"
#include "storage_engine/Tuple.h"

namespace sqlcompiler {

// U3-2：扫描内预聚合算子。
//
// 对应逻辑计划中的 PreAggScanNode：把「单表 + 裸 COUNT/SUM 聚合」下推至扫描层计算。
// 与 AggregateExecutor 的差异：
//   1) 直接在扫描循环内累计 COUNT/SUM 状态，不构造逐行 Tuple 流水；
//   2) 分组用哈希表（O(1) 均摊查找），替代 AggregateExecutor 对 groups_ 的线性扫描
//      + 每轮重新拼接 GroupKeyOf 字符串（O(组数) 比较）；
//   3) 输出形状与 AggregateExecutor 完全一致（按 aggregate_exprs 逐项输出），
//      上层 Project / Filter(HAVING) / Sort / Window 可透明复用。
//
// 资格由 Optimizer::PushDownAggregates 保守判定：仅当每个 aggregate_expr 是
// 「裸 COUNT(*)/COUNT(col)/SUM(col)（无 DISTINCT、无 AVG/MIN/MAX、无标量包装）」或
// 「不含聚合调用的普通表达式（典型为 GROUP BY 列，输出时在组内样本行上求值）」时改写，
// 保证与 AggregateExecutor 的语义（含空输入、全 NULL 列、COUNT(*) 计行）逐项一致。
class PreAggScanExecutor : public Executor {
public:
    PreAggScanExecutor(ExecutionContext* context, ExecutorPtr child,
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

    // 与 AggregateExecutor::AggregateState 等价的子集：仅 COUNT/SUM 需要的状态。
    struct AggState {
        int64_t count = 0;          // 组内行数（COUNT(*) 输出）
        int64_t count_non_null = 0; // COUNT(col) 输出：组内该列非 NULL 行数
        double  sum = 0.0;          // SUM 累计（INT 转 double 与 FLOAT 统一）
        bool    any_numeric = false; // 出现过非 NULL 值（SUM 空输入输出 NULL 的判据）
    };

    struct Group {
        std::vector<Value> key_values;
        std::vector<AggState> states;  // 与 aggregate_exprs 等长（仅裸 COUNT/SUM 项使用）
        Tuple sample;                  // 组内样本行：普通（非聚合）表达式输出时求值用
    };

    std::vector<Group> groups_;
    std::unordered_map<std::string, size_t> group_index_;  // 分组键 → groups_ 下标
    size_t cursor_ = 0;
};

}  // namespace sqlcompiler
