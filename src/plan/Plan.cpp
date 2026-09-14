#include "plan/Plan.h"

#include <sstream>

namespace sqlcompiler {

namespace {

const char* JoinTypeName(JoinType t) {
    switch (t) {
        case JoinType::INNER: return "INNER";
        case JoinType::LEFT:  return "LEFT";
        case JoinType::RIGHT: return "RIGHT";
    }
    return "?";
}

std::string Indent(int depth) {
    return std::string(static_cast<size_t>(depth) * 2, ' ');
}

std::string NodeBodyToString(const PlanNode& node, int depth) {
    std::ostringstream oss;
    oss << Indent(depth);
    switch (node.GetType()) {
        case PlanNodeType::SEQ_SCAN: {
            auto& n = static_cast<const SeqScanNode&>(node);
            oss << "SeqScan(" << n.table_name;
            if (n.predicate) {
                oss << ", [" << n.predicate->ToString() << "]";
            }
            oss << ")";
            break;
        }
        case PlanNodeType::FILTER: {
            auto& n = static_cast<const FilterNode&>(node);
            oss << "Filter(" << (n.predicate ? n.predicate->ToString() : "?") << ")";
            break;
        }
        case PlanNodeType::PROJECT: {
            auto& n = static_cast<const ProjectNode&>(node);
            oss << "Project(";
            for (size_t i = 0; i < n.columns.size(); ++i) {
                if (i) oss << ", ";
                oss << (n.columns[i] ? n.columns[i]->ToString() : "?");
            }
            oss << ")";
            break;
        }
        case PlanNodeType::JOIN: {
            auto& n = static_cast<const JoinNode&>(node);
            oss << "Join(" << JoinTypeName(n.join_type) << ", "
                << (n.condition ? n.condition->ToString() : "?") << ")";
            break;
        }
        case PlanNodeType::SORT: {
            auto& n = static_cast<const SortNode&>(node);
            oss << "Sort(";
            for (size_t i = 0; i < n.order_items.size(); ++i) {
                if (i) oss << ", ";
                oss << (n.order_items[i].expr ? n.order_items[i].expr->ToString() : "?")
                    << (n.order_items[i].ascending ? " ASC" : " DESC");
            }
            oss << ")";
            break;
        }
        case PlanNodeType::LIMIT: {
            auto& n = static_cast<const LimitNode&>(node);
            oss << "Limit(" << n.limit_count << ")";
            break;
        }
        case PlanNodeType::AGGREGATE: {
            auto& n = static_cast<const AggregateNode&>(node);
            oss << "Aggregate(";
            oss << "GROUP BY [";
            for (size_t i = 0; i < n.group_by_exprs.size(); ++i) {
                if (i) oss << ", ";
                oss << (n.group_by_exprs[i] ? n.group_by_exprs[i]->ToString() : "?");
            }
            oss << "], AGG [";
            for (size_t i = 0; i < n.aggregate_exprs.size(); ++i) {
                if (i) oss << ", ";
                oss << (n.aggregate_exprs[i] ? n.aggregate_exprs[i]->ToString() : "?");
            }
            oss << "])";
            break;
        }
        case PlanNodeType::INSERT: {
            auto& n = static_cast<const InsertNode&>(node);
            oss << "Insert(" << n.table_name << ", "
                << n.values_list.size() << " rows)";
            break;
        }
        case PlanNodeType::UPDATE: {
            auto& n = static_cast<const UpdateNode&>(node);
            oss << "Update(" << n.table_name << ")";
            break;
        }
        case PlanNodeType::DELETE: {
            auto& n = static_cast<const DeleteNode&>(node);
            oss << "Delete(" << n.table_name << ")";
            break;
        }
        case PlanNodeType::CREATE_TABLE: {
            auto& n = static_cast<const CreateTableNode&>(node);
            oss << "CreateTable(" << n.table_name << ", "
                << n.columns.size() << " cols)";
            break;
        }
        case PlanNodeType::DROP_TABLE: {
            auto& n = static_cast<const DropTableNode&>(node);
            oss << "DropTable(" << n.table_name << ")";
            break;
        }
        case PlanNodeType::CREATE_INDEX: {
            auto& n = static_cast<const CreateIndexNode&>(node);
            oss << "CreateIndex(" << n.index_name << " on " << n.table_name << ")";
            break;
        }
        case PlanNodeType::DROP_INDEX: {
            auto& n = static_cast<const DropIndexNode&>(node);
            oss << "DropIndex(" << n.index_name << ")";
            break;
        }
        case PlanNodeType::INDEX_SCAN: {
            auto& n = static_cast<const IndexScanNode&>(node);
            oss << "IndexScan(" << n.table_name << " using " << n.index_name << ")";
            break;
        }
        case PlanNodeType::TRUNCATE_TABLE: {
            auto& n = static_cast<const TruncateTableNode&>(node);
            oss << "TruncateTable(" << n.table_name << ")";
            break;
        }
        case PlanNodeType::SET_OP: {
            auto& n = static_cast<const SetOpNode&>(node);
            const char* kn = "?";
            switch (n.kind) {
                case SetOpNode::Kind::UNION: kn = "UNION"; break;
                case SetOpNode::Kind::UNION_ALL: kn = "UNION ALL"; break;
                case SetOpNode::Kind::INTERSECT: kn = "INTERSECT"; break;
                case SetOpNode::Kind::EXCEPT: kn = "EXCEPT"; break;
            }
            oss << "SetOp(" << kn << ")";
            break;
        }
        case PlanNodeType::WINDOW: {
            auto& n = static_cast<const WindowNode&>(node);
            oss << "Window(";
            for (size_t i = 0; i < n.select_list.size(); ++i) {
                if (i) oss << ", ";
                oss << (n.select_list[i] ? n.select_list[i]->ToString() : "?");
            }
            oss << ")";
            break;
        }
        case PlanNodeType::VALUES: {
            auto& n = static_cast<const ValuesNode&>(node);
            oss << "Values(" << n.derived_alias << ", "
                << n.rows.size() << " rows)";
            break;
        }
        case PlanNodeType::APPLY: {
            auto& n = static_cast<const ApplyNode&>(node);
            oss << "Apply("
                << (n.is_left_outer ? "LEFT_OUTER" : "CROSS") << ")";
            break;
        }
    }
    oss << "\n";
    for (auto& child : node.children) {
        if (child) oss << NodeBodyToString(*child, depth + 1);
    }
    return oss.str();
}

// =========================================================================
// JSON / S-expr 结构化序列化
// =========================================================================
//
// 与 ToString 的区别：ToString 走 NodeBodyToString 中心化 dispatcher 后再
// 由各子类 ToString() 决定要不要递归 children（很多子类自定义 ToString 不
// 递归，导致 plan 树被截断）。JSON / S-expr 必须看到完整子树，因此 base
// class 的 ToJson / ToSExpr 默认实现走下面两个 free function，由它们负责
// 递归 children。各子类无需 override 即可得到完整结构化输出。

// 写一个 JSON 字符串字面量（含必要的转义）。
static void JsonWriteString(std::ostringstream& oss, const std::string& s) {
    oss << '"';
    for (char c : s) {
        switch (c) {
            case '"':  oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\n': oss << "\\n";  break;
            case '\r': oss << "\\r";  break;
            case '\t': oss << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    oss << buf;
                } else {
                    oss << c;
                }
        }
    }
    oss << '"';
}

// 把若干 (key, value) 对写成 JSON object 字段。fields 为空就输出 {}。
// indent 是当前节点的缩进（不含字段自身的 +2），写每个 key 时再加 2 空格
// 让字段比 `{` 多缩一级，与主流 JSON 美化器一致。
static void WriteJsonFields(std::ostringstream& oss,
                            const std::string& indent,
                            const std::vector<std::pair<std::string, std::string>>& fields,
                            bool comma_prefix) {
    for (const auto& [k, v] : fields) {
        if (comma_prefix) oss << ",";
        oss << "\n" << indent << "  ";
        JsonWriteString(oss, k);
        oss << ": ";
        JsonWriteString(oss, v);
        comma_prefix = true;
    }
}

// 各节点类型的 fields（key-value 字符串对）。空 fields 表示无字段。
static std::vector<std::pair<std::string, std::string>> NodeJsonFields(const PlanNode& node) {
    using P = std::pair<std::string, std::string>;
    std::vector<P> f;
    switch (node.GetType()) {
        case PlanNodeType::SEQ_SCAN: {
            auto& n = static_cast<const SeqScanNode&>(node);
            f.emplace_back("table", n.table_name);
            if (!n.table_alias.empty()) f.emplace_back("alias", n.table_alias);
            if (n.predicate) f.emplace_back("predicate", n.predicate->ToString());
            break;
        }
        case PlanNodeType::INDEX_SCAN: {
            auto& n = static_cast<const IndexScanNode&>(node);
            f.emplace_back("table", n.table_name);
            f.emplace_back("index", n.index_name);
            if (!n.table_alias.empty()) f.emplace_back("alias", n.table_alias);
            if (n.residual_predicate) {
                f.emplace_back("residual", n.residual_predicate->ToString());
            }
            break;
        }
        case PlanNodeType::FILTER: {
            auto& n = static_cast<const FilterNode&>(node);
            if (n.predicate) f.emplace_back("predicate", n.predicate->ToString());
            break;
        }
        case PlanNodeType::PROJECT: {
            auto& n = static_cast<const ProjectNode&>(node);
            for (size_t i = 0; i < n.columns.size(); ++i) {
                std::string col = n.columns[i] ? n.columns[i]->ToString() : "?";
                if (i < n.aliases.size() && !n.aliases[i].empty()) {
                    col += " AS " + n.aliases[i];
                }
                f.emplace_back("col_" + std::to_string(i), col);
            }
            if (n.is_distinct) f.emplace_back("distinct", "true");
            break;
        }
        case PlanNodeType::JOIN: {
            auto& n = static_cast<const JoinNode&>(node);
            const char* jt = "INNER";
            switch (n.join_type) {
                case JoinType::INNER: jt = "INNER"; break;
                case JoinType::LEFT:  jt = "LEFT";  break;
                case JoinType::RIGHT: jt = "RIGHT"; break;
            }
            f.emplace_back("type", jt);
            if (n.condition) f.emplace_back("condition", n.condition->ToString());
            break;
        }
        case PlanNodeType::SORT: {
            auto& n = static_cast<const SortNode&>(node);
            for (size_t i = 0; i < n.order_items.size(); ++i) {
                const auto& it = n.order_items[i];
                std::string s = it.expr ? it.expr->ToString() : "?";
                s += it.ascending ? " ASC" : " DESC";
                f.emplace_back("key_" + std::to_string(i), s);
            }
            break;
        }
        case PlanNodeType::LIMIT: {
            auto& n = static_cast<const LimitNode&>(node);
            f.emplace_back("count", std::to_string(n.limit_count));
            break;
        }
        case PlanNodeType::AGGREGATE: {
            auto& n = static_cast<const AggregateNode&>(node);
            for (size_t i = 0; i < n.group_by_exprs.size(); ++i) {
                std::string g = n.group_by_exprs[i] ? n.group_by_exprs[i]->ToString() : "?";
                f.emplace_back("group_" + std::to_string(i), g);
            }
            for (size_t i = 0; i < n.aggregate_exprs.size(); ++i) {
                std::string a = n.aggregate_exprs[i] ? n.aggregate_exprs[i]->ToString() : "?";
                if (i < n.aliases.size() && !n.aliases[i].empty()) a += " AS " + n.aliases[i];
                f.emplace_back("agg_" + std::to_string(i), a);
            }
            break;
        }
        case PlanNodeType::INSERT: {
            auto& n = static_cast<const InsertNode&>(node);
            f.emplace_back("table", n.table_name);
            f.emplace_back("rows", std::to_string(n.values_list.size()));
            break;
        }
        case PlanNodeType::UPDATE: {
            auto& n = static_cast<const UpdateNode&>(node);
            f.emplace_back("table", n.table_name);
            break;
        }
        case PlanNodeType::DELETE: {
            auto& n = static_cast<const DeleteNode&>(node);
            f.emplace_back("table", n.table_name);
            break;
        }
        case PlanNodeType::CREATE_TABLE: {
            auto& n = static_cast<const CreateTableNode&>(node);
            f.emplace_back("table", n.table_name);
            f.emplace_back("columns", std::to_string(n.columns.size()));
            break;
        }
        case PlanNodeType::DROP_TABLE: {
            auto& n = static_cast<const DropTableNode&>(node);
            f.emplace_back("table", n.table_name);
            break;
        }
        case PlanNodeType::CREATE_INDEX: {
            auto& n = static_cast<const CreateIndexNode&>(node);
            f.emplace_back("index", n.index_name);
            f.emplace_back("table", n.table_name);
            break;
        }
        case PlanNodeType::DROP_INDEX: {
            auto& n = static_cast<const DropIndexNode&>(node);
            f.emplace_back("index", n.index_name);
            break;
        }
        case PlanNodeType::TRUNCATE_TABLE: {
            auto& n = static_cast<const TruncateTableNode&>(node);
            f.emplace_back("table", n.table_name);
            break;
        }
        case PlanNodeType::SET_OP: {
            auto& n = static_cast<const SetOpNode&>(node);
            const char* k = "?";
            switch (n.kind) {
                case SetOpNode::Kind::UNION: k = "UNION"; break;
                case SetOpNode::Kind::UNION_ALL: k = "UNION ALL"; break;
                case SetOpNode::Kind::INTERSECT: k = "INTERSECT"; break;
                case SetOpNode::Kind::EXCEPT: k = "EXCEPT"; break;
            }
            f.emplace_back("op", k);
            break;
        }
        case PlanNodeType::WINDOW: {
            auto& n = static_cast<const WindowNode&>(node);
            for (size_t i = 0; i < n.select_list.size(); ++i) {
                f.emplace_back("col_" + std::to_string(i),
                               n.select_list[i] ? n.select_list[i]->ToString() : "?");
            }
            break;
        }
        case PlanNodeType::VALUES: {
            auto& n = static_cast<const ValuesNode&>(node);
            f.emplace_back("alias", n.derived_alias);
            f.emplace_back("rows", std::to_string(n.rows.size()));
            break;
        }
        case PlanNodeType::APPLY: {
            auto& n = static_cast<const ApplyNode&>(node);
            f.emplace_back("type", n.is_left_outer ? "LEFT_OUTER" : "CROSS");
            break;
        }
        case PlanNodeType::EXPLAIN: {
            auto& n = static_cast<const ExplainNode&>(node);
            f.emplace_back("analyze", n.analyze ? "true" : "false");
            f.emplace_back("format", n.format);
            break;
        }
        default:
            break;
    }
    return f;
}

static std::string PlanNodeTypeName(PlanNodeType t) {
    switch (t) {
        case PlanNodeType::SEQ_SCAN: return "SeqScan";
        case PlanNodeType::INDEX_SCAN: return "IndexScan";
        case PlanNodeType::FILTER: return "Filter";
        case PlanNodeType::PROJECT: return "Project";
        case PlanNodeType::JOIN: return "Join";
        case PlanNodeType::SORT: return "Sort";
        case PlanNodeType::LIMIT: return "Limit";
        case PlanNodeType::AGGREGATE: return "Aggregate";
        case PlanNodeType::INSERT: return "Insert";
        case PlanNodeType::UPDATE: return "Update";
        case PlanNodeType::DELETE: return "Delete";
        case PlanNodeType::CREATE_TABLE: return "CreateTable";
        case PlanNodeType::DROP_TABLE: return "DropTable";
        case PlanNodeType::TRUNCATE_TABLE: return "TruncateTable";
        case PlanNodeType::CREATE_INDEX: return "CreateIndex";
        case PlanNodeType::DROP_INDEX: return "DropIndex";
        case PlanNodeType::ALTER_TABLE: return "AlterTable";
        case PlanNodeType::SET_OP: return "SetOp";
        case PlanNodeType::WINDOW: return "Window";
        case PlanNodeType::SUBQUERY: return "Subquery";
        case PlanNodeType::CTE_BIND: return "CteBind";
        case PlanNodeType::CTE_DEFINE: return "CteDefine";
        case PlanNodeType::NO_OP: return "NoOp";
        case PlanNodeType::CREATE_VIEW: return "CreateView";
        case PlanNodeType::CREATE_TRIGGER: return "CreateTrigger";
        case PlanNodeType::CREATE_FUNCTION: return "CreateFunction";
        case PlanNodeType::CREATE_PROCEDURE: return "CreateProcedure";
        case PlanNodeType::CALL: return "Call";
        case PlanNodeType::VIEW_DEFINE: return "ViewDefine";
        case PlanNodeType::UPSERT: return "Upsert";
        case PlanNodeType::BEGIN_TXN: return "BeginTxn";
        case PlanNodeType::COMMIT_TXN: return "CommitTxn";
        case PlanNodeType::ROLLBACK_TXN: return "RollbackTxn";
        case PlanNodeType::SAVEPOINT: return "Savepoint";
        case PlanNodeType::ROLLBACK_TO_SP: return "RollbackToSp";
        case PlanNodeType::RELEASE_SP: return "ReleaseSp";
        case PlanNodeType::EXPLAIN: return "Explain";
        case PlanNodeType::SHOW: return "Show";
        case PlanNodeType::CREATE_SCHEMA: return "CreateSchema";
        case PlanNodeType::DROP_SCHEMA: return "DropSchema";
        case PlanNodeType::CREATE_SEQUENCE: return "CreateSequence";
        case PlanNodeType::DROP_SEQUENCE: return "DropSequence";
        case PlanNodeType::UPDATE_FROM: return "UpdateFrom";
        case PlanNodeType::MERGE: return "Merge";
        case PlanNodeType::VALUES: return "Values";
        case PlanNodeType::APPLY: return "Apply";
        case PlanNodeType::CREATE_MATERIALIZED_VIEW: return "CreateMaterializedView";
        case PlanNodeType::ALTER_MATERIALIZED_VIEW: return "AlterMaterializedView";
    }
    return "?";
}

static std::string SerializeNodeToJson(const PlanNode& node, int depth) {
    std::ostringstream oss;
    std::string indent(depth * 2, ' ');
    oss << indent << "{\n";
    oss << indent << "  \"type\": ";
    JsonWriteString(oss, PlanNodeTypeName(node.GetType()));
    auto fields = NodeJsonFields(node);
    WriteJsonFields(oss, indent, fields, true);
    if (!node.children.empty()) {
        oss << ",\n" << indent << "  \"children\": [";
        bool first = true;
        for (const auto& c : node.children) {
            if (!c) continue;
            if (!first) oss << ",";
            oss << "\n";
            oss << SerializeNodeToJson(*c, depth + 1);
            first = false;
        }
        if (!first) oss << "\n" << indent << "  ";
        oss << "]";
    }
    oss << "\n" << indent << "}";
    return oss.str();
}

// S-expr 序列化：(Name :key "val" ... child1 child2 ...)
// child 是另一个 (Name ...) 表达式；无 children 时输出 (Name :key "val")。
static std::string SerializeNodeToSExpr(const PlanNode& node) {
    std::ostringstream oss;
    oss << "(" << PlanNodeTypeName(node.GetType());
    auto fields = NodeJsonFields(node);
    for (const auto& [k, v] : fields) {
        oss << " :" << k << " ";
        JsonWriteString(oss, v);  // 借用 JSON 转义：同样处理 \ " \n 等
    }
    for (const auto& c : node.children) {
        if (c) oss << " " << SerializeNodeToSExpr(*c);
    }
    oss << ")";
    return oss.str();
}

}  // namespace

