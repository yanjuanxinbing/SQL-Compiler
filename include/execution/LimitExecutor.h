#pragma once

#include "execution/Executor.h"

namespace sqlcompiler {

// LIMIT 算子：从子算子拉取前 limit_count 条 Tuple（跳过前 offset 个）。
// 对应逻辑计划中的 LimitNode。
class LimitExecutor : public Executor {
public:
    LimitExecutor(ExecutionContext* context, ExecutorPtr child,
                  int limit_count, int offset = 0);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    ExecutorPtr child_;
    int limit_count_;
    int offset_;
    int emitted_;
    int skipped_;
};

}  // namespace sqlcompiler