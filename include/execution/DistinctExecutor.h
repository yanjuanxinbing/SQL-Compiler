#pragma once

#include <unordered_set>

#include "execution/Executor.h"
#include "storage_engine/Tuple.h"

namespace sqlcompiler {

// 去重算子：从子算子逐条拉取 Tuple，仅向上返回第一次出现的不同 Tuple。
// 对应 SELECT DISTINCT。
class DistinctExecutor : public Executor {
public:
    DistinctExecutor(ExecutionContext* context, ExecutorPtr child);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    ExecutorPtr child_;
    std::unordered_set<std::string> seen_;
};

}  // namespace sqlcompiler