// ============ PlanNode 基类方法 ============

std::string PlanNode::ToJson() const {
    return SerializeNodeToJson(*this, 0);
}

std::string PlanNode::ToSExpr() const {
    return SerializeNodeToSExpr(*this);
}

// ============ SeqScanNode ============

SeqScanNode::SeqScanNode(std::string table_name, std::string table_alias,
                         ExprPtr predicate)
    : table_name(std::move(table_name)),
      table_alias(std::move(table_alias)),
      predicate(std::move(predicate)) {
}

PlanNodeType SeqScanNode::GetType() const {
    return PlanNodeType::SEQ_SCAN;
}

std::string SeqScanNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ FilterNode ============

FilterNode::FilterNode(ExprPtr predicate) : predicate(std::move(predicate)) {
}

PlanNodeType FilterNode::GetType() const {
    return PlanNodeType::FILTER;
}

std::string FilterNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ ProjectNode ============

ProjectNode::ProjectNode(std::vector<ExprPtr> columns,
                          std::vector<std::string> aliases,
                          bool is_distinct)
    : columns(std::move(columns)), aliases(std::move(aliases)), is_distinct(is_distinct) {
}

PlanNodeType ProjectNode::GetType() const {
    return PlanNodeType::PROJECT;
}

std::string ProjectNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ JoinNode ============

