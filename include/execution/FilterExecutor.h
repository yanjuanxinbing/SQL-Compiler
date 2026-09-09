#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "ast/AST.h"
#include "execution/Executor.h"

namespace sqlcompiler {

// 过滤算子：从子算子逐条拉取Tuple，仅向上返回满足predicate条件的记录，
// 对应逻辑计划中的 FilterNode（WHERE / HAVING子句）
class FilterExecutor : public Executor {
public:
    FilterExecutor(ExecutionContext* context, ExecutorPtr child, ExprPtr predicate,
                   std::unordered_map<std::string, size_t> column_index_map);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    ExecutorPtr child_;
    ExprPtr predicate_;
    std::unordered_map<std::string, size_t> column_index_map_;
};

}  // namespace sqlcompiler
