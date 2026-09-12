#include "optimizer/Optimizer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "storage_engine/Value.h"
#include "optimizer/IndexAccessPath.h"

namespace sqlcompiler {

Optimizer::Optimizer(SystemCatalog* catalog) : catalog_(catalog) {
}

PlanNodePtr Optimizer::Optimize(PlanNodePtr plan) {
    // 顺序：先 ChooseAccessPaths（把能转 IndexScan 的 Filter -> SeqScan 改写），
    // 再 FoldConstants（编译期常量折叠，并把恒真 Filter 移除），
    // 再 PushDownPredicates（下推剩余的 Filter 到 SeqScan.predicate），
    // 再 FoldConstants 第二遍（下推后的 SeqScan.predicate 也可能有可化简常量），
    // 最后 PruneColumns（列裁剪）。
    plan = ChooseAccessPaths(plan);
    plan = FoldConstants(plan);
    plan = PushDownPredicates(plan);
    plan = FoldConstants(plan);
    plan = PruneColumns(plan);
    return plan;
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

namespace {

// 收集表达式里所有 ColumnRefExpr。每遇到一个 column，记录它的限定表名
// 与列名；遇到 SubqueryExprNode / WindowFuncNode 时把标志位置 true，
// 表明该合取项不可下推。
struct ColumnUsage {
    std::vector<std::pair<std::string, std::string>> refs;  // (table_name_or_empty, column_name)
    bool has_subquery_or_window = false;
};

void CollectColumns(const ExprPtr& e, ColumnUsage& out) {
    if (!e) return;
    switch (e->GetType()) {
        case NodeType::COLUMN_REF_EXPR: {
            const auto* c = static_cast<const ColumnRefExpr*>(e.get());
            out.refs.emplace_back(c->table_name, c->column_name);
            break;
        }
        case NodeType::BINARY_EXPR: {
            const auto* b = static_cast<const BinaryExpr*>(e.get());
            CollectColumns(b->left, out);
            CollectColumns(b->right, out);
            break;
        }
        case NodeType::UNARY_EXPR: {
            const auto* u = static_cast<const UnaryExpr*>(e.get());
            CollectColumns(u->operand, out);
            break;
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            const auto* f = static_cast<const FunctionCallExpr*>(e.get());
            for (const auto& a : f->arguments) CollectColumns(a, out);
            break;
        }
        case NodeType::SUBQUERY_EXPR:
        case NodeType::WINDOW_FUNC_EXPR:
            out.has_subquery_or_window = true;
            break;
        case NodeType::CASE_EXPR: {
            const auto* c = static_cast<const CaseExprNode*>(e.get());
            for (const auto& w : c->whens) {
                CollectColumns(w.when_expr, out);
                CollectColumns(w.then_expr, out);
            }
            CollectColumns(c->else_expr, out);
            break;
        }
        case NodeType::CAST_EXPR: {
            const auto* c = static_cast<const CastExprNode*>(e.get());
            CollectColumns(c->expr, out);
            break;
        }
        default:
            break;
    }
}

// 合取项 c 是否可以下沉到 seq_scan 的执行器里求值？
// 判定规则（与 IndexAccessPath::MatchColumnCompare 保持同一套约定）：
//   1. 不含子查询 / 窗口函数（外层相关引用，下推会破坏语义）
//   2. 每个 ColumnRefExpr 的 table_name 为空（未限定），或等于 seq_scan 的
//      table_name / table_alias（限定到本表）
//   3. 当 ColumnRefExpr 不带限定名时，若 Catalog 里该表存在同名列，视为
//      本表引用；否则不下推（可能引用了其他表的同名列，留给 Filter）
bool CanPushIntoScan(const ExprPtr& c, const SeqScanNode* scan,
                     SystemCatalog* catalog) {
    if (!c) return false;
    ColumnUsage u;
    CollectColumns(c, u);
    if (u.has_subquery_or_window) return false;
    for (const auto& [tbl, col] : u.refs) {
        if (!tbl.empty() && tbl != scan->table_name && tbl != scan->table_alias) {
            return false;
        }
    }
    // 不带限定名的列引用：在 Catalog 中检查是否存在该列。
    // 全部命中本表 → 可下推；否则视为跨表/未知，保守地不下推。
    if (catalog != nullptr) {
        const TableInfo* info = catalog->GetTable(scan->table_name);
        for (const auto& [tbl, col] : u.refs) {
            if (!tbl.empty()) continue;
            bool found = false;
            if (info) {
                for (const auto& ci : info->columns) {
                    if (ci.name == col) { found = true; break; }
                }
            }
            if (!found) return false;
        }
    }
    return true;
}

// 把若干合取项用 AND 串成一个表达式。
ExprPtr MakeConjunction(const std::vector<ExprPtr>& conjuncts) {
    if (conjuncts.empty()) return nullptr;
    ExprPtr acc = conjuncts.front();
    for (size_t i = 1; i < conjuncts.size(); ++i) {
        acc = std::make_shared<BinaryExpr>(BinaryOperator::AND, acc, conjuncts[i]);
    }
    return acc;
}

// 把新谓语与已有下推谓语用 AND 合并。两者都可能为空。
ExprPtr MergePredicate(ExprPtr existing, ExprPtr extra) {
    if (!existing) return extra;
    if (!extra) return existing;
    return std::make_shared<BinaryExpr>(BinaryOperator::AND, existing, extra);
}

}  // namespace

PlanNodePtr Optimizer::PushDownPredicates(PlanNodePtr plan) {
    if (!plan) return plan;
    // 1) 自底向上：先递归处理子树，让下推在内层完成后再考虑外层
    for (auto& c : plan->children) {
        c = PushDownPredicates(c);
    }

    // 2) 仅处理 Filter 节点。其余节点形态保持不变。
    //    - HAVING (Filter over Aggregate)：不下推跨过 Aggregate，列引用
    //      在聚合后是聚合结果位置，与 scan 列下标不一致。
    //    - Filter over SeqScan：把合取项按「能否推到本表」分类，
    //      可下推部分下沉到 scan.predicate，残留合取项保留在 Filter 里。
    //    - Filter over Join：拆合取项，分别尝试推两侧的 SeqScan；
    //      跨表合取项保留在 Filter 里。
    if (plan->GetType() != PlanNodeType::FILTER) return plan;
    if (plan->children.size() != 1) return plan;
    auto& child = plan->children[0];
    if (child->GetType() == PlanNodeType::SEQ_SCAN) {
        return PushDownFilterOverScan(plan);
    }
    if (child->GetType() == PlanNodeType::JOIN) {
        return PushDownFilterOverJoin(plan);
    }
    return plan;
}

// 把 Filter -> SeqScan 形态的下推逻辑封装出来
PlanNodePtr Optimizer::PushDownFilterOverScan(PlanNodePtr plan) {
    auto filter = std::static_pointer_cast<FilterNode>(plan);
    auto scan = std::static_pointer_cast<SeqScanNode>(plan->children[0]);

    std::vector<ExprPtr> conjuncts;
    SplitConjuncts(filter->predicate, &conjuncts);

    std::vector<ExprPtr> residual;
    std::vector<ExprPtr> pushable;
    for (const auto& c : conjuncts) {
        if (CanPushIntoScan(c, scan.get(), catalog_)) {
            pushable.push_back(c);
        } else {
            residual.push_back(c);
        }
    }

    if (pushable.empty()) {
        return plan;
    }

    ExprPtr extra = MakeConjunction(pushable);
    scan->predicate = MergePredicate(scan->predicate, extra);

    if (residual.empty()) {
        return scan;
    }

    filter->predicate = MakeConjunction(residual);
    return plan;
}

// 收集计划子树里 SeqScanNode 的 table_name / table_alias，去重。
// 用于判定 Filter 合取项「只引用左/右两侧哪一侧的列」。
void CollectScanTables(const PlanNodePtr& node, std::unordered_set<std::string>* out) {
    if (!node || !out) return;
    if (node->GetType() == PlanNodeType::SEQ_SCAN) {
        auto s = std::static_pointer_cast<SeqScanNode>(node);
        if (!s->table_name.empty()) out->insert(s->table_name);
        if (!s->table_alias.empty()) out->insert(s->table_alias);
        return;
    }
    for (const auto& c : node->children) CollectScanTables(c, out);
}

// Filter -> Join 形态：拆分合取项，按列所属表分别下沉到两侧的 SeqScan。
//
// 实现策略（仅处理 Join 直接左右子节点是 SeqScan 的常见情形）：
//   - 收集 left / right 子树里出现的所有表名（含 alias）
//   - 对每个合取项 c：
//       c 仅引用 left 表 → 加入 left_pushable
//       c 仅引用 right 表 → 加入 right_pushable
//       c 跨两侧 / 引用其他表 / 含子查询 → 加入 residual
//   - 在 left/right 节点之上插入 Filter(left_pushable) / Filter(right_pushable)
//     （若非空），递归本函数会让内层 Filter 继续下推到 SeqScan.predicate
//   - 跨表合取项保留在外层 Filter 里
PlanNodePtr Optimizer::PushDownFilterOverJoin(PlanNodePtr plan) {
    auto filter = std::static_pointer_cast<FilterNode>(plan);
    auto join = std::static_pointer_cast<JoinNode>(plan->children[0]);
    if (join->children.size() != 2) return plan;

    std::unordered_set<std::string> left_tables, right_tables;
    CollectScanTables(join->children[0], &left_tables);
    CollectScanTables(join->children[1], &right_tables);

    std::vector<ExprPtr> conjuncts;
    SplitConjuncts(filter->predicate, &conjuncts);

    std::vector<ExprPtr> residual, left_pushable, right_pushable;
    for (const auto& c : conjuncts) {
        ColumnUsage u;
        CollectColumns(c, u);
        if (u.has_subquery_or_window) {
            residual.push_back(c);
            continue;
        }
        bool touches_left = false, touches_right = false, touches_other = false;
        for (const auto& [tbl, col] : u.refs) {
            if (tbl.empty()) {
                // 未限定列名：保守地视为跨表，不下沉
                touches_other = true;
                continue;
            }
            if (left_tables.count(tbl)) touches_left = true;
            else if (right_tables.count(tbl)) touches_right = true;
            else touches_other = true;
        }
        if (touches_other || (touches_left && touches_right)) {
            residual.push_back(c);
        } else if (touches_left) {
            left_pushable.push_back(c);
        } else if (touches_right) {
            right_pushable.push_back(c);
        } else {
            // 不引用任何表（常量表达式），保留在 residual 即可
            residual.push_back(c);
        }
    }

    bool changed = false;
    if (!left_pushable.empty()) {
        auto new_left = std::make_shared<FilterNode>(MakeConjunction(left_pushable));
        new_left->children.push_back(join->children[0]);
        join->children[0] = new_left;
        // 立即递归：让内层 Filter 继续下推到 SeqScan.predicate
        join->children[0] = PushDownPredicates(join->children[0]);
        changed = true;
    }
    if (!right_pushable.empty()) {
        auto new_right = std::make_shared<FilterNode>(MakeConjunction(right_pushable));
        new_right->children.push_back(join->children[1]);
        join->children[1] = new_right;
        join->children[1] = PushDownPredicates(join->children[1]);
        changed = true;
    }

    if (!changed) {
        return plan;
    }

    if (residual.empty()) {
        // 外层 Filter 已无残留，直接返回 Join 子树
        return join;
    }
    filter->predicate = MakeConjunction(residual);
    return plan;
}

PlanNodePtr Optimizer::PruneColumns(PlanNodePtr plan) {
    if (!plan) return plan;
    PruneColumnsCtx ctx;
    ctx.catalog = catalog_;
    PruneNode(plan.get(), ctx);
    return plan;
}

namespace {

// 把表达式中所有列引用记录到 ColumnUsage。
// 已存在于本文件中，沿用即可。
struct PruneColumnUsage {
    // 限定列引用（"tbl.col" 或 "alias.col"），用于在 JOIN 上做左右分派
    std::vector<std::pair<std::string, std::string>> qualified;  // (tbl, col)
    // 未限定列引用，只存列名；具体归属哪张表由叶子节点根据 catalog 决定
    std::vector<std::string> unqualified;
};

// 与 PushDownPredicates 中 CollectColumns 同一份语义。复用即可。
void CollectPruneColumns(const ExprPtr& e, PruneColumnUsage& out) {
    if (!e) return;
    switch (e->GetType()) {
        case NodeType::COLUMN_REF_EXPR: {
            const auto* c = static_cast<const ColumnRefExpr*>(e.get());
            if (c->table_name.empty()) {
                out.unqualified.push_back(c->column_name);
            } else {
                out.qualified.emplace_back(c->table_name, c->column_name);
            }
            break;
        }
        case NodeType::BINARY_EXPR: {
            const auto* b = static_cast<const BinaryExpr*>(e.get());
            CollectPruneColumns(b->left, out);
            CollectPruneColumns(b->right, out);
            break;
        }
        case NodeType::UNARY_EXPR: {
            const auto* u = static_cast<const UnaryExpr*>(e.get());
            CollectPruneColumns(u->operand, out);
            break;
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            const auto* f = static_cast<const FunctionCallExpr*>(e.get());
            for (const auto& a : f->arguments) CollectPruneColumns(a, out);
            // FILTER (WHERE ...) 子句里的列也要算进去
            if (f->filter_expr) CollectPruneColumns(f->filter_expr, out);
            break;
        }
        case NodeType::CASE_EXPR: {
            const auto* c = static_cast<const CaseExprNode*>(e.get());
            for (const auto& w : c->whens) {
                CollectPruneColumns(w.when_expr, out);
                CollectPruneColumns(w.then_expr, out);
            }
            CollectPruneColumns(c->else_expr, out);
            break;
        }
        case NodeType::CAST_EXPR: {
            const auto* c = static_cast<const CastExprNode*>(e.get());
            CollectPruneColumns(c->expr, out);
            break;
        }
        case NodeType::LIKE_EXPR: {
            const auto* l = static_cast<const LikeExprNode*>(e.get());
            CollectPruneColumns(l->operand, out);
            CollectPruneColumns(l->pattern, out);
            break;
        }
        case NodeType::EXTRACT_EXPR: {
            const auto* ee = static_cast<const ExtractExprNode*>(e.get());
            CollectPruneColumns(ee->source, out);
            break;
        }
        default:
            break;
    }
}

// 判断 ProjectNode 是否表达「SELECT *」。
// 形态 1：单列 FunctionCallExpr("*" | "STAR")
// 形态 2：单列 ColumnRefExpr(column_name=="*")
// 形态 3：多列 SELECT t.* —— Planner 把 t.* 展开成显式列引用，调用方按
// 普通投影处理；故此处只有前两种形态算 SELECT *。
bool ProjectIsSelectStar(const ProjectNode* proj) {
    if (!proj) return false;
    if (proj->columns.size() != 1 || !proj->columns[0]) return false;
    const auto& e = proj->columns[0];
    if (e->GetType() == NodeType::FUNCTION_CALL_EXPR) {
        const auto* fc = static_cast<const FunctionCallExpr*>(e.get());
        return fc->function_name == "*" || fc->function_name == "STAR";
    }
    if (e->GetType() == NodeType::COLUMN_REF_EXPR) {
        const auto* cr = static_cast<const ColumnRefExpr*>(e.get());
        return cr->column_name == "*" && cr->table_name.empty();
    }
    return false;
}

// 把 (qual, unqual) 中的列挑出属于指定表 (table_name / table_alias) 的列。
// 返回值按表 schema 的列序排列，保证下游 Executor 的 column_index_map
// 与 prune 前一致（前提：catalog 中表的列序未被列裁剪改动）。
std::vector<std::string> ResolveColumnsForTable(
    const std::vector<std::pair<std::string, std::string>>& qualified,
    const std::vector<std::string>& unqualified,
    const std::string& table_name,
    const std::string& table_alias,
    const TableInfo* info) {
    std::vector<std::string> result;
    if (!info) return result;
    auto add_unique = [&](const std::string& col) {
        for (const auto& x : result) {
            if (x == col) return;
        }
        result.push_back(col);
    };
    for (const auto& [tbl, col] : qualified) {
        if (tbl == table_name || (!table_alias.empty() && tbl == table_alias)) {
            add_unique(col);
        }
    }
    for (const auto& col : unqualified) {
        bool found = false;
        for (const auto& ci : info->columns) {
            if (ci.name == col) { found = true; break; }
        }
        if (found) add_unique(col);
    }
    // 按表 schema 列序重新排列，保留下推谓词与上层 column_index_map 的稳定次序
    std::vector<std::string> ordered;
    ordered.reserve(result.size());
    for (const auto& ci : info->columns) {
        for (const auto& c : result) {
            if (c == ci.name) { ordered.push_back(c); break; }
        }
    }
    return ordered;
}

// 在表达式向量上做并集：把每个 expr 的列引用合并到 (qual, unqual)。
void MergeExprColumns(const std::vector<ExprPtr>& exprs,
                      std::vector<std::pair<std::string, std::string>>* qual,
                      std::vector<std::string>* unqual) {
    for (const auto& e : exprs) {
        PruneColumnUsage u;
        CollectPruneColumns(e, u);
        for (auto& q : u.qualified) qual->push_back(std::move(q));
        for (auto& uq : u.unqualified) unqual->push_back(std::move(uq));
    }
}

}  // namespace

// 自顶向下递归 prune：
//   - parent_required：父节点「从本节点输出里消费」的列（限定 + 未限定）
//   - 在本节点需要的列（select_list / predicate / join cond / ...）并入后传给子节点
//   - 叶子节点 (SeqScan / IndexScan) 把「本表实际需要读」的列写到 read_columns
void Optimizer::PruneNode(PlanNode* node, const PruneColumnsCtx& ctx) {
    if (!node) return;

    auto type = node->GetType();
    switch (type) {
        case PlanNodeType::SEQ_SCAN: {
            auto* scan = static_cast<SeqScanNode*>(node);
            const TableInfo* info = ctx.catalog ? ctx.catalog->GetTable(scan->table_name) : nullptr;
            // 本节点作为叶子：把上级要求 + 下推谓词引用的列解析到本表
            std::vector<std::pair<std::string, std::string>> qual = ctx.required_qualified;
            std::vector<std::string> unqual = ctx.required_unqualified;
            // 下推谓词：执行器评估 predicate_ 时也需要这些列
            if (scan->predicate) {
                PruneColumnUsage u;
                CollectPruneColumns(scan->predicate, u);
                for (auto& q : u.qualified) qual.push_back(std::move(q));
                for (auto& uq : u.unqualified) unqual.push_back(std::move(uq));
            }
            scan->read_columns = ResolveColumnsForTable(qual, unqual,
                                                       scan->table_name,
                                                       scan->table_alias,
                                                       info);
            return;
        }
        case PlanNodeType::INDEX_SCAN: {
            auto* scan = static_cast<IndexScanNode*>(node);
            const TableInfo* info = ctx.catalog ? ctx.catalog->GetTable(scan->table_name) : nullptr;
            std::vector<std::pair<std::string, std::string>> qual = ctx.required_qualified;
            std::vector<std::string> unqual = ctx.required_unqualified;
            // residual_predicate 回表后还要再判一次
            if (scan->residual_predicate) {
                PruneColumnUsage u;
                CollectPruneColumns(scan->residual_predicate, u);
                for (auto& q : u.qualified) qual.push_back(std::move(q));
                for (auto& uq : u.unqualified) unqual.push_back(std::move(uq));
            }
            scan->read_columns = ResolveColumnsForTable(qual, unqual,
                                                       scan->table_name,
                                                       scan->table_alias,
                                                       info);
            return;
        }

        case PlanNodeType::PROJECT: {
            auto* proj = static_cast<ProjectNode*>(node);
            if (ProjectIsSelectStar(proj)) {
                // SELECT * —— 不裁剪，让子计划原样保留。
                // 即使父节点要求了某些列，SELECT * 的语义是「输出源的全部列」，
                // 强行裁剪会破坏下游 ORDER BY 之类的隐式列引用。
                return;
            }
            // select_list 的列需要从子节点读取
            std::vector<std::pair<std::string, std::string>> qual;
            std::vector<std::string> unqual;
            MergeExprColumns(proj->columns, &qual, &unqual);
            // 与父节点要求并集（理论上父节点要求通常已被 select_list 覆盖；
            // 但若父节点是 Aggregate / Sort 等引用了 select_list 之外的列，
            // 这里要补上）
            for (auto& q : ctx.required_qualified) qual.push_back(q);
            for (auto& uq : ctx.required_unqualified) unqual.push_back(uq);
            PruneColumnsCtx child = ctx;
            child.required_qualified = std::move(qual);
            child.required_unqualified = std::move(unqual);
            for (auto& c : node->children) PruneNode(c.get(), child);
            return;
        }

        case PlanNodeType::FILTER: {
            auto* f = static_cast<FilterNode*>(node);
            std::vector<std::pair<std::string, std::string>> qual = ctx.required_qualified;
            std::vector<std::string> unqual = ctx.required_unqualified;
            // 谓词用到的列也要从子节点读取
            PruneColumnUsage u;
            CollectPruneColumns(f->predicate, u);
            for (auto& q : u.qualified) qual.push_back(std::move(q));
            for (auto& uq : u.unqualified) unqual.push_back(uq);
            PruneColumnsCtx child = ctx;
            child.required_qualified = std::move(qual);
            child.required_unqualified = std::move(unqual);
            for (auto& c : node->children) PruneNode(c.get(), child);
            return;
        }

        case PlanNodeType::SORT: {
            auto* s = static_cast<SortNode*>(node);
            std::vector<std::pair<std::string, std::string>> qual = ctx.required_qualified;
            std::vector<std::string> unqual = ctx.required_unqualified;
            // ORDER BY 用到的列
            for (const auto& it : s->order_items) {
                PruneColumnUsage u;
                CollectPruneColumns(it.expr, u);
                for (auto& q : u.qualified) qual.push_back(std::move(q));
                for (auto& uq : u.unqualified) unqual.push_back(uq);
            }
            PruneColumnsCtx child = ctx;
            child.required_qualified = std::move(qual);
            child.required_unqualified = std::move(unqual);
            for (auto& c : node->children) PruneNode(c.get(), child);
            return;
        }

        case PlanNodeType::AGGREGATE: {
            auto* a = static_cast<AggregateNode*>(node);
            // 聚合需要子节点提供：GROUP BY 列 + 聚合函数参数列。
            // 父节点要求保留（让 Filter/HAVING 等上层能引用聚合输出位置）。
            std::vector<std::pair<std::string, std::string>> qual = ctx.required_qualified;
            std::vector<std::string> unqual = ctx.required_unqualified;
            MergeExprColumns(a->group_by_exprs, &qual, &unqual);
            MergeExprColumns(a->aggregate_exprs, &qual, &unqual);
            PruneColumnsCtx child = ctx;
            child.required_qualified = std::move(qual);
            child.required_unqualified = std::move(unqual);
            for (auto& c : node->children) PruneNode(c.get(), child);
            return;
        }

        case PlanNodeType::WINDOW: {
            auto* w = static_cast<WindowNode*>(node);
            std::vector<std::pair<std::string, std::string>> qual = ctx.required_qualified;
            std::vector<std::string> unqual = ctx.required_unqualified;
            MergeExprColumns(w->select_list, &qual, &unqual);
            // PARTITION BY / ORDER BY 内引用的列
            for (const auto& [name, spec] : w->named_windows) {
                (void)name;
                MergeExprColumns(spec.partition_by, &qual, &unqual);
                for (const auto& it : spec.order_by) {
                    PruneColumnUsage u;
                    CollectPruneColumns(it.expr, u);
                    for (auto& q : u.qualified) qual.push_back(std::move(q));
                    for (auto& uq : u.unqualified) unqual.push_back(uq);
                }
            }
            PruneColumnsCtx child = ctx;
            child.required_qualified = std::move(qual);
            child.required_unqualified = std::move(unqual);
            for (auto& c : node->children) PruneNode(c.get(), child);
            return;
        }

        case PlanNodeType::JOIN: {
            auto* j = static_cast<JoinNode*>(node);
            // 父节点要求 + join condition 用到的列合并后，按表限定分派到左右两侧
            std::vector<std::pair<std::string, std::string>> all_qual = ctx.required_qualified;
            std::vector<std::string> all_unqual = ctx.required_unqualified;
            {
                PruneColumnUsage u;
                CollectPruneColumns(j->condition, u);
                for (auto& q : u.qualified) all_qual.push_back(std::move(q));
                for (auto& uq : u.unqualified) all_unqual.push_back(uq);
            }
            // 收集左右子树中的所有表名（含 alias），便于限定列分派
            std::unordered_set<std::string> left_tables, right_tables;
            CollectScanTables(j->children[0], &left_tables);
            CollectScanTables(j->children[1], &right_tables);
            // 分派
            std::vector<std::pair<std::string, std::string>> lq, rq;
            std::vector<std::string> lu = all_unqual, ru = all_unqual;  // 未限定列下放到两侧（保守）
            for (auto& q : all_qual) {
                const auto& tbl = q.first;
                if (left_tables.count(tbl)) lq.push_back(q);
                else if (right_tables.count(tbl)) rq.push_back(q);
                else { lq.push_back(q); rq.push_back(q); }  // 无法判定：下放到两侧
            }
            if (node->children.size() >= 1) {
                PruneColumnsCtx lctx = ctx;
                lctx.required_qualified = std::move(lq);
                lctx.required_unqualified = std::move(lu);
                PruneNode(node->children[0].get(), lctx);
            }
            if (node->children.size() >= 2) {
                PruneColumnsCtx rctx = ctx;
                rctx.required_qualified = std::move(rq);
                rctx.required_unqualified = std::move(ru);
                PruneNode(node->children[1].get(), rctx);
            }
            return;
        }

        case PlanNodeType::LIMIT: {
            // LIMIT 不增加额外列要求，原样向下传播
            for (auto& c : node->children) PruneNode(c.get(), ctx);
            return;
        }

        case PlanNodeType::SET_OP: {
            // UNION / INTERSECT / EXCEPT：两侧需要产出相同的列结构。
            // 这里把父节点要求原样下传给两侧。
            for (auto& c : node->children) PruneNode(c.get(), ctx);
            return;
        }

        case PlanNodeType::VALUES: {
            // 字面量行，无需列裁剪
            return;
        }

        case PlanNodeType::SUBQUERY: {
            // SubqueryExprNode 把 subquery_plan 挂在自己的 subquery_plan 上。
            // 本节点（SubqueryNode）本身没有可裁剪的内容，跳过。
            return;
        }

        case PlanNodeType::APPLY: {
            // LATERAL：内层可能引用外层列。保守起见把父节点要求透传给外层，
            // 内层作为独立子查询重新裁剪。
            if (node->children.size() >= 1) {
                PruneNode(node->children[0].get(), ctx);
            }
            if (node->children.size() >= 2) {
                PruneColumnsCtx inner = ctx;
                inner.required_qualified.clear();
                inner.required_unqualified.clear();
                PruneNode(node->children[1].get(), inner);
            }
            return;
        }

        // ---- DML：叶子为表本身，执行器按列名直接更新，不需要 prune。
        // 但 INSERT/UPDATE/MERGE 可能携带 SELECT 子计划，仍要独立裁剪。
        case PlanNodeType::INSERT: {
            auto* ins = static_cast<InsertNode*>(node);
            if (ins->query_plan) PruneNode(ins->query_plan.get(), PruneColumnsCtx{ctx.catalog, {}, {}});
            // returning_exprs 中的列也需要从源计划读取
            if (!ins->returning_exprs.empty() && ins->query_plan) {
                std::vector<std::pair<std::string, std::string>> qual;
                std::vector<std::string> unqual;
                MergeExprColumns(ins->returning_exprs, &qual, &unqual);
                PruneColumnsCtx sub = ctx;
                sub.required_qualified = std::move(qual);
                sub.required_unqualified = std::move(unqual);
                PruneNode(ins->query_plan.get(), sub);
            }
            return;
        }
        case PlanNodeType::UPDATE: {
            auto* upd = static_cast<UpdateNode*>(node);
            // 没有子计划：执行器按 table_name 直接改写，跳过
            (void)upd;
            return;
        }
        case PlanNodeType::UPDATE_FROM: {
            // children[0] 是 JoinNode 子计划，作为独立查询裁剪
            if (!node->children.empty()) {
                PruneNode(node->children[0].get(), PruneColumnsCtx{ctx.catalog, {}, {}});
            }
            return;
        }
        case PlanNodeType::DELETE: {
            return;  // 同 UPDATE，无子计划
        }
        case PlanNodeType::UPSERT: {
            auto* up = static_cast<UpsertNode*>(node);
            // VALUES 路径无子计划；如有 query_plan（未来扩展）则裁剪
            (void)up;
            return;
        }
        case PlanNodeType::MERGE: {
            auto* m = static_cast<MergeNode*>(node);
            if (m->source_plan) PruneNode(m->source_plan.get(), PruneColumnsCtx{ctx.catalog, {}, {}});
            return;
        }

        // ---- DDL / 元命令 / no-op：跳过 ----
        case PlanNodeType::CREATE_TABLE:
        case PlanNodeType::DROP_TABLE:
        case PlanNodeType::TRUNCATE_TABLE:
        case PlanNodeType::CREATE_INDEX:
        case PlanNodeType::DROP_INDEX:
        case PlanNodeType::ALTER_TABLE:
        case PlanNodeType::CREATE_SCHEMA:
        case PlanNodeType::DROP_SCHEMA:
        case PlanNodeType::CREATE_SEQUENCE:
        case PlanNodeType::DROP_SEQUENCE:
        case PlanNodeType::CREATE_VIEW:
        case PlanNodeType::CREATE_TRIGGER:
        case PlanNodeType::CREATE_FUNCTION:
        case PlanNodeType::CREATE_PROCEDURE:
        case PlanNodeType::CREATE_MATERIALIZED_VIEW:
        case PlanNodeType::ALTER_MATERIALIZED_VIEW:
        case PlanNodeType::NO_OP:
        case PlanNodeType::CALL:
        case PlanNodeType::SHOW:
        case PlanNodeType::EXPLAIN:
        case PlanNodeType::BEGIN_TXN:
        case PlanNodeType::COMMIT_TXN:
        case PlanNodeType::ROLLBACK_TXN:
        case PlanNodeType::SAVEPOINT:
        case PlanNodeType::ROLLBACK_TO_SP:
        case PlanNodeType::RELEASE_SP:
        case PlanNodeType::VIEW_DEFINE:
        case PlanNodeType::CTE_BIND:
        case PlanNodeType::CTE_DEFINE:
            return;

        default:
            // 未知/未覆盖节点：保守地向下递归
            for (auto& c : node->children) PruneNode(c.get(), ctx);
            return;
    }
}

// =====================================================================
// 常量折叠（constant folding）
// =====================================================================
// 目标：在优化期把表达式里所有可化简的常量子表达式化成一个 LiteralExpr。
// 例如 `1 + 2 * 3` → `7`、`'a' || 'b'` → `'ab'`、`1 = 1` → `1`（TRUE）。
// 对于 FilterNode，若其谓词被化简为已知 TRUE 的字面量，则直接以子节点替换
// 该 Filter（谓词恒假则保留，FilterExecutor 会自然返回空集）。
//
// 设计要点：
//   - 不修改 AST 节点的位置（line/column），仅替换共享指针的指向；
//     这点对执行期错误定位很重要。
//   - 不递归下推子查询 / 聚合 / 窗口函数 / NEXTVAL —— 它们包含运行时语义，
//     折叠它们会改变语义。
//   - 对布尔结果统一用 LiteralType::INTEGER 表示（"1"/"0"），与执行期
//     BOOLEAN/BOOL = INTEGER 的约定一致。

namespace {

// ---------- 字面量 ↔ Value 互转 ----------

Value LiteralToValue(const LiteralExpr& lit) {
    switch (lit.literal_type) {
        case LiteralType::INTEGER:
            try {
                return Value::MakeInt(static_cast<int32_t>(std::stoll(lit.value)));
            } catch (...) {
                return Value::MakeNull();
            }
        case LiteralType::FLOAT:
            try {
                return Value::MakeFloat(std::stod(lit.value));
            } catch (...) {
                return Value::MakeNull();
            }
        case LiteralType::STRING:
            return Value::MakeVarchar(lit.value);
        case LiteralType::BOOLEAN: {
            // BOOLEAN 字面量在 lexer 阶段以 "TRUE"/"FALSE" 字符串形式存储。
            const std::string& v = lit.value;
            bool t = (v == "TRUE" || v == "true" || v == "1");
            return Value::MakeInt(t ? 1 : 0);
        }
        case LiteralType::NULL_VALUE:
        default:
            return Value::MakeNull();
    }
}

ExprPtr ValueToLiteral(const Value& v) {
    if (v.IsNull()) {
        return std::make_shared<LiteralExpr>(LiteralType::NULL_VALUE, "NULL");
    }
    switch (v.GetType()) {
        case ValueType::INTEGER:
            return std::make_shared<LiteralExpr>(
                LiteralType::INTEGER, std::to_string(v.AsInt()));
        case ValueType::FLOAT: {
            // std::tostring 在 double 上输出 6 位有效数字；改用 snprintf 保留
            // 完整精度（与 ExpressionEvaluator::EvaluateLiteral 对齐）。
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%g", v.AsFloat());
            return std::make_shared<LiteralExpr>(LiteralType::FLOAT, buf);
        }
        case ValueType::VARCHAR:
            return std::make_shared<LiteralExpr>(LiteralType::STRING, v.AsVarchar());
        case ValueType::NULL_TYPE:
            return std::make_shared<LiteralExpr>(LiteralType::NULL_VALUE, "NULL");
    }
    return nullptr;
}

// ---------- 字面量真假判定 ----------

bool IsLiteralTrueExpr(const ExprPtr& e) {
    if (!e || e->GetType() != NodeType::LITERAL_EXPR) return false;
    const auto* lit = static_cast<const LiteralExpr*>(e.get());
    switch (lit->literal_type) {
        case LiteralType::BOOLEAN:
            return lit->value == "TRUE" || lit->value == "true" || lit->value == "1";
        case LiteralType::INTEGER:
            try { return std::stoi(lit->value) != 0; } catch (...) { return false; }
        case LiteralType::NULL_VALUE:
            return false;
        default:
            return false;
    }
}

bool IsLiteralFalseExpr(const ExprPtr& e) {
    if (!e || e->GetType() != NodeType::LITERAL_EXPR) return false;
    const auto* lit = static_cast<const LiteralExpr*>(e.get());
    switch (lit->literal_type) {
        case LiteralType::BOOLEAN:
            return lit->value == "FALSE" || lit->value == "false" || lit->value == "0";
        case LiteralType::INTEGER:
            try { return std::stoi(lit->value) == 0; } catch (...) { return false; }
        case LiteralType::NULL_VALUE:
            return false;
        default:
            return false;
    }
}

// 谓词上下文里"恒真"：除 TRUE/非零 INT 外，字符串非空、非空集合等也算。
// 实际只用上面两个 is* 函数，但保留接口对称。
bool IsConstTrueForFilter(const ExprPtr& e) { return IsLiteralTrueExpr(e); }

// ---------- 折叠结果构造辅助 ----------

// 返回 std::optional<Value> 形式不便于表达「无法折叠」，改用
// 一个 bool + Value 形式：bool=false 表示「无法折叠成常量」。
struct FoldResult {
    bool ok = false;
    Value v;
};

// ---------- 二元运算常量折叠 ----------
//
// 用 Value 直接参与算术，再以 Value::Compare 做比较。两端类型不同时，按
// 既有语义提升：INT + FLOAT → FLOAT；INT/FLOAT 与 VARCHAR 之间尝试按数值解析。
//
// NULL 传播规则：任一操作数为 NULL，则结果为 NULL（三值逻辑）。但「比较
// 结果本身就是 NULL」的情况不折叠（避免把 `NULL = 1` 折叠成 NULL literal，
// 这会让原本被 FilterExecutor 当作不通过的行继续保留，破坏语义）。

static FoldResult FoldBinary(BinaryOperator op, const Value& lv, const Value& rv) {
    FoldResult r;
    // 任一为 NULL：保留为不可折叠（FilterExecutor 会按 NULL 处理），只有
    // IS NULL / IS NOT NULL 显式语义下才折叠。
    bool l_null = lv.IsNull();
    bool r_null = rv.IsNull();
    if (l_null || r_null) {
        if (op == BinaryOperator::IS_NULL) {
            r.ok = true; r.v = Value::MakeInt(l_null ? 1 : 0); return r;
        }
        if (op == BinaryOperator::IS_NOT_NULL) {
            r.ok = true; r.v = Value::MakeInt(l_null ? 0 : 1); return r;
        }
        return r;  // 不可折叠
    }
    // 数值提升
    auto promote = [](Value x) -> Value {
        if (x.GetType() == ValueType::VARCHAR) {
            try {
                size_t pos = 0;
                std::string s = x.AsVarchar();
                while (pos < s.size() &&
                       std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
                if (pos >= s.size()) return Value::MakeNull();
                double d = std::stod(s, &pos);
                if (s.find('.') != std::string::npos ||
                    s.find('e') != std::string::npos ||
                    s.find('E') != std::string::npos) {
                    return Value::MakeFloat(d);
                }
                return Value::MakeInt(static_cast<int32_t>(d));
            } catch (...) {
                return Value::MakeNull();
            }
        }
        return x;
    };
    Value L = promote(lv);
    Value R = promote(rv);
    if (L.IsNull() || R.IsNull()) return r;

    auto numeric_promote = [](Value a, Value b) -> std::pair<Value, Value> {
        if (a.GetType() == b.GetType()) return {a, b};
        if ((a.GetType() == ValueType::INTEGER || a.GetType() == ValueType::FLOAT) &&
            (b.GetType() == ValueType::INTEGER || b.GetType() == ValueType::FLOAT)) {
            if (a.GetType() == ValueType::INTEGER)
                a = Value::MakeFloat(static_cast<double>(a.AsInt()));
            if (b.GetType() == ValueType::INTEGER)
                b = Value::MakeFloat(static_cast<double>(b.AsInt()));
        }
        return {a, b};
    };

    switch (op) {
        case BinaryOperator::ADD: {
            auto [x, y] = numeric_promote(L, R);
            if (x.GetType() == ValueType::FLOAT) { r.ok = true; r.v = Value::MakeFloat(x.AsFloat() + y.AsFloat()); }
            else { r.ok = true; r.v = Value::MakeInt(x.AsInt() + y.AsInt()); }
            return r;
        }
        case BinaryOperator::SUB: {
            auto [x, y] = numeric_promote(L, R);
            if (x.GetType() == ValueType::FLOAT) { r.ok = true; r.v = Value::MakeFloat(x.AsFloat() - y.AsFloat()); }
            else { r.ok = true; r.v = Value::MakeInt(x.AsInt() - y.AsInt()); }
            return r;
        }
        case BinaryOperator::MUL: {
            auto [x, y] = numeric_promote(L, R);
            if (x.GetType() == ValueType::FLOAT) { r.ok = true; r.v = Value::MakeFloat(x.AsFloat() * y.AsFloat()); }
            else { r.ok = true; r.v = Value::MakeInt(x.AsInt() * y.AsInt()); }
            return r;
        }
        case BinaryOperator::DIV: {
            auto [x, y] = numeric_promote(L, R);
            if (y.GetType() == ValueType::FLOAT ? (y.AsFloat() == 0.0) : (y.AsInt() == 0)) return r;
            if (x.GetType() == ValueType::FLOAT || y.GetType() == ValueType::FLOAT) {
                r.ok = true; r.v = Value::MakeFloat(x.AsFloat() / y.AsFloat());
            } else {
                r.ok = true; r.v = Value::MakeInt(x.AsInt() / y.AsInt());
            }
            return r;
        }
        case BinaryOperator::CONCAT: {
            r.ok = true;
            r.v = Value::MakeVarchar(L.AsVarchar() + R.AsVarchar());
            return r;
        }
        case BinaryOperator::EQUAL: {
            r.ok = true;
            r.v = Value::MakeInt(Value::Compare(L, R) == 0 ? 1 : 0);
            return r;
        }
        case BinaryOperator::NOT_EQUAL: {
            r.ok = true;
            r.v = Value::MakeInt(Value::Compare(L, R) != 0 ? 1 : 0);
            return r;
        }
        case BinaryOperator::LESS: {
            r.ok = true;
            r.v = Value::MakeInt(Value::Compare(L, R) < 0 ? 1 : 0);
            return r;
        }
        case BinaryOperator::LESS_EQUAL: {
            r.ok = true;
            r.v = Value::MakeInt(Value::Compare(L, R) <= 0 ? 1 : 0);
            return r;
        }
        case BinaryOperator::GREATER: {
            r.ok = true;
            r.v = Value::MakeInt(Value::Compare(L, R) > 0 ? 1 : 0);
            return r;
        }
        case BinaryOperator::GREATER_EQUAL: {
            r.ok = true;
            r.v = Value::MakeInt(Value::Compare(L, R) >= 0 ? 1 : 0);
            return r;
        }
        case BinaryOperator::AND: {
            r.ok = true;
            // 三值逻辑：任一为 NULL 结果 NULL；这里两端都非 NULL（前面已判定）。
            int li = (L.GetType() == ValueType::INTEGER) ? L.AsInt() : 0;
            int ri = (R.GetType() == ValueType::INTEGER) ? R.AsInt() : 0;
            r.v = Value::MakeInt((li != 0 && ri != 0) ? 1 : 0);
            return r;
        }
        case BinaryOperator::OR: {
            r.ok = true;
            int li = (L.GetType() == ValueType::INTEGER) ? L.AsInt() : 0;
            int ri = (R.GetType() == ValueType::INTEGER) ? R.AsInt() : 0;
            r.v = Value::MakeInt((li != 0 || ri != 0) ? 1 : 0);
            return r;
        }
        case BinaryOperator::IS_NULL: {
            // 已由前置 NULL 分支处理过，这里再保险一次
            r.ok = true;
            r.v = Value::MakeInt(0);
            return r;
        }
        case BinaryOperator::IS_NOT_NULL: {
            r.ok = true;
            r.v = Value::MakeInt(1);
            return r;
        }
        default:
            return r;  // LIKE / BETWEEN / IN_LIST / INTERVAL_* 不在折叠范围
    }
}

// ---------- 一元运算常量折叠 ----------

static FoldResult FoldUnary(UnaryOperator op, const Value& v) {
    FoldResult r;
    if (v.IsNull()) return r;
    switch (op) {
        case UnaryOperator::NEGATE:
            r.ok = true;
            if (v.GetType() == ValueType::FLOAT) r.v = Value::MakeFloat(-v.AsFloat());
            else r.v = Value::MakeInt(-v.AsInt());
            return r;
        case UnaryOperator::NOT: {
            // SQL 布尔值约定：INTEGER 非 0 即真
            r.ok = true;
            int x = (v.GetType() == ValueType::INTEGER) ? v.AsInt() : 0;
            r.v = Value::MakeInt(x == 0 ? 1 : 0);
            return r;
        }
    }
    return r;
}

// ---------- CAST 常量折叠 ----------
//
// 简化版：只处理 INT / FLOAT / VARCHAR 三种目标类型；对 DateTime 解析
// 留作运行期。

static FoldResult FoldCast(const std::string& target_type, const Value& v) {
    FoldResult r;
    if (v.IsNull()) return r;
    std::string u = target_type;
    std::transform(u.begin(), u.end(), u.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (u == "INT" || u == "INTEGER" || u == "BIGINT") {
        if (v.GetType() == ValueType::INTEGER) { r.ok = true; r.v = v; return r; }
        if (v.GetType() == ValueType::FLOAT) {
            r.ok = true; r.v = Value::MakeInt(static_cast<int32_t>(v.AsFloat())); return r;
        }
        if (v.GetType() == ValueType::VARCHAR) {
            try {
                r.ok = true;
                r.v = Value::MakeInt(static_cast<int32_t>(std::stol(v.AsVarchar())));
            } catch (...) { /* 不可折叠 */ }
            return r;
        }
    } else if (u == "FLOAT" || u == "DOUBLE" || u == "REAL") {
        if (v.GetType() == ValueType::FLOAT) { r.ok = true; r.v = v; return r; }
        if (v.GetType() == ValueType::INTEGER) {
            r.ok = true; r.v = Value::MakeFloat(static_cast<double>(v.AsInt())); return r;
        }
        if (v.GetType() == ValueType::VARCHAR) {
            try {
                r.ok = true;
                r.v = Value::MakeFloat(std::stod(v.AsVarchar()));
            } catch (...) { /* 不可折叠 */ }
            return r;
        }
    } else if (u == "VARCHAR" || u == "STRING" || u == "TEXT" || u == "CHAR") {
        r.ok = true;
        if (v.GetType() == ValueType::VARCHAR) r.v = v;
        else if (v.GetType() == ValueType::INTEGER) {
            r.v = Value::MakeVarchar(std::to_string(v.AsInt()));
        } else if (v.GetType() == ValueType::FLOAT) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%g", v.AsFloat());
            r.v = Value::MakeVarchar(buf);
        }
        return r;
    }
    return r;
}

// ---------- 函数调用常量折叠 ----------
//
// 只实现无副作用、与运行结果一致的内建函数：
//   UPPER / LOWER / LENGTH / CHAR_LENGTH / ABS / COALESCE（两参简化版）
// 其他函数（聚合 / 日期 / CURRENT_TIMESTAMP 等）不折叠。

static FoldResult FoldFunctionCall(const std::string& name,
                                   const std::vector<Value>& args) {
    FoldResult r;
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    auto need_str = [&](const Value& v) -> std::string {
        if (v.IsNull()) return std::string();
        if (v.GetType() == ValueType::VARCHAR) return v.AsVarchar();
        if (v.GetType() == ValueType::INTEGER) return std::to_string(v.AsInt());
        if (v.GetType() == ValueType::FLOAT) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%g", v.AsFloat());
            return buf;
        }
        return std::string();
    };
    if (n == "UPPER" || n == "UCASE") {
        if (args.size() != 1 || args[0].IsNull()) return r;
        std::string s = need_str(args[0]);
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        r.ok = true; r.v = Value::MakeVarchar(s); return r;
    }
    if (n == "LOWER" || n == "LCASE") {
        if (args.size() != 1 || args[0].IsNull()) return r;
        std::string s = need_str(args[0]);
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        r.ok = true; r.v = Value::MakeVarchar(s); return r;
    }
    if (n == "LENGTH" || n == "CHAR_LENGTH" || n == "CHARACTER_LENGTH") {
        if (args.size() != 1 || args[0].IsNull()) return r;
        r.ok = true; r.v = Value::MakeInt(static_cast<int32_t>(need_str(args[0]).size()));
        return r;
    }
    if (n == "ABS") {
        if (args.size() != 1 || args[0].IsNull()) return r;
        if (args[0].GetType() == ValueType::FLOAT) {
            r.ok = true; r.v = Value::MakeFloat(std::fabs(args[0].AsFloat())); return r;
        }
        r.ok = true; r.v = Value::MakeInt(std::abs(args[0].AsInt())); return r;
    }
    if (n == "COALESCE") {
        for (const auto& a : args) {
            if (!a.IsNull()) { r.ok = true; r.v = a; return r; }
        }
        return r;  // 全部 NULL：保留为不可折叠（运行期语义是 NULL）
    }
    if (n == "NULLIF") {
        if (args.size() != 2) return r;
        if (Value::Compare(args[0], args[1]) == 0) {
            r.ok = true; r.v = Value::MakeNull(); return r;
        }
        r.ok = true; r.v = args[0]; return r;
    }
    return r;
}

// ---------- CASE 常量折叠 ----------

static ExprPtr FoldCaseExpr(const CaseExprNode& c) {
    // 要求 subject（若存在）/所有 when_expr /then_expr / else_expr 都为常量
    for (const auto& w : c.whens) {
        if (!IsConstTrueForFilter(w.when_expr) && !IsLiteralFalseExpr(w.when_expr)
            && !(w.when_expr && w.when_expr->GetType() == NodeType::LITERAL_EXPR)) {
            // 上面的判定是为了再走一次简化路径判定；只要有任一非常量就放弃
            if (!w.when_expr || w.when_expr->GetType() != NodeType::LITERAL_EXPR) return nullptr;
        }
        if (!w.then_expr || w.then_expr->GetType() != NodeType::LITERAL_EXPR) return nullptr;
    }
    if (c.subject) {
        if (c.subject->GetType() != NodeType::LITERAL_EXPR) return nullptr;
        Value subj = LiteralToValue(*static_cast<const LiteralExpr*>(c.subject.get()));
        for (const auto& w : c.whens) {
            Value when = LiteralToValue(*static_cast<const LiteralExpr*>(w.when_expr.get()));
            if (Value::Compare(subj, when) == 0) {
                return w.then_expr;
            }
        }
        if (c.else_expr && c.else_expr->GetType() == NodeType::LITERAL_EXPR) {
            return c.else_expr;
        }
        return nullptr;
    } else {
        // 搜索式 CASE：按 WHEN 顺序求值
        for (const auto& w : c.whens) {
            if (IsConstTrueForFilter(w.when_expr)) return w.then_expr;
        }
        if (c.else_expr && c.else_expr->GetType() == NodeType::LITERAL_EXPR) {
            return c.else_expr;
        }
        return nullptr;
    }
}

// ---------- LIKE 常量折叠 ----------
//
// 简化：仅处理操作数 + pattern 都为 STRING 字面量的情形。
// 完整 LIKE 语义（含 ESCAPE）与运行期 MatchLikePattern 保持一致，
// 因此这里只做"两边都是常量 → 调用相同逻辑"。

static bool SqlLikeMatch(const std::string& s, const std::string& pat, char esc) {
    auto match_impl = [](auto&& self, const std::string& s, size_t si,
                         const std::string& pat, size_t pi, char esc) -> bool {
        while (pi < pat.size()) {
            char pc = pat[pi];
            if (pc == esc && pi + 1 < pat.size()) {
                if (si >= s.size() || s[si] != pat[pi + 1]) return false;
                si++; pi += 2;
            } else if (pc == '%') {
                // 跳过连续 %
                while (pi < pat.size() && pat[pi] == '%') ++pi;
                if (pi >= pat.size()) return true;
                for (; si <= s.size(); ++si) {
                    if (self(self, s, si, pat, pi, esc)) return true;
                }
                return false;
            } else if (pc == '_') {
                if (si >= s.size()) return false;
                si++; pi++;
            } else {
                if (si >= s.size() || s[si] != pc) return false;
                si++; pi++;
            }
        }
        return si == s.size();
    };
    return match_impl(match_impl, s, 0, pat, 0, esc);
}

// ---------- 表达式级 FoldExpr ----------

ExprPtr FoldExpr(const ExprPtr& e) {
    if (!e) return e;
    switch (e->GetType()) {
        case NodeType::LITERAL_EXPR:
        case NodeType::COLUMN_REF_EXPR:
            return e;
        case NodeType::UNARY_EXPR: {
            const auto* u = static_cast<const UnaryExpr*>(e.get());
            auto op = FoldExpr(u->operand);
            if (!op) return e;
            if (op->GetType() == NodeType::LITERAL_EXPR) {
                auto v = LiteralToValue(*static_cast<const LiteralExpr*>(op.get()));
                auto fr = FoldUnary(u->op, v);
                if (fr.ok) return ValueToLiteral(fr.v);
            }
            if (op != u->operand) return std::make_shared<UnaryExpr>(u->op, op);
            return e;
        }
        case NodeType::BINARY_EXPR: {
            const auto* b = static_cast<const BinaryExpr*>(e.get());
            auto L = FoldExpr(b->left);
            auto R = FoldExpr(b->right);
            // 代数恒等式（AND/OR + TRUE/FALSE）
            if (b->op == BinaryOperator::AND) {
                if (IsLiteralFalseExpr(L) || IsLiteralFalseExpr(R)) {
                    return std::make_shared<LiteralExpr>(LiteralType::INTEGER, "0");
                }
                if (IsConstTrueForFilter(L)) return R;
                if (IsConstTrueForFilter(R)) return L;
            }
            if (b->op == BinaryOperator::OR) {
                if (IsConstTrueForFilter(L) || IsConstTrueForFilter(R)) {
                    return std::make_shared<LiteralExpr>(LiteralType::INTEGER, "1");
                }
                if (IsLiteralFalseExpr(L)) return R;
                if (IsLiteralFalseExpr(R)) return L;
            }
            // BETWEEN / IN_LIST 简化处理
            if (b->op == BinaryOperator::BETWEEN) {
                // a BETWEEN b AND c 语义为 b<=a AND a<=c；折叠为三值 AND
                if (L && R && R->GetType() == NodeType::BINARY_EXPR) {
                    const auto* bc = static_cast<const BinaryExpr*>(R.get());
                    if (bc->op == BinaryOperator::AND) {
                        auto inner = std::make_shared<BinaryExpr>(
                            BinaryOperator::AND,
                            std::make_shared<BinaryExpr>(BinaryOperator::LESS_EQUAL, bc->left, L),
                            std::make_shared<BinaryExpr>(BinaryOperator::LESS_EQUAL, L, bc->right));
                        return FoldExpr(inner);
                    }
                }
            }
            // 两端都是字面量：尝试完整折叠
            if (L && R && L->GetType() == NodeType::LITERAL_EXPR &&
                R->GetType() == NodeType::LITERAL_EXPR) {
                auto lv = LiteralToValue(*static_cast<const LiteralExpr*>(L.get()));
                auto rv = LiteralToValue(*static_cast<const LiteralExpr*>(R.get()));
                auto fr = FoldBinary(b->op, lv, rv);
                if (fr.ok) return ValueToLiteral(fr.v);
            }
            // 子表达式有更新但未整体折叠 → 重建
            if (L != b->left || R != b->right) {
                return std::make_shared<BinaryExpr>(b->op, L, R);
            }
            return e;
        }
        case NodeType::CAST_EXPR: {
            const auto* c = static_cast<const CastExprNode*>(e.get());
            auto inner = FoldExpr(c->expr);
            if (inner && inner->GetType() == NodeType::LITERAL_EXPR) {
                auto v = LiteralToValue(*static_cast<const LiteralExpr*>(inner.get()));
                auto fr = FoldCast(c->target_type, v);
                if (fr.ok) return ValueToLiteral(fr.v);
            }
            if (inner != c->expr) return std::make_shared<CastExprNode>(inner, c->target_type);
            return e;
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            const auto* f = static_cast<const FunctionCallExpr*>(e.get());
            // 聚合 / 过滤 / WITHIN GROUP 不在折叠范围
            if (f->is_distinct || f->filter_expr || !f->within_group_order_by.empty()) {
                return e;
            }
            std::vector<ExprPtr> new_args;
            new_args.reserve(f->arguments.size());
            bool any_changed = false;
            std::vector<Value> arg_values;
            arg_values.reserve(f->arguments.size());
            for (const auto& a : f->arguments) {
                auto folded = FoldExpr(a);
                if (folded != a) any_changed = true;
                if (!folded || folded->GetType() != NodeType::LITERAL_EXPR) {
                    return e;  // 任一参数无法折叠 → 放弃整体折叠
                }
                new_args.push_back(folded);
                arg_values.push_back(LiteralToValue(*static_cast<const LiteralExpr*>(folded.get())));
            }
            (void)new_args; (void)any_changed;
            auto fr = FoldFunctionCall(f->function_name, arg_values);
            if (fr.ok) return ValueToLiteral(fr.v);
            return e;
        }
        case NodeType::LIKE_EXPR: {
            const auto* l = static_cast<const LikeExprNode*>(e.get());
            auto op = FoldExpr(l->operand);
            auto pat = FoldExpr(l->pattern);
            if (op && pat && op->GetType() == NodeType::LITERAL_EXPR &&
                pat->GetType() == NodeType::LITERAL_EXPR) {
                const auto* op_lit = static_cast<const LiteralExpr*>(op.get());
                const auto* pat_lit = static_cast<const LiteralExpr*>(pat.get());
                if (op_lit->literal_type == LiteralType::STRING &&
                    pat_lit->literal_type == LiteralType::STRING) {
                    char esc = l->has_escape ? l->escape_char : '\\';
                    // ILIKE: 大小写不敏感比较
                    std::string s = op_lit->value, p = pat_lit->value;
                    if (l->kind == LikeExprNode::Kind::ILIKE) {
                        std::transform(s.begin(), s.end(), s.begin(),
                            [](unsigned char c){ return std::tolower(c); });
                        std::transform(p.begin(), p.end(), p.begin(),
                            [](unsigned char c){ return std::tolower(c); });
                    }
                    bool match = SqlLikeMatch(s, p, esc);
                    return std::make_shared<LiteralExpr>(
                        LiteralType::INTEGER, match ? "1" : "0");
                }
            }
            if (op != l->operand || pat != l->pattern) {
                return std::make_shared<LikeExprNode>(l->kind, op, pat, l->escape_char, l->has_escape);
            }
            return e;
        }
        case NodeType::CASE_EXPR: {
            const auto* c = static_cast<const CaseExprNode*>(e.get());
            // 先递归折叠子表达式
            auto subj = c->subject ? FoldExpr(c->subject) : nullptr;
            std::vector<CaseWhen> new_whens;
            new_whens.reserve(c->whens.size());
            for (const auto& w : c->whens) {
                CaseWhen nw;
                nw.when_expr = w.when_expr ? FoldExpr(w.when_expr) : nullptr;
                nw.then_expr = w.then_expr ? FoldExpr(w.then_expr) : nullptr;
                new_whens.push_back(std::move(nw));
            }
            auto el = c->else_expr ? FoldExpr(c->else_expr) : nullptr;
            (void)el;
            // 尝试整体折叠
            CaseExprNode tmp;
            tmp.subject = subj;
            tmp.whens = new_whens;
            tmp.else_expr = el;
            auto overall = FoldCaseExpr(tmp);
            if (overall) return overall;
            // 子表达式有变化则重建
            bool changed = (subj != c->subject) || (el != c->else_expr);
            if (!changed) {
                for (size_t i = 0; i < c->whens.size() && !changed; ++i) {
                    if (new_whens[i].when_expr != c->whens[i].when_expr ||
                        new_whens[i].then_expr != c->whens[i].then_expr) {
                        changed = true;
                    }
                }
            }
            if (changed) {
                auto rebuilt = std::make_shared<CaseExprNode>();
                rebuilt->subject = subj;
                rebuilt->whens = std::move(new_whens);
                rebuilt->else_expr = el;
                return rebuilt;
            }
            return e;
        }
        default:
            // 其它节点（含 SUBQUERY/WINDOW_FUNC/EXTRACT/INTERVAL/NEXTVAL/UPSERT_VALUES_REF）
            // 包含运行期语义，不折叠，原样返回。
            return e;
    }
}

// ---------- 计划级 FoldNode ----------
//
// 在每个 plan 节点的"表达式字段"上调用 FoldExpr，然后递归处理 children。
// 对 FilterNode 做特殊处理：谓词若被折叠为 TRUE 字面量，则返回其子节点，
// 让父节点的 children[i] 自然指向被穿透的子节点。

PlanNodePtr FoldNode(const PlanNodePtr& node) {
    if (!node) return node;
    // 自顶向下递归处理 children
    for (auto& c : node->children) {
        c = FoldNode(c);
    }
    auto fold_assign = [](std::pair<std::string, ExprPtr>& a) {
        a.second = FoldExpr(a.second);
    };
    auto fold_list = [](std::vector<ExprPtr>& v) {
        for (auto& x : v) x = FoldExpr(x);
    };
    auto fold_item = [](OrderByItem& it) {
        it.expr = FoldExpr(it.expr);
    };
    switch (node->GetType()) {
        case PlanNodeType::SEQ_SCAN: {
            auto* n = static_cast<SeqScanNode*>(node.get());
            if (n->predicate) n->predicate = FoldExpr(n->predicate);
            return node;
        }
        case PlanNodeType::INDEX_SCAN: {
            auto* n = static_cast<IndexScanNode*>(node.get());
            if (n->residual_predicate) {
                n->residual_predicate = FoldExpr(n->residual_predicate);
            }
            return node;
        }
        case PlanNodeType::FILTER: {
            auto* n = static_cast<FilterNode*>(node.get());
            if (n->predicate) n->predicate = FoldExpr(n->predicate);
            // 谓词恒真 → 移除 Filter（以唯一子节点替代）
            if (IsConstTrueForFilter(n->predicate) && node->children.size() == 1) {
                return node->children[0];
            }
            return node;
        }
        case PlanNodeType::PROJECT: {
            auto* n = static_cast<ProjectNode*>(node.get());
            fold_list(n->columns);
            return node;
        }
        case PlanNodeType::JOIN: {
            auto* n = static_cast<JoinNode*>(node.get());
            if (n->condition) n->condition = FoldExpr(n->condition);
            return node;
        }
        case PlanNodeType::SORT: {
            auto* n = static_cast<SortNode*>(node.get());
            for (auto& it : n->order_items) fold_item(it);
            return node;
        }
        case PlanNodeType::AGGREGATE: {
            auto* n = static_cast<AggregateNode*>(node.get());
            fold_list(n->group_by_exprs);
            fold_list(n->aggregate_exprs);
            return node;
        }
        case PlanNodeType::WINDOW: {
            auto* n = static_cast<WindowNode*>(node.get());
            fold_list(n->select_list);
            for (auto& [name, spec] : n->named_windows) {
                (void)name;
                fold_list(spec.partition_by);
                for (auto& it : spec.order_by) fold_item(it);
            }
            return node;
        }
        case PlanNodeType::INSERT: {
            auto* n = static_cast<InsertNode*>(node.get());
            for (auto& row : n->values_list) {
                for (auto& v : row) v = FoldExpr(v);
            }
            // InsertNode 不含 upsert_assignments（UpsertNode 才有）。
            // Planner 会在解析 ON DUPLICATE KEY UPDATE 时直接构造 UpsertNode，
            // 所以这里不需要处理 INSERT 上的 upsert_assignments。
            fold_list(n->returning_exprs);
            return node;
        }
        case PlanNodeType::UPSERT: {
            auto* n = static_cast<UpsertNode*>(node.get());
            for (auto& row : n->values_list) {
                for (auto& v : row) v = FoldExpr(v);
            }
            for (auto& a : n->upsert_assignments) fold_assign(a);
            fold_list(n->returning_exprs);
            return node;
        }
        case PlanNodeType::UPDATE: {
            auto* n = static_cast<UpdateNode*>(node.get());
            for (auto& a : n->assignments) fold_assign(a);
            if (n->predicate) n->predicate = FoldExpr(n->predicate);
            fold_list(n->returning_exprs);
            return node;
        }
        case PlanNodeType::UPDATE_FROM: {
            auto* n = static_cast<UpdateFromNode*>(node.get());
            for (auto& a : n->assignments) fold_assign(a);
            if (n->where_clause) n->where_clause = FoldExpr(n->where_clause);
            fold_list(n->returning_exprs);
            return node;
        }
        case PlanNodeType::DELETE: {
            auto* n = static_cast<DeleteNode*>(node.get());
            if (n->predicate) n->predicate = FoldExpr(n->predicate);
            fold_list(n->returning_exprs);
            return node;
        }
        case PlanNodeType::MERGE: {
            auto* n = static_cast<MergeNode*>(node.get());
            if (n->on_condition) n->on_condition = FoldExpr(n->on_condition);
            for (auto& a : n->matched_assignments) fold_assign(a);
            fold_list(n->not_matched_values);
            return node;
        }
        case PlanNodeType::VALUES: {
            auto* n = static_cast<ValuesNode*>(node.get());
            for (auto& row : n->rows) {
                for (auto& v : row) v = FoldExpr(v);
            }
            return node;
        }
        case PlanNodeType::CALL: {
            auto* n = static_cast<CallNode*>(node.get());
            fold_list(n->arguments);
            return node;
        }
        default:
            // 其它节点类型无表达式字段；children 已经在上面递归处理过。
            return node;
    }
}

}  // namespace

// ============ Optimizer 公有/私有方法 ============

PlanNodePtr Optimizer::FoldConstants(PlanNodePtr plan) {
    return FoldNode(plan);
}

ExprPtr Optimizer::FoldConstants(ExprPtr expr) {
    return FoldExpr(expr);
}

bool Optimizer::IsConstantExpr(const ExprPtr& expr) const {
    return FoldExpr(expr) ? IsConstTrueForFilter(expr) || (expr->GetType() == NodeType::LITERAL_EXPR) : false;
}

}  // namespace sqlcompiler