JoinNode::JoinNode(JoinType join_type, ExprPtr condition)
    : join_type(join_type), condition(std::move(condition)) {
}

PlanNodeType JoinNode::GetType() const {
    return PlanNodeType::JOIN;
}

std::string JoinNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ SortNode ============

SortNode::SortNode(std::vector<OrderByItem> order_items) : order_items(std::move(order_items)) {
}

PlanNodeType SortNode::GetType() const {
    return PlanNodeType::SORT;
}

std::string SortNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ LimitNode ============

LimitNode::LimitNode(int limit_count, int offset)
    : limit_count(limit_count), offset(offset) {
}

PlanNodeType LimitNode::GetType() const {
    return PlanNodeType::LIMIT;
}

std::string LimitNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ AggregateNode ============

AggregateNode::AggregateNode(std::vector<ExprPtr> group_by_exprs,
                              std::vector<ExprPtr> aggregate_exprs,
                              std::vector<std::string> aliases)
    : group_by_exprs(std::move(group_by_exprs)),
      aggregate_exprs(std::move(aggregate_exprs)),
      aliases(std::move(aliases)) {
}

PlanNodeType AggregateNode::GetType() const {
    return PlanNodeType::AGGREGATE;
}

std::string AggregateNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ InsertNode ============

InsertNode::InsertNode(std::string table_name, std::vector<std::string> columns,
                        std::vector<std::vector<ExprPtr>> values_list)
    : table_name(std::move(table_name)),
      columns(std::move(columns)),
      values_list(std::move(values_list)) {
}

