#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ast/AST.h"
#include "execution/Executor.h"

namespace sqlcompiler {

// 投影算子：从子算子拉取Tuple，按select_list计算出新的输出列，
// 对应逻辑计划中的 ProjectNode（SELECT 列表）
class ProjectExecutor : public Executor {
public:
    ProjectExecutor(ExecutionContext* context, ExecutorPtr child,
                     std::vector<ExprPtr> select_list,
                     std::unordered_map<std::string, size_t> column_index_map,
                     std::vector<std::string> aliases = {});

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    ExecutorPtr child_;
    std::vector<ExprPtr> select_list_;
    std::unordered_map<std::string, size_t> column_index_map_;
    std::vector<std::string> aliases_;  // 与 select_list 平行，可空
    bool has_emitted_;
};

}  // namespace sqlcompiler
