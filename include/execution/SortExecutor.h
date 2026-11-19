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
    // Item #13 (perf): 在 Init() 中把每条 SortedEntry 的 ascending[i] 提
    // 取为成员字段（O(K)），比较器仅直接读 this->materialized_[a].ascending[i]，
    // 省掉 std::vector<bool>::operator[] 的间接访问。
    std::vector<bool> ascending_;  // per-key 升序标志；多个 order_items 共享。

public:
    // Item #3 (perf)：ApplyExecutor 探测相关性时读取 order_items。
    const std::vector<OrderByItem>& order_items_for_scan() const { return order_items_; }

};

}  // namespace sqlcompiler