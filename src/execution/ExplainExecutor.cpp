// ============ EXPLAIN 算子：打印计划树 / 收集并打印执行统计 ============
//
// 输出格式约定（与任务文档保持一致）：
//   - 顶层节点无缩进；每深一层缩进 +2 空格。
//   - 每个节点单行呈现：<NodeName>(<body>)，body 由 PlanNode::ToString() 决定。
//   - 子节点按 children 顺序依次追加。
//
// 支持三种格式，由 ExplainNode::format 决定：
//   - TEXT  ：原行为，缩进式文本树（默认，向后兼容）
//   - JSON  ：单行嵌套 JSON 对象，type + fields + children 三段
//   - SEXPR ：Lisp 风格 (NodeName :key "val" ... child ...)
//
// EXPLAIN ANALYZE 行为（69_explain_analyze）：
//   - 解析阶段把 analyze=true 子句翻译到 ExplainNode.analyze。
//   - Next() 检测到 analyze 时调用 RenderAnalyze：
//       1) 创建 ExecutionEngine 实例，wrap_timing=true 重新构建子树；
//          通过 TimingProxyExecutor 的注册表把 PlanNode* → Proxy 绑定起来。
//       2) Init 整棵子树，循环驱动 Next 至 EOF。
//          中途失败（如 1/0）会把异常吞掉并以「部分统计」的形式记录。
//       3) 按 format 选择序列化方式，每节点追加
//             (rows=N time=NNN.NNNms)
//          （TEXT 后缀、JSON 的 "rows"/"time_ms" 字段、SEXPR 的 :rows/:time_ms
//          键值）。
//
// 我们把整段文本打包成单列（"plan"）单行的结果集，方便上层 PrintResult
// 直接复用表格输出，无需为 EXPLAIN 单独写一套 I/O。

#include "execution/ExplainExecutor.h"

#include <cstdio>
#include <sstream>
#include <unordered_map>

#include "common/Error.h"
#include "execution/ExecutionEngine.h"
#include "execution/TimingProxyExecutor.h"

namespace sqlcompiler {

namespace {

// 数字格式化辅助：rows 显示整数，time 显示小数（精度 3 位）。
std::string FmtInt(int64_t v) { return std::to_string(v); }

std::string FmtMs(double ms) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(3);
    os << ms;
    return os.str();
}

// 把 PlanNode::ToString 拆成多行。绝大多数节点 ToString 返回单行；少数
// （Project / Sort / SetOp 等）可能带换行表示子结构。我们对第一行追加
// stats 注释，其余行原样保留。
std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) {
            out.emplace_back(s.substr(pos));
            break;
        }
        out.emplace_back(s.substr(pos, nl - pos));
        pos = nl + 1;
    }
    if (out.empty()) out.emplace_back("");
    return out;
}

// TEXT 格式递归渲染：每节点自身首行追加 (rows=N time=Xms) 注释，children
// 按 2 空格缩进续接。
std::string RenderAnalyzeText(
    const PlanNodePtr& node,
    const std::unordered_map<const PlanNode*, TimingProxyExecutor*>& proxy_map,
    int depth) {
    if (!node) return std::string("(empty plan)");

    // 注意：PlanNode::ToString() 内部已经递归打印了所有 children（带
    // depth+1 缩进）。我们这里只取首行（节点自身描述），子节点交给递归
    // 处理 —— 避免重复打印。
    auto lines = SplitLines(node->ToString());
    std::string out;
    out.reserve(256);
    std::string indent(depth * 2, ' ');
    out += indent + lines[0];
    auto it = proxy_map.find(node.get());
    if (it != proxy_map.end() && it->second) {
        const auto* p = it->second;
        out += "  (rows=" + FmtInt(p->rows_produced()) +
               " time=" + FmtMs(p->total_ms()) + "ms)";
    }
    out += "\n";
    // children 递归：按 plan->children 顺序
    for (auto& ch : node->children) {
        out += RenderAnalyzeText(ch, proxy_map, depth + 1);
    }
    return out;
}

std::string EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void AppendJsonField(std::string& out, const std::string& key,
                     const std::string& value) {
    if (!out.empty() && out.back() != '{') out += ",";
    out += "\"" + key + "\": " + value;
}

