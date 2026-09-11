// ============ EXPLAIN 算子：打印计划树 ============
//
// 输出格式约定（与任务文档保持一致）：
//   - 顶层节点无缩进；每深一层缩进 +2 空格。
//   - 每个节点单行呈现：<NodeName>(<body>)，body 由 PlanNode::ToString() 决定。
//   - 子节点按 children 顺序依次追加。
//
// EXPLAIN ANALYZE 的支持是任务文档允许的 scope-cut：检测到 analyze==true 时
// 只打印 "EXPLAIN ANALYZE not supported"，不渲染计划树。
//
// 我们把整段文本打包成单列（"plan"）单行的结果集，方便上层 PrintResult
// 直接复用表格输出，无需为 EXPLAIN 单独写一套 I/O。

#include "execution/ExplainExecutor.h"

namespace sqlcompiler {

ExplainExecutor::ExplainExecutor(ExecutionContext* context, ExplainNode* node)
    : Executor(context), node_(node), produced_(false) {
}

void ExplainExecutor::Init() {
    // 一次性把渲染文本算好，存到 result 行里。
}

bool ExplainExecutor::Next(Tuple* tuple) {
    if (produced_) return false;
    produced_ = true;

    std::string text;
    if (node_->analyze) {
        text = "EXPLAIN ANALYZE not supported";
    } else if (!node_->children.empty() && node_->children[0]) {
        text = node_->children[0]->ToString();
        // 去掉 ToString 末尾多余的换行，保持结果单元格内紧凑。
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
            text.pop_back();
        }
    } else {
        text = "(empty plan)";
    }

    if (tuple) {
        *tuple = Tuple({Value::MakeVarchar(text)});
    }
    return true;
}

}  // namespace sqlcompiler