PlanNodeType InsertNode::GetType() const {
    return PlanNodeType::INSERT;
}

std::string InsertNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ UpdateNode ============

UpdateNode::UpdateNode(std::string table_name,
                        std::vector<std::pair<std::string, ExprPtr>> assignments,
                        ExprPtr predicate)
    : table_name(std::move(table_name)),
      assignments(std::move(assignments)),
      predicate(std::move(predicate)) {
}

PlanNodeType UpdateNode::GetType() const {
    return PlanNodeType::UPDATE;
}

std::string UpdateNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ UpsertNode (43_upsert) ============

UpsertNode::UpsertNode(std::string table_name, std::vector<std::string> columns,
                       std::vector<std::vector<ExprPtr>> values_list,
                       std::vector<std::pair<std::string, ExprPtr>> upsert_assignments)
    : table_name(std::move(table_name)),
      columns(std::move(columns)),
      values_list(std::move(values_list)),
      upsert_assignments(std::move(upsert_assignments)) {
}

PlanNodeType UpsertNode::GetType() const {
    return PlanNodeType::UPSERT;
}

std::string UpsertNode::ToString() const {
    std::ostringstream oss;
    oss << "Upsert(" << table_name << ", "
        << values_list.size() << " rows, " << upsert_assignments.size() << " assigns)\n";
    return oss.str();
}

