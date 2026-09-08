#include "codegen/CodeGenerator.h"

#include <sstream>
#include <string>

#include "common/Error.h"

namespace sqlcompiler {

namespace {

// 把 JOIN 类型映射到字符串字面量，便于指令输出
std::string JoinTypeToString(JoinType type) {
    switch (type) {
        case JoinType::INNER: return "INNER";
        case JoinType::LEFT:  return "LEFT";
        case JoinType::RIGHT: return "RIGHT";
    }
    return "UNKNOWN";
}

// 表达式转字符串；为 null 时返回空串，避免指令中出现 "<null>"
std::string ExprToText(const ExprPtr& expr) {
    if (!expr) return std::string();
    return expr->ToString();
}

// 拼接字符串数组为单个字符串，例如 ["a", "b"] -> "a b"
std::string JoinOperands(const std::vector<std::string>& operands) {
    std::string result;
    for (size_t i = 0; i < operands.size(); ++i) {
        if (i > 0) result.push_back(' ');
        result += operands[i];
    }
    return result;
}

// 构造一条指令并 push 到输出末尾
void Emit(std::vector<Instruction>& out,
          std::string opcode,
          std::vector<std::string> operands) {
    Instruction ins;
    ins.opcode = std::move(opcode);
    ins.operands = std::move(operands);
    out.push_back(std::move(ins));
}

}  // namespace

std::string Instruction::ToString() const {
    // 形如 "SCAN users" 或 "PROJECT col1 col2"
    std::ostringstream oss;
    oss << opcode;
    for (const auto& op : operands) {
        oss << ' ' << op;
    }
    return oss.str();
}

CodeGenerator::CodeGenerator() {
    // 暂时无需任何成员状态
}

std::vector<Instruction> CodeGenerator::Generate(const PlanNodePtr& plan) {
    std::vector<Instruction> instructions;
    if (!plan) {
        throw CompilerException(ErrorStage::CODEGEN, "Cannot generate code for null plan");
    }
    // 后序遍历：先子节点再自身
    GenerateNode(plan, instructions);
    return instructions;
}

void CodeGenerator::GenerateNode(const PlanNodePtr& node, std::vector<Instruction>& out) {
    if (!node) return;

    // 先处理所有子节点
    for (const auto& child : node->children) {
        GenerateNode(child, out);
    }

    // 再根据当前节点类型分派
    switch (node->GetType()) {
        case PlanNodeType::SEQ_SCAN:      GenerateSeqScan(node, out);     break;
        case PlanNodeType::FILTER:        GenerateFilter(node, out);       break;
        case PlanNodeType::PROJECT:       GenerateProject(node, out);      break;
        case PlanNodeType::JOIN:          GenerateJoin(node, out);         break;
        case PlanNodeType::SORT:          GenerateSort(node, out);         break;
        case PlanNodeType::LIMIT:         GenerateLimit(node, out);        break;
        case PlanNodeType::AGGREGATE:     GenerateAggregate(node, out);    break;
        case PlanNodeType::INSERT:        GenerateInsert(node, out);       break;
        case PlanNodeType::UPDATE:        GenerateUpdate(node, out);       break;
        case PlanNodeType::DELETE:        GenerateDelete(node, out);       break;
        case PlanNodeType::CREATE_TABLE:  GenerateCreateTable(node, out);  break;
        case PlanNodeType::DROP_TABLE:     GenerateDropTable(node, out);    break;
    }
}

void CodeGenerator::GenerateSeqScan(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto* scan = static_cast<const SeqScanNode*>(node.get());
    Emit(out, "SCAN", {scan->table_name});
}

void CodeGenerator::GenerateFilter(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto* filter = static_cast<const FilterNode*>(node.get());
    Emit(out, "FILTER", {ExprToText(filter->predicate)});
}

void CodeGenerator::GenerateProject(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto* project = static_cast<const ProjectNode*>(node.get());
    std::vector<std::string> cols;
    cols.reserve(project->columns.size());
    for (const auto& c : project->columns) {
        cols.push_back(ExprToText(c));
    }
    Emit(out, "PROJECT", std::move(cols));
}

void CodeGenerator::GenerateJoin(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto* join = static_cast<const JoinNode*>(node.get());
    Emit(out, "JOIN", {JoinTypeToString(join->join_type), ExprToText(join->condition)});
}

void CodeGenerator::GenerateSort(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto* sort = static_cast<const SortNode*>(node.get());
    std::vector<std::string> items;
    items.reserve(sort->order_items.size());
    for (const auto& ob : sort->order_items) {
        std::string text = ExprToText(ob.expr);
        text += ob.ascending ? " ASC" : " DESC";
        items.push_back(std::move(text));
    }
    Emit(out, "SORT", std::move(items));
}

void CodeGenerator::GenerateLimit(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto* limit = static_cast<const LimitNode*>(node.get());
    Emit(out, "LIMIT", {std::to_string(limit->limit_count)});
}

void CodeGenerator::GenerateAggregate(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto* agg = static_cast<const AggregateNode*>(node.get());
    // 输出格式：AGGREGATE group_by:expr1,expr2 aggs:expr1,expr2
    std::string group_text;
    for (size_t i = 0; i < agg->group_by_exprs.size(); ++i) {
        if (i > 0) group_text += ",";
        group_text += ExprToText(agg->group_by_exprs[i]);
    }
    std::string agg_text;
    for (size_t i = 0; i < agg->aggregate_exprs.size(); ++i) {
        if (i > 0) agg_text += ",";
        agg_text += ExprToText(agg->aggregate_exprs[i]);
    }
    Emit(out, "AGGREGATE", {"group_by=" + group_text, "aggs=" + agg_text});
}

void CodeGenerator::GenerateInsert(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto* ins = static_cast<const InsertNode*>(node.get());
    std::vector<std::string> ops;
    ops.push_back("table=" + ins->table_name);
    std::string cols = "cols=" + JoinOperands(ins->columns);
    ops.push_back(cols);
    ops.push_back("rows=" + std::to_string(ins->values_list.size()));
    Emit(out, "INSERT", std::move(ops));
}

void CodeGenerator::GenerateUpdate(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto* upd = static_cast<const UpdateNode*>(node.get());
    std::vector<std::string> ops;
    ops.push_back("table=" + upd->table_name);
    std::string assigns = "set=";
    for (size_t i = 0; i < upd->assignments.size(); ++i) {
        if (i > 0) assigns += ",";
        assigns += upd->assignments[i].first + "=" + ExprToText(upd->assignments[i].second);
    }
    ops.push_back(assigns);
    ops.push_back("where=" + ExprToText(upd->predicate));
    Emit(out, "UPDATE", std::move(ops));
}

void CodeGenerator::GenerateDelete(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto* del = static_cast<const DeleteNode*>(node.get());
    Emit(out, "DELETE", {"table=" + del->table_name, "where=" + ExprToText(del->predicate)});
}

void CodeGenerator::GenerateCreateTable(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto* ct = static_cast<const CreateTableNode*>(node.get());
    std::vector<std::string> cols;
    cols.reserve(ct->columns.size());
    for (const auto& c : ct->columns) {
        std::string text = c.column_name + " " + c.data_type;
        if (c.is_primary_key) text += " PRIMARY KEY";
        if (c.is_not_null)    text += " NOT NULL";
        cols.push_back(std::move(text));
    }
    std::vector<std::string> ops;
    ops.push_back("table=" + ct->table_name);
    ops.push_back("cols=" + JoinOperands(cols));
    Emit(out, "CREATE_TABLE", std::move(ops));
}

void CodeGenerator::GenerateDropTable(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto* dt = static_cast<const DropTableNode*>(node.get());
    Emit(out, "DROP_TABLE", {dt->table_name});
}

}  // namespace sqlcompiler