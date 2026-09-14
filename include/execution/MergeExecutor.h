#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ast/AST.h"
#include "execution/Executor.h"
#include "storage_engine/TableHeap.h"

namespace sqlcompiler {

// 54_dml：MERGE INTO 算子。
//
// 语义（SQL:2003 标准 + PG/Oracle 简化）：
//   对 source 的每一行：
//     - 用 ON 条件与 target 做匹配（cross product + WHERE 过滤）；
//     - 命中任一 target 行 → 执行 WHEN MATCHED THEN UPDATE SET（取第一命中即可；
//       多 MATCHED 行的处理在 V1 范围外，PG/Oracle 同样要求 ON 唯一匹配）；
//     - 未命中任何 target 行 → 执行 WHEN NOT MATCHED THEN INSERT。
//   约束 / 索引 / WAL 路径与既有 UpdateExecutor / InsertExecutor 完全一致。
//
// children[0]：source_plan（由 Planner 提供的子计划，可能是 SeqScanNode 或
//              包装 (SELECT ...) 的递归子计划）。
class MergeExecutor : public Executor {
public:
    MergeExecutor(ExecutionContext* context,
                  std::string target_table,
                  std::string target_alias,
                  std::string source_alias,
                  ExprPtr on_condition,
                  std::unordered_map<std::string, size_t> target_column_index_map,
                  std::unordered_map<std::string, size_t> combined_column_index_map,
                  ExecutorPtr source_child,
                  bool has_matched_update,
                  std::vector<std::pair<std::string, ExprPtr>> matched_assignments,
                  bool has_not_matched_insert,
                  std::vector<std::string> not_matched_columns,
                  std::vector<ExprPtr> not_matched_values);

    void Init() override;
    // 单条 source 行最多影响一次目标表（MATCHED 取第一命中后停止；NOT MATCHED
    // 执行一次 INSERT）；每个 source 行处理完后 Next 返回 true（emit 一行 "OK"）。
    // 全部 source 行处理完后 Next 返回 false。
    bool Next(Tuple* tuple) override;

private:
    std::string target_table_;
    std::string target_alias_;
    std::string source_alias_;
    ExprPtr on_condition_;
    std::unordered_map<std::string, size_t> target_column_index_map_;
    std::unordered_map<std::string, size_t> combined_column_index_map_;
    ExecutorPtr source_child_;
    bool has_matched_update_;
    std::vector<std::pair<std::string, ExprPtr>> matched_assignments_;
    bool has_not_matched_insert_;
    std::vector<std::string> not_matched_columns_;
    std::vector<ExprPtr> not_matched_values_;

    TableHeap* target_heap_ = nullptr;
    std::vector<ValueType> target_column_types_;
    size_t target_column_count_ = 0;

    bool executed_ = false;
    int affected_rows_ = 0;
};

}  // namespace sqlcompiler