// ============ DeleteNode ============

DeleteNode::DeleteNode(std::string table_name, ExprPtr predicate)
    : table_name(std::move(table_name)), predicate(std::move(predicate)) {
}

PlanNodeType DeleteNode::GetType() const {
    return PlanNodeType::DELETE;
}

std::string DeleteNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ CreateTableNode ============

CreateTableNode::CreateTableNode(std::string table_name, std::vector<ColumnDefinition> columns,
                                 std::vector<std::vector<std::string>> primary_keys,
                                 std::vector<std::vector<std::string>> unique_constraints,
                                 std::vector<ForeignKeyDef> foreign_keys,
                                 std::vector<TableCheckDef> table_checks,
                                 bool if_not_exists)
    : table_name(std::move(table_name)), columns(std::move(columns)),
      primary_keys(std::move(primary_keys)),
      unique_constraints(std::move(unique_constraints)),
      foreign_keys(std::move(foreign_keys)),
      table_checks(std::move(table_checks)),
      if_not_exists(if_not_exists) {
}

PlanNodeType CreateTableNode::GetType() const {
    return PlanNodeType::CREATE_TABLE;
}

std::string CreateTableNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ DropTableNode ============

DropTableNode::DropTableNode(std::string table_name, bool if_exists)
    : table_name(std::move(table_name)), if_exists(if_exists) {
}

PlanNodeType DropTableNode::GetType() const {
    return PlanNodeType::DROP_TABLE;
}

std::string DropTableNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ IndexScanNode ============

IndexScanNode::IndexScanNode(std::string table_name, std::string index_name,
                             std::string table_alias)
    : table_name(std::move(table_name)), index_name(std::move(index_name)),
      table_alias(std::move(table_alias)) {
}

PlanNodeType IndexScanNode::GetType() const {
    return PlanNodeType::INDEX_SCAN;
}

std::string IndexScanNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ CreateIndexNode ============