const char* PlanNodeTypeJsonName(PlanNodeType t) {
    switch (t) {
        case PlanNodeType::SEQ_SCAN:        return "SeqScan";
        case PlanNodeType::INDEX_SCAN:      return "IndexScan";
        case PlanNodeType::FILTER:          return "Filter";
        case PlanNodeType::PROJECT:         return "Project";
        case PlanNodeType::JOIN:            return "Join";
        case PlanNodeType::SORT:            return "Sort";
        case PlanNodeType::LIMIT:           return "Limit";
        case PlanNodeType::AGGREGATE:       return "Aggregate";
        case PlanNodeType::INSERT:          return "Insert";
        case PlanNodeType::UPDATE:          return "Update";
        case PlanNodeType::UPDATE_FROM:     return "UpdateFrom";
        case PlanNodeType::DELETE:          return "Delete";
        case PlanNodeType::CREATE_TABLE:    return "CreateTable";
        case PlanNodeType::DROP_TABLE:      return "DropTable";
        case PlanNodeType::TRUNCATE_TABLE:  return "TruncateTable";
        case PlanNodeType::CREATE_INDEX:    return "CreateIndex";
        case PlanNodeType::DROP_INDEX:      return "DropIndex";
        case PlanNodeType::ALTER_TABLE:     return "AlterTable";
        case PlanNodeType::SET_OP:          return "SetOp";
        case PlanNodeType::WINDOW:          return "Window";
        case PlanNodeType::SUBQUERY:        return "Subquery";
        case PlanNodeType::CTE_BIND:        return "CteBind";
        case PlanNodeType::CTE_DEFINE:      return "CteDefine";
        case PlanNodeType::NO_OP:           return "NoOp";
        case PlanNodeType::CREATE_VIEW:     return "CreateView";
        case PlanNodeType::CREATE_TRIGGER:  return "CreateTrigger";
        case PlanNodeType::CREATE_FUNCTION: return "CreateFunction";
        case PlanNodeType::CREATE_PROCEDURE:return "CreateProcedure";
        case PlanNodeType::CALL:            return "Call";
        case PlanNodeType::VIEW_DEFINE:     return "ViewDefine";
        case PlanNodeType::UPSERT:          return "Upsert";
        case PlanNodeType::BEGIN_TXN:       return "BeginTxn";
        case PlanNodeType::COMMIT_TXN:      return "CommitTxn";
        case PlanNodeType::ROLLBACK_TXN:    return "RollbackTxn";
        case PlanNodeType::SAVEPOINT:       return "Savepoint";
        case PlanNodeType::ROLLBACK_TO_SP:  return "RollbackToSp";
        case PlanNodeType::RELEASE_SP:      return "ReleaseSp";
        case PlanNodeType::EXPLAIN:         return "Explain";
        case PlanNodeType::SHOW:            return "Show";
        case PlanNodeType::CREATE_SCHEMA:   return "CreateSchema";
        case PlanNodeType::DROP_SCHEMA:     return "DropSchema";
        case PlanNodeType::CREATE_SEQUENCE: return "CreateSequence";
        case PlanNodeType::DROP_SEQUENCE:   return "DropSequence";
        case PlanNodeType::MERGE:           return "Merge";
        case PlanNodeType::VALUES:          return "Values";
        case PlanNodeType::APPLY:           return "Apply";
        case PlanNodeType::CREATE_MATERIALIZED_VIEW:
                                            return "CreateMaterializedView";
        case PlanNodeType::ALTER_MATERIALIZED_VIEW:
                                            return "AlterMaterializedView";
        default:                            return "Unknown";
    }
}

