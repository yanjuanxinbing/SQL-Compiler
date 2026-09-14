#pragma once

#include <unordered_set>

#include "execution/Executor.h"
#include "storage_engine/Tuple.h"

namespace sqlcompiler {

// 去重算子：从子算子逐条拉取 Tuple，仅向上返回第一次出现的不同 Tuple。
// 对应 SELECT DISTINCT。
//
// ProjectExecutor 把 `[select_values ++ underlying_tuple]` 整体下传，下游 Sort
// 借 underlying 列解析未投影的 ORDER BY 列；Distinct 必须只按"投影列"哈希，
// 否则同一 dept 的两行因为 id/name 不同而永远不被识别为重复。
// `distinct_column_count` 表示"参与去重的前缀列数"，传 0 表示"整行参与去重"
// （AggregateExecutor 的输出天然只有聚合结果列，没有 underlying）。
class DistinctExecutor : public Executor {
public:
    DistinctExecutor(ExecutionContext* context, ExecutorPtr child,
                     size_t distinct_column_count = 0);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    ExecutorPtr child_;
    std::unordered_set<std::string> seen_;
    size_t distinct_column_count_;
};

}  // namespace sqlcompiler