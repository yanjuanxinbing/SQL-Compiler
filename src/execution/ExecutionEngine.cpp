#include "execution/ExecutionEngine.h"

#include "common/Error.h"
#include "execution/CreateTableExecutor.h"
#include "execution/DeleteExecutor.h"
#include "execution/DropTableExecutor.h"
#include "execution/FilterExecutor.h"
#include "execution/InsertExecutor.h"
#include "execution/ProjectExecutor.h"
#include "execution/SeqScanExecutor.h"
#include "execution/UpdateExecutor.h"

#include <utility>

namespace sqlcompiler {

namespace {

std::string FindScanTableName(const PlanNodePtr& node) {
    if (!node) return "";
    if (node->GetType() == PlanNodeType::SEQ_SCAN) {
        return std::static_pointer_cast<SeqScanNode>(node)->table_name;
    }
    for (auto& c : node->children) {
        std::string t = FindScanTableName(c);
        if (!t.empty()) return t;
    }
    return "";
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

        // Determine if root is a query (project) or DML/DDL
        bool is_query = (plan->GetType() == PlanNodeType::PROJECT);
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
        case PlanNodeType::FILTER: {
            auto n = std::static_pointer_cast<FilterNode>(plan_node);
            auto child = BuildExecutor(plan_node->children.empty() ? nullptr : plan_node->children[0], context);
            if (!child) return nullptr;
            std::string tname = FindScanTableName(plan_node);
            return std::make_unique<FilterExecutor>(context, std::move(child), n->predicate,
                                                     BuildColumnIndexMap(tname));
        }
        case PlanNodeType::PROJECT: {
            auto n = std::static_pointer_cast<ProjectNode>(plan_node);
            auto child = BuildExecutor(plan_node->children.empty() ? nullptr : plan_node->children[0], context);
            if (!child) return nullptr;
            std::string tname = FindScanTableName(plan_node);
            return std::make_unique<ProjectExecutor>(context, std::move(child), n->columns,
                                                      BuildColumnIndexMap(tname));
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
            return std::make_unique<CreateTableExecutor>(context, n->table_name, n->columns);
        }
        case PlanNodeType::DROP_TABLE: {
            auto n = std::static_pointer_cast<DropTableNode>(plan_node);
            return std::make_unique<DropTableExecutor>(context, n->table_name);
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
    if (plan_node->GetType() != PlanNodeType::PROJECT) return names;
    auto proj = std::static_pointer_cast<ProjectNode>(plan_node);
    for (const auto& e : proj->columns) {
        if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
            auto cr = std::static_pointer_cast<ColumnRefExpr>(e);
            names.push_back(cr->column_name);
        } else if (e && e->GetType() == NodeType::FUNCTION_CALL_EXPR) {
            auto fc = std::static_pointer_cast<FunctionCallExpr>(e);
            if (fc->function_name == "*" || fc->function_name == "STAR") {
                // Use underlying schema
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