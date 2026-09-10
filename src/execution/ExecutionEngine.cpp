#include "execution/ExecutionEngine.h"

#include "common/Error.h"
#include "execution/AggregateExecutor.h"
#include "execution/CreateTableExecutor.h"
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
#include "execution/TruncateTableExecutor.h"
#include "execution/UpdateExecutor.h"

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

// Collect all tables referenced in the plan (for JOIN's combined column map)
std::vector<std::string> CollectScanTableNames(const PlanNodePtr& node) {
    std::vector<std::string> names;
    if (!node) return names;
    if (node->GetType() == PlanNodeType::SEQ_SCAN) {
        names.push_back(std::static_pointer_cast<SeqScanNode>(node)->table_name);
    }
    if (node->GetType() == PlanNodeType::INDEX_SCAN) {
        names.push_back(std::static_pointer_cast<IndexScanNode>(node)->table_name);
    }
    for (auto& c : node->children) {
        auto sub = CollectScanTableNames(c);
        names.insert(names.end(), sub.begin(), sub.end());
    }
    return names;
}

// Build a combined column_index_map for all referenced tables.
// Joined tuples are concatenated: left_cols (in left table's order) then right_cols.
// So positions of column "x" from table i = offset(i) + table_x_index_in_table_i.
// For simplicity we use unqualified column names: name -> position in concatenated tuple.
std::unordered_map<std::string, size_t> BuildCombinedColumnIndexMap(
    SystemCatalog* catalog, const std::vector<std::string>& table_names) {
    std::unordered_map<std::string, size_t> m;
    size_t offset = 0;
    for (const auto& tname : table_names) {
        const TableInfo* info = catalog->GetTable(tname);
        if (!info) continue;
        for (size_t i = 0; i < info->columns.size(); ++i) {
            const auto& c = info->columns[i];
            // First table wins on name conflicts (simple rule)
            if (m.find(c.name) == m.end()) {
                m[c.name] = offset + i;
            }
        }
        offset += info->columns.size();
    }
    return m;
}

}  // namespace

ExecutionEngine::ExecutionEngine(SystemCatalog* catalog) : catalog_(catalog) {
}

ExecutionResult ExecutionEngine::Execute(const PlanNodePtr& plan) {
    ExecutionResult result;
    if (!plan) return result;
    try {
        ExecutionContext ctx(catalog_);
        auto root = BuildExecutor(plan, &ctx);
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
                         plan->GetType() == PlanNodeType::AGGREGATE);
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
            return std::make_unique<SeqScanExecutor>(context, n->table_name);
        }
        case PlanNodeType::INDEX_SCAN: {
            auto n = std::static_pointer_cast<IndexScanNode>(plan_node);
            auto col_map = BuildCombinedColumnIndexMap(context->GetCatalog(), {n->table_name});
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
                cmap = BuildCombinedColumnIndexMap(catalog_, CollectScanTableNames(plan_node));
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
                    return std::make_unique<DistinctExecutor>(context, std::move(proj));
                }
                return proj;
            }
            auto child = BuildExecutor(plan_node->children[0], context);
            if (!child) return nullptr;
            // If child is an Aggregate, the aggregate already produced tuples
            // matching the SELECT list; pass through unchanged.
            if (plan_node->children[0]->GetType() == PlanNodeType::AGGREGATE) {
                if (n->is_distinct) {
                    return std::make_unique<DistinctExecutor>(context, std::move(child));
                }
                return child;
            }
            // If child is a Filter (HAVING) whose own child is Aggregate, pass
            // through unchanged.
            if (plan_node->children[0]->GetType() == PlanNodeType::FILTER &&
                plan_node->children[0]->children.size() > 0 &&
                plan_node->children[0]->children[0]->GetType() == PlanNodeType::AGGREGATE) {
                if (n->is_distinct) {
                    return std::make_unique<DistinctExecutor>(context, std::move(child));
                }
                return child;
            }
            auto cmap = BuildCombinedColumnIndexMap(catalog_, CollectScanTableNames(plan_node));
            ExecutorPtr proj_exec = std::make_unique<ProjectExecutor>(context, std::move(child), n->columns, cmap);
            if (n->is_distinct) {
                return std::make_unique<DistinctExecutor>(context, std::move(proj_exec));
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
            // If child is Project, the tuples arriving at Sort have already been
            // projected to select_list. Build cmap from select_list order.
            std::unordered_map<std::string, size_t> cmap;
            if (!plan_node->children.empty() &&
                plan_node->children[0]->GetType() == PlanNodeType::PROJECT) {
                auto proj = std::static_pointer_cast<ProjectNode>(plan_node->children[0]);
                for (size_t i = 0; i < proj->columns.size(); ++i) {
                    const auto& e = proj->columns[i];
                    if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
                        cmap[std::static_pointer_cast<ColumnRefExpr>(e)->column_name] = i;
                    }
                }
            } else {
                cmap = BuildCombinedColumnIndexMap(catalog_, CollectScanTableNames(plan_node));
            }
            return std::make_unique<SortExecutor>(context, std::move(child), n->order_items, cmap);
        }
        case PlanNodeType::AGGREGATE: {
            auto n = std::static_pointer_cast<AggregateNode>(plan_node);
            auto child = BuildExecutor(plan_node->children.empty() ? nullptr : plan_node->children[0], context);
            if (!child) return nullptr;
            auto cmap = BuildCombinedColumnIndexMap(catalog_, CollectScanTableNames(plan_node));
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
            auto cmap = BuildCombinedColumnIndexMap(catalog_, CollectScanTableNames(plan_node));
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
    // Walk through Sort/Limit wrappers to find the underlying Project (or Aggregate)
    PlanNodePtr p = plan_node;
    while (p && (p->GetType() == PlanNodeType::SORT ||
                 p->GetType() == PlanNodeType::LIMIT)) {
        if (p->children.empty()) return names;
        p = p->children[0];
    }
    if (!p) return names;
    // AggregateNode directly holds the SELECT list as its aggregate_exprs
    if (p->GetType() == PlanNodeType::AGGREGATE) {
        auto agg = std::static_pointer_cast<AggregateNode>(p);
        for (const auto& e : agg->aggregate_exprs) {
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
                std::string tname = FindScanTableName(proj);
                const TableInfo* info = catalog_->GetTable(tname);
                if (info) {
                    for (const auto& c : info->columns) names.push_back(c.name);
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