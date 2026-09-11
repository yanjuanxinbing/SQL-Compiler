#include "execution/ExecutionEngine.h"

#include "common/Error.h"
#include "execution/AggregateExecutor.h"
#include "execution/CreateTableExecutor.h"
#include "execution/CteExecutor.h"
#include "execution/DeleteExecutor.h"
#include "execution/DistinctExecutor.h"
#include "execution/CreateIndexExecutor.h"
#include "execution/IndexScanExecutor.h"
#include "execution/DropIndexExecutor.h"
#include "execution/DropTableExecutor.h"
#include "execution/FilterExecutor.h"
#include "execution/InsertExecutor.h"
#include "execution/JoinExecutor.h"
#include "execution/LimitExecutor.h"
#include "execution/ProjectExecutor.h"
#include "execution/SeqScanExecutor.h"
#include "execution/SortExecutor.h"
#include "execution/SetOpExecutor.h"
#include "execution/SubqueryExecutor.h"
#include "execution/TruncateTableExecutor.h"
#include "execution/UpdateExecutor.h"
#include "execution/WindowExecutor.h"

#include <functional>
#include <utility>

namespace sqlcompiler {

namespace {

std::string FindScanTableName(const PlanNodePtr& node) {
    if (!node) return "";
    if (node->GetType() == PlanNodeType::SEQ_SCAN) {
        return std::static_pointer_cast<SeqScanNode>(node)->table_name;
    }
    if (node->GetType() == PlanNodeType::INDEX_SCAN) {
        return std::static_pointer_cast<IndexScanNode>(node)->table_name;
    }
    for (auto& c : node->children) {
        std::string t = FindScanTableName(c);
        if (!t.empty()) return t;
    }
    return "";
}

// 收集计划中所有 (real_table, alias) 扫描节点。DFS 前序保证对于左深 JoinNode 树，
// 收集顺序与 JoinExecutor 拼接 (left || right) 后的元组列序一致：左侧子树全部在右侧子树之前。
std::vector<std::pair<std::string, std::string>> CollectScanTableNames(
    const PlanNodePtr& node) {
    std::vector<std::pair<std::string, std::string>> names;
    if (!node) return names;
    if (node->GetType() == PlanNodeType::SEQ_SCAN) {
        auto s = std::static_pointer_cast<SeqScanNode>(node);
        names.emplace_back(s->table_name, s->table_alias);
    }
    if (node->GetType() == PlanNodeType::INDEX_SCAN) {
        auto s = std::static_pointer_cast<IndexScanNode>(node);
        names.emplace_back(s->table_name, s->table_alias);
    }
    for (auto& c : node->children) {
        auto sub = CollectScanTableNames(c);
        names.insert(names.end(), sub.begin(), sub.end());
    }
    return names;
}

// 将 vector<string> 形式 (仅有表名) 适配到新签名；用作纯单表路径的兼容入口。
static std::vector<std::pair<std::string, std::string>> ToPairs(
    const std::vector<std::string>& names) {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(names.size());
    for (const auto& n : names) out.emplace_back(n, std::string{});
    return out;
}

// Build a combined column_index_map for all referenced tables.
// Joined tuples are concatenated: left_cols (in left table's order) then right_cols.
// So positions of column "x" from table i = offset(i) + table_x_index_in_table_i.
//
// 在同一 map 中登记三种键：
//   1. "<real_table>.<col>"  → 位置  限定到真实表名
//   2. "<alias>.<col>"       → 位置  限定到别名（若别名非空且与表名不同）
//   3. "<col>"               → 位置  未限定回退，first-table-wins 解决冲突
//
// 这样 `d.id` 与 `users.id` 都能正确指向各自表里的列，而未限定的 `id` 仍按
// "首个匹配表" 的语义解析（保持原行为）。
std::unordered_map<std::string, size_t> BuildCombinedColumnIndexMap(
    SystemCatalog* catalog,
    const std::vector<std::pair<std::string, std::string>>& table_info) {
    std::unordered_map<std::string, size_t> m;
    size_t offset = 0;
    for (const auto& [tname, alias] : table_info) {
        const TableInfo* info = catalog->GetTable(tname);
        if (!info) continue;
        for (size_t i = 0; i < info->columns.size(); ++i) {
            const auto& c = info->columns[i];
            // Qualified by real table name (always registered, may overwrite)
            m[tname + "." + c.name] = offset + i;
            // Qualified by alias (only if distinct and non-empty)
            if (!alias.empty() && alias != tname) {
                m[alias + "." + c.name] = offset + i;
            }
            // Unqualified: first table wins on name conflicts (legacy behavior)
            if (m.find(c.name) == m.end()) {
                m[c.name] = offset + i;
            }
        }
        offset += info->columns.size();
    }
    return m;
}

// 提取内层计划的输出列名（递归走到最近的 Project/Window/Aggregate 终止节点）。
// 用于派生表 (FROM (SELECT ...) AS alias)：SeqScanNode(table_name==alias, children[0]=sub_plan) 的
// 输出列由 sub_plan 的终端节点决定，调用方据此把"alias.col"与裸"col"都登记到 column_index_map。
//
// 如果过程中遇到另一个派生表占位 SeqScanNode，会继续递归到其子计划，确保 SELECT *
// 在多层嵌套派生表下也能拿到真实表的列。
std::vector<std::string> DeriveTerminalColumns(SystemCatalog* catalog,
                                               const PlanNodePtr& node) {
    std::vector<std::string> cols;
    if (!node) return cols;
    PlanNodePtr p = node;
    while (p && (p->GetType() == PlanNodeType::SORT ||
                 p->GetType() == PlanNodeType::LIMIT ||
                 p->GetType() == PlanNodeType::SET_OP ||
                 p->GetType() == PlanNodeType::CTE_DEFINE ||
                 p->GetType() == PlanNodeType::CTE_BIND)) {
        if (p->children.empty()) return cols;
        p = p->children[0];
    }
    // 派生表占位 SEQ_SCAN（table_name==alias 且 children 非空）→ 递归到内层计划
    if (p && p->GetType() == PlanNodeType::SEQ_SCAN) {
        auto sn = std::static_pointer_cast<SeqScanNode>(p);
        if (!sn->table_alias.empty() && sn->table_alias == sn->table_name &&
            !p->children.empty()) {
            return DeriveTerminalColumns(catalog, p->children[0]);
        }
    }
    if (!p) return cols;
    if (p->GetType() == PlanNodeType::WINDOW) {
        auto wn = std::static_pointer_cast<WindowNode>(p);
        cols.reserve(wn->select_list.size());
        for (size_t i = 0; i < wn->select_list.size(); ++i) {
            if (i < wn->aliases.size() && !wn->aliases[i].empty()) {
                cols.push_back(wn->aliases[i]); continue;
            }
            const auto& e = wn->select_list[i];
            if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
                cols.push_back(std::static_pointer_cast<ColumnRefExpr>(e)->column_name);
            } else if (e && e->GetType() == NodeType::FUNCTION_CALL_EXPR) {
                cols.push_back(std::static_pointer_cast<FunctionCallExpr>(e)->function_name);
            } else if (e && e->GetType() == NodeType::WINDOW_FUNC_EXPR) {
                cols.push_back(std::static_pointer_cast<WindowFuncNode>(e)->function_name);
            } else if (e) {
                cols.push_back(e->ToString());
            } else {
                cols.push_back("?");
            }
        }
        return cols;
    }
    if (p->GetType() == PlanNodeType::AGGREGATE) {
        auto agg = std::static_pointer_cast<AggregateNode>(p);
        cols.reserve(agg->aggregate_exprs.size());
        for (size_t i = 0; i < agg->aggregate_exprs.size(); ++i) {
            if (i < agg->aliases.size() && !agg->aliases[i].empty()) {
                cols.push_back(agg->aliases[i]); continue;
            }
            const auto& e = agg->aggregate_exprs[i];
            if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
                cols.push_back(std::static_pointer_cast<ColumnRefExpr>(e)->column_name);
            } else if (e && e->GetType() == NodeType::FUNCTION_CALL_EXPR) {
                cols.push_back(std::static_pointer_cast<FunctionCallExpr>(e)->function_name);
            } else if (e) {
                cols.push_back(e->ToString());
            } else {
                cols.push_back("?");
            }
        }
        return cols;
    }
    if (p->GetType() == PlanNodeType::PROJECT) {
        auto proj = std::static_pointer_cast<ProjectNode>(p);
        cols.reserve(proj->columns.size());
        for (size_t i = 0; i < proj->columns.size(); ++i) {
            if (i < proj->aliases.size() && !proj->aliases[i].empty()) {
                cols.push_back(proj->aliases[i]); continue;
            }
            const auto& e = proj->columns[i];
            if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
                auto cr = std::static_pointer_cast<ColumnRefExpr>(e);
                cols.push_back(cr->column_name);
            } else if (e && e->GetType() == NodeType::FUNCTION_CALL_EXPR) {
                auto fc = std::static_pointer_cast<FunctionCallExpr>(e);
                if (fc->function_name == "*" || fc->function_name == "STAR") {
                    // 递归查找扫描表的列
                    std::string tname = FindScanTableName(p);
                    const TableInfo* info = catalog->GetTable(tname);
                    if (info) {
                        for (const auto& c : info->columns) cols.push_back(c.name);
                        continue;
                    }
                    // 派生表占位（table_name == alias 且 children[0] 非空）：
                    // 找到占位节点后递归到其内层计划的输出列。
                    PlanNodePtr placeholder_child = nullptr;
                    std::function<void(const PlanNodePtr&)> find_placeholder =
                        [&](const PlanNodePtr& n) {
                        if (!n || placeholder_child) return;
                        if (n->GetType() == PlanNodeType::SEQ_SCAN) {
                            auto s = std::static_pointer_cast<SeqScanNode>(n);
                            if (!s->table_alias.empty() && s->table_alias == s->table_name &&
                                !n->children.empty()) {
                                placeholder_child = n->children[0];
                                return;
                            }
                        }
                        for (auto& ch : n->children) {
                            find_placeholder(ch);
                            if (placeholder_child) return;
                        }
                    };
                    find_placeholder(p);
                    if (placeholder_child) {
                        auto inner_cols = DeriveTerminalColumns(catalog, placeholder_child);
                        for (const auto& cn : inner_cols) cols.push_back(cn);
                    }
                    continue;
                }
                cols.push_back(fc->function_name);
            } else if (e) {
                cols.push_back(e->ToString());
            } else {
                cols.push_back("?");
            }
        }
        return cols;
    }
    return cols;
}

