#include "plan/Plan.h"

#include <ostream>
#include <sstream>
#include <string>

namespace sqlcompiler {

namespace {

const char* JoinTypeName(JoinType t) {
    switch (t) {
        case JoinType::INNER: return "INNER";
        case JoinType::LEFT:  return "LEFT";
        case JoinType::RIGHT: return "RIGHT";
        case JoinType::FULL_OUTER: return "FULL_OUTER";
        case JoinType::CROSS: return "CROSS";
        // U3-3：子查询去关联产生的半连接（EXISTS/IN/ANY）与反连接（NOT EXISTS/NOT IN）。
        case JoinType::SEMI:  return "SEMI";
        case JoinType::ANTI:  return "ANTI";
    }
    return "?";
}

std::string Indent(int depth) {
    return std::string(static_cast<size_t>(depth) * 2, ' ');
}

// ---- item #3 / #2: 表达式 inline 流式写出 ----
//
// 之前 ToString 路径走 n.columns[i]->ToString() 会分配中间 std::string；
// 当 N 列 / M 表达式时累计 O(N+M) 次分配。这里把每种 Expr 的输出直接
// 写到 ostream 上，避免中间 std::string 缓冲。NodeBodyToString 与
// JSON/SExpr 序列化共享同一函数（消除重复实现）。
//
// 注意：本函数输出格式必须与对应 Expr::ToString 完全等价 —— EXPLAIN
// 等依赖 ToString 的快照由此函数承担，回归测试会覆盖到。

void WriteExpr(std::ostream& oss, const Expr* e);

const char* LiteralTypeName(LiteralType t) {
    switch (t) {
        case LiteralType::INTEGER:    return "INT";
        case LiteralType::FLOAT:      return "FLOAT";
        case LiteralType::STRING:     return "STRING";
        case LiteralType::NULL_VALUE: return "NULL";
        case LiteralType::BOOLEAN:    return "BOOL";
        case LiteralType::DATE:       return "DATE";
        case LiteralType::TIMESTAMP:  return "TIMESTAMP";
        case LiteralType::TIME:       return "TIME";
        case LiteralType::JSON:       return "JSON";
    }
    return "?";
}

const char* BinaryOpName(BinaryOperator op) {
    switch (op) {
        case BinaryOperator::ADD:            return "+";
        case BinaryOperator::SUB:            return "-";
        case BinaryOperator::MUL:            return "*";
        case BinaryOperator::DIV:            return "/";
        case BinaryOperator::MOD:            return "%";
        case BinaryOperator::EQUAL:          return "=";
        case BinaryOperator::NOT_EQUAL:      return "<>";
        case BinaryOperator::LESS:           return "<";
        case BinaryOperator::LESS_EQUAL:     return "<=";
        case BinaryOperator::GREATER:        return ">";
        case BinaryOperator::GREATER_EQUAL:  return ">=";
        case BinaryOperator::AND:            return "AND";
        case BinaryOperator::OR:             return "OR";
        case BinaryOperator::CONCAT:         return "||";
        case BinaryOperator::LIKE:           return "LIKE";
        case BinaryOperator::IN_LIST:        return "IN";
        case BinaryOperator::BETWEEN:        return "BETWEEN";
        case BinaryOperator::IS_NULL:        return "IS NULL";
        case BinaryOperator::IS_NOT_NULL:    return "IS NOT NULL";
        case BinaryOperator::IS_TRUE:        return "IS TRUE";
        case BinaryOperator::IS_FALSE:       return "IS FALSE";
        case BinaryOperator::IS_NOT_TRUE:    return "IS NOT TRUE";
        case BinaryOperator::IS_NOT_FALSE:   return "IS NOT FALSE";
        case BinaryOperator::INTERVAL_ADD:   return "+";
        case BinaryOperator::INTERVAL_SUB:   return "-";
    }
    return "?";
}

const char* UnaryOpName(UnaryOperator op) {
    switch (op) {
        case UnaryOperator::NOT:    return "NOT";
        case UnaryOperator::NEGATE: return "-";
    }
    return "?";
}

void WriteExpr(std::ostream& oss, const Expr* e) {
    if (e == nullptr) {
        oss << "?";
        return;
    }
    switch (e->GetType()) {
        case NodeType::LITERAL_EXPR: {
            const auto& n = static_cast<const LiteralExpr&>(*e);
            switch (n.literal_type) {
                case LiteralType::STRING:
                    oss << '\'' << n.value << '\'';
                    return;
                case LiteralType::NULL_VALUE:
                    oss << "NULL";
                    return;
                case LiteralType::DATE:
                    oss << "DATE '" << n.value << '\'';
                    return;
                case LiteralType::TIMESTAMP:
                    oss << "TIMESTAMP '" << n.value << '\'';
                    return;
                default:
                    oss << n.value;
                    return;
            }
        }
        case NodeType::COLUMN_REF_EXPR: {
            const auto& n = static_cast<const ColumnRefExpr&>(*e);
            if (!n.table_name.empty()) oss << n.table_name << '.';
            oss << n.column_name;
            return;
        }
        case NodeType::BINARY_EXPR: {
            const auto& n = static_cast<const BinaryExpr&>(*e);
            oss << '(';
            WriteExpr(oss, n.left.get());
            oss << ' ' << BinaryOpName(n.op) << ' ';
            WriteExpr(oss, n.right.get());
            oss << ')';
            return;
        }
        case NodeType::UNARY_EXPR: {
            const auto& n = static_cast<const UnaryExpr&>(*e);
            if (n.op == UnaryOperator::NOT) {
                oss << "NOT (";
                WriteExpr(oss, n.operand.get());
                oss << ')';
            } else {
                oss << '-';
                WriteExpr(oss, n.operand.get());
            }
            return;
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            const auto& n = static_cast<const FunctionCallExpr&>(*e);
            oss << n.function_name << '(';
            if (n.is_distinct) oss << "DISTINCT ";
            if (n.function_name == "*" || n.arguments.empty()) {
                if (n.function_name == "*") oss << '*';
            } else {
                for (size_t i = 0; i < n.arguments.size(); ++i) {
                    if (i) oss << ", ";
                    WriteExpr(oss, n.arguments[i].get());
                }
            }
            oss << ')';
            if (n.filter_expr) {
                oss << " FILTER (WHERE ";
                WriteExpr(oss, n.filter_expr.get());
                oss << ')';
            }
            if (!n.within_group_order_by.empty()) {
                oss << " WITHIN GROUP (ORDER BY ";
                for (size_t i = 0; i < n.within_group_order_by.size(); ++i) {
                    if (i) oss << ", ";
                    const auto& ob = n.within_group_order_by[i];
                    WriteExpr(oss, ob.expr.get());
                    if (!ob.ascending) oss << " DESC";
                }
                oss << ')';
            }
            return;
        }
        case NodeType::CASE_EXPR: {
            const auto& n = static_cast<const CaseExprNode&>(*e);
            oss << "CASE";
            if (n.subject) {
                oss << ' ';
                WriteExpr(oss, n.subject.get());
            }
            for (const auto& w : n.whens) {
                oss << " WHEN ";
                WriteExpr(oss, w.when_expr.get());
                oss << " THEN ";
                WriteExpr(oss, w.then_expr.get());
            }
            if (n.else_expr) {
                oss << " ELSE ";
                WriteExpr(oss, n.else_expr.get());
            }
            oss << " END";
            return;
        }
        case NodeType::CAST_EXPR: {
            const auto& n = static_cast<const CastExprNode&>(*e);
            oss << "CAST(";
            WriteExpr(oss, n.expr.get());
            oss << " AS " << n.target_type;
            if (n.char_length >= 0) {
                oss << '(' << n.char_length;
                if (n.numeric_scale >= 0) oss << ", " << n.numeric_scale;
                oss << ')';
            }
            oss << ')';
            return;
        }
        case NodeType::WINDOW_FUNC_EXPR: {
            const auto& n = static_cast<const WindowFuncNode&>(*e);
            oss << n.function_name << '(';
            for (size_t i = 0; i < n.arguments.size(); ++i) {
                if (i) oss << ", ";
                WriteExpr(oss, n.arguments[i].get());
            }
            oss << ") " << (n.ignore_nulls ? "IGNORE NULLS " : "RESPECT NULLS ")
                << "OVER ";
            if (!n.window_name.empty()) {
                oss << n.window_name;
            } else {
                oss << '(';
                if (!n.spec.partition_by.empty()) {
                    oss << "PARTITION BY ";
                    for (size_t i = 0; i < n.spec.partition_by.size(); ++i) {
                        if (i) oss << ", ";
                        WriteExpr(oss, n.spec.partition_by[i].get());
                    }
                    oss << ' ';
                }
                if (!n.spec.order_by.empty()) {
                    oss << "ORDER BY ";
                    for (size_t i = 0; i < n.spec.order_by.size(); ++i) {
                        if (i) oss << ", ";
                        WriteExpr(oss, n.spec.order_by[i].expr.get());
                        oss << (n.spec.order_by[i].ascending ? " ASC" : " DESC");
                    }
                }
                oss << ')';
            }
            return;
        }
        case NodeType::SUBQUERY_EXPR: {
            const auto& n = static_cast<const SubqueryExprNode&>(*e);
            switch (n.kind) {
                case SubqueryType::EXISTS:
                    oss << "EXISTS(";
                    if (n.subquery) n.subquery->ToString();  // rare; keep fallback
                    oss << ')';
                    return;
                case SubqueryType::IN:
                    oss << '(';
                    WriteExpr(oss, n.outer_expr.get());
                    oss << " IN (";
                    if (n.subquery) n.subquery->ToString();
                    oss << "))";
                    return;
                case SubqueryType::ANY:
                    oss << '(';
                    WriteExpr(oss, n.outer_expr.get());
                    oss << ' ' << n.comparison_op << " ANY (";
                    if (n.subquery) n.subquery->ToString();
                    oss << "))";
                    return;
                default:
                    oss << '(';
                    if (n.subquery) n.subquery->ToString();
                    oss << ')';
                    return;
            }
        }
        case NodeType::UPSERT_VALUES_REF_EXPR: {
            const auto& n = static_cast<const UpsertValuesRefExpr&>(*e);
            oss << "VALUES(" << n.column_name << ')';
            return;
        }
        case NodeType::LIKE_EXPR: {
            const auto& n = static_cast<const LikeExprNode&>(*e);
            oss << '(';
            WriteExpr(oss, n.operand.get());
            oss << ' ';
            switch (n.kind) {
                case LikeExprNode::Kind::LIKE:        oss << "LIKE";        break;
                case LikeExprNode::Kind::ILIKE:       oss << "ILIKE";       break;
                case LikeExprNode::Kind::REGEXP:      oss << "REGEXP";      break;
                case LikeExprNode::Kind::RLIKE:       oss << "RLIKE";       break;
                case LikeExprNode::Kind::SIMILAR_TO:  oss << "SIMILAR TO";  break;
            }
            oss << ' ';
            WriteExpr(oss, n.pattern.get());
            if (n.has_escape) oss << " ESCAPE '" << n.escape_char << '\'';
            oss << ')';
            return;
        }
        case NodeType::EXTRACT_EXPR: {
            const auto& n = static_cast<const ExtractExprNode&>(*e);
            oss << "EXTRACT(";
            // field is IntervalUnit; reuse name map from AST.cpp-equivalent
            switch (n.field) {
                case 0: oss << "YEAR"; break;
                case 1: oss << "MONTH"; break;
                case 2: oss << "DAY"; break;
                case 3: oss << "HOUR"; break;
                case 4: oss << "MINUTE"; break;
                case 5: oss << "SECOND"; break;
                default: oss << "?"; break;
            }
            oss << " FROM ";
            WriteExpr(oss, n.source.get());
            oss << ')';
            return;
        }
        case NodeType::INTERVAL_EXPR: {
            const auto& n = static_cast<const IntervalExprNode&>(*e);
            oss << "INTERVAL " << n.count << ' ';
            switch (n.unit) {
                case 0: oss << "YEAR"; break;
                case 1: oss << "MONTH"; break;
                case 2: oss << "DAY"; break;
                case 3: oss << "HOUR"; break;
                case 4: oss << "MINUTE"; break;
                case 5: oss << "SECOND"; break;
                default: oss << "?"; break;
            }
            return;
        }
        case NodeType::NEXTVAL_EXPR: {
            const auto& n = static_cast<const NextvalExpr&>(*e);
            oss << "NEXTVAL FOR " << n.sequence_name;
            return;
        }
        case NodeType::DEFAULT_EXPR: {
            const auto& n = static_cast<const DefaultExprNode&>(*e);
            if (n.column_name.empty()) {
                oss << "DEFAULT";
            } else {
                oss << "DEFAULT(" << n.column_name << ')';
            }
            return;
        }
        default:
            // 未知 / 未覆盖：回退到 ToString。
            oss << e->ToString();
            return;
    }
}

std::string NodeBodyToString(const PlanNode& node, int depth) {
    std::ostringstream oss;
    oss << Indent(depth);
    switch (node.GetType()) {
        case PlanNodeType::SEQ_SCAN: {
            auto& n = static_cast<const SeqScanNode&>(node);
            oss << "SeqScan(" << n.table_name;
            if (n.predicate) {
                oss << ", [";
                WriteExpr(oss, n.predicate.get());
                oss << "]";
            }
            oss << ")";
            break;
        }
        case PlanNodeType::FILTER: {
            auto& n = static_cast<const FilterNode&>(node);
            oss << "Filter(";
            WriteExpr(oss, n.predicate.get());
            oss << ")";
            break;
        }
        case PlanNodeType::PROJECT: {
            auto& n = static_cast<const ProjectNode&>(node);
            oss << "Project(";
            for (size_t i = 0; i < n.columns.size(); ++i) {
                if (i) oss << ", ";
                WriteExpr(oss, n.columns[i].get());
            }
            oss << ")";
            break;
        }
        case PlanNodeType::JOIN: {
            auto& n = static_cast<const JoinNode&>(node);
            oss << "Join(" << JoinTypeName(n.join_type) << ", ";
            WriteExpr(oss, n.condition.get());
            oss << ")";
            break;
        }
        case PlanNodeType::SORT: {
            auto& n = static_cast<const SortNode&>(node);
            oss << "Sort(";
            for (size_t i = 0; i < n.order_items.size(); ++i) {
                if (i) oss << ", ";
                WriteExpr(oss, n.order_items[i].expr.get());
                oss << (n.order_items[i].ascending ? " ASC" : " DESC");
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
                WriteExpr(oss, n.group_by_exprs[i].get());
            }
            oss << "], AGG [";
            for (size_t i = 0; i < n.aggregate_exprs.size(); ++i) {
                if (i) oss << ", ";
                WriteExpr(oss, n.aggregate_exprs[i].get());
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
            oss << "IndexScan(" << n.table_name << " using " << n.index_name;
            // Phase 5：打印区间边界（含复合索引多列前缀），供 EXPLAIN / 收敛验证。
            const auto print_key = [&](const std::vector<Value>& key) {
                for (size_t i = 0; i < key.size(); ++i) {
                    if (i) oss << ",";
                    oss << key[i].ToString();
                }
            };
            if (!n.low_key.empty() || !n.high_key.empty()) {
                oss << " [";
                if (!n.low_key.empty()) {
                    oss << "low=";
                    print_key(n.low_key);
                    if (!n.low_inclusive) oss << " excl";
                }
                if (!n.high_key.empty()) {
                    if (!n.low_key.empty()) oss << " ";
                    oss << "high=";
                    print_key(n.high_key);
                    if (!n.high_inclusive) oss << " excl";
                }
                oss << "]";
            }
            oss << ")";
            break;
        }
        // U3-2 扫描内预聚合（继承 AggregateNode，输出形状一致）：打印 GROUP BY 与
        // 聚合表达式，供 EXPLAIN 断言「聚合已下推到扫描层」。
        case PlanNodeType::PRE_AGG_SCAN: {
            auto& n = static_cast<const PreAggScanNode&>(node);
            oss << "PreAggScan(";
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
                WriteExpr(oss, n.select_list[i].get());
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
static void JsonWriteString(std::ostream& oss, const std::string& s) {
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

// 把 Expr 直接写成 JSON string value（"..." 含转义）。
// Expr 输出由我们控制：不会产生 \" \\ \n 等需要转义的字符，但仍走
// JsonWriteString 走一遍 escape 兜底，保证健壮性。
static void WriteJsonExprValue(std::ostream& oss, const Expr* e) {
    oss << '"';
    if (e != nullptr) {
        std::ostringstream inner;
        WriteExpr(inner, e);
        JsonWriteString(oss, inner.str());
    }
    oss << '"';
}

// item #2：直接把节点 fields 写到 oss，不再构造中间
// vector<pair<string,string>>；每条 Expr 也走 WriteJsonExprValue inline，
// 省掉 N+M 次 std::string 分配。
static void WriteJsonNodeFields(std::ostream& oss,
                                const std::string& indent,
                                const PlanNode& node,
                                bool& first) {
    auto write_kv = [&](const char* k, const std::string& v) {
        if (!first) oss << ",";
        first = false;
        oss << "\n" << indent << "  ";
        JsonWriteString(oss, k);
        oss << ": ";
        JsonWriteString(oss, v);
    };
    auto write_kv_expr = [&](const char* k, const Expr* e) {
        if (!first) oss << ",";
        first = false;
        oss << "\n" << indent << "  ";
        JsonWriteString(oss, k);
        oss << ": ";
        WriteJsonExprValue(oss, e);
    };
    switch (node.GetType()) {
        case PlanNodeType::SEQ_SCAN: {
            auto& n = static_cast<const SeqScanNode&>(node);
            write_kv("table", n.table_name);
            if (!n.table_alias.empty()) write_kv("alias", n.table_alias);
            if (n.predicate) write_kv_expr("predicate", n.predicate.get());
            break;
        }
        case PlanNodeType::INDEX_SCAN: {
            auto& n = static_cast<const IndexScanNode&>(node);
            write_kv("table", n.table_name);
            write_kv("index", n.index_name);
            if (!n.table_alias.empty()) write_kv("alias", n.table_alias);
            if (n.residual_predicate) {
                write_kv_expr("residual", n.residual_predicate.get());
            }
            break;
        }
        case PlanNodeType::FILTER: {
            auto& n = static_cast<const FilterNode&>(node);
            if (n.predicate) write_kv_expr("predicate", n.predicate.get());
            break;
        }
        case PlanNodeType::PROJECT: {
            auto& n = static_cast<const ProjectNode&>(node);
            for (size_t i = 0; i < n.columns.size(); ++i) {
                std::string col;
                if (n.columns[i]) {
                    std::ostringstream inner;
                    WriteExpr(inner, n.columns[i].get());
                    col = inner.str();
                } else {
                    col = "?";
                }
                if (i < n.aliases.size() && !n.aliases[i].empty()) {
                    col += " AS " + n.aliases[i];
                }
                write_kv(("col_" + std::to_string(i)).c_str(), col);
            }
            if (n.is_distinct) write_kv("distinct", "true");
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
            write_kv("type", jt);
            if (n.condition) write_kv_expr("condition", n.condition.get());
            break;
        }
        case PlanNodeType::SORT: {
            auto& n = static_cast<const SortNode&>(node);
            for (size_t i = 0; i < n.order_items.size(); ++i) {
                const auto& it = n.order_items[i];
                std::string s;
                if (it.expr) {
                    std::ostringstream inner;
                    WriteExpr(inner, it.expr.get());
                    s = inner.str();
                } else {
                    s = "?";
                }
                s += it.ascending ? " ASC" : " DESC";
                write_kv(("key_" + std::to_string(i)).c_str(), s);
            }
            break;
        }
        case PlanNodeType::LIMIT: {
            auto& n = static_cast<const LimitNode&>(node);
            write_kv("count", std::to_string(n.limit_count));
            break;
        }
        case PlanNodeType::AGGREGATE: {
            auto& n = static_cast<const AggregateNode&>(node);
            for (size_t i = 0; i < n.group_by_exprs.size(); ++i) {
                std::string g;
                if (n.group_by_exprs[i]) {
                    std::ostringstream inner;
                    WriteExpr(inner, n.group_by_exprs[i].get());
                    g = inner.str();
                } else {
                    g = "?";
                }
                write_kv(("group_" + std::to_string(i)).c_str(), g);
            }
            for (size_t i = 0; i < n.aggregate_exprs.size(); ++i) {
                std::string a;
                if (n.aggregate_exprs[i]) {
                    std::ostringstream inner;
                    WriteExpr(inner, n.aggregate_exprs[i].get());
                    a = inner.str();
                } else {
                    a = "?";
                }
                if (i < n.aliases.size() && !n.aliases[i].empty()) {
                    a += " AS " + n.aliases[i];
                }
                write_kv(("agg_" + std::to_string(i)).c_str(), a);
            }
            break;
        }
        case PlanNodeType::INSERT: {
            auto& n = static_cast<const InsertNode&>(node);
            write_kv("table", n.table_name);
            write_kv("rows", std::to_string(n.values_list.size()));
            break;
        }
        case PlanNodeType::UPDATE: {
            auto& n = static_cast<const UpdateNode&>(node);
            write_kv("table", n.table_name);
            break;
        }
        case PlanNodeType::DELETE: {
            auto& n = static_cast<const DeleteNode&>(node);
            write_kv("table", n.table_name);
            break;
        }
        case PlanNodeType::CREATE_TABLE: {
            auto& n = static_cast<const CreateTableNode&>(node);
            write_kv("table", n.table_name);
            write_kv("columns", std::to_string(n.columns.size()));
            break;
        }
        case PlanNodeType::DROP_TABLE: {
            auto& n = static_cast<const DropTableNode&>(node);
            write_kv("table", n.table_name);
            break;
        }
        case PlanNodeType::CREATE_INDEX: {
            auto& n = static_cast<const CreateIndexNode&>(node);
            write_kv("index", n.index_name);
            write_kv("table", n.table_name);
            break;
        }
        case PlanNodeType::DROP_INDEX: {
            auto& n = static_cast<const DropIndexNode&>(node);
            write_kv("index", n.index_name);
            break;
        }
        case PlanNodeType::TRUNCATE_TABLE: {
            auto& n = static_cast<const TruncateTableNode&>(node);
            write_kv("table", n.table_name);
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
            write_kv("op", k);
            break;
        }
        case PlanNodeType::WINDOW: {
            auto& n = static_cast<const WindowNode&>(node);
            for (size_t i = 0; i < n.select_list.size(); ++i) {
                std::string col;
                if (n.select_list[i]) {
                    std::ostringstream inner;
                    WriteExpr(inner, n.select_list[i].get());
                    col = inner.str();
                } else {
                    col = "?";
                }
                write_kv(("col_" + std::to_string(i)).c_str(), col);
            }
            break;
        }
        case PlanNodeType::VALUES: {
            auto& n = static_cast<const ValuesNode&>(node);
            write_kv("alias", n.derived_alias);
            write_kv("rows", std::to_string(n.rows.size()));
            break;
        }
        case PlanNodeType::APPLY: {
            auto& n = static_cast<const ApplyNode&>(node);
            write_kv("type", n.is_left_outer ? "LEFT_OUTER" : "CROSS");
            break;
        }
        case PlanNodeType::EXPLAIN: {
            auto& n = static_cast<const ExplainNode&>(node);
            write_kv("analyze", n.analyze ? "true" : "false");
            write_kv("format", n.format);
            break;
        }
        default:
            break;
    }
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
        // U3-2 扫描内预聚合（继承 AggregateNode）：JSON / S-expr 序列化按原名标注。
        case PlanNodeType::PRE_AGG_SCAN: return "PreAggScan";
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
        case PlanNodeType::SET_ISOLATION: return "SetIsolation";
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
    bool first = true;  // type 字段已写，fields 从 "," 开始
    WriteJsonNodeFields(oss, indent, node, first);
    if (!node.children.empty()) {
        oss << ",\n" << indent << "  \"children\": [";
        bool first_child = true;
        for (const auto& c : node.children) {
            if (!c) continue;
            if (!first_child) oss << ",";
            oss << "\n";
            oss << SerializeNodeToJson(*c, depth + 1);
            first_child = false;
        }
        if (!first_child) oss << "\n" << indent << "  ";
        oss << "]";
    }
    oss << "\n" << indent << "}";
    return oss.str();
}

// S-expr 序列化：(Name :key "val" ... child1 child2 ...)
// child 是另一个 (Name ...) 表达式；无 children 时输出 (Name :key "val")。
// item #2：直接写到 oss，不再走 NodeJsonFields + vector<pair>。
static void WriteSExprNodeFields(std::ostream& oss, const PlanNode& node);
static void WriteSExprFieldKV(std::ostream& oss,
                              const std::string& key,
                              const std::string& val) {
    oss << " :" << key << " ";
    JsonWriteString(oss, val);  // 借用 JSON 转义：同样处理 \ " \n 等
}
static void WriteSExprFieldExpr(std::ostream& oss,
                                const std::string& key,
                                const Expr* e) {
    oss << " :" << key << " ";
    WriteJsonExprValue(oss, e);
}
static void WriteSExprNodeFields(std::ostream& oss, const PlanNode& node) {
    auto kv = [&](const char* k, const std::string& v) {
        WriteSExprFieldKV(oss, k, v);
    };
    auto kve = [&](const char* k, const Expr* e) {
        WriteSExprFieldExpr(oss, k, e);
    };
    auto kv_col = [&](size_t i, const Expr* e) {
        std::string col;
        if (e) {
            std::ostringstream inner;
            WriteExpr(inner, e);
            col = inner.str();
        } else {
            col = "?";
        }
        kv(("col_" + std::to_string(i)).c_str(), col);
    };
    switch (node.GetType()) {
        case PlanNodeType::SEQ_SCAN: {
            auto& n = static_cast<const SeqScanNode&>(node);
            kv("table", n.table_name);
            if (!n.table_alias.empty()) kv("alias", n.table_alias);
            if (n.predicate) kve("predicate", n.predicate.get());
            break;
        }
        case PlanNodeType::INDEX_SCAN: {
            auto& n = static_cast<const IndexScanNode&>(node);
            kv("table", n.table_name);
            kv("index", n.index_name);
            if (!n.table_alias.empty()) kv("alias", n.table_alias);
            if (n.residual_predicate) kve("residual", n.residual_predicate.get());
            break;
        }
        case PlanNodeType::FILTER: {
            auto& n = static_cast<const FilterNode&>(node);
            if (n.predicate) kve("predicate", n.predicate.get());
            break;
        }
        case PlanNodeType::PROJECT: {
            auto& n = static_cast<const ProjectNode&>(node);
            for (size_t i = 0; i < n.columns.size(); ++i) {
                std::string col;
                if (n.columns[i]) {
                    std::ostringstream inner;
                    WriteExpr(inner, n.columns[i].get());
                    col = inner.str();
                } else {
                    col = "?";
                }
                if (i < n.aliases.size() && !n.aliases[i].empty()) {
                    col += " AS " + n.aliases[i];
                }
                kv(("col_" + std::to_string(i)).c_str(), col);
            }
            if (n.is_distinct) kv("distinct", "true");
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
            kv("type", jt);
            if (n.condition) kve("condition", n.condition.get());
            break;
        }
        case PlanNodeType::SORT: {
            auto& n = static_cast<const SortNode&>(node);
            for (size_t i = 0; i < n.order_items.size(); ++i) {
                const auto& it = n.order_items[i];
                std::string s;
                if (it.expr) {
                    std::ostringstream inner;
                    WriteExpr(inner, it.expr.get());
                    s = inner.str();
                } else {
                    s = "?";
                }
                s += it.ascending ? " ASC" : " DESC";
                kv(("key_" + std::to_string(i)).c_str(), s);
            }
            break;
        }
        case PlanNodeType::LIMIT: {
            auto& n = static_cast<const LimitNode&>(node);
            kv("count", std::to_string(n.limit_count));
            break;
        }
        case PlanNodeType::AGGREGATE: {
            auto& n = static_cast<const AggregateNode&>(node);
            for (size_t i = 0; i < n.group_by_exprs.size(); ++i) {
                std::string g;
                if (n.group_by_exprs[i]) {
                    std::ostringstream inner;
                    WriteExpr(inner, n.group_by_exprs[i].get());
                    g = inner.str();
                } else {
                    g = "?";
                }
                kv(("group_" + std::to_string(i)).c_str(), g);
            }
            for (size_t i = 0; i < n.aggregate_exprs.size(); ++i) {
                std::string a;
                if (n.aggregate_exprs[i]) {
                    std::ostringstream inner;
                    WriteExpr(inner, n.aggregate_exprs[i].get());
                    a = inner.str();
                } else {
                    a = "?";
                }
                if (i < n.aliases.size() && !n.aliases[i].empty()) {
                    a += " AS " + n.aliases[i];
                }
                kv(("agg_" + std::to_string(i)).c_str(), a);
            }
            break;
        }
        case PlanNodeType::INSERT: {
            auto& n = static_cast<const InsertNode&>(node);
            kv("table", n.table_name);
            kv("rows", std::to_string(n.values_list.size()));
            break;
        }
        case PlanNodeType::UPDATE: {
            auto& n = static_cast<const UpdateNode&>(node);
            kv("table", n.table_name);
            break;
        }
        case PlanNodeType::DELETE: {
            auto& n = static_cast<const DeleteNode&>(node);
            kv("table", n.table_name);
            break;
        }
        case PlanNodeType::CREATE_TABLE: {
            auto& n = static_cast<const CreateTableNode&>(node);
            kv("table", n.table_name);
            kv("columns", std::to_string(n.columns.size()));
            break;
        }
        case PlanNodeType::DROP_TABLE: {
            auto& n = static_cast<const DropTableNode&>(node);
            kv("table", n.table_name);
            break;
        }
        case PlanNodeType::CREATE_INDEX: {
            auto& n = static_cast<const CreateIndexNode&>(node);
            kv("index", n.index_name);
            kv("table", n.table_name);
            break;
        }
        case PlanNodeType::DROP_INDEX: {
            auto& n = static_cast<const DropIndexNode&>(node);
            kv("index", n.index_name);
            break;
        }
        case PlanNodeType::TRUNCATE_TABLE: {
            auto& n = static_cast<const TruncateTableNode&>(node);
            kv("table", n.table_name);
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
            kv("op", k);
            break;
        }
        case PlanNodeType::WINDOW: {
            auto& n = static_cast<const WindowNode&>(node);
            for (size_t i = 0; i < n.select_list.size(); ++i) {
                kv_col(i, n.select_list[i].get());
            }
            break;
        }
        case PlanNodeType::VALUES: {
            auto& n = static_cast<const ValuesNode&>(node);
            kv("alias", n.derived_alias);
            kv("rows", std::to_string(n.rows.size()));
            break;
        }
        case PlanNodeType::APPLY: {
            auto& n = static_cast<const ApplyNode&>(node);
            kv("type", n.is_left_outer ? "LEFT_OUTER" : "CROSS");
            break;
        }
        case PlanNodeType::EXPLAIN: {
            auto& n = static_cast<const ExplainNode&>(node);
            kv("analyze", n.analyze ? "true" : "false");
            kv("format", n.format);
            break;
        }
        default:
            break;
    }
}

static std::string SerializeNodeToSExpr(const PlanNode& node) {
    std::ostringstream oss;
    oss << "(" << PlanNodeTypeName(node.GetType());
    WriteSExprNodeFields(oss, node);
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

// ============ PreAggScanNode（U3-2 扫描内预聚合）============

PreAggScanNode::PreAggScanNode(std::vector<ExprPtr> group_by_exprs,
                               std::vector<ExprPtr> aggregate_exprs,
                               std::vector<std::string> aliases)
    : AggregateNode(std::move(group_by_exprs), std::move(aggregate_exprs),
                    std::move(aliases)) {
}

PlanNodeType PreAggScanNode::GetType() const {
    return PlanNodeType::PRE_AGG_SCAN;
}

std::string PreAggScanNode::ToString() const {
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
    // item #1: 一次性 reserve + ostringstream，避免 N² 链式 += 反复 realloc。
    std::ostringstream oss;
    oss << "Call(" << procedure_name << '(';
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (i) oss << ", ";
        WriteExpr(oss, arguments[i].get());
    }
    oss << "))\n";
    return oss.str();
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

// SET TRANSACTION ISOLATION LEVEL ...：只写入会话默认隔离级别，无结果集。
SetIsolationNode::SetIsolationNode(IsolationLevel level) : isolation_level(level) {}
PlanNodeType SetIsolationNode::GetType() const { return PlanNodeType::SET_ISOLATION; }
std::string SetIsolationNode::ToString() const {
    return "SetIsolation()\n";
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