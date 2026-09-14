#include "execution/CteExecutor.h"

#include "ast/AST.h"
#include "catalog/SystemCatalog.h"
#include "execution/ExecutionEngine.h"
#include "execution/ExpressionEvaluator.h"

#include <algorithm>

namespace sqlcompiler {

namespace {

// 查找子计划里最近的 SEQ_SCAN 节点表名（含 INDEX_SCAN）。与
// ExecutionEngine::DeriveTerminalColumns 中 `*` 展开路径同源，用于
// `SELECT *` / `SELECT t.*` 在 CTE 物化时把 * 映射到底层真实表的列。
std::string FindCteScanTableName(const PlanNodePtr& node) {
    if (!node) return "";
    if (node->GetType() == PlanNodeType::SEQ_SCAN) {
        return std::static_pointer_cast<SeqScanNode>(node)->table_name;
    }
    if (node->GetType() == PlanNodeType::INDEX_SCAN) {
        return std::static_pointer_cast<IndexScanNode>(node)->table_name;
    }
    for (auto& c : node->children) {
        std::string t = FindCteScanTableName(c);
        if (!t.empty()) return t;
    }
    return "";
}

// ---- 31_cte bug fix: CTE 输出列名推导 ----
//
// 从 CTE 的 cte_plan 走到终止节点（Project / Aggregate / Window / Values），
// 按列顺序提取列名。语义与 ExecutionEngine::DeriveTerminalColumns /
// ProjectExecutor 维护的输出列解析一致——优先 alias，回退到表达式自身。
//
// 该函数被 CteDefineExecutor::Init 在调用 RegisterCte 之前调用，确保
// `WHERE total >= 200` 等下推到 CTE SeqScanNode 的谓词在执行期能找到
// 列下标映射。
//
// ---- 83_cte_select_star bug fix: catalog 扩展 * ----
// 接受 SystemCatalog* 后，遇到 Project 列表里的 `*` / `t.*` 通配符时按底层
// 真实表（或派生表内层计划）的列展开。否则 `WITH t AS (SELECT * FROM emp)`
// 的 cols.size() = 1，trim 把 3 列的物化行裁到 1 列，导致外层引用 name/mgr
// 时拿不到数据。
std::vector<std::string> DeriveCteOutputColumns(SystemCatalog* catalog,
                                                const PlanNodePtr& plan) {
    std::vector<std::string> cols;
    if (!plan) return cols;
    // 跳过不影响输出列的包装节点。
    PlanNodePtr p = plan;
    while (p && (p->GetType() == PlanNodeType::SORT ||
                 p->GetType() == PlanNodeType::LIMIT ||
                 p->GetType() == PlanNodeType::FILTER ||
                 p->GetType() == PlanNodeType::SET_OP ||
                 p->GetType() == PlanNodeType::CTE_DEFINE ||
                 p->GetType() == PlanNodeType::CTE_BIND ||
                 p->GetType() == PlanNodeType::JOIN ||
                 p->GetType() == PlanNodeType::WINDOW)) {
        if (p->GetType() == PlanNodeType::WINDOW) break;  // 见下方 WINDOW 分支
        if (p->children.empty()) return cols;
        p = p->children[0];
    }
    if (!p) return cols;
    switch (p->GetType()) {
        case PlanNodeType::PROJECT: {
            auto proj = std::static_pointer_cast<ProjectNode>(p);
            for (size_t i = 0; i < proj->columns.size(); ++i) {
                if (i < proj->aliases.size() && !proj->aliases[i].empty()) {
                    cols.push_back(proj->aliases[i]);
                } else if (proj->columns[i] &&
                           proj->columns[i]->GetType() == NodeType::COLUMN_REF_EXPR) {
                    cols.push_back(
                        std::static_pointer_cast<ColumnRefExpr>(proj->columns[i])
                            ->column_name);
                } else if (proj->columns[i] &&
                           proj->columns[i]->GetType() == NodeType::FUNCTION_CALL_EXPR) {
                    // 83_cte_select_star: `*` / `t.*` 按底层表展开。
                    // `t.*` 在 parser 中表达为 `*` 通配符（table 限定在执行期
                    // 已被 ProjectExecutor 透传时丢弃），这里统一按 catalog 列
                    // 展开。`FunctionCallExpr::function_name` 还覆盖到 `STAR`
                    // 别名（来自某些 grammar 变体）。
                    auto fc = std::static_pointer_cast<FunctionCallExpr>(
                        proj->columns[i]);
                    if (fc->function_name == "*" ||
                        fc->function_name == "STAR") {
                        std::string tname = FindCteScanTableName(p);
                        const TableInfo* info =
                            catalog ? catalog->GetTable(tname) : nullptr;
                        if (info) {
                            for (const auto& c : info->columns) {
                                cols.push_back(c.name);
                            }
                            continue;
                        }
                        // 找不到表时退回函数名（与旧行为兼容，避免空名）。
                        cols.push_back(fc->function_name);
                    } else {
                        cols.push_back(fc->function_name);
                    }
                } else if (proj->columns[i]) {
                    cols.push_back(proj->columns[i]->ToString());
                } else {
                    cols.push_back("col" + std::to_string(i));
                }
            }
            break;
        }
        case PlanNodeType::WINDOW: {
            auto wn = std::static_pointer_cast<WindowNode>(p);
            cols.reserve(wn->select_list.size());
            for (size_t i = 0; i < wn->select_list.size(); ++i) {
                if (i < wn->aliases.size() && !wn->aliases[i].empty()) {
                    cols.push_back(wn->aliases[i]);
                } else if (wn->select_list[i] &&
                           wn->select_list[i]->GetType() ==
                               NodeType::COLUMN_REF_EXPR) {
                    cols.push_back(
                        std::static_pointer_cast<ColumnRefExpr>(
                            wn->select_list[i])
                            ->column_name);
                } else if (wn->select_list[i]) {
                    cols.push_back(wn->select_list[i]->ToString());
                } else {
                    cols.push_back("col" + std::to_string(i));
                }
            }
            break;
        }
        case PlanNodeType::AGGREGATE: {
            auto agg = std::static_pointer_cast<AggregateNode>(p);
            cols.reserve(agg->aggregate_exprs.size());
            for (size_t i = 0; i < agg->aggregate_exprs.size(); ++i) {
                if (i < agg->aliases.size() && !agg->aliases[i].empty()) {
                    cols.push_back(agg->aliases[i]);
                } else if (agg->aggregate_exprs[i] &&
                           agg->aggregate_exprs[i]->GetType() ==
                               NodeType::COLUMN_REF_EXPR) {
                    cols.push_back(
                        std::static_pointer_cast<ColumnRefExpr>(
                            agg->aggregate_exprs[i])
                            ->column_name);
                } else if (agg->aggregate_exprs[i]) {
                    cols.push_back(agg->aggregate_exprs[i]->ToString());
                } else {
                    cols.push_back("col" + std::to_string(i));
                }
            }
            break;
        }
        case PlanNodeType::VALUES: {
            auto v = std::static_pointer_cast<ValuesNode>(p);
            size_t ncols = v->column_aliases.empty()
                              ? (v->rows.empty() ? 0 : v->rows[0].size())
                              : v->column_aliases.size();
            cols.reserve(ncols);
            for (size_t i = 0; i < ncols; ++i) {
                if (i < v->column_aliases.size() &&
                    !v->column_aliases[i].empty()) {
                    cols.push_back(v->column_aliases[i]);
                } else {
                    cols.push_back("col" + std::to_string(i));
                }
            }
            break;
        }
        default:
            break;
    }
    return cols;
}

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
            // ---- 31_cte bug fix: 传入输出列名 ----
            // 让外层 SeqScanNode.predicate（被 Optimizer 下推的 WHERE / HAVING）
            // 在 CteBindExecutor 处能正确解析 CTE 输出列上的 ColumnRefExpr。
            auto cols = DeriveCteOutputColumns(context_->GetCatalog(),
                                                node_->cte_plan);
            // ---- 81_cte_join bug fix: trim materialized rows to select_list width ----
            // ProjectExecutor 的输出元组形状是 [select_values ++ underlying_tuple]，
            // 但 CTE 在语义层被当作只有 select_list 列的虚拟表（见 SemanticAnalyzer
            // 注册的 TableInfo.columns）。BuildCombinedColumnIndexMap 以「虚拟表列数」
            // 为偏移累加 CTE 之间的 offset。
            // 若不裁剪，物化行宽度 = select_count + base_cols，join 后 b.x 仍被
            // cmap 指向 a.underlying_x 而不是真正的 b.x 列，导致：
            //   - ON 条件失效（a.x = b.x 退化为 a.x = a.underlying_x 恒真变体）
            //   - SELECT b.<non_key_col> 拿到 a.<同位列> 的值
            // 这里按 cols.size() 截断元组，使每行宽度与语义层虚拟表一致，join 的
            // cmap 偏移就能与真实元组布局对齐。
            size_t strip_extra_cols = cols.size();
            auto rows = MaterializePlan(context_, node_->cte_plan, strip_extra_cols);
            context_->RegisterCte(node_->cte_name, std::move(rows),
                                  std::move(cols));
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
    // 累计行（外层 SELECT 最终读这份）：初始 = anchor_rows。
    // 列名以 anchor 为准（递归部分的 SELECT list 与 anchor 保持同列定义）。
    auto cols = DeriveCteOutputColumns(context_->GetCatalog(), node_->cte_plan);
    context_->RegisterCte(node_->cte_name, anchor_rows, std::move(cols));

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
        // U3-3：递归 CTE 每轮迭代的工作集变化，非相关子查询若引用 CTE 结果，
        // 上一轮物化的缓存已陈旧——迭代边界失效缓存。
        context_->ClearSubqueryCache();
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

CteBindExecutor::CteBindExecutor(ExecutionContext* context, std::string cte_name,
                                 ExprPtr predicate,
                                 std::unordered_map<std::string, size_t> column_index_map)
    : Executor(context),
      cte_name_(std::move(cte_name)),
      cursor_(0),
      predicate_(std::move(predicate)),
      column_index_map_(std::move(column_index_map)) {
}

void CteBindExecutor::Init() {
    cursor_ = 0;
}

bool CteBindExecutor::Next(Tuple* tuple) {
    if (cte_name_.empty() || !context_->HasCte(cte_name_)) return false;
    const auto* rows = context_->GetCteRows(cte_name_);
    if (!rows) return false;

    // ---- 31_cte bug fix: 行级谓词 ----
    // 没有谓词时直接顺序发射，行为与原实现一致；有谓词时按 SeqScanExecutor
    // 同样的方式扫描：拉一条 Tuple，用 column_index_map_ 求值，命中才返回。
    // 这使 Optimizer 下推到 CTE 引用 SeqScanNode 的 WHERE（如 `WHERE total
    // >= 200`）在改写为 CteBindExecutor 后仍生效——否则会出现 test
    // 31_cte.sql 第 23–28 行那种「外层 WHERE 被静默丢弃」的回归。
    if (!predicate_) {
        if (cursor_ >= rows->size()) return false;
        if (tuple) *tuple = (*rows)[cursor_];
        ++cursor_;
        return true;
    }
    ExpressionEvaluator eval(column_index_map_, context_, nullptr);
    while (cursor_ < rows->size()) {
        const Tuple& t = (*rows)[cursor_];
        ++cursor_;
        if (!tuple) return true;  // 仅推进 cursor（消费者丢弃元组的情况）
        Value v = eval.Evaluate(predicate_, t);
        if (!v.IsNull() && v.AsInt() != 0) {
            *tuple = t;
            return true;
        }
    }
    return false;
}

}  // namespace sqlcompiler