// 把派生表 SeqScanNode（table_name==alias 且 children[0] 非空）按其内层计划的输出列
// 注册成 column_index_map 的额外项。这样外层 WHERE / SELECT 引用 `rk` 或 `alias.rk`
// 都能落到正确的下标。
std::unordered_map<std::string, size_t> BuildCombinedColumnIndexMapWithDerived(
    SystemCatalog* catalog,
    const PlanNodePtr& plan_node,
    const std::vector<std::pair<std::string, std::string>>& table_info) {
    auto m = BuildCombinedColumnIndexMap(catalog, table_info);
    if (!plan_node) return m;
    std::function<void(const PlanNodePtr&, size_t&)> walk =
        [&](const PlanNodePtr& n, size_t& offset) {
        if (!n) return;
        if (n->GetType() == PlanNodeType::SEQ_SCAN) {
            auto s = std::static_pointer_cast<SeqScanNode>(n);
            // 派生表：table_name == alias 且 children[0] 非空
            if (!s->table_alias.empty() && s->table_alias == s->table_name &&
                !n->children.empty()) {
                auto cols = DeriveTerminalColumns(catalog, n->children[0]);
                for (size_t i = 0; i < cols.size(); ++i) {
                    const std::string& cname = cols[i];
                    m[s->table_alias + "." + cname] = offset + i;
                    if (m.find(cname) == m.end()) {
                        m[cname] = offset + i;
                    }
                }
                offset += cols.size();
                return;
            }
            // 普通表：catalog 已有对应列，下标已在 m 中；只需推进 offset
            const TableInfo* info = catalog->GetTable(s->table_name);
            if (info) offset += info->columns.size();
            return;
        }
        if (n->GetType() == PlanNodeType::INDEX_SCAN) {
            auto s = std::static_pointer_cast<IndexScanNode>(n);
            const TableInfo* info = catalog->GetTable(s->table_name);
            if (info) offset += info->columns.size();
            return;
        }
        for (auto& ch : n->children) walk(ch, offset);
    };
    size_t offset = 0;
    walk(plan_node, offset);
    return m;
}

}  // namespace

