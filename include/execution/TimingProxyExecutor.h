#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "execution/Executor.h"
#include "plan/Plan.h"
#include "storage_engine/Tuple.h"

namespace sqlcompiler {

// TimingProxyExecutor —— EXPLAIN ANALYZE 专用代理算子。
//
// 设计目标：
//   - 不修改 Executor 基类、不引入跨算子的 telemetry 字段。
//   - 在 BuildExecutor 中按 `wrap_timing=true` 标志包到每个真实算子外层，
//     拦截 Init() / Next() 调用并累加墙钟时间与行数。
//   - 通过 thread_local 注册表，把「计划节点指针 → Proxy」绑定起来，让
//     ExplainExecutor 在格式化输出时按计划树并行回查每节点统计。
//
// 注意：
//   - Proxy 的 init_ns_/next_ns_ 包含被包装算子内部所有耗时（含其子 Proxy）。
//     因为每次 `inner_->Next(...)` 都把下游整条调用链的耗时一并吃进来，
//     所以「自顶向下」遍历 Proxy 树会重复计算同一段执行时间。
//     使用方（ExplainExecutor）只按「计划节点 ↔ Proxy」一对一展示耗时，
//     不再做求和，因此重复计算不会反映到用户可见输出上。
class TimingProxyExecutor : public Executor {
public:
    TimingProxyExecutor(ExecutionContext* context, ExecutorPtr inner,
                        const PlanNode* plan_node);

    void Init() override;
    bool Next(Tuple* tuple) override;

    // ---- 统计查询接口 ----
    int64_t rows_produced() const { return rows_produced_; }
    int64_t loops() const { return loops_; }
    double init_ms() const {
        return static_cast<double>(init_ns_) / 1e6;
    }
    double next_ms() const {
        return static_cast<double>(next_ns_) / 1e6;
    }
    double total_ms() const {
        return static_cast<double>(init_ns_ + next_ns_) / 1e6;
    }
    const std::string& label() const { return label_; }
    Executor* inner() const { return inner_.get(); }

    // 线程局部注册表：构造时若非空，把自己登记进去；供 ExplainExecutor 在
    // BuildExecutor 之后按 PlanNode* 查找每节点统计。线程局部保证并发安全
    // （当前 SQL-Compiler 单线程执行，注册表主要是为「不污染其它调用」）。
    //
    // 通过 Save/Restore 访问，避免把内部状态公开为 public（保留封装性）。
    struct RegistrySlot {
        std::unordered_map<const PlanNode*, TimingProxyExecutor*>* prev;
        RegistrySlot(std::unordered_map<const PlanNode*, TimingProxyExecutor*>* r)
            : prev(s_registry_) {
            s_registry_ = r;
        }
        ~RegistrySlot() { s_registry_ = prev; }
    };

    static std::unordered_map<const PlanNode*, TimingProxyExecutor*>* PeekRegistry() {
        return s_registry_;
    }

private:
    ExecutorPtr inner_;
    const PlanNode* plan_node_;  // not owned; 仅作注册表 key 用
    std::string label_;
    int64_t rows_produced_ = 0;
    int64_t loops_ = 0;
    int64_t init_ns_ = 0;
    int64_t next_ns_ = 0;

    static std::unordered_map<const PlanNode*, TimingProxyExecutor*>*
        s_registry_;
};

}  // namespace sqlcompiler
