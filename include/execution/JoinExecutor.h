#pragma once

#include <queue>
#include <unordered_map>
#include <vector>

#include "ast/AST.h"
#include "execution/Executor.h"
#include "execution/ExpressionEvaluator.h"
#include "storage_engine/Tuple.h"

namespace sqlcompiler {

// 连接算子（默认嵌套循环；INNER JOIN 在等值谓词下走 O(N+M) 哈希连接）：
// 对左右两个子算子做 INNER/LEFT/RIGHT/FULL OUTER/CROSS JOIN，
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

    // Item #4 (perf)：hash join 状态。
    bool use_hash_join_ = false;
    // 探测侧的当前左行下标 / 当前 hash bucket 内已发射的右行位置
    size_t hj_probe_idx_ = 0;
    size_t hj_cur_bucket_pos_ = 0;
    std::vector<size_t> hj_cur_bucket_;  // 当前 hash bucket 内的右行下标
    std::vector<size_t> hj_probe_unmatched_;  // 未匹配的左行下标（hash join 不维护）
    // hash 表：key_str -> [build 侧行下标列表]。hash join 在较小侧构建。
    std::unordered_map<std::string, std::vector<size_t>> hj_hash_;
    // 探测列（probe 侧 join key 在 probe tuple 里的列下标）。
    size_t hj_probe_left_col_ = 0;
    // 构建列（build 侧 join key 在 build tuple 里的列下标）。
    size_t hj_build_right_col_ = 0;
    // 探测侧是否为左表：true → probe=left_buffer_，build=right_buffer_；
    // false → probe=right_buffer_，build=left_buffer_。
    bool hj_probe_is_left_ = true;
};

}  // namespace sqlcompiler