CreateIndexNode::CreateIndexNode(std::string index_name, std::string table_name,
                                 std::vector<std::string> key_columns,
                                 bool is_unique)
    : index_name(std::move(index_name)), table_name(std::move(table_name)),
      key_columns(std::move(key_columns)), is_unique(is_unique) {
}

PlanNodeType CreateIndexNode::GetType() const {
    return PlanNodeType::CREATE_INDEX;
}

std::string CreateIndexNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ DropIndexNode ============

DropIndexNode::DropIndexNode(std::string index_name, bool if_exists)
    : index_name(std::move(index_name)), if_exists(if_exists) {
}

PlanNodeType DropIndexNode::GetType() const {
    return PlanNodeType::DROP_INDEX;
}

std::string DropIndexNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ TruncateTableNode ============

TruncateTableNode::TruncateTableNode(std::string table_name) : table_name(std::move(table_name)) {
}

PlanNodeType TruncateTableNode::GetType() const {
    return PlanNodeType::TRUNCATE_TABLE;
}

std::string TruncateTableNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ AlterTableNode ============

AlterTableNode::AlterTableNode(AlterAction action, std::string table_name)
    : action(action), table_name(std::move(table_name)) {
}

PlanNodeType AlterTableNode::GetType() const {
    return PlanNodeType::ALTER_TABLE;
}

std::string AlterTableNode::ToString() const {
    std::ostringstream oss;
    oss << "AlterTable(" << table_name << ", ";
    switch (action) {
        case AlterAction::ADD_COLUMN:    oss << "ADD_COLUMN"; break;
        case AlterAction::DROP_COLUMN:   oss << "DROP_COLUMN"; break;
        case AlterAction::RENAME_TO:     oss << "RENAME_TO"; break;
        case AlterAction::MODIFY_COLUMN: oss << "MODIFY_COLUMN"; break;
    }
    oss << ")";
    return oss.str();
}

// ============ SetOpNode ============

SetOpNode::SetOpNode(Kind kind) : kind(kind) {
}

PlanNodeType SetOpNode::GetType() const {
    return PlanNodeType::SET_OP;
}

std::string SetOpNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ WindowNode ============

WindowNode::WindowNode(std::vector<ExprPtr> select_list,
                       std::vector<std::string> aliases,
                       std::vector<std::pair<std::string, WindowSpec>> named_windows)
    : select_list(std::move(select_list)),
      aliases(std::move(aliases)),
      named_windows(std::move(named_windows)) {
}

PlanNodeType WindowNode::GetType() const {
    return PlanNodeType::WINDOW;
}

std::string WindowNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ SubqueryNode ============

SubqueryNode::SubqueryNode(SubqueryType kind, ExprPtr outer_expr,
                            std::string comparison_op)
    : kind(kind),
      outer_expr(std::move(outer_expr)),
      comparison_op(std::move(comparison_op)) {
}

PlanNodeType SubqueryNode::GetType() const {
    return PlanNodeType::SUBQUERY;
}

std::string SubqueryNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ CteDefineNode ============

CteDefineNode::CteDefineNode(std::string cte_name, bool is_recursive)
    : cte_name(std::move(cte_name)), is_recursive(is_recursive) {
}

PlanNodeType CteDefineNode::GetType() const {
    return PlanNodeType::CTE_DEFINE;
}

std::string CteDefineNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ CteBindNode ============

CteBindNode::CteBindNode(std::string cte_name)
    : cte_name(std::move(cte_name)) {
}

PlanNodeType CteBindNode::GetType() const {
    return PlanNodeType::CTE_BIND;
}

std::string CteBindNode::ToString() const {
    return NodeBodyToString(*this, 0);
}

// ============ 40_txn_view_udf：NoOpNode / CreateViewNode / etc ============

NoOpNode::NoOpNode(std::string description) : description(std::move(description)) {
}
PlanNodeType NoOpNode::GetType() const { return PlanNodeType::NO_OP; }
std::string NoOpNode::ToString() const {
    return "NoOp(" + description + ")\n";
}

CreateViewNode::CreateViewNode(std::string view_name) : view_name(std::move(view_name)) {
}
PlanNodeType CreateViewNode::GetType() const { return PlanNodeType::CREATE_VIEW; }
std::string CreateViewNode::ToString() const {
    return "CreateView(" + view_name + ")\n";
}

CreateTriggerNode::CreateTriggerNode(std::string trigger_name)
    : trigger_name(std::move(trigger_name)) {
}
PlanNodeType CreateTriggerNode::GetType() const { return PlanNodeType::CREATE_TRIGGER; }
std::string CreateTriggerNode::ToString() const {
    return "CreateTrigger(" + trigger_name + ")\n";
}

CreateFunctionNode::CreateFunctionNode(std::string function_name)
    : function_name(std::move(function_name)) {
}
PlanNodeType CreateFunctionNode::GetType() const { return PlanNodeType::CREATE_FUNCTION; }
std::string CreateFunctionNode::ToString() const {
    return "CreateFunction(" + function_name + ")\n";
}

// ============ 59_procs (Category 8) ============

CreateProcedureNode::CreateProcedureNode(std::string procedure_name)
    : procedure_name(std::move(procedure_name)) {
}
PlanNodeType CreateProcedureNode::GetType() const { return PlanNodeType::CREATE_PROCEDURE; }
std::string CreateProcedureNode::ToString() const {
    return "CreateProcedure(" + procedure_name + ")\n";
}

