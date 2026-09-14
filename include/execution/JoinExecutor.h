#pragma once

#include <queue>
#include <unordered_map>
#include <vector>

#include "ast/AST.h"
#include "execution/Executor.h"
#include "execution/ExpressionEvaluator.h"
#include "storage_engine/Tuple.h"

namespace sqlcompiler {

// 连接算子（嵌套循环实现）：对左右两个子算子做 INNER/LEFT/RIGHT/FULL OUTER/CROSS JOIN，
// 按 on 条件过滤。对应逻辑计划中的 JoinNode。
// 注：输出 Tuple 是左右元组的拼接（先左后右），列下标为左表列数 + 右表列下标。
class JoinExecutor : public Executor {
public:
    JoinExecutor(ExecutionContext* context, ExecutorPtr left, ExecutorPtr right,
                 JoinType join_type, ExprPtr condition,
                 std::unordered_map<std::string, size_t> column_index_map);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    ExecutorPtr left_;
    ExecutorPtr right_;
    JoinType join_type_;
    ExprPtr condition_;
    std::unordered_map<std::string, size_t> column_index_map_;

    // Materialized inputs (nested loop requires two-pass)
    std::vector<Tuple> left_buffer_;
    std::vector<Tuple> right_buffer_;

    // RIGHT JOIN / FULL OUTER JOIN: pre-collect matched flags for right tuples
    std::vector<bool> right_matched_;
    // FULL OUTER JOIN: pre-collect matched flags for left tuples
    std::vector<bool> left_matched_;

    size_t li_;
    size_t ri_;
    bool cur_left_pushed_;  // LEFT JOIN / FULL OUTER: whether current left tuple has been emitted
};

}  // namespace sqlcompiler