// 把任意 PlanNode 序列化成 JSON 对象内容（不含 children 数组、不含外层
// 大括号），并追加 rows / time_ms（若注册表里有对应 Proxy）。
std::string PlanNodeJsonObject(
    const PlanNodePtr& node,
    const std::unordered_map<const PlanNode*, TimingProxyExecutor*>& proxy_map) {
    std::string body;
    AppendJsonField(body, "type",
                    std::string("\"") + PlanNodeTypeJsonName(node->GetType()) + "\"");

    auto str_field = [&](const std::string& key, const std::string& val) {
        AppendJsonField(body, key, std::string("\"") + EscapeJson(val) + "\"");
    };
    auto expr_field = [&](const std::string& key, const ExprPtr& e) {
        str_field(key, e ? e->ToString() : "");
    };

    switch (node->GetType()) {
        case PlanNodeType::SEQ_SCAN: {
            auto n = std::static_pointer_cast<SeqScanNode>(node);
            str_field("table", n->table_name);
            if (!n->table_alias.empty()) str_field("alias", n->table_alias);
            if (n->predicate) expr_field("predicate", n->predicate);
            break;
        }
        case PlanNodeType::INDEX_SCAN: {
            auto n = std::static_pointer_cast<IndexScanNode>(node);
            str_field("table", n->table_name);
            str_field("index", n->index_name);
            if (!n->table_alias.empty()) str_field("alias", n->table_alias);
            if (n->residual_predicate)
                expr_field("residual", n->residual_predicate);
            break;
        }
        case PlanNodeType::FILTER: {
            auto n = std::static_pointer_cast<FilterNode>(node);
            expr_field("predicate", n->predicate);
            break;
        }
        case PlanNodeType::PROJECT: {
            auto n = std::static_pointer_cast<ProjectNode>(node);
            for (size_t i = 0; i < n->columns.size(); ++i) {
                std::string key = "col_" + std::to_string(i);
                if (n->columns[i]) str_field(key, n->columns[i]->ToString());
                else str_field(key, "");
            }
            for (size_t i = 0; i < n->aliases.size(); ++i) {
                if (!n->aliases[i].empty())
                    str_field("alias_" + std::to_string(i), n->aliases[i]);
            }
            if (n->is_distinct) AppendJsonField(body, "distinct", "true");
            break;
        }
        case PlanNodeType::JOIN: {
            auto n = std::static_pointer_cast<JoinNode>(node);
            const char* jt = "?";
            switch (n->join_type) {
                case JoinType::INNER: jt = "INNER"; break;
                case JoinType::LEFT:  jt = "LEFT";  break;
                case JoinType::RIGHT: jt = "RIGHT"; break;
                case JoinType::FULL_OUTER: jt = "FULL_OUTER"; break;
                case JoinType::CROSS: jt = "CROSS"; break;
            }
            str_field("type_str", jt);
            expr_field("condition", n->condition);
            break;
        }
        case PlanNodeType::SORT: {
            auto n = std::static_pointer_cast<SortNode>(node);
            std::string items;
            for (size_t i = 0; i < n->order_items.size(); ++i) {
                if (i) items += ",";
                items += n->order_items[i].expr
                           ? n->order_items[i].expr->ToString()
                           : "";
                items += (n->order_items[i].ascending ? " ASC" : " DESC");
            }
            str_field("order_items", items);
            break;
        }
        case PlanNodeType::LIMIT: {
            auto n = std::static_pointer_cast<LimitNode>(node);
            AppendJsonField(body, "limit", std::to_string(n->limit_count));
            AppendJsonField(body, "offset", std::to_string(n->offset));
            break;
        }
        case PlanNodeType::AGGREGATE: {
            auto n = std::static_pointer_cast<AggregateNode>(node);
            for (size_t i = 0; i < n->aggregate_exprs.size(); ++i) {
                std::string key = "agg_" + std::to_string(i);
                if (n->aggregate_exprs[i])
                    str_field(key, n->aggregate_exprs[i]->ToString());
            }
            break;
        }
        case PlanNodeType::CREATE_TABLE: {
            auto n = std::static_pointer_cast<CreateTableNode>(node);
            str_field("table", n->table_name);
            break;
        }
        case PlanNodeType::DROP_TABLE: {
            auto n = std::static_pointer_cast<DropTableNode>(node);
            str_field("table", n->table_name);
            break;
        }
        case PlanNodeType::CREATE_INDEX: {
            auto n = std::static_pointer_cast<CreateIndexNode>(node);
            str_field("index", n->index_name);
            str_field("table", n->table_name);
            break;
        }
        case PlanNodeType::DROP_INDEX: {
            auto n = std::static_pointer_cast<DropIndexNode>(node);
            str_field("index", n->index_name);
            break;
        }
        case PlanNodeType::TRUNCATE_TABLE: {
            auto n = std::static_pointer_cast<TruncateTableNode>(node);
            str_field("table", n->table_name);
            break;
        }
        case PlanNodeType::ALTER_TABLE: {
            auto n = std::static_pointer_cast<AlterTableNode>(node);
            str_field("table", n->table_name);
            break;
        }
        case PlanNodeType::INSERT: {
            auto n = std::static_pointer_cast<InsertNode>(node);
            str_field("table", n->table_name);
            break;
        }
        case PlanNodeType::UPDATE: {
            auto n = std::static_pointer_cast<UpdateNode>(node);
            str_field("table", n->table_name);
            break;
        }
        case PlanNodeType::UPDATE_FROM: {
            auto n = std::static_pointer_cast<UpdateFromNode>(node);
            str_field("table", n->table_name);
            break;
        }
        case PlanNodeType::DELETE: {
            auto n = std::static_pointer_cast<DeleteNode>(node);
            str_field("table", n->table_name);
            break;
        }
        case PlanNodeType::MERGE: {
            auto n = std::static_pointer_cast<MergeNode>(node);
            str_field("target", n->target_table);
            break;
        }
        case PlanNodeType::VALUES: {
            auto n = std::static_pointer_cast<ValuesNode>(node);
            str_field("alias", n->derived_alias);
            break;
        }
        case PlanNodeType::CTE_BIND: {
            auto n = std::static_pointer_cast<CteBindNode>(node);
            str_field("cte_name", n->cte_name);
            break;
        }
        case PlanNodeType::CTE_DEFINE: {
            auto n = std::static_pointer_cast<CteDefineNode>(node);
            str_field("cte_name", n->cte_name);
            break;
        }
        case PlanNodeType::NO_OP: {
            auto n = std::static_pointer_cast<NoOpNode>(node);
            str_field("description", n->description);
            break;
        }
        case PlanNodeType::CREATE_VIEW: {
            auto n = std::static_pointer_cast<CreateViewNode>(node);
            str_field("view_name", n->view_name);
            break;
        }
        case PlanNodeType::CREATE_TRIGGER: {
            auto n = std::static_pointer_cast<CreateTriggerNode>(node);
            str_field("trigger_name", n->trigger_name);
            break;
        }
        case PlanNodeType::CREATE_FUNCTION: {
            auto n = std::static_pointer_cast<CreateFunctionNode>(node);
            str_field("function_name", n->function_name);
            break;
        }
        case PlanNodeType::CREATE_PROCEDURE: {
            auto n = std::static_pointer_cast<CreateProcedureNode>(node);
            str_field("procedure_name", n->procedure_name);
            break;
        }
        case PlanNodeType::CALL: {
            auto n = std::static_pointer_cast<CallNode>(node);
            str_field("procedure_name", n->procedure_name);
            break;
        }
        case PlanNodeType::VIEW_DEFINE: {
            auto n = std::static_pointer_cast<ViewDefineNode>(node);
            str_field("view_name", n->view_name);
            break;
        }
        case PlanNodeType::UPSERT: {
            auto n = std::static_pointer_cast<UpsertNode>(node);
            str_field("table", n->table_name);
            break;
        }
        case PlanNodeType::BEGIN_TXN:
        case PlanNodeType::COMMIT_TXN:
        case PlanNodeType::ROLLBACK_TXN:
            break;
        case PlanNodeType::SAVEPOINT: {
            auto n = std::static_pointer_cast<SavepointNode>(node);
            str_field("savepoint_name", n->savepoint_name);
            break;
        }
        case PlanNodeType::ROLLBACK_TO_SP: {
            auto n = std::static_pointer_cast<RollbackToSavepointNode>(node);
            str_field("savepoint_name", n->savepoint_name);
            break;
        }
        case PlanNodeType::RELEASE_SP: {
            auto n = std::static_pointer_cast<ReleaseSavepointNode>(node);
            str_field("savepoint_name", n->savepoint_name);
            break;
        }
        case PlanNodeType::EXPLAIN: {
            auto n = std::static_pointer_cast<ExplainNode>(node);
            str_field("format", n->format);
            AppendJsonField(body, "analyze", n->analyze ? "true" : "false");
            break;
        }
        case PlanNodeType::SHOW: {
            auto n = std::static_pointer_cast<ShowNode>(node);
            str_field("target_table", n->target_table);
            break;
        }
        case PlanNodeType::CREATE_SCHEMA: {
            auto n = std::static_pointer_cast<CreateSchemaNode>(node);
            str_field("schema_name", n->schema_name);
            break;
        }
        case PlanNodeType::DROP_SCHEMA: {
            auto n = std::static_pointer_cast<DropSchemaNode>(node);
            str_field("schema_name", n->schema_name);
            break;
        }
        case PlanNodeType::CREATE_SEQUENCE: {
            auto n = std::static_pointer_cast<CreateSequenceNode>(node);
            str_field("sequence_name", n->sequence_name);
            break;
        }
        case PlanNodeType::DROP_SEQUENCE: {
            auto n = std::static_pointer_cast<DropSequenceNode>(node);
            str_field("sequence_name", n->sequence_name);
            break;
        }
        case PlanNodeType::WINDOW: {
            auto n = std::static_pointer_cast<WindowNode>(node);
            for (size_t i = 0; i < n->select_list.size(); ++i) {
                std::string key = "col_" + std::to_string(i);
                if (n->select_list[i])
                    str_field(key, n->select_list[i]->ToString());
            }
            break;
        }
        case PlanNodeType::SUBQUERY: {
            auto n = std::static_pointer_cast<SubqueryNode>(node);
            switch (n->kind) {
                case SubqueryType::SCALAR:  str_field("kind", "SCALAR"); break;
                case SubqueryType::EXISTS:  str_field("kind", "EXISTS"); break;
                case SubqueryType::IN:      str_field("kind", "IN");     break;
                case SubqueryType::ANY:     str_field("kind", "ANY");    break;
            }
            break;
        }
        case PlanNodeType::SET_OP: {
            auto n = std::static_pointer_cast<SetOpNode>(node);
            switch (n->kind) {
                case SetOpNode::Kind::UNION:     str_field("kind", "UNION");     break;
                case SetOpNode::Kind::UNION_ALL: str_field("kind", "UNION ALL"); break;
                case SetOpNode::Kind::INTERSECT: str_field("kind", "INTERSECT"); break;
                case SetOpNode::Kind::EXCEPT:    str_field("kind", "EXCEPT");    break;
            }
            break;
        }
        case PlanNodeType::APPLY: {
            auto n = std::static_pointer_cast<ApplyNode>(node);
            str_field("lateral_alias", n->lateral_alias);
            break;
        }
        case PlanNodeType::CREATE_MATERIALIZED_VIEW: {
            auto n = std::static_pointer_cast<CreateMaterializedViewNode>(node);
            str_field("view_name", n->view_name);
            break;
        }
        case PlanNodeType::ALTER_MATERIALIZED_VIEW: {
            auto n = std::static_pointer_cast<AlterMaterializedViewNode>(node);
            str_field("view_name", n->view_name);
            break;
        }
        default:
            break;
    }

    // EXPLAIN ANALYZE 注入：rows / time_ms
    auto it = proxy_map.find(node.get());
    if (it != proxy_map.end() && it->second) {
        const auto* p = it->second;
        AppendJsonField(body, "rows", FmtInt(p->rows_produced()));
        AppendJsonField(body, "time_ms", FmtMs(p->total_ms()));
    }

    return body;
}

