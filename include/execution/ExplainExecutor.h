#pragma once

#include <memory>
#include <string>
#include <vector>

#include "execution/Executor.h"
#include "plan/Plan.h"
#include "storage_engine/Tuple.h"
#include "storage_engine/Value.h"

namespace sqlcompiler {

// EXPLAIN 算子：
//   1) 若 analyze==true，打印一行 "EXPLAIN ANALYZE not supported"，不打印计划树。
//   2) 否则把 children[0] 的 PlanNodePtr 渲染为缩进式文本，按单列结果集返回：
//        plan
//        ----
//        <text>
//
// 与任务文档"EXPLAIN 输出格式"约定一致：
//   - 子节点按 2 空格缩进；
//   - 节点单行呈现（header + children 链在一起）；
//   - 最终落到一个名为 "plan" 的单列结果集，便于与其它查询共用 CLI 表格输出。
class ExplainExecutor : public Executor {
public:
    ExplainExecutor(ExecutionContext* context, ExplainNode* node);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    ExplainNode* node_;
    bool produced_;
};

}  // namespace sqlcompiler
