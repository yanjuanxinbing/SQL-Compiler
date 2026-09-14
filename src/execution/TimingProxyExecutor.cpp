#include "execution/TimingProxyExecutor.h"

#include <utility>

namespace sqlcompiler {

// 线程局部注册表：默认 nullptr。ExplainExecutor 在 EXPLAIN ANALYZE 路径
// 进入 BuildExecutor 前用 RAII 把它设为自己持有的 map，构造完后再清空。
std::unordered_map<const PlanNode*, TimingProxyExecutor*>*
    TimingProxyExecutor::s_registry_ = nullptr;

TimingProxyExecutor::TimingProxyExecutor(ExecutionContext* context,
                                         ExecutorPtr inner,
                                         const PlanNode* plan_node)
    : Executor(context),
      inner_(std::move(inner)),
      plan_node_(plan_node) {
    // label 用 plan_node 的 ToString() 单行表示 —— 与 EXPLAIN 文本输出对齐。
    // 对 ProjectNode 等会产生多行输出的节点，去掉多余换行/缩进保持单行紧凑。
    if (plan_node_) {
        label_ = plan_node_->ToString();
        // 去掉 ToString 可能带出的尾部空白 / 换行，便于后面拼装成一行。
        while (!label_.empty() &&
               (label_.back() == '\n' || label_.back() == ' ')) {
            label_.pop_back();
        }
    }
    if (s_registry_ && plan_node_) {
        (*s_registry_)[plan_node_] = this;
    }
}

void TimingProxyExecutor::Init() {
    if (!inner_) return;
    auto start = std::chrono::steady_clock::now();
    inner_->Init();
    auto end = std::chrono::steady_clock::now();
    init_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end - start)
                    .count();
}

bool TimingProxyExecutor::Next(Tuple* tuple) {
    if (!inner_) return false;
    ++loops_;
    auto start = std::chrono::steady_clock::now();
    bool ok = inner_->Next(tuple);
    auto end = std::chrono::steady_clock::now();
    next_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end - start)
                    .count();
    if (ok) ++rows_produced_;
    return ok;
}

}  // namespace sqlcompiler
