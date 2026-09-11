#pragma once

#include <memory>
#include <unordered_set>

#include "execution/Executor.h"
#include "storage_engine/Tuple.h"

namespace sqlcompiler {

// 集合运算算子：UNION / UNION ALL / INTERSECT / EXCEPT。
//
// 实现策略（Volcano / Iterator Model 兼容）：
//   - UNION ALL: 直接把 RHS 接到 LHS 之后，不需要中间缓冲；
//   - UNION    : LHS 全部去重输出，再逐个喂 RHS；遇到 RHS 中与 LHS 已输出重复的行则跳过。
//   - INTERSECT: 先把 RHS 全部物化为「集合视图」并去重，再扫描 LHS，仅保留出现在 RHS 中的行。
//   - EXCEPT   : 先把 RHS 全部物化为「集合视图」并去重，再扫描 LHS，跳过出现在 RHS 中的行。
//
// 去重以「每列 ToString + 单元分隔符」的拼接做键，类型系统保证了 NULL 走单独分支
// （ToString 返回 "NULL"），不会与字符串 "NULL" 之外的字面值冲突。
class SetOpExecutor : public Executor {
public:
    SetOpExecutor(ExecutionContext* context, ExecutorPtr left, ExecutorPtr right,
                  std::string kind);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    // 把当前 Tuple 的内容编码成 hash key。
    static std::string TupleKey(const Tuple& t);

    ExecutorPtr left_;
    ExecutorPtr right_;
    std::string kind_;  // "UNION" / "UNION ALL" / "INTERSECT" / "EXCEPT"

    // UNION 的 LHS 输出过的集合视图；用于在 RHS 阶段过滤重复。
    std::unordered_set<std::string> seen_union_;
    // INTERSECT / EXCEPT 中 RHS 的去重集合视图。
    std::unordered_set<std::string> right_set_;
    bool right_set_built_ = false;

    // UNION: 当前正在消费的是哪一侧
    enum class Phase { kLeft, kRight };
    Phase phase_ = Phase::kLeft;
};

}  // namespace sqlcompiler