ExecutionEngine::ExecutionEngine(SystemCatalog* catalog) : catalog_(catalog) {
}

ExecutionResult ExecutionEngine::Execute(const PlanNodePtr& plan) {
    ExecutionContext ctx(catalog_);
    return ExecuteSubplan(plan, &ctx);
}

ExecutionResult ExecutionEngine::ExecuteSubplan(const PlanNodePtr& plan, ExecutionContext* ctx) {
    ExecutionResult result;
    if (!plan || !ctx) return result;
    try {
        auto root = BuildExecutor(plan, ctx);
        if (!root) {
            result.success = false;
            result.message = "failed to build executor";
            return result;
        }
        root->Init();

        // Determine if root is a query (any read operator) or DML/DDL
        bool is_query = (plan->GetType() == PlanNodeType::PROJECT ||
                         plan->GetType() == PlanNodeType::SEQ_SCAN ||
                         plan->GetType() == PlanNodeType::INDEX_SCAN ||
                         plan->GetType() == PlanNodeType::FILTER ||
                         plan->GetType() == PlanNodeType::JOIN ||
                         plan->GetType() == PlanNodeType::SORT ||
                         plan->GetType() == PlanNodeType::LIMIT ||
                         plan->GetType() == PlanNodeType::AGGREGATE ||
                         plan->GetType() == PlanNodeType::SUBQUERY ||
                         plan->GetType() == PlanNodeType::CTE_BIND ||
                         plan->GetType() == PlanNodeType::CTE_DEFINE ||
                         plan->GetType() == PlanNodeType::SET_OP ||
                         plan->GetType() == PlanNodeType::WINDOW);
        if (is_query) {
            result.column_names = DeriveOutputColumnNames(plan);
        }
        while (true) {
            Tuple t;
            if (!root->Next(&t)) break;
            if (is_query) {
                result.rows.push_back(std::move(t));
            }
        }
        if (!is_query) {
            result.message = "OK";
        }
    } catch (const CompilerException& e) {
        result.success = false;
        result.message = FormatError(e);
    } catch (const std::exception& e) {
        result.success = false;
        result.message = std::string("error: ") + e.what();
    }
    return result;
}