std::string PlanJsonWithStatsImpl(
    const PlanNodePtr& node,
    const std::unordered_map<const PlanNode*, TimingProxyExecutor*>& proxy_map) {
    if (!node) return "null";
    std::string body = PlanNodeJsonObject(node, proxy_map);
    if (!node->children.empty()) {
        body += ", \"children\": [";
        for (size_t i = 0; i < node->children.size(); ++i) {
            if (i) body += ",";
            body += " {";
            body += PlanJsonWithStatsImpl(node->children[i], proxy_map);
            body += "}";
        }
        body += "]";
    }
    return body;
}

// SEXPR 格式：递归地把 :rows/:time_ms 键值插入到每个节点 S-表达式末尾 ')' 前。
//
// 实现策略：对每个 plan 节点，调用 ToSExpr() 拿到完整串（已经包含 children
// 子表达式）。然后用「string replace」把每个 child 的原始 S-表达式段替换
// 为 RenderAnalyzeSExprNode(c) 的结果（递归，带 stats）。最后在自身末尾
// 追加 :rows/:time_ms。
std::string RenderAnalyzeSExprNode(
    const PlanNodePtr& node,
    const std::unordered_map<const PlanNode*, TimingProxyExecutor*>& proxy_map) {
    if (!node) return "()";
    std::string out = node->ToSExpr();
    // 把每个 child 的 S-表达式替换为带 stats 的版本。
    for (auto& c : node->children) {
        if (!c) continue;
        std::string child_orig = c->ToSExpr();
        std::string child_new = RenderAnalyzeSExprNode(c, proxy_map);
        auto pos = out.find(child_orig);
        if (pos != std::string::npos) {
            out.replace(pos, child_orig.size(), child_new);
        }
    }
    // 注入本节点 stats。
    auto it = proxy_map.find(node.get());
    if (it != proxy_map.end() && it->second) {
        const auto* p = it->second;
        if (!out.empty() && out.back() == ')') {
            out.pop_back();
            out += " :rows \"" + FmtInt(p->rows_produced()) +
                   "\" :time_ms \"" + FmtMs(p->total_ms()) + "\")";
        }
    }
    return out;
}