CallNode::CallNode(std::string procedure_name, std::vector<ExprPtr> arguments)
    : procedure_name(std::move(procedure_name)), arguments(std::move(arguments)) {
}
PlanNodeType CallNode::GetType() const { return PlanNodeType::CALL; }
std::string CallNode::ToString() const {
    std::string out = "Call(" + procedure_name + "(";
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (i) out += ", ";
        out += arguments[i] ? arguments[i]->ToString() : "?";
    }
    out += "))\n";
    return out;
}

DropObjectNode::DropObjectNode(Kind kind, std::string object_name, bool if_exists)
    : kind(kind), object_name(std::move(object_name)), if_exists(if_exists) {
}
PlanNodeType DropObjectNode::GetType() const { return PlanNodeType::NO_OP; }
std::string DropObjectNode::ToString() const {
    const char* kn = "?";
    switch (kind) {
        case Kind::VIEW:     kn = "VIEW"; break;
        case Kind::TRIGGER:  kn = "TRIGGER"; break;
        case Kind::FUNCTION: kn = "FUNCTION"; break;
        case Kind::PROCEDURE: kn = "PROCEDURE"; break;
    }
    std::string out = "Drop";
    out += kn;
    out += "(";
    out += object_name;
    out += ")\n";
    return out;
}

ViewDefineNode::ViewDefineNode(std::string view_name, std::string view_alias)
    : view_name(std::move(view_name)), view_alias(std::move(view_alias)) {
}
PlanNodeType ViewDefineNode::GetType() const { return PlanNodeType::VIEW_DEFINE; }
std::string ViewDefineNode::ToString() const {
    return "ViewDefine(" + view_name + " AS " + view_alias + ")\n";
}

// ============ 46_meta: EXPLAIN / SHOW ============

ExplainNode::ExplainNode(bool analyze, std::string format)
    : analyze(analyze), format(std::move(format)) {
}
PlanNodeType ExplainNode::GetType() const { return PlanNodeType::EXPLAIN; }
std::string ExplainNode::ToString() const {
    std::ostringstream oss;
    oss << "Explain(";
    if (analyze) oss << "ANALYZE ";
    oss << "inner plan)\n";
    return oss.str();
}

ShowNode::ShowNode(Kind kind, std::string target_table)
    : kind(kind), target_table(std::move(target_table)) {
}
PlanNodeType ShowNode::GetType() const { return PlanNodeType::SHOW; }
std::string ShowNode::ToString() const {
    std::ostringstream oss;
    oss << "Show(";
    switch (kind) {
        case Kind::TABLES:       oss << "TABLES"; break;
        case Kind::COLUMNS:      oss << "COLUMNS FROM " << target_table; break;
        case Kind::INDEX:        oss << "INDEX FROM " << target_table; break;
        case Kind::CREATE_TABLE: oss << "CREATE TABLE " << target_table; break;
    }
    oss << ")\n";
    return oss.str();
}

// ============ 48_acid_undo: 事务控制节点 ============

BeginTxnNode::BeginTxnNode() = default;
PlanNodeType BeginTxnNode::GetType() const { return PlanNodeType::BEGIN_TXN; }
std::string BeginTxnNode::ToString() const { return "BeginTxn()\n"; }

CommitTxnNode::CommitTxnNode() = default;
PlanNodeType CommitTxnNode::GetType() const { return PlanNodeType::COMMIT_TXN; }
std::string CommitTxnNode::ToString() const { return "CommitTxn()\n"; }

RollbackTxnNode::RollbackTxnNode() = default;
PlanNodeType RollbackTxnNode::GetType() const { return PlanNodeType::ROLLBACK_TXN; }
std::string RollbackTxnNode::ToString() const { return "RollbackTxn()\n"; }

SavepointNode::SavepointNode(std::string name) : savepoint_name(std::move(name)) {}
PlanNodeType SavepointNode::GetType() const { return PlanNodeType::SAVEPOINT; }
std::string SavepointNode::ToString() const {
    return "Savepoint(" + savepoint_name + ")\n";
}

RollbackToSavepointNode::RollbackToSavepointNode(std::string name)
    : savepoint_name(std::move(name)) {}
PlanNodeType RollbackToSavepointNode::GetType() const {
    return PlanNodeType::ROLLBACK_TO_SP;
}
std::string RollbackToSavepointNode::ToString() const {
    return "RollbackToSavepoint(" + savepoint_name + ")\n";
}

ReleaseSavepointNode::ReleaseSavepointNode(std::string name)
    : savepoint_name(std::move(name)) {}
PlanNodeType ReleaseSavepointNode::GetType() const { return PlanNodeType::RELEASE_SP; }
std::string ReleaseSavepointNode::ToString() const {
    return "ReleaseSavepoint(" + savepoint_name + ")\n";
}

// ============ 53_ddl: SCHEMA / SEQUENCE ============

CreateSchemaNode::CreateSchemaNode(std::string name, bool if_not_exists)
    : schema_name(std::move(name)), if_not_exists(if_not_exists) {}