ExecutorPtr ExecutionEngine::BuildExecutor(const PlanNodePtr& plan_node,
                                           ExecutionContext* context) {
    if (!plan_node) return nullptr;
    switch (plan_node->GetType()) {
        case PlanNodeType::SEQ_SCAN: {
            auto n = std::static_pointer_cast<SeqScanNode>(plan_node);
            // CTE hint: table_alias 以 "__cte__" 开头时优先路由到 CTE_BIND
            if (!n->table_alias.empty() && n->table_alias.rfind("__cte__", 0) == 0) {
                std::string cte_name = n->table_alias.substr(7);
                return std::make_unique<CteBindExecutor>(context, std::move(cte_name));
            }
            // 派生表占位（FROM (SELECT ...) AS alias）：table_name == alias 且
            // children[0] 挂有 Planner 递归生成的子计划。让真正的子计划替代占位 SeqScan。
            if (!n->table_alias.empty() && n->table_alias == n->table_name &&
                !plan_node->children.empty()) {
                return BuildExecutor(plan_node->children[0], context);
            }
            // 递归 CTE 在递归部分里以 `JOIN cte_name alias` 形式引用 CTE。
            // Planner 没有改写这种带别名的 SeqScanNode，于是这里补一次检查：
            // 若 table_name 已在 ExecutionContext 中物化（递归 CTE 迭代时由
            // CteDefineExecutor 注册），改走 CteBindExecutor。否则退回普通的
            // SeqScanExecutor，由 catalog 查找 table_heap。
            if (!n->table_name.empty() && context->HasCte(n->table_name)) {
                return std::make_unique<CteBindExecutor>(context, n->table_name);
            }
            return std::make_unique<SeqScanExecutor>(context, n->table_name);
        }
        case PlanNodeType::INDEX_SCAN: {
            auto n = std::static_pointer_cast<IndexScanNode>(plan_node);
            auto col_map = BuildCombinedColumnIndexMap(
                context->GetCatalog(), std::vector<std::pair<std::string, std::string>>{{n->table_name, n->table_alias}});
            return std::make_unique<IndexScanExecutor>(context, n, col_map);
        }
        case PlanNodeType::FILTER: {
            auto n = std::static_pointer_cast<FilterNode>(plan_node);
            auto child = BuildExecutor(plan_node->children.empty() ? nullptr : plan_node->children[0], context);
            if (!child) return nullptr;
            // For HAVING (child is Aggregate): aggregate_exprs become named slots
            // in the output tuple. We map aggregate function calls to positions.
            std::unordered_map<std::string, size_t> cmap;
            if (!plan_node->children.empty() &&
                plan_node->children[0]->GetType() == PlanNodeType::AGGREGATE) {
                auto agg = std::static_pointer_cast<AggregateNode>(plan_node->children[0]);
                for (size_t i = 0; i < agg->aggregate_exprs.size(); ++i) {
                    const auto& e = agg->aggregate_exprs[i];
                    if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
                        cmap[std::static_pointer_cast<ColumnRefExpr>(e)->column_name] = i;
                    } else if (e && e->GetType() == NodeType::FUNCTION_CALL_EXPR) {
                        cmap[std::static_pointer_cast<FunctionCallExpr>(e)->function_name] = i;
                    }
                }
            } else {
                cmap = BuildCombinedColumnIndexMapWithDerived(catalog_, plan_node, CollectScanTableNames(plan_node));
            }
            // HAVING / 上层 Filter 引用 SELECT 别名时，把别名映射到对应位置。
            if (!plan_node->children.empty() &&
                plan_node->children[0]->GetType() == PlanNodeType::AGGREGATE) {
                auto agg = std::static_pointer_cast<AggregateNode>(plan_node->children[0]);
                for (size_t i = 0; i < agg->aggregate_exprs.size(); ++i) {
                    if (i < agg->aliases.size() && !agg->aliases[i].empty()) {
                        cmap[agg->aliases[i]] = i;
                    }
                }
            } else if (!plan_node->children.empty() &&
                       plan_node->children[0]->GetType() == PlanNodeType::PROJECT) {
                // Filter 在 Project 之后：可直接用 Project 的 aliases 作为列下标。
                auto proj = std::static_pointer_cast<ProjectNode>(plan_node->children[0]);
                for (size_t i = 0; i < proj->columns.size(); ++i) {
                    if (i < proj->aliases.size() && !proj->aliases[i].empty()) {
                        cmap[proj->aliases[i]] = i;
                    }
                }
            }
            return std::make_unique<FilterExecutor>(context, std::move(child), n->predicate, cmap);
        }
        case PlanNodeType::PROJECT: {
            auto n = std::static_pointer_cast<ProjectNode>(plan_node);
            // 无 FROM（如 SELECT 1 / SELECT 'label'）：Project 直接发射常量行
            if (plan_node->children.empty()) {
                std::unordered_map<std::string, size_t> empty_cmap;
                ExecutorPtr proj = std::make_unique<ProjectExecutor>(
                    context, nullptr, n->columns, empty_cmap);
                if (n->is_distinct) {
                    return std::make_unique<DistinctExecutor>(context, std::move(proj),
                                                              n->columns.size());
                }
                return proj;
            }
            auto child = BuildExecutor(plan_node->children[0], context);
            if (!child) return nullptr;
            // If child is an Aggregate, the aggregate already produced tuples
            // matching the SELECT list; pass through unchanged.
            if (plan_node->children[0]->GetType() == PlanNodeType::AGGREGATE) {
                if (n->is_distinct) {
                    // AggregateExecutor 的输出列数 == aggregate_exprs 数，
                    // 没有 underlying 追加；DISTINCT 仍按全部列参与去重。
                    return std::make_unique<DistinctExecutor>(context, std::move(child),
                                                              n->columns.size());
                }
                return child;
            }
            // If child is a Filter (HAVING) whose own child is Aggregate, pass
            // through unchanged.
            if (plan_node->children[0]->GetType() == PlanNodeType::FILTER &&
                plan_node->children[0]->children.size() > 0 &&
                plan_node->children[0]->children[0]->GetType() == PlanNodeType::AGGREGATE) {
                if (n->is_distinct) {
                    return std::make_unique<DistinctExecutor>(context, std::move(child),
                                                              n->columns.size());
                }
                return child;
            }
            auto cmap = BuildCombinedColumnIndexMapWithDerived(catalog_, plan_node, CollectScanTableNames(plan_node));
            ExecutorPtr proj_exec = std::make_unique<ProjectExecutor>(context, std::move(child), n->columns, cmap, n->aliases);
            if (n->is_distinct) {
                // ProjectExecutor 输出的元组形状是 [select_values ++ underlying_tuple]，
                // Distinct 必须仅按前缀 n->columns.size() 列哈希，否则 underlying
                // 列（id/name 等）会让同一 dept 的两行永远不被识别为重复。
                return std::make_unique<DistinctExecutor>(context, std::move(proj_exec),
                                                          n->columns.size());
            }
            return proj_exec;
        }
        case PlanNodeType::LIMIT: {
            auto n = std::static_pointer_cast<LimitNode>(plan_node);
            auto child = BuildExecutor(plan_node->children.empty() ? nullptr : plan_node->children[0], context);
            if (!child) return nullptr;
            return std::make_unique<LimitExecutor>(context, std::move(child), n->limit_count, n->offset);
        }
        case PlanNodeType::SORT: {
            auto n = std::static_pointer_cast<SortNode>(plan_node);
            auto child = BuildExecutor(plan_node->children.empty() ? nullptr : plan_node->children[0], context);
            if (!child) return nullptr;
            // Build a column_index_map for evaluating ORDER BY expressions against
            // the tuple stream that reaches Sort.
            //
            // Project's output tuple layout is [select_values ++ child_underlying_tuple].
            // For Project-over-Aggregate, ProjectExecutor pass-throughs and only the
            // aggregate output is emitted (no underlying appended). The cmap must
            // reflect the actual layout that reaches Sort.
            std::unordered_map<std::string, size_t> cmap;
            PlanNodePtr proj_node = nullptr;
            if (!plan_node->children.empty() &&
                plan_node->children[0]->GetType() == PlanNodeType::PROJECT) {
                proj_node = plan_node->children[0];
            }

            // Helper: populate cmap from AggregateNode's output positions
            // (positions 0..N-1 correspond to aggregate_exprs, with aliases too).
            auto build_aggregate_cmap =
                [](const std::shared_ptr<AggregateNode>& agg,
                   std::unordered_map<std::string, size_t>& out) {
                for (size_t i = 0; i < agg->aggregate_exprs.size(); ++i) {
                    const auto& e = agg->aggregate_exprs[i];
                    if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
                        auto cr = std::static_pointer_cast<ColumnRefExpr>(e);
                        if (!cr->table_name.empty()) {
                            out[cr->table_name + "." + cr->column_name] = i;
                        }
                        out[cr->column_name] = i;
                    } else if (e && e->GetType() == NodeType::FUNCTION_CALL_EXPR) {
                        auto fc = std::static_pointer_cast<FunctionCallExpr>(e);
                        out[fc->function_name] = i;
                    }
                    if (i < agg->aliases.size() && !agg->aliases[i].empty()) {
                        out[agg->aliases[i]] = i;
                    }
                }
            };

            // Detect pass-through-over-Aggregate cases. Project's child is Aggregate
            // (or Filter wrapping Aggregate) → output tuple == Aggregate output.
            std::shared_ptr<AggregateNode> agg_for_cmap;
            if (proj_node) {
                auto proj = std::static_pointer_cast<ProjectNode>(proj_node);
                auto proj_child = proj->children.empty() ? nullptr : proj->children[0];
                if (proj_child && proj_child->GetType() == PlanNodeType::AGGREGATE) {
                    agg_for_cmap = std::static_pointer_cast<AggregateNode>(proj_child);
                } else if (proj_child && proj_child->GetType() == PlanNodeType::FILTER &&
                           !proj_child->children.empty() &&
                           proj_child->children[0]->GetType() == PlanNodeType::AGGREGATE) {
                    agg_for_cmap = std::static_pointer_cast<AggregateNode>(proj_child->children[0]);
                }
            }
            if (!agg_for_cmap && !plan_node->children.empty() &&
                plan_node->children[0]->GetType() == PlanNodeType::AGGREGATE) {
                agg_for_cmap = std::static_pointer_cast<AggregateNode>(plan_node->children[0]);
            }
            if (!agg_for_cmap && !plan_node->children.empty() &&
                plan_node->children[0]->GetType() == PlanNodeType::FILTER &&
                !plan_node->children[0]->children.empty() &&
                plan_node->children[0]->children[0]->GetType() == PlanNodeType::AGGREGATE) {
                agg_for_cmap = std::static_pointer_cast<AggregateNode>(plan_node->children[0]->children[0]);
            }

            if (agg_for_cmap) {
                build_aggregate_cmap(agg_for_cmap, cmap);
            } else if (proj_node) {
                auto proj = std::static_pointer_cast<ProjectNode>(proj_node);
                bool is_star = (proj->columns.size() == 1 &&
                                proj->columns[0] &&
                                proj->columns[0]->GetType() == NodeType::FUNCTION_CALL_EXPR &&
                                (std::static_pointer_cast<FunctionCallExpr>(proj->columns[0])->function_name == "*" ||
                                 std::static_pointer_cast<FunctionCallExpr>(proj->columns[0])->function_name == "STAR"));
                // Seed from scans (gets first-table-wins and the qualified forms).
                cmap = BuildCombinedColumnIndexMapWithDerived(catalog_, plan_node, CollectScanTableNames(plan_node));
                if (is_star) {
                    // SELECT * — Project passes through the scan tuple unchanged.
                    // cmap from scans is already correct.
                } else {
                    // Non-star Project over Scan/Join/etc. The output tuple layout is
                    // [select_values ++ underlying_tuple]. All underlying columns
                    // shift by proj->columns.size(), so we must remap every scan-based
                    // entry to the offset position.
                    size_t offset = proj->columns.size();
                    auto scans = CollectScanTableNames(plan_node);
                    size_t running = offset;
                    for (const auto& ti : scans) {
                        const std::string& tname = ti.first;
                        const std::string& alias = ti.second;
                        const TableInfo* info = catalog_->GetTable(tname);
                        if (!info) continue;
                        for (size_t i = 0; i < info->columns.size(); ++i) {
                            const auto& c = info->columns[i];
                            cmap[tname + "." + c.name] = running + i;
                            if (!alias.empty() && alias != tname) {
                                cmap[alias + "." + c.name] = running + i;
                            }
                            cmap[c.name] = running + i;
                        }
                        running += info->columns.size();
                    }
                }
                // Project list refs / aliases → positions 0..N-1.
                for (size_t i = 0; i < proj->columns.size(); ++i) {
                    const auto& e = proj->columns[i];
                    if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
                        auto cr = std::static_pointer_cast<ColumnRefExpr>(e);
                        cmap[cr->column_name] = i;
                        if (!cr->table_name.empty()) {
                            cmap[cr->table_name + "." + cr->column_name] = i;
                        }
                    }
                    if (i < proj->aliases.size() && !proj->aliases[i].empty()) {
                        cmap[proj->aliases[i]] = i;
                    }
                }
            } else if (!plan_node->children.empty() &&
                       plan_node->children[0]->GetType() == PlanNodeType::WINDOW) {
                // WindowExecutor outputs only the SELECT list values; no underlying.
                auto wn = std::static_pointer_cast<WindowNode>(plan_node->children[0]);
                for (size_t i = 0; i < wn->select_list.size(); ++i) {
                    const auto& e = wn->select_list[i];
                    if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
                        cmap[std::static_pointer_cast<ColumnRefExpr>(e)->column_name] = i;
                    } else if (e && e->GetType() == NodeType::FUNCTION_CALL_EXPR) {
                        cmap[std::static_pointer_cast<FunctionCallExpr>(e)->function_name] = i;
                    }
                    if (i < wn->aliases.size() && !wn->aliases[i].empty()) {
                        cmap[wn->aliases[i]] = i;
                    }
                }
            } else {
                cmap = BuildCombinedColumnIndexMapWithDerived(catalog_, plan_node, CollectScanTableNames(plan_node));
            }
            return std::make_unique<SortExecutor>(context, std::move(child), n->order_items, cmap);
        }
        case PlanNodeType::AGGREGATE: {
            auto n = std::static_pointer_cast<AggregateNode>(plan_node);
            auto child = BuildExecutor(plan_node->children.empty() ? nullptr : plan_node->children[0], context);
            if (!child) return nullptr;
            auto cmap = BuildCombinedColumnIndexMapWithDerived(catalog_, plan_node, CollectScanTableNames(plan_node));
            return std::make_unique<AggregateExecutor>(context, std::move(child),
                                                        n->group_by_exprs, n->aggregate_exprs,
                                                        cmap);
        }
        case PlanNodeType::JOIN: {
            auto n = std::static_pointer_cast<JoinNode>(plan_node);
            if (plan_node->children.size() < 2) return nullptr;
            auto left = BuildExecutor(plan_node->children[0], context);
            auto right = BuildExecutor(plan_node->children[1], context);
            if (!left || !right) return nullptr;
            auto cmap = BuildCombinedColumnIndexMapWithDerived(catalog_, plan_node, CollectScanTableNames(plan_node));
            return std::make_unique<JoinExecutor>(context, std::move(left), std::move(right),
                                                  n->join_type, n->condition, cmap);
        }
        case PlanNodeType::INSERT: {
            auto n = std::static_pointer_cast<InsertNode>(plan_node);
            return std::make_unique<InsertExecutor>(context, n->table_name, n->columns,
                                                     n->values_list);
        }
        case PlanNodeType::UPDATE: {
            auto n = std::static_pointer_cast<UpdateNode>(plan_node);
            return std::make_unique<UpdateExecutor>(context, n->table_name, n->assignments,
                                                     n->predicate, BuildColumnIndexMap(n->table_name));
        }
        case PlanNodeType::DELETE: {
            auto n = std::static_pointer_cast<DeleteNode>(plan_node);
            return std::make_unique<DeleteExecutor>(context, n->table_name, n->predicate,
                                                     BuildColumnIndexMap(n->table_name));
        }
        case PlanNodeType::CREATE_TABLE: {
            auto n = std::static_pointer_cast<CreateTableNode>(plan_node);
            return std::make_unique<CreateTableExecutor>(context, n->table_name,
                                                          n->columns, n->primary_keys,
                                                          n->if_not_exists);
        }
        case PlanNodeType::DROP_TABLE: {
            auto n = std::static_pointer_cast<DropTableNode>(plan_node);
            return std::make_unique<DropTableExecutor>(context, n->table_name,
                                                       n->if_exists);
        }
        case PlanNodeType::CREATE_INDEX: {
            auto n = std::static_pointer_cast<CreateIndexNode>(plan_node);
            return std::make_unique<CreateIndexExecutor>(
                context, n->index_name, n->table_name, n->key_columns, n->is_unique);
        }
        case PlanNodeType::DROP_INDEX: {
            auto n = std::static_pointer_cast<DropIndexNode>(plan_node);
            return std::make_unique<DropIndexExecutor>(context, n->index_name,
                                                       n->if_exists);
        }
        case PlanNodeType::TRUNCATE_TABLE: {
            auto n = std::static_pointer_cast<TruncateTableNode>(plan_node);
            return std::make_unique<TruncateTableExecutor>(context, n->table_name);
        }
        case PlanNodeType::SET_OP: {
            auto n = std::static_pointer_cast<SetOpNode>(plan_node);
            if (plan_node->children.size() < 2) return nullptr;
            auto left = BuildExecutor(plan_node->children[0], context);
            auto right = BuildExecutor(plan_node->children[1], context);
            if (!left || !right) return nullptr;
            std::string kind_str;
            switch (n->kind) {
                case SetOpNode::Kind::UNION:     kind_str = "UNION"; break;
                case SetOpNode::Kind::UNION_ALL: kind_str = "UNION ALL"; break;
                case SetOpNode::Kind::INTERSECT: kind_str = "INTERSECT"; break;
                case SetOpNode::Kind::EXCEPT:    kind_str = "EXCEPT"; break;
            }
            return std::make_unique<SetOpExecutor>(context, std::move(left),
                                                    std::move(right), kind_str);
        }
        case PlanNodeType::SUBQUERY: {
            auto n = std::static_pointer_cast<SubqueryNode>(plan_node);
            return std::make_unique<SubqueryExecutor>(context, n.get());
        }
        case PlanNodeType::CTE_BIND: {
            auto n = std::static_pointer_cast<CteBindNode>(plan_node);
            return std::make_unique<CteBindExecutor>(context, n->cte_name);
        }
        case PlanNodeType::CTE_DEFINE: {
            auto n = std::static_pointer_cast<CteDefineNode>(plan_node);
            return std::make_unique<CteDefineExecutor>(context, n.get());
        }
        case PlanNodeType::WINDOW: {
            auto n = std::static_pointer_cast<WindowNode>(plan_node);
            // 派生表（derived table）的 SeqScanNode 占位：实际跑子计划
            // Planner 把子计划挂到了 SeqScanNode.children[0] 上。这里识别
            // table_name == table_alias（即表名 == 派生别名）的 SeqScan
            // 改走子计划路径，并把列映射到子计划输出列上。
            if (!plan_node->children.empty()) {
                auto child = BuildExecutor(plan_node->children[0], context);
                if (!child) return nullptr;
                // 列引用通过 alias.col 形式；column_index_map 从扫描表中派生。
                // 若 child 是 derived-table 占位 SeqScan，其 table_name 在 catalog 中
                // 查不到，会被 CollectScanTableNames 跳过，此时 cmap 仅由后续可解析表填充。
                std::unordered_map<std::string, size_t> cmap;
                if (plan_node->children[0]->GetType() == PlanNodeType::AGGREGATE) {
                    // 子计划是聚合时，物化的 tuple 形状由 AggregateNode 的输出决定；
                    // 需要按 AggregateNode 的输出位置而非扫描表位置构建 cmap，
                    // 同时把每个 WindowFuncNode 内层聚合涉及的列映射到该窗口表达式
                    // 在 select_list 中的位置（AggregateExecutor 在 EvalAggregateExpr
                    // 中已把内层聚合的状态值塞到了对应下标）。
                    auto agg = std::static_pointer_cast<AggregateNode>(plan_node->children[0]);
                    auto register_window_inner_arg =
                        [&](ExprPtr inner, size_t pos) {
                        // 递归剥到最里层 COLUMN_REF_EXPR（处理 MAX(MAX(salary)) 等嵌套）。
                        while (inner && inner->GetType() == NodeType::FUNCTION_CALL_EXPR) {
                            auto ifc = std::static_pointer_cast<FunctionCallExpr>(inner);
                            if (ifc->arguments.empty()) break;
                            inner = ifc->arguments[0];
                        }
                        if (inner && inner->GetType() == NodeType::COLUMN_REF_EXPR) {
                            auto cr = std::static_pointer_cast<ColumnRefExpr>(inner);
                            cmap[cr->column_name] = pos;
                            if (!cr->table_name.empty()) {
                                cmap[cr->table_name + "." + cr->column_name] = pos;
                            }
                        }
                    };
                    for (size_t i = 0; i < agg->aggregate_exprs.size(); ++i) {
                        const auto& e = agg->aggregate_exprs[i];
                        if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
                            auto cr = std::static_pointer_cast<ColumnRefExpr>(e);
                            cmap[cr->column_name] = i;
                            if (!cr->table_name.empty()) {
                                cmap[cr->table_name + "." + cr->column_name] = i;
                            }
                        } else if (e && e->GetType() == NodeType::FUNCTION_CALL_EXPR) {
                            auto fc = std::static_pointer_cast<FunctionCallExpr>(e);
                            cmap[fc->function_name] = i;
                        } else if (e && e->GetType() == NodeType::WINDOW_FUNC_EXPR) {
                            auto wf = std::static_pointer_cast<WindowFuncNode>(e);
                            cmap[wf->function_name] = i;
                            // 聚合型窗口（MAX/MIN/SUM/AVG/COUNT(... OVER)）的外层函数名
                            // 也映射到当前下标，方便后续 ComputeWindowValue 找到对应位置。
                            register_window_inner_arg(
                                wf->arguments.empty() ? nullptr : wf->arguments[0], i);
                        }
                        if (i < agg->aliases.size() && !agg->aliases[i].empty()) {
                            cmap[agg->aliases[i]] = i;
                        }
                    }
                } else {
                    cmap = BuildCombinedColumnIndexMapWithDerived(
                        catalog_, plan_node, CollectScanTableNames(plan_node));
                }
                return std::make_unique<WindowExecutor>(context, std::move(child),
                                                        n->select_list, n->aliases,
                                                        cmap, n->named_windows);
            }
            return nullptr;
        }
        default:
            throw CompilerException(ErrorStage::CODEGEN,
                "feature not implemented: unsupported plan node");
    }
}

