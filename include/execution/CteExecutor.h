#pragma once

#include <memory>
#include <string>
#include <vector>

#include "execution/Executor.h"
#include "plan/Plan.h"

namespace sqlcompiler {

// CTE 定义节点执行器：跑一次 children[0]，把结果行收集到 ExecutionContext。
// 对应 CteDefineNode：is_recursive=true 时由"工作集+递归 SELECT"驱动，
// 每轮迭代把新行追加到 CTE 累计行，并把本轮 delta 作为本轮"可见行"。
class CteDefineExecutor : public Executor {
public:
    CteDefineExecutor(ExecutionContext* context, CteDefineNode* node);
    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    CteDefineNode* node_;
    bool emitted_;
    ExecutorPtr body_;
};

// CTE 引用节点执行器：从 ExecutionContext 的 cte_results_[name] 中按行发射。
// 对应 CteBindNode。直接持有 cte_name 字符串，不依赖 CteBindNode 的生命周期，
// 便于在 ExecutionEngine 端按值构造，避免 shared_ptr 生命周期问题。
//
// ---- 31_cte bug fix: 可选 predicate + column_index_map ----
// Optimizer::PushDownPredicates 把 `WHERE total >= 200` 这类谓词下沉到 CTE
// 引用位置的 SeqScanNode 上。ExecutionEngine 在 BuildExecutor 时把这类
// SeqScanNode 改写成 CteBindExecutor，原本下推的 predicate 必须在这里继续
// 生效——否则 CTE 上「外层 WHERE」会被静默丢弃（参考 test 31_cte.sql 第
// 23–28 行）。column_index_map 用 cte_results_[name].column_names 构建，
// 让 ExpressionEvaluator 能解析 `total` 这种限定到 CTE 输出列的引用。
class CteBindExecutor : public Executor {
public:
    CteBindExecutor(ExecutionContext* context, std::string cte_name,
                    ExprPtr predicate = nullptr,
                    std::unordered_map<std::string, size_t> column_index_map = {});
    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    std::string cte_name_;
    size_t cursor_;
    ExprPtr predicate_;
    std::unordered_map<std::string, size_t> column_index_map_;
};

}  // namespace sqlcompiler