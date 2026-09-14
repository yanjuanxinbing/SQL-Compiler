#include "optimizer/Optimizer.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <unordered_set>

#include "optimizer/IndexAccessPath.h"

namespace sqlcompiler {

Optimizer::Optimizer(SystemCatalog* catalog) : catalog_(catalog) {
}

PlanNodePtr Optimizer::Optimize(PlanNodePtr plan) {
    plan = DecorrelateSubqueries(plan);  // U3-3：相关子查询 → SEMI/ANTI 连接
    plan = PushDownPredicates(plan);
    plan = ReorderJoins(plan);          // U3-1：JOIN 顺序启发式（小表驱动，3 表以上）
    plan = PushDownAggregates(plan);    // U3-2：聚合下推（Aggregate → PreAggScan）
    plan = ChooseAccessPaths(plan);
    plan = PruneColumns(plan);
    return plan;
}

// ---- U3-1 JOIN 顺序启发式 ----

// 规范化表限定符，便于大小写不敏感的别名/表名匹配。
static std::string Norm(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// ---- U3-2 聚合下推辅助 ----

// 表达式是否出现任何聚合函数调用（COUNT/SUM/AVG/MIN/MAX，含窗口函数包装聚合）。
// 与 AggregateExecutor::ContainsAggregate 同一组函数名单；只做保守中止用。
bool ExprContainsAgg(const ExprPtr& e) {
    if (!e) return false;
    switch (e->GetType()) {
        case NodeType::FUNCTION_CALL_EXPR: {
            auto f = std::static_pointer_cast<FunctionCallExpr>(e);
            const std::string u = Norm(f->function_name);
            if (u == "count" || u == "sum" || u == "avg" || u == "min" || u == "max") {
                return true;
            }
            for (const auto& a : f->arguments) {
                if (ExprContainsAgg(a)) return true;
            }
            return false;
        }
        case NodeType::BINARY_EXPR: {
            auto b = std::static_pointer_cast<BinaryExpr>(e);
            return ExprContainsAgg(b->left) || ExprContainsAgg(b->right);
        }
        case NodeType::UNARY_EXPR:
            return ExprContainsAgg(std::static_pointer_cast<UnaryExpr>(e)->operand);
        case NodeType::CASE_EXPR: {
            auto c = std::static_pointer_cast<CaseExprNode>(e);
            if (c->subject && ExprContainsAgg(c->subject)) return true;
            for (const auto& w : c->whens) {
                if (ExprContainsAgg(w.when_expr)) return true;
                if (ExprContainsAgg(w.then_expr)) return true;
            }
            if (c->else_expr && ExprContainsAgg(c->else_expr)) return true;
            return false;
        }
        case NodeType::CAST_EXPR:
            return ExprContainsAgg(std::static_pointer_cast<CastExprNode>(e)->expr);
        case NodeType::WINDOW_FUNC_EXPR: {
            auto wf = std::static_pointer_cast<WindowFuncNode>(e);
            const std::string u = Norm(wf->function_name);
            if (u == "count" || u == "sum" || u == "avg" || u == "min" || u == "max") {
                return true;
            }
            for (const auto& a : wf->arguments) {
                if (ExprContainsAgg(a)) return true;
            }
            return false;
        }
        default:
            return false;  // COLUMN_REF/LITERAL/LIKE/EXTRACT/INTERVAL/SUBQUERY 等：非聚合
    }
}

PlanNodePtr Optimizer::ReorderJoins(PlanNodePtr plan) {
    if (!plan) return plan;
    // 若本节点就是一棵可直接重排的 JOIN 子树根，整棵一次重排（而非先递归子节点，
    // 避免把左深链折成多段分别处理）。
    if (plan->GetType() == PlanNodeType::JOIN) {
        PlanNodePtr reordered = plan;
        if (TryReorderJoinSubtree(reordered)) return reordered;
        return plan;
    }
    for (auto& c : plan->children) c = ReorderJoins(c);
    return plan;
}

bool Optimizer::CollectLeftDeepSpine(const PlanNodePtr& node,
                                     SpineTables& tables,
                                     SpineConds& conds) const {
    if (!node) return false;
    if (node->GetType() == PlanNodeType::SEQ_SCAN) {
        tables.push_back(node);
        return true;
    }
    if (node->GetType() != PlanNodeType::JOIN) return false;
    if (node->children.size() != 2) return false;
    auto j = std::static_pointer_cast<JoinNode>(node);
    if (j->join_type != JoinType::INNER) return false;  // 仅全 INNER 可安全重排
    if (!CollectLeftDeepSpine(node->children[0], tables, conds)) return false;
    if (node->children[1]->GetType() != PlanNodeType::SEQ_SCAN) return false;
    conds.emplace_back(j->condition, j->join_type);
    tables.push_back(node->children[1]);
    return true;
}

void Optimizer::CollectQualifiers(const ExprPtr& e, std::vector<std::string>& quals,
                                  bool& fail) const {
    if (!e) return;
    switch (e->GetType()) {
        case NodeType::COLUMN_REF_EXPR: {
            auto c = std::static_pointer_cast<ColumnRefExpr>(e);
            if (c->table_name.empty()) { fail = true; return; }  // 未限定列：无法归属
            quals.push_back(c->table_name);
            return;
        }
        case NodeType::BINARY_EXPR: {
            auto b = std::static_pointer_cast<BinaryExpr>(e);
            CollectQualifiers(b->left, quals, fail);
            CollectQualifiers(b->right, quals, fail);
            return;
        }
        case NodeType::UNARY_EXPR: {
            auto u = std::static_pointer_cast<UnaryExpr>(e);
            CollectQualifiers(u->operand, quals, fail);
            return;
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            auto f = std::static_pointer_cast<FunctionCallExpr>(e);
            for (const auto& a : f->arguments) CollectQualifiers(a, quals, fail);
            return;
        }
        case NodeType::LIKE_EXPR:
        case NodeType::CASE_EXPR:
        case NodeType::CAST_EXPR:
        case NodeType::EXTRACT_EXPR:
        case NodeType::INTERVAL_EXPR:
            // 少见复杂节点：保守中止，避免条件挂错位置。
            fail = true;
            return;
        case NodeType::LITERAL_EXPR:
            return;  // 常量，无列引用
        default:
            // 未知节点（SUBQUERY/WINDOW/UPSERT_VALUES 等）：保守中止。
            fail = true;
            return;
    }
}

int Optimizer::ResolveTableIndex(const std::string& qual, const SpineTables& tables) const {
    std::string q = Norm(qual);
    int found = -1;
    for (size_t i = 0; i < tables.size(); ++i) {
        auto s = std::static_pointer_cast<SeqScanNode>(tables[i]);
        const std::string candidate = s->table_alias.empty() ? s->table_name : s->table_alias;
        if (Norm(candidate) == q) {
            if (found >= 0) return -1;  // 多表同名限定符：归属不唯一，放弃
            found = static_cast<int>(i);
        }
    }
    return found;
}

// ---- U3-2 聚合下推 ----

bool Optimizer::IsBareCountSumExpr(const ExprPtr& e) const {
    if (!e || e->GetType() != NodeType::FUNCTION_CALL_EXPR) return false;
    auto f = std::static_pointer_cast<FunctionCallExpr>(e);
    const std::string u = Norm(f->function_name);
    if (u != "count" && u != "sum") return false;
    if (f->is_distinct) return false;
    if (f->arguments.size() > 1) return false;
    // 参数（若有）不得嵌套聚合/窗口调用：PreAggScanExecutor 会在扫描行上直接求值该参数，
    // 嵌套聚合无法在单行上求值，保守排除。
    for (const auto& a : f->arguments) {
        if (ExprContainsAgg(a)) return false;
    }
    return true;
}

bool Optimizer::IsPreAggEligible(const PlanNodePtr& agg_node) const {
    if (!agg_node || agg_node->GetType() != PlanNodeType::AGGREGATE) return false;
    auto a = std::static_pointer_cast<AggregateNode>(agg_node);

    // 1) 子树必须为单表访问路径：SeqScan 或 Filter* -> SeqScan。
    //    无 FROM 的聚合（SELECT COUNT(*) 无表）无下推收益，保持原样；
    //    JOIN/Project/子查询 等非单表形态一律不改写（零回归守底）。
    const PlanNodePtr* p = agg_node->children.empty() ? nullptr : &agg_node->children[0];
    if (!p) return false;
    while (*p && (*p)->GetType() == PlanNodeType::FILTER) {
        if ((*p)->children.size() != 1) return false;
        p = &(*p)->children[0];
    }
    if (!(*p) || (*p)->GetType() != PlanNodeType::SEQ_SCAN) return false;

    // 2) 每个 aggregate_expr 须为「裸 COUNT/SUM」或「不含任何聚合调用的普通表达式」。
    //    含 AVG/MIN/MAX/DISTINCT/标量包装（如 COALESCE(SUM(x),0)）的项 → 不合格。
    for (const auto& e : a->aggregate_exprs) {
        if (!e) return false;
        if (IsBareCountSumExpr(e)) continue;
        if (ExprContainsAgg(e)) return false;
    }

    // 3) 防御：GROUP BY 表达式不得含聚合调用（合法 SQL 不会出现，仅保险）。
    for (const auto& e : a->group_by_exprs) {
        if (e && ExprContainsAgg(e)) return false;
    }
    return true;
}

PlanNodePtr Optimizer::PushDownAggregates(PlanNodePtr plan) {
    if (!plan) return plan;
    // 本节点是合格聚合 → 整棵改写：子树的 Filter/SeqScan 原样保留给 PreAggScan 执行。
    if (plan->GetType() == PlanNodeType::AGGREGATE && IsPreAggEligible(plan)) {
        auto a = std::static_pointer_cast<AggregateNode>(plan);
        auto pre = std::make_shared<PreAggScanNode>(a->group_by_exprs, a->aggregate_exprs,
                                                    a->aliases);
        pre->children = std::move(plan->children);
        return pre;
    }
    for (auto& c : plan->children) c = PushDownAggregates(c);
    return plan;
}

bool Optimizer::TryReorderJoinSubtree(PlanNodePtr& subtree) {
    if (!catalog_) return false;
    SpineTables tables;
    SpineConds conds;
    if (!CollectLeftDeepSpine(subtree, tables, conds)) return false;
    const size_t n = tables.size();
    if (n < 3) return false;  // 2 表重排对乘积成本无差别，略过
    if (conds.size() != n - 1) return false;

    // 1) 逐条件解析其引用的表下标集合。
    std::vector<std::vector<int>> refs(conds.size());
    bool abort = false;
    for (size_t k = 0; k < conds.size(); ++k) {
        if (!conds[k].first) { abort = true; break; }  // CROSS 结点混入 INNER 链，跳过
        std::vector<std::string> quals;
        bool fail = false;
        CollectQualifiers(conds[k].first, quals, fail);
        if (fail) { abort = true; break; }
        std::set<int> s;
        for (const auto& q : quals) {
            int idx = ResolveTableIndex(q, tables);
            if (idx < 0) { abort = true; break; }
            s.insert(idx);
        }
        if (abort) break;
        if (s.size() < 2) { abort = true; break; }  // 条件须涉及 ≥2 表
        refs[k].assign(s.begin(), s.end());
    }
    if (abort) return false;

    // 2) 每张表估计基数。任一表探针失败 → 放弃重排（保持原顺序）。
    std::vector<double> est(n, 0.0);
    bool est_ok = true;
    for (size_t i = 0; i < n; ++i) {
        auto s = std::static_pointer_cast<SeqScanNode>(tables[i]);
        TableHeap* heap = catalog_->GetTableHeap(s->table_name);
        if (!heap) { est_ok = false; break; }
        // 表规模×谓词选择性：当前无表内 Filter 下推，选择性取 1（粗估足够排序）。
        est[i] = static_cast<double>(heap->GetApproxRowCount());
    }
    if (!est_ok) return false;

    // 3) 就近贪心排序：优先选「与已放置集合有 join 边」的最小基数表，避免中间笛卡尔积。
    auto connected_to_placed = [&](int i, const std::vector<bool>& placed) -> bool {
        for (size_t k = 0; k < refs.size(); ++k) {
            bool has_i = false, has_placed = false;
            for (int r : refs[k]) {
                if (r == i) has_i = true;
                if (placed[r]) has_placed = true;
            }
            if (has_i && has_placed) return true;
        }
        return false;
    };
    std::vector<int> order;
    std::vector<bool> placed(n, false);
    int seed = 0;
    for (size_t i = 1; i < n; ++i) if (est[i] < est[seed]) seed = static_cast<int>(i);
    order.push_back(seed);
    placed[seed] = true;
    while (static_cast<int>(order.size()) < static_cast<int>(n)) {
        int chosen = -1;
        bool any_connected = false;
        for (int i = 0; i < static_cast<int>(n); ++i) {
            if (placed[i]) continue;
            if (connected_to_placed(i, placed)) { any_connected = true; break; }
        }
        for (int i = 0; i < static_cast<int>(n); ++i) {
            if (placed[i]) continue;
            if (any_connected && !connected_to_placed(i, placed)) continue;
            if (chosen < 0 || est[i] < est[chosen]) chosen = i;
        }
        if (chosen < 0) break;  // 不应发生
        order.push_back(chosen);
        placed[chosen] = true;
    }
    if (static_cast<int>(order.size()) != static_cast<int>(n)) return false;

    // 记录重排后每张原始表的新位置。
    std::vector<int> pos(n, -1);
    for (int p = 0; p < static_cast<int>(n); ++p) pos[order[p]] = p;

    // 4) 每个条件挂在「其引用表里新位置最大」的那级 spine（该级引入该表）。
    //    全 INNER 下，左右子树次序不影响结果集，只影响中间集大小，故挂法与顺序解耦。
    std::vector<std::vector<ExprPtr>> attach(static_cast<size_t>(n));
    for (size_t k = 0; k < conds.size(); ++k) {
        int maxp = -1;
        for (int r : refs[k]) maxp = std::max(maxp, pos[r]);
        if (maxp < 1) return false;  // 防御：条件引用 ≥2 表时 maxp 必 ≥1
        attach[static_cast<size_t>(maxp)].push_back(conds[k].first);
    }

    // 5) 重建左深链。
    auto clone_scan = [](const PlanNodePtr& t) {
        auto s = std::static_pointer_cast<SeqScanNode>(t);
        return std::make_shared<SeqScanNode>(s->table_name, s->table_alias);
    };
    PlanNodePtr rel = clone_scan(tables[static_cast<size_t>(order[0])]);
    for (int p = 1; p < static_cast<int>(n); ++p) {
        ExprPtr combined = nullptr;
        for (const auto& c : attach[static_cast<size_t>(p)]) {
            if (!combined) combined = c;
            else combined = std::make_shared<BinaryExpr>(BinaryOperator::AND, combined, c);
        }
        JoinType jt = combined ? JoinType::INNER : JoinType::CROSS;
        auto jn = std::make_shared<JoinNode>(jt, combined);
        jn->children.push_back(rel);
        jn->children.push_back(clone_scan(tables[static_cast<size_t>(order[p])]));
        rel = jn;
    }
    subtree = rel;
    return true;
}

// ==================== U3-3：子查询去关联 ====================
//
// 把 WHERE 中的相关子查询改写为 SEMI/ANTI 连接：
//   EXISTS (SELECT ... WHERE <相关条件>)  → SEMI JOIN，连接条件 = 相关条件；
//   NOT EXISTS (…)                        → ANTI JOIN，连接条件 = 相关条件；
//   expr IN (SELECT col FROM … WHERE …)   → SEMI JOIN，连接条件 =
//                                            expr = col（AND 内层相关条件）；
//   expr op ANY (SELECT col …)            → SEMI JOIN，连接条件 = expr op col。
// 被改写后内层计划只执行一次（物化在 JoinExecutor 右缓冲），消除逐行重跑子计划。
//
// 保守守底：以下形态一律保持原求值路径（ExpressionEvaluator 逐行/缓存）：
//   - NOT IN / NOT ANY（NULL 语义与 ANTI 不等价）；
//   - 子查询含 GROUP BY/HAVING/DISTINCT/LIMIT/ORDER BY/派生表/JOIN/聚合/窗口；
//   - 相关合取项含嵌套子查询或未限定列引用；
//   - 连接条件中有无法解析到左/右扫描表的列引用。

PlanNodePtr Optimizer::DecorrelateSubqueries(PlanNodePtr plan) {
    if (!plan) return plan;
    if (plan->GetType() == PlanNodeType::FILTER) {
        PlanNodePtr rewritten = TryDecorrelateFilter(plan);
        if (rewritten) {
            // 改写成功：继续递归处理新计划子树。右子（内层计划）或左子（剩余
            // 合取项）内部可能还有嵌套相关子查询，逐层去关联。
            for (auto& c : rewritten->children) {
                c = DecorrelateSubqueries(c);
            }
            return rewritten;
        }
    }
    for (auto& c : plan->children) {
        c = DecorrelateSubqueries(c);
    }
    return plan;
}

PlanNodePtr Optimizer::TryDecorrelateFilter(const PlanNodePtr& filter) {
    auto f = std::static_pointer_cast<FilterNode>(filter);
    if (!f || !f->predicate) return nullptr;

    std::vector<ExprPtr> conjuncts;
    SplitConjuncts(f->predicate, conjuncts);

    for (size_t i = 0; i < conjuncts.size(); ++i) {
        // ---- 识别 (NOT) EXISTS / IN / ANY 合取项 ----
        bool negated = false;
        SubqueryExprNode* sq = nullptr;
        if (conjuncts[i]->GetType() == NodeType::SUBQUERY_EXPR) {
            auto s = std::static_pointer_cast<SubqueryExprNode>(conjuncts[i]);
            if (s->kind != SubqueryType::SCALAR) {
                sq = s.get();
            }
        } else if (conjuncts[i]->GetType() == NodeType::UNARY_EXPR) {
            auto u = std::static_pointer_cast<UnaryExpr>(conjuncts[i]);
            if (u->op == UnaryOperator::NOT &&
                u->operand->GetType() == NodeType::SUBQUERY_EXPR) {
                auto s = std::static_pointer_cast<SubqueryExprNode>(u->operand);
                // NOT IN / NOT ANY 的 NULL 语义与 ANTI 不等价 → 保留原求值路径。
                if (s->kind == SubqueryType::EXISTS) {
                    sq = s.get();
                    negated = true;
                }
            }
        }
        if (!sq) continue;

        // ---- 形态守卫 ----
        if (!sq->subquery || !sq->subquery_plan) continue;
        if (!IsSubqueryDecorrelatable(*sq)) continue;
        const SelectStatement& sub = *sq->subquery;

        std::unordered_set<std::string> inner_quals;
        CollectInnerQualifiers(sub, inner_quals);

        // ---- 提取相关合取项（移到连接条件）----
        std::vector<ExprPtr> correlated;
        CollectCorrelatedConjuncts(sub, inner_quals, correlated);
        bool bail = false;
        for (const auto& cc : correlated) {
            // 相关合取项里的嵌套子查询依赖外层绑定，连接上下文无法提供 → 放弃。
            if (ExprHasNestedSubquery(cc)) { bail = true; break; }
            // 未限定列移到连接条件会因「首表优先」解析歧义 → 放弃。
            if (HasUnqualifiedColumnRef(cc)) { bail = true; break; }
        }
        if (bail) continue;

        // ---- 构建连接条件 ----
        std::vector<ExprPtr> conds;
        if (sq->kind == SubqueryType::IN || sq->kind == SubqueryType::ANY) {
            // 内层 SELECT 首列须为裸列引用（形态守卫已保证）。
            if (!sq->outer_expr || sub.select_list.size() != 1) continue;
            auto col0 = std::static_pointer_cast<ColumnRefExpr>(sub.select_list[0]);
            ExprPtr inner_col = col0;
            if (col0->table_name.empty()) {
                // 未限定 → 补上内层表名/别名，让连接条件里的内层列解析到右侧。
                std::string qual = !sub.from_table_alias.empty() ? sub.from_table_alias
                                                                 : sub.from_table;
                if (qual.empty()) continue;
                inner_col = std::make_shared<ColumnRefExpr>(qual, col0->column_name);
            }
            std::string op = (sq->kind == SubqueryType::IN) ? "=" : sq->comparison_op;
            BinaryOperator bop;
            if (op == "=")            bop = BinaryOperator::EQUAL;
            else if (op == "<>")      bop = BinaryOperator::NOT_EQUAL;
            else if (op == "<")       bop = BinaryOperator::LESS;
            else if (op == "<=")      bop = BinaryOperator::LESS_EQUAL;
            else if (op == ">")       bop = BinaryOperator::GREATER;
            else if (op == ">=")      bop = BinaryOperator::GREATER_EQUAL;
            else continue;  // 未知比较符，放弃
            conds.push_back(std::make_shared<BinaryExpr>(bop, sq->outer_expr, inner_col));
        }
        conds.insert(conds.end(), correlated.begin(), correlated.end());
        ExprPtr cond = CombineConjuncts(conds);

        // ---- 构建右子树（内层计划：克隆 + 剥 Project + 移除相关合取项）----
        PlanNodePtr from = f->children.empty() ? nullptr : f->children[0];
        if (!from) continue;
        PlanNodePtr right = ClonePlan(sq->subquery_plan);
        if (!right) continue;
        if (right->GetType() == PlanNodeType::PROJECT) {
            // EXISTS/IN/ANY 语义只关心内层行是否存在（或首列值）；剥掉顶层
            // Project 让内层全列输出，连接条件里的内层限定列才可解析。
            right = right->children.empty() ? nullptr : right->children[0];
        }
        if (!right) continue;
        right = StripCorrelatedFilters(right, inner_quals);
        if (!CondRefsResolve(cond, from, right)) continue;

        // ---- 构建左子树：其余合取项 + 原 FROM ----
        std::vector<ExprPtr> remaining;
        for (size_t j = 0; j < conjuncts.size(); ++j) {
            if (j != i) remaining.push_back(conjuncts[j]);
        }
        PlanNodePtr left;
        if (!remaining.empty()) {
            auto nf = std::make_shared<FilterNode>(CombineConjuncts(remaining));
            nf->children.push_back(from);
            left = nf;
        } else {
            left = from;
        }

        // ---- 组装 SEMI / ANTI 连接 ----
        auto join = std::make_shared<JoinNode>(negated ? JoinType::ANTI : JoinType::SEMI,
                                               cond);
        join->children.push_back(left);
        join->children.push_back(right);
        return join;
    }
    return nullptr;
}

bool Optimizer::IsSubqueryDecorrelatable(const SubqueryExprNode& sq) const {
    if (!sq.subquery || !sq.subquery_plan) return false;
    const SelectStatement& sub = *sq.subquery;
    if (sub.is_distinct) return false;
    if (!sub.group_by.empty() || sub.having_clause) return false;
    if (!sub.order_by.empty() || sub.limit >= 0) return false;
    if (sub.derived_table) return false;
    if (sub.from_table.empty()) return false;  // 无 FROM 的 SELECT 不改写
    if (!sub.joins.empty()) return false;      // 内层多表暂不支持
    if (sq.kind == SubqueryType::IN || sq.kind == SubqueryType::ANY) {
        if (!sq.outer_expr) return false;
        if (sub.select_list.size() != 1) return false;
        if (sub.select_list[0]->GetType() != NodeType::COLUMN_REF_EXPR) return false;
        std::unordered_set<std::string> inner;
        CollectInnerQualifiers(sub, inner);
        auto cr = std::static_pointer_cast<ColumnRefExpr>(sub.select_list[0]);
        if (!cr->table_name.empty() &&
            inner.find(Norm(cr->table_name)) == inner.end()) {
            return false;
        }
    }
    return IsSimpleSubqueryPlanShape(sq.subquery_plan);
}

bool Optimizer::IsSimpleSubqueryPlanShape(const PlanNodePtr& plan) const {
    if (!plan) return false;
    PlanNodePtr cur = plan;
    if (cur->GetType() == PlanNodeType::PROJECT) {
        auto p = std::static_pointer_cast<ProjectNode>(cur);
        if (p->is_distinct) return false;
        if (p->children.size() != 1) return false;
        cur = p->children[0];
    }
    int scans = 0;
    while (cur) {
        switch (cur->GetType()) {
            case PlanNodeType::FILTER:
                if (cur->children.size() != 1) return false;
                cur = cur->children[0];
                break;
            case PlanNodeType::SEQ_SCAN:
                ++scans;
                cur = nullptr;
                break;
            default:
                // Aggregate/Join/Sort/Limit/Window/SetOp/CTE 等 → 放弃
                return false;
        }
    }
    return scans == 1;
}

void Optimizer::CollectInnerQualifiers(
    const SelectStatement& sub, std::unordered_set<std::string>& quals) const {
    if (!sub.from_table.empty()) quals.insert(Norm(sub.from_table));
    if (!sub.from_table_alias.empty()) quals.insert(Norm(sub.from_table_alias));
    for (const auto& j : sub.joins) {
        if (!j.table_name.empty()) quals.insert(Norm(j.table_name));
        if (!j.table_alias.empty()) quals.insert(Norm(j.table_alias));
    }
    if (!sub.derived_alias.empty()) quals.insert(Norm(sub.derived_alias));
}

bool Optimizer::ExprHasNestedSubquery(const ExprPtr& e) const {
    if (!e) return false;
    bool found = false;
    std::function<void(const ExprPtr&)> walk = [&](const ExprPtr& x) {
        if (!x || found) return;
        if (x->GetType() == NodeType::SUBQUERY_EXPR) { found = true; return; }
        switch (x->GetType()) {
            case NodeType::BINARY_EXPR: {
                auto b = std::static_pointer_cast<BinaryExpr>(x);
                walk(b->left);
                walk(b->right);
                return;
            }
            case NodeType::UNARY_EXPR:
                walk(std::static_pointer_cast<UnaryExpr>(x)->operand);
                return;
            case NodeType::FUNCTION_CALL_EXPR: {
                auto f = std::static_pointer_cast<FunctionCallExpr>(x);
                for (auto& a : f->arguments) walk(a);
                return;
            }
            case NodeType::CASE_EXPR: {
                auto c = std::static_pointer_cast<CaseExprNode>(x);
                if (c->subject) walk(c->subject);
                for (auto& w : c->whens) {
                    walk(w.when_expr);
                    walk(w.then_expr);
                }
                if (c->else_expr) walk(c->else_expr);
                return;
            }
            case NodeType::CAST_EXPR:
                walk(std::static_pointer_cast<CastExprNode>(x)->expr);
                return;
            default:
                return;
        }
    };
    walk(e);
    return found;
}

bool Optimizer::ReferencesOuterTable(
    const ExprPtr& e, const std::unordered_set<std::string>& inner_quals) const {
    if (!e) return false;
    bool outer = false;
    std::function<void(const ExprPtr&)> walk = [&](const ExprPtr& x) {
        if (!x || outer) return;
        if (x->GetType() == NodeType::COLUMN_REF_EXPR) {
            auto cr = std::static_pointer_cast<ColumnRefExpr>(x);
            if (!cr->table_name.empty() &&
                inner_quals.find(Norm(cr->table_name)) == inner_quals.end()) {
                outer = true;
            }
            return;
        }
        switch (x->GetType()) {
            case NodeType::BINARY_EXPR: {
                auto b = std::static_pointer_cast<BinaryExpr>(x);
                walk(b->left);
                walk(b->right);
                return;
            }
            case NodeType::UNARY_EXPR:
                walk(std::static_pointer_cast<UnaryExpr>(x)->operand);
                return;
            case NodeType::FUNCTION_CALL_EXPR: {
                auto f = std::static_pointer_cast<FunctionCallExpr>(x);
                for (auto& a : f->arguments) walk(a);
                return;
            }
            case NodeType::CASE_EXPR: {
                auto c = std::static_pointer_cast<CaseExprNode>(x);
                if (c->subject) walk(c->subject);
                for (auto& w : c->whens) {
                    walk(w.when_expr);
                    walk(w.then_expr);
                }
                if (c->else_expr) walk(c->else_expr);
                return;
            }
            case NodeType::CAST_EXPR:
                walk(std::static_pointer_cast<CastExprNode>(x)->expr);
                return;
            default:
                // LITERAL / SUBQUERY / LIKE / EXTRACT 等：不产生外层列引用
                return;
        }
    };
    walk(e);
    return outer;
}

bool Optimizer::HasUnqualifiedColumnRef(const ExprPtr& e) const {
    if (!e) return false;
    bool found = false;
    std::function<void(const ExprPtr&)> walk = [&](const ExprPtr& x) {
        if (!x || found) return;
        if (x->GetType() == NodeType::COLUMN_REF_EXPR) {
            if (std::static_pointer_cast<ColumnRefExpr>(x)->table_name.empty()) {
                found = true;
            }
            return;
        }
        switch (x->GetType()) {
            case NodeType::BINARY_EXPR: {
                auto b = std::static_pointer_cast<BinaryExpr>(x);
                walk(b->left);
                walk(b->right);
                return;
            }
            case NodeType::UNARY_EXPR:
                walk(std::static_pointer_cast<UnaryExpr>(x)->operand);
                return;
            case NodeType::FUNCTION_CALL_EXPR: {
                auto f = std::static_pointer_cast<FunctionCallExpr>(x);
                for (auto& a : f->arguments) walk(a);
                return;
            }
            case NodeType::CASE_EXPR: {
                auto c = std::static_pointer_cast<CaseExprNode>(x);
                if (c->subject) walk(c->subject);
                for (auto& w : c->whens) {
                    walk(w.when_expr);
                    walk(w.then_expr);
                }
                if (c->else_expr) walk(c->else_expr);
                return;
            }
            case NodeType::CAST_EXPR:
                walk(std::static_pointer_cast<CastExprNode>(x)->expr);
                return;
            default:
                return;
        }
    };
    walk(e);
    return found;
}

bool Optimizer::CondRefsResolve(const ExprPtr& cond, const PlanNodePtr& left,
                                const PlanNodePtr& right) const {
    if (!cond) return true;
    std::vector<std::pair<std::string, std::string>> lscans, rscans;
    std::function<void(const PlanNodePtr&, std::vector<std::pair<std::string, std::string>>&)>
        collect = [&](const PlanNodePtr& n,
                      std::vector<std::pair<std::string, std::string>>& out) {
        if (!n) return;
        if (n->GetType() == PlanNodeType::SEQ_SCAN) {
            auto s = std::static_pointer_cast<SeqScanNode>(n);
            out.emplace_back(s->table_name, s->table_alias);
        }
        if (n->GetType() == PlanNodeType::INDEX_SCAN) {
            auto s = std::static_pointer_cast<IndexScanNode>(n);
            out.emplace_back(s->table_name, s->table_alias);
        }
        for (auto& c : n->children) collect(c, out);
    };
    collect(left, lscans);
    collect(right, rscans);

    bool ok = true;
    std::function<void(const ExprPtr&)> walk = [&](const ExprPtr& x) {
        if (!x || !ok) return;
        if (x->GetType() == NodeType::COLUMN_REF_EXPR) {
            auto cr = std::static_pointer_cast<ColumnRefExpr>(x);
            if (cr->table_name.empty()) {
                // 未限定列仅当左侧单表时允许（解析到左侧唯一表，无歧义）。
                if (lscans.size() != 1) ok = false;
                return;
            }
            const std::string q = Norm(cr->table_name);
            const std::string* real_table = nullptr;
            for (const auto& s : lscans) {
                if (Norm(s.first) == q ||
                    (!s.second.empty() && Norm(s.second) == q)) {
                    real_table = &s.first;
                    break;
                }
            }
            if (!real_table) {
                for (const auto& s : rscans) {
                    if (Norm(s.first) == q ||
                        (!s.second.empty() && Norm(s.second) == q)) {
                        real_table = &s.first;
                        break;
                    }
                }
            }
            if (!real_table) {
                ok = false;
                return;
            }
            if (catalog_) {
                const TableInfo* info = catalog_->GetTable(*real_table);
                if (info && !info->HasColumn(cr->column_name)) {
                    ok = false;
                    return;
                }
            }
            return;
        }
        switch (x->GetType()) {
            case NodeType::BINARY_EXPR: {
                auto b = std::static_pointer_cast<BinaryExpr>(x);
                walk(b->left);
                walk(b->right);
                return;
            }
            case NodeType::UNARY_EXPR:
                walk(std::static_pointer_cast<UnaryExpr>(x)->operand);
                return;
            case NodeType::FUNCTION_CALL_EXPR: {
                auto f = std::static_pointer_cast<FunctionCallExpr>(x);
                for (auto& a : f->arguments) walk(a);
                return;
            }
            case NodeType::CASE_EXPR: {
                auto c = std::static_pointer_cast<CaseExprNode>(x);
                if (c->subject) walk(c->subject);
                for (auto& w : c->whens) {
                    walk(w.when_expr);
                    walk(w.then_expr);
                }
                if (c->else_expr) walk(c->else_expr);
                return;
            }
            case NodeType::CAST_EXPR:
                walk(std::static_pointer_cast<CastExprNode>(x)->expr);
                return;
            default:
                return;
        }
    };
    walk(cond);
    return ok;
}

void Optimizer::SplitConjuncts(const ExprPtr& expr, std::vector<ExprPtr>& out) const {
    if (!expr) return;
    if (expr->GetType() == NodeType::BINARY_EXPR) {
        auto b = std::static_pointer_cast<BinaryExpr>(expr);
        if (b->op == BinaryOperator::AND) {
            SplitConjuncts(b->left, out);
            SplitConjuncts(b->right, out);
            return;
        }
    }
    out.push_back(expr);
}

ExprPtr Optimizer::CombineConjuncts(const std::vector<ExprPtr>& conjuncts) const {
    ExprPtr out;
    for (const auto& c : conjuncts) {
        if (!c) continue;
        if (!out) {
            out = c;
        } else {
            out = std::make_shared<BinaryExpr>(BinaryOperator::AND, out, c);
        }
    }
    return out;
}

PlanNodePtr Optimizer::ClonePlan(const PlanNodePtr& node) {
    if (!node) return nullptr;
    PlanNodePtr copy;
    switch (node->GetType()) {
        case PlanNodeType::SEQ_SCAN: {
            auto s = std::static_pointer_cast<SeqScanNode>(node);
            copy = std::make_shared<SeqScanNode>(s->table_name, s->table_alias);
            break;
        }
        case PlanNodeType::FILTER: {
            auto f = std::static_pointer_cast<FilterNode>(node);
            copy = std::make_shared<FilterNode>(f->predicate);
            break;
        }
        case PlanNodeType::PROJECT: {
            auto p = std::static_pointer_cast<ProjectNode>(node);
            copy = std::make_shared<ProjectNode>(p->columns, p->aliases, p->is_distinct);
            break;
        }
        case PlanNodeType::INDEX_SCAN: {
            auto s = std::static_pointer_cast<IndexScanNode>(node);
            auto ix = std::make_shared<IndexScanNode>(s->table_name, s->index_name,
                                                      s->table_alias);
            ix->low_key = s->low_key;
            ix->high_key = s->high_key;
            ix->low_inclusive = s->low_inclusive;
            ix->high_inclusive = s->high_inclusive;
            ix->low_bound_cols = s->low_bound_cols;
            ix->high_bound_cols = s->high_bound_cols;
            ix->residual_predicate = s->residual_predicate;
            copy = ix;
            break;
        }
        default:
            // 形状守卫保证内层计划不含其他节点；未知节点共享原指针（不改写）。
            return node;
    }
    for (const auto& c : node->children) {
        copy->children.push_back(ClonePlan(c));
    }
    return copy;
}

PlanNodePtr Optimizer::StripCorrelatedFilters(
    const PlanNodePtr& node, const std::unordered_set<std::string>& inner_quals) {
    if (!node) return nullptr;
    PlanNodePtr child = nullptr;
    if (!node->children.empty()) {
        child = StripCorrelatedFilters(node->children[0], inner_quals);
    }
    if (node->GetType() == PlanNodeType::FILTER) {
        auto f = std::static_pointer_cast<FilterNode>(node);
        std::vector<ExprPtr> conjuncts;
        SplitConjuncts(f->predicate, conjuncts);
        std::vector<ExprPtr> keep;
        for (const auto& c : conjuncts) {
            if (!ReferencesOuterTable(c, inner_quals)) keep.push_back(c);
        }
        if (keep.empty()) {
            // 该 Filter 的所有子句都是相关子句（已移到连接条件）→ 整层移除。
            return child;
        }
        f->predicate = CombineConjuncts(keep);
        f->children.clear();
        f->children.push_back(child);
        return node;
    }
    if (node->GetType() == PlanNodeType::SEQ_SCAN) return node;
    return node;  // 其余节点（PROJECT 已在外层剥掉）：children 已处理，原样返回
}

void Optimizer::CollectCorrelatedConjuncts(
    const SelectStatement& sub, const std::unordered_set<std::string>& inner_quals,
    std::vector<ExprPtr>& out) const {
    if (!sub.where_clause) return;
    std::vector<ExprPtr> conjuncts;
    SplitConjuncts(sub.where_clause, conjuncts);
    for (const auto& c : conjuncts) {
        if (ReferencesOuterTable(c, inner_quals)) out.push_back(c);
    }
}

PlanNodePtr Optimizer::ChooseAccessPaths(PlanNodePtr plan) {
    if (!plan || catalog_ == nullptr) return plan;
    for (auto& c : plan->children) {
        c = ChooseAccessPaths(c);
    }
    // 只处理 Filter 直接盖在 SeqScan 上的形态。JOIN 下的扫描暂不改写：
    // 连接谓词涉及两侧的列，不能当作常量区间。
    if (plan->GetType() != PlanNodeType::FILTER) return plan;
    if (plan->children.size() != 1) return plan;
    if (plan->children[0]->GetType() != PlanNodeType::SEQ_SCAN) return plan;

    auto filter = std::static_pointer_cast<FilterNode>(plan);
    auto scan = std::static_pointer_cast<SeqScanNode>(plan->children[0]);
    PlanNodePtr rewritten = TryRewriteWithIndex(catalog_, scan->table_name,
                                                scan->table_alias,
                                                filter->predicate);
    // 改写不成立时保持原计划：宁可慢，也不能因为改写出错而少返回行
    return rewritten ? rewritten : plan;
}

PlanNodePtr Optimizer::PushDownPredicates(PlanNodePtr plan) {
    if (!plan) return plan;
    // Recurse into children first
    for (auto& c : plan->children) {
        c = PushDownPredicates(c);
    }
    return plan;
}

PlanNodePtr Optimizer::PruneColumns(PlanNodePtr plan) {
    (void)plan;
    return plan;
}

ExprPtr Optimizer::FoldConstants(ExprPtr expr) {
    return expr;
}

bool Optimizer::IsConstantExpr(const ExprPtr& expr) const {
    (void)expr;
    return false;
}

}  // namespace sqlcompiler