std::unordered_map<std::string, size_t> ExecutionEngine::BuildColumnIndexMap(
    const std::string& table_name) {
    std::unordered_map<std::string, size_t> m;
    if (table_name.empty()) return m;
    const TableInfo* info = catalog_->GetTable(table_name);
    if (!info) return m;
    for (size_t i = 0; i < info->columns.size(); ++i) {
        m[info->columns[i].name] = i;
    }
    return m;
}

std::vector<std::string> ExecutionEngine::DeriveOutputColumnNames(const PlanNodePtr& plan_node) {
    std::vector<std::string> names;
    if (!plan_node) return names;
    // Walk through Sort/Limit/SET_OP wrappers to find the underlying Project (or Aggregate)
    PlanNodePtr p = plan_node;
    while (p && (p->GetType() == PlanNodeType::SORT ||
                 p->GetType() == PlanNodeType::LIMIT ||
                 p->GetType() == PlanNodeType::SET_OP ||
                 p->GetType() == PlanNodeType::CTE_DEFINE ||
                 p->GetType() == PlanNodeType::CTE_BIND)) {
        if (p->children.empty()) return names;
        // SQL 语义：UNION/INTERSECT/EXCEPT 的列名由左侧 SELECT 决定。
        p = p->children[0];
    }
    if (!p) return names;
    // WindowNode holds the SELECT list directly; produce column names from aliases / column refs.
    if (p->GetType() == PlanNodeType::WINDOW) {
        auto wn = std::static_pointer_cast<WindowNode>(p);
        for (size_t i = 0; i < wn->select_list.size(); ++i) {
            // Alias wins
            if (i < wn->aliases.size() && !wn->aliases[i].empty()) {
                names.push_back(wn->aliases[i]);
                continue;
            }
            const auto& e = wn->select_list[i];
            if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
                auto cr = std::static_pointer_cast<ColumnRefExpr>(e);
                names.push_back(cr->column_name);
            } else if (e && e->GetType() == NodeType::FUNCTION_CALL_EXPR) {
                auto fc = std::static_pointer_cast<FunctionCallExpr>(e);
                if (fc->function_name == "*" || fc->function_name == "STAR") {
                    std::string tname = FindScanTableName(p);
                    const TableInfo* info = catalog_->GetTable(tname);
                    if (info) {
                        for (const auto& c : info->columns) names.push_back(c.name);
                    }
                    continue;
                }
                names.push_back(fc->function_name);
            } else if (e && e->GetType() == NodeType::WINDOW_FUNC_EXPR) {
                auto wf = std::static_pointer_cast<WindowFuncNode>(e);
                names.push_back(wf->function_name);
            } else if (e) {
                names.push_back(e->ToString());
            } else {
                names.push_back("?");
            }
        }
        return names;
    }
    // AggregateNode directly holds the SELECT list as its aggregate_exprs
    if (p->GetType() == PlanNodeType::AGGREGATE) {
        auto agg = std::static_pointer_cast<AggregateNode>(p);
        for (size_t i = 0; i < agg->aggregate_exprs.size(); ++i) {
            // 别名优先
            if (i < agg->aliases.size() && !agg->aliases[i].empty()) {
                names.push_back(agg->aliases[i]);
                continue;
            }
            const auto& e = agg->aggregate_exprs[i];
            if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
                names.push_back(std::static_pointer_cast<ColumnRefExpr>(e)->column_name);
            } else if (e && e->GetType() == NodeType::FUNCTION_CALL_EXPR) {
                auto fc = std::static_pointer_cast<FunctionCallExpr>(e);
                names.push_back(fc->function_name);
            } else if (e) {
                names.push_back(e->ToString());
            } else {
                names.push_back("?");
            }
        }
        return names;
    }
    if (p->GetType() != PlanNodeType::PROJECT) return names;
    auto proj = std::static_pointer_cast<ProjectNode>(p);
    for (size_t i = 0; i < proj->columns.size(); ++i) {
        const auto& e = proj->columns[i];
        // 别名优先
        if (i < proj->aliases.size() && !proj->aliases[i].empty()) {
            names.push_back(proj->aliases[i]);
            continue;
        }
        if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
            auto cr = std::static_pointer_cast<ColumnRefExpr>(e);
            names.push_back(cr->column_name);
        } else if (e && e->GetType() == NodeType::FUNCTION_CALL_EXPR) {
            auto fc = std::static_pointer_cast<FunctionCallExpr>(e);
            if (fc->function_name == "*" || fc->function_name == "STAR") {
                // SELECT * : 从底层扫描表（或派生表内层计划）推导列名
                std::string tname = FindScanTableName(proj);
                const TableInfo* info = catalog_->GetTable(tname);
                if (info) {
                    for (const auto& c : info->columns) names.push_back(c.name);
                    continue;
                }
                // 派生表占位：把内层计划的输出列填入
                auto derived_cols = DeriveTerminalColumns(catalog_, proj);
                if (!derived_cols.empty()) {
                    for (const auto& cn : derived_cols) names.push_back(cn);
                    continue;
                }
                continue;
            }
            names.push_back(fc->function_name);
        } else if (e) {
            names.push_back(e->ToString());
        } else {
            names.push_back("?");
        }
    }
    return names;
}

}  // namespace sqlcompiler