std::string RenderAnalyzeSExpr(
    const PlanNodePtr& node,
    const std::unordered_map<const PlanNode*, TimingProxyExecutor*>& proxy_map) {
    return RenderAnalyzeSExprNode(node, proxy_map);
}

}  // namespace

ExplainExecutor::ExplainExecutor(ExecutionContext* context, ExplainNode* node)
    : Executor(context), node_(node), produced_(false) {
}

void ExplainExecutor::Init() {
    analyze_failure_msg_.clear();
    proxy_map_.clear();
}

bool ExplainExecutor::Next(Tuple* tuple) {
    if (produced_) return false;
    produced_ = true;

    std::string text;
    if (!node_->children.empty() && node_->children[0]) {
        const std::string& fmt = node_->format;
        if (node_->analyze) {
            bool had_error = false;
            text = RenderAnalyze(fmt, node_->children[0], had_error);
            if (had_error && !analyze_failure_msg_.empty()) {
                text += "\n[error] " + analyze_failure_msg_;
            }
        } else if (fmt == "JSON") {
            text = node_->children[0]->ToJson();
        } else if (fmt == "SEXPR") {
            text = node_->children[0]->ToSExpr();
        } else {
            text = node_->children[0]->ToString();
            while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
                text.pop_back();
            }
        }
    } else {
        text = "(empty plan)";
    }

    if (tuple) {
        *tuple = Tuple({Value::MakeVarchar(text)});
    }
    return true;
}

