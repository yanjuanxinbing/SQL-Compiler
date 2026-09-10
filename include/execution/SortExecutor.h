#pragma once

#include <utility>
#include <vector>

#include "ast/AST.h"
#include "execution/Executor.h"
#include "execution/ExpressionEvaluator.h"
#include "storage_engine/Tuple.h"

namespace sqlcompiler {

// 排序算子：物化子算子的所有 Tuple，按 order_items 的求值结果升序/降序排列后逐条返回。
// 对应逻辑计划中的 SortNode（ORDER BY）。
class SortExecutor : public Executor {
public:
    SortExecutor(ExecutionContext* context, ExecutorPtr child,
                 std::vector<OrderByItem> order_items,
                 std::unordered_map<std::string, size_t> column_index_map);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    ExecutorPtr child_;
    std::vector<OrderByItem> order_items_;
    std::unordered_map<std::string, size_t> column_index_map_;

    // 排序用的索引向量：每条记录 (Tuple, keys array, ascending flag)
    struct SortedEntry {
        Tuple tuple;
        std::vector<Value> keys;
        std::vector<bool> ascending;
    };

    std::vector<SortedEntry> materialized_;
    std::vector<size_t> sorted_indices_;
    size_t cursor_;
};

}  // namespace sqlcompiler