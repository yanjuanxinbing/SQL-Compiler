#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "execution/Executor.h"
#include "plan/Plan.h"
#include "storage_engine/Tuple.h"
#include "storage_engine/Value.h"

namespace sqlcompiler {

// 前向声明：避免在头文件暴露 TimingProxyExecutor.h。
class TimingProxyExecutor;

// EXPLAIN 算子：
//   1) 若 analyze==false，按 children[0] 的 PlanNodePtr 渲染为缩进式文本（TEXT/
//      JSON/SEXPR 之一），结果装进单列 "plan" 行返回；
//   2) 若 analyze==true：
//        - 实际驱动 children[0] 的执行器跑一遍（用 TimingProxyExecutor 收集
//          per-node Init/Next 时长与行数）；
//        - 出错时打印已访问节点的「部分统计」并把异常抛出，由 ExecutionEngine
//          顶层捕获（保持普通 EXPLAIN 的失败传播路径）；
//        - 正常完成后把计划树按 TEXT/JSON/SEXPR 之一渲染，每个节点后追加
//          (rows=N time=NNN.NNNms) 注释。
//
// 与任务文档"EXPLAIN 输出格式"约定一致：
//   - 子节点按 2 空格缩进；
//   - 节点单行呈现（header + 可选注释）；
//   - 最终落到一个名为 "plan" 的单列结果集，便于与其它查询共用 CLI 表格输出。
class ExplainExecutor : public Executor {
public:
    ExplainExecutor(ExecutionContext* context, ExplainNode* node);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    ExplainNode* node_;
    bool produced_;

    // ANALYZE 渲染时使用的辅助结构：把 plan_node_ → TimingProxyExecutor 映射
    // 缓存在成员里，避免每次 Next 调用都重新建表。Init() 中按需填充。
    std::unordered_map<const PlanNode*, TimingProxyExecutor*> proxy_map_;
    std::string analyze_failure_msg_;  // Init/Next 期间发生的失败信息（向 Next 传播）

    // ANALYZE 渲染实现：
    //   1) 调用 ExecutionEngine 重新构建 children[0] 子树（wrap_timing=true）；
    //   2) 通过 TimingProxyExecutor 注册表建立 plan→proxy 映射；
    //   3) 驱动 Init + 反复 Next 至 EOF（或失败抛出）；
    //   4) 按 format 选择序列化方式，对每个节点追加 (rows=N time=Xms) 注释。
    std::string RenderAnalyze(const std::string& format,
                              const PlanNodePtr& inner,
                              bool& had_error);
};

}  // namespace sqlcompiler