PlanNodeType CreateSchemaNode::GetType() const {
    return PlanNodeType::CREATE_SCHEMA;
}
std::string CreateSchemaNode::ToString() const {
    return "CreateSchema(" + schema_name + ")\n";
}

DropSchemaNode::DropSchemaNode(std::string name, bool if_exists)
    : schema_name(std::move(name)), if_exists(if_exists) {}
PlanNodeType DropSchemaNode::GetType() const {
    return PlanNodeType::DROP_SCHEMA;
}
std::string DropSchemaNode::ToString() const {
    return "DropSchema(" + schema_name + ")\n";
}

CreateSequenceNode::CreateSequenceNode(std::string name, int64_t start_value,
                                       int64_t increment, bool if_not_exists)
    : sequence_name(std::move(name)), start_value(start_value),
      increment(increment), if_not_exists(if_not_exists) {}
PlanNodeType CreateSequenceNode::GetType() const {
    return PlanNodeType::CREATE_SEQUENCE;
}
std::string CreateSequenceNode::ToString() const {
    return "CreateSequence(" + sequence_name + ")\n";
}

DropSequenceNode::DropSequenceNode(std::string name, bool if_exists)
    : sequence_name(std::move(name)), if_exists(if_exists) {}
PlanNodeType DropSequenceNode::GetType() const {
    return PlanNodeType::DROP_SEQUENCE;
}
std::string DropSequenceNode::ToString() const {
    return "DropSequence(" + sequence_name + ")\n";
}

// ============ 60_view_trigger (Category 9)：物化视图节点 ============

CreateMaterializedViewNode::CreateMaterializedViewNode(std::string view_name,
                                                       std::vector<ColumnDefinition> columns,
                                                       bool if_not_exists)
    : view_name(std::move(view_name)), columns(std::move(columns)),
      if_not_exists(if_not_exists) {}
PlanNodeType CreateMaterializedViewNode::GetType() const {
    return PlanNodeType::CREATE_MATERIALIZED_VIEW;
}
std::string CreateMaterializedViewNode::ToString() const {
    std::ostringstream oss;
    oss << "CreateMaterializedView(" << view_name << ", cols=[";
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i) oss << ", ";
        oss << columns[i].column_name << ":" << columns[i].data_type;
    }
    oss << "])\n";
    return oss.str();
}

AlterMaterializedViewNode::AlterMaterializedViewNode(std::string view_name)
    : view_name(std::move(view_name)) {}
PlanNodeType AlterMaterializedViewNode::GetType() const {
    return PlanNodeType::ALTER_MATERIALIZED_VIEW;
}
std::string AlterMaterializedViewNode::ToString() const {
    std::ostringstream oss;
    oss << "AlterMaterializedView(" << view_name << " REFRESH, cols=[";
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i) oss << ", ";
        oss << columns[i].column_name << ":" << columns[i].data_type;
    }
    oss << "])\n";
    return oss.str();
}

// ============ 54_dml: UPDATE FROM / MERGE ============

UpdateFromNode::UpdateFromNode(std::string table_name,
                               std::vector<std::pair<std::string, ExprPtr>> assignments)
    : table_name(std::move(table_name)), assignments(std::move(assignments)) {
}
PlanNodeType UpdateFromNode::GetType() const { return PlanNodeType::UPDATE_FROM; }
std::string UpdateFromNode::ToString() const {
    std::ostringstream oss;
    oss << "UpdateFrom(" << table_name << ", "
        << assignments.size() << " assigns)\n";
    return oss.str();
}

MergeNode::MergeNode(std::string target_table)
    : target_table(std::move(target_table)) {
}
PlanNodeType MergeNode::GetType() const { return PlanNodeType::MERGE; }
std::string MergeNode::ToString() const {
    std::ostringstream oss;
    oss << "Merge(target=" << target_table
        << ", source=" << source_table << ")\n";
    return oss.str();
}

// ============ 55_query: VALUES / APPLY ============

ValuesNode::ValuesNode(std::vector<std::vector<ExprPtr>> rows,
                       std::vector<std::string> column_aliases,
                       std::string derived_alias)
    : rows(std::move(rows)),
      column_aliases(std::move(column_aliases)),
      derived_alias(std::move(derived_alias)) {
}
PlanNodeType ValuesNode::GetType() const { return PlanNodeType::VALUES; }
std::string ValuesNode::ToString() const {
    std::ostringstream oss;
    oss << "Values(" << derived_alias << ", "
        << rows.size() << " rows)\n";
    return oss.str();
}

ApplyNode::ApplyNode(bool is_left_outer, std::string lateral_alias,
                     std::vector<std::string> lateral_inner_tables)
    : is_left_outer(is_left_outer), lateral_alias(std::move(lateral_alias)),
      lateral_inner_tables(std::move(lateral_inner_tables)) {
}
PlanNodeType ApplyNode::GetType() const { return PlanNodeType::APPLY; }
std::string ApplyNode::ToString() const {
    std::ostringstream oss;
    oss << "Apply(" << (is_left_outer ? "LEFT_OUTER" : "CROSS")
        << ", alias=" << lateral_alias << ")\n";
    for (auto& ch : children) {
        if (ch) oss << NodeBodyToString(*ch, 1);
    }
    return oss.str();
}

}  // namespace sqlcompiler