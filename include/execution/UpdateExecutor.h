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

// 更新算子（可选扩展语法 UPDATE）：顺序扫描目标表，
// 对满足predicate的记录按assignments计算新值后写回，
// 对应逻辑计划中的 UpdateNode
//
// 54_dml: RETURNING 支持——返回 post-image（更新后的行）。
class UpdateExecutor : public Executor {
public:
    UpdateExecutor(ExecutionContext* context, std::string table_name,
                    std::vector<std::pair<std::string, ExprPtr>> assignments, ExprPtr predicate,
                    std::unordered_map<std::string, size_t> column_index_map,
                    std::vector<ExprPtr> returning_exprs = {},
                    std::vector<std::string> returning_aliases = {});

    void Init() override;
    // 无 RETURNING 时返回一次（受影响行数）。
    // 有 RETURNING 时按顺序 emit 每条更新后的 RETURNING 行；emit 完毕后再调一次
    // Next 返回 false。
    bool Next(Tuple* tuple) override;

private:
    std::string table_name_;
    std::vector<std::pair<std::string, ExprPtr>> assignments_;
    ExprPtr predicate_;  // 可为空
    std::unordered_map<std::string, size_t> column_index_map_;
    std::vector<ExprPtr> returning_exprs_;
    std::vector<std::string> returning_aliases_;

    TableHeap* table_heap_;
    std::vector<ValueType> column_types_;
    std::unique_ptr<TableHeap::Iterator> iterator_;
    bool executed_;
    // 54_dml: RETURNING 缓冲。
    std::vector<Tuple> pending_returning_;
    size_t pending_pos_ = 0;
};

// 54_dml: UPDATE ... FROM 算子。
//
// 走 PG/Oracle 风格：children[0] 是 Planner 拼出的 join 子计划，每行 tuple 是
// "target × from_sources"的左深拼接结果。target 列在前；后续 from_sources
// 按 Planner 中的 join 顺序追加。对每行 joined tuple：
//   1) 在 joined tuple 上评估 where_clause（含连接 + 过滤）；
//   2) 取 target 那段（joined tuple 前 N 列，N = target 表列数）；
//   3) 在 joined tuple 上评估 SET 右侧表达式（支持 target 与 source 列混用）；
//   4) 约束 / 索引 / WAL 等更新路径与 UpdateExecutor 完全一致。
// RETURNING emit 与 UpdateExecutor 共享。
class UpdateFromExecutor : public Executor {
public:
    UpdateFromExecutor(ExecutionContext* context, std::string table_name,
                       std::string target_alias,
                       std::vector<std::pair<std::string, ExprPtr>> assignments,
                       ExprPtr predicate,
                       std::unordered_map<std::string, size_t> target_column_index_map,
                       std::unordered_map<std::string, size_t> combined_column_index_map,
                       ExecutorPtr join_child,
                       std::vector<ExprPtr> returning_exprs,
                       std::vector<std::string> returning_aliases);

    void Init() override;
    // 反复 Next 直到 join 子计划耗尽；每次产出一条 "RETURNING 行" 或 false。
    // 该执行器与 ExecutionEngine 的「RETURNING 走 query 路径」约定对齐：详见
    // src/execution/ExecutionEngine.cpp 中 is_query 的判定逻辑。
    bool Next(Tuple* tuple) override;

private:
    std::string table_name_;
    std::string target_alias_;
    std::vector<std::pair<std::string, ExprPtr>> assignments_;
    ExprPtr predicate_;  // 可为空
    std::unordered_map<std::string, size_t> target_column_index_map_;
    std::unordered_map<std::string, size_t> combined_column_index_map_;
    ExecutorPtr join_child_;
    std::vector<ExprPtr> returning_exprs_;
    std::vector<std::string> returning_aliases_;

    // 表堆、列类型缓存（Init 时填充）。
    TableHeap* table_heap_ = nullptr;
    std::vector<ValueType> column_types_;
    size_t target_column_count_ = 0;

    // RETURNING 缓冲：每次从 join_child 读到一行匹配 WHERE 的目标行，
    // 先评估 SET 写回堆，再把评估后的 returning_exprs 序列化为 Tuple 暂存。
    // 下一轮 Next 调用时优先返回缓冲行；缓冲空才再驱动 join_child。
    std::vector<Tuple> pending_returning_;
    size_t pending_pos_ = 0;

    // 当前 join_child 行：评估 SET / WHERE 时使用。
    Tuple current_joined_;
    bool has_current_ = false;
};

}  // namespace sqlcompiler
