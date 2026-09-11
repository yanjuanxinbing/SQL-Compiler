#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ast/AST.h"
#include "execution/Executor.h"
#include "storage_engine/Tuple.h"

namespace sqlcompiler {

// 窗口算子：消费子算子的全部 Tuple，对 SELECT 列表中的 WindowFuncNode
// 求值；对非 Window 的表达式继续按行内 Tuple 求值。子算子本身可能是
// SeqScan / Filter / Aggregate（聚合 over 聚合时）。
class WindowExecutor : public Executor {
public:
    WindowExecutor(ExecutionContext* context, ExecutorPtr child,
                   std::vector<ExprPtr> select_list,
                   std::vector<std::string> select_aliases,
                   std::unordered_map<std::string, size_t> column_index_map,
                   std::vector<std::pair<std::string, WindowSpec>> named_windows);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    ExecutorPtr child_;
    std::vector<ExprPtr> select_list_;
    std::vector<std::string> select_aliases_;
    std::unordered_map<std::string, size_t> column_index_map_;
    std::vector<std::pair<std::string, WindowSpec>> named_windows_;

    struct WindowKey {
        // 用于将行分组到分区的 PARTITION BY 值
        std::vector<Value> partition_values;
        // 用于在分区内排序的 ORDER BY 值
        std::vector<Value> order_values;
        std::vector<bool> order_ascending;
    };

    struct Partition {
        std::vector<Value> partition_values;
        // 排序后的行索引（在 materialized_ 中的下标）
        std::vector<size_t> ordered_indices;
    };

    // 已物化的子算子输出
    std::vector<Tuple> materialized_;
    // 每一行对应的 partition 引用（在 partitions_ 中的下标）
    std::vector<size_t> row_partition_;

    // 唯一的 partition 列表
    std::vector<Partition> partitions_;

    // 计算单个行的 window function 值。
    Value EvaluateWindowFunc(const WindowFuncNode& wf,
                             size_t row_index_in_partition,
                             const Partition& partition);

    // 解析 named window 引用或直接使用 spec
    WindowSpec ResolveSpec(const WindowFuncNode& wf) const;

    // 评估 WindowSpec.partition_by 与 order_by 表达式（使用全局 column_index_map）
    std::vector<Value> EvalExprList(const std::vector<ExprPtr>& exprs,
                                     const Tuple& tuple);

    // 在分区内计算 window function（行索引在 partition.ordered_indices 内的位置 pos）
    Value ComputeWindowValue(const std::string& func_name,
                             const std::vector<ExprPtr>& args,
                             const WindowSpec& spec,
                             const Partition& partition,
                             size_t pos);

    // 计算 frame 在分区内的实际 [start, end] 索引区间
    void ComputeFrame(const WindowSpec& spec,
                      const Partition& partition,
                      size_t current_pos,
                      bool has_order_by,
                      size_t* out_start,
                      size_t* out_end) const;

    // 单个窗口函数表达式是否为 window 列
    static bool IsWindowExpr(const ExprPtr& e);

    size_t cursor_;
};

}  // namespace sqlcompiler