std::string ExplainExecutor::RenderAnalyze(const std::string& format,
                                            const PlanNodePtr& inner,
                                            bool& had_error) {
    had_error = false;

    // 创建临时 ExecutionEngine 实例，wrap_timing=true 构建子树。
    ExecutionEngine engine(context_->GetCatalog(),
                           context_->GetTransactionManager());
    if (context_->GetStorage()) {
        engine.SetStorageAccess(context_->GetStorage());
    }

    // RAII：注册表作用域。TimingProxyExecutor 在构造时把 (PlanNode* → Proxy*)
    // 登记到 proxy_map_；析构时清空回 nullptr。
    TimingProxyExecutor::RegistrySlot scope(&proxy_map_);

    ExecutorPtr root;
    try {
        root = engine.BuildExecutor(inner, context_, /*wrap_timing=*/true);
        if (!root) {
            had_error = true;
            analyze_failure_msg_ = "failed to build executor";
            return "(failed to build executor)";
        }
        root->Init();
    } catch (const CompilerException& e) {
        had_error = true;
        analyze_failure_msg_ = FormatError(e);
        return std::string("(build/init failed: ") + analyze_failure_msg_ + ")";
    } catch (const std::exception& e) {
        had_error = true;
        analyze_failure_msg_ = std::string("error: ") + e.what();
        return std::string("(build/init failed: ") + analyze_failure_msg_ + ")";
    }

    // 驱动 Next 至 EOF 或失败。
    try {
        Tuple t;
        while (root->Next(&t)) {
            // 忽略返回行；ANALYZE 只关心统计。
        }
    } catch (const CompilerException& e) {
        had_error = true;
        analyze_failure_msg_ = FormatError(e);
        // 不吞错误：继续渲染已收集的 partial stats；message 在 Next() 里附加。
    } catch (const std::exception& e) {
        had_error = true;
        analyze_failure_msg_ = std::string("error: ") + e.what();
    }

    // 渲染：按 format 选择。
    std::string out;
    if (format == "JSON") {
        std::string body = "{" + PlanJsonWithStatsImpl(inner, proxy_map_) + "}";
        out = body;
    } else if (format == "SEXPR") {
        out = RenderAnalyzeSExpr(inner, proxy_map_);
    } else {
        out = RenderAnalyzeText(inner, proxy_map_, 0);
        while (!out.empty() && out.back() == '\n') out.pop_back();
    }
    return out;
}

}  // namespace sqlcompiler
