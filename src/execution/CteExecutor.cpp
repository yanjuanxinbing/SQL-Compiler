#include "execution/CteExecutor.h"

#include "execution/ExecutionEngine.h"

#include <algorithm>

namespace sqlcompiler {

namespace {

// 在给定 ExecutionContext 上把子计划跑完，返回所有结果行。
// 复用 ExecutionEngine::ExecuteSubplan，与子查询走同一路径。
// strip_extra_cols > 0 时，只保留每行前 N 个值（去掉 ProjectExecutor 追加的
// underlying tuple 列），供递归 CTE 迭代把每轮的 delta 与累计结果约束到
// "anchor 选择的列数"上，避免下一轮 join 时 cmap 与实际元组宽度不一致。
std::vector<Tuple> MaterializePlan(ExecutionContext* ctx, const PlanNodePtr& plan,
                                    size_t strip_extra_cols = 0) {
    std::vector<Tuple> rows;
    if (!ctx || !plan) return rows;
    ExecutionEngine engine(ctx->GetCatalog());
    ExecutionResult r = engine.ExecuteSubplan(plan, ctx);
    if (r.success) rows = std::move(r.rows);
    if (strip_extra_cols > 0) {
        std::vector<Tuple> trimmed;
        trimmed.reserve(rows.size());
        for (auto& t : rows) {
            size_t keep = std::min(strip_extra_cols, t.ColumnCount());
            std::vector<Value> v;
            v.reserve(keep);
            for (size_t i = 0; i < keep; ++i) v.push_back(t.GetValue(i));
            trimmed.emplace_back(std::move(v));
        }
        rows.swap(trimmed);
    }
    return rows;
}

// 递归 CTE：cte_plan = anchor；anchor_child = 递归 SELECT 计划。
// 实现策略：
//   1. 跑一次 anchor，得到初始工作集 R0，注册到 cte_results_[name]。
//   2. 每轮迭代：把 R_{i-1} 推到 cte_overrides_ 上（让递归 SELECT 内的
//      CTE_BIND 只看到上一轮 delta），跑递归 SELECT 拿到 R_i。
//      pop override 后把 R_i 追加到 cte_results_[name]。
//   3. 直到 R_i 为空或达到深度上限。
constexpr int kRecursiveDepthLimit = 1000;

}  // namespace

CteDefineExecutor::CteDefineExecutor(ExecutionContext* context, CteDefineNode* node)
    : Executor(context), node_(node), emitted_(false) {
}

void CteDefineExecutor::Init() {
    if (emitted_) return;
    emitted_ = true;
    if (!node_) return;

    if (!node_->is_recursive) {
        // 普通 CTE：先物化 cte_plan 把结果注册到 context，然后构造 body
        if (node_->cte_plan) {
            auto rows = MaterializePlan(context_, node_->cte_plan);
            context_->RegisterCte(node_->cte_name, std::move(rows));
        }
        if (!node_->children.empty()) {
            ExecutionEngine engine(context_->GetCatalog());
            body_ = engine.BuildExecutor(node_->children[0], context_);
            if (body_) body_->Init();
        }
        return;
    }

    // 递归 CTE：cte_plan = anchor，anchor_child = 递归部分。
    if (!node_->cte_plan) return;
    auto anchor_rows = MaterializePlan(context_, node_->cte_plan);
    // 累计行（外层 SELECT 最终读这份）：初始 = anchor_rows
    context_->RegisterCte(node_->cte_name, anchor_rows);

    // 没有递归部分时退化为 anchor-only（旧语义；保留兼容）。
    if (!node_->anchor_child) {
        if (!node_->children.empty()) {
            ExecutionEngine engine(context_->GetCatalog());
            body_ = engine.BuildExecutor(node_->children[0], context_);
            if (body_) body_->Init();
        }
        return;
    }

    // 推算每轮递归 SELECT 的输出列数：取 SelectStatement 的 select_list.size()
    // 作为保留前 N 列的依据（ProjectExecutor 会把 underlying tuple 也拼到输出
    // 元组尾部，需要截掉以保证下一轮 CteBindExecutor 返回的元组宽度与 cmap 一致）。
    size_t keep_cols = 0;
    {
        PlanNodePtr p = node_->anchor_child;
        while (p && (p->GetType() == PlanNodeType::SORT ||
                     p->GetType() == PlanNodeType::LIMIT)) {
            if (p->children.empty()) break;
            p = p->children[0];
        }
        if (p && p->GetType() == PlanNodeType::PROJECT) {
            keep_cols = std::static_pointer_cast<ProjectNode>(p)->columns.size();
        } else if (p && p->GetType() == PlanNodeType::AGGREGATE) {
            keep_cols = std::static_pointer_cast<AggregateNode>(p)->aggregate_exprs.size();
        } else if (p && p->GetType() == PlanNodeType::WINDOW) {
            keep_cols = std::static_pointer_cast<WindowNode>(p)->select_list.size();
        }
    }
    // fallback：从累计结果第一行的宽度推断（取其一半，假设 Project
    // 把等长的 SELECT 值与 underlying 拼起来；若 anchor 也是 Project-over-Scan
    // 的简单形态，宽度 = select_cols + scan_cols，select_cols = width/2 仅当
    // scan_cols == select_cols）。这里只是粗略推断，下游 CteBind 不要求严格。
    if (keep_cols == 0 && !anchor_rows.empty()) {
        size_t w = anchor_rows.front().ColumnCount();
        keep_cols = w / 2;
        if (keep_cols == 0) keep_cols = w;
    }

    // 迭代：用 anchor_rows 作为本轮"可见工作集"，每轮把上一轮的 delta
    // 推到 cte_overrides_ 上，让递归 SELECT 内的 CTE_BIND 只看到 delta；
    // 跑完一轮后 pop override，把本轮新行追加到累计结果。
    std::vector<Tuple> delta = context_->GetCteRows(node_->cte_name)
        ? std::vector<Tuple>(*context_->GetCteRows(node_->cte_name))
        : std::vector<Tuple>{};

    for (int iter = 0; iter < kRecursiveDepthLimit; ++iter) {
        if (delta.empty()) break;
        // 把 delta 推到 override：让递归 SELECT 看到本轮的 delta。
        context_->PushCteOverride(node_->cte_name, delta);
        std::vector<Tuple> new_rows;
        try {
            new_rows = MaterializePlan(context_, node_->anchor_child, keep_cols);
        } catch (...) {
            context_->PopCteOverride(node_->cte_name);
            throw;
        }
        context_->PopCteOverride(node_->cte_name);
        if (new_rows.empty()) break;
        // 追加到累计结果
        context_->AppendCteRows(node_->cte_name, new_rows);
        delta = std::move(new_rows);
    }

    // 构造 body 并初始化
    if (!node_->children.empty()) {
        ExecutionEngine engine(context_->GetCatalog());
        body_ = engine.BuildExecutor(node_->children[0], context_);
        if (body_) body_->Init();
    }
}

bool CteDefineExecutor::Next(Tuple* tuple) {
    if (!body_) return false;
    return body_->Next(tuple);
}

// -------- CteBindExecutor --------

CteBindExecutor::CteBindExecutor(ExecutionContext* context, std::string cte_name)
    : Executor(context), cte_name_(std::move(cte_name)), cursor_(0) {
}

void CteBindExecutor::Init() {
    cursor_ = 0;
}

bool CteBindExecutor::Next(Tuple* tuple) {
    if (cte_name_.empty() || !context_->HasCte(cte_name_)) return false;
    const auto* rows = context_->GetCteRows(cte_name_);
    if (!rows) return false;
    if (cursor_ >= rows->size()) return false;
    if (tuple) *tuple = (*rows)[cursor_];
    ++cursor_;
    return true;
}

}  // namespace sqlcompiler
