#include "codegen/CodeGenerator.h"

#include <sstream>

namespace sqlcompiler {

namespace {

std::string JoinOperands(const std::vector<std::string>& ops) {
    std::string s;
    for (size_t i = 0; i < ops.size(); ++i) {
        if (i > 0) s += " ";
        s += ops[i];
    }
    return s;
}

}  // namespace

std::string Instruction::ToString() const {
    std::ostringstream oss;
    oss << opcode << " " << JoinOperands(operands);
    return oss.str();
}

CodeGenerator::CodeGenerator() {
}

std::vector<Instruction> CodeGenerator::Generate(const PlanNodePtr& plan) {
    std::vector<Instruction> out;
    if (plan) GenerateNode(plan, out);
    return out;
}

void CodeGenerator::GenerateNode(const PlanNodePtr& node, std::vector<Instruction>& out) {
    if (!node) return;
    for (auto& child : node->children) {
        GenerateNode(child, out);
    }
    switch (node->GetType()) {
        case PlanNodeType::SEQ_SCAN:     GenerateSeqScan(node, out); break;
        // 索引扫描沿用全表扫描的伪指令输出：代码生成器只用于展示执行计划，
        // 不区分访问路径。
        case PlanNodeType::INDEX_SCAN:   GenerateSeqScan(node, out); break;
        case PlanNodeType::FILTER:       GenerateFilter(node, out); break;
        case PlanNodeType::PROJECT:      GenerateProject(node, out); break;
        case PlanNodeType::JOIN:         GenerateJoin(node, out); break;
        case PlanNodeType::SORT:         GenerateSort(node, out); break;
        case PlanNodeType::LIMIT:        GenerateLimit(node, out); break;
        case PlanNodeType::AGGREGATE:    GenerateAggregate(node, out); break;
        case PlanNodeType::INSERT:       GenerateInsert(node, out); break;
        case PlanNodeType::UPDATE:       GenerateUpdate(node, out); break;
        case PlanNodeType::DELETE:       GenerateDelete(node, out); break;
        case PlanNodeType::CREATE_TABLE: GenerateCreateTable(node, out); break;
        case PlanNodeType::DROP_TABLE:   GenerateDropTable(node, out); break;
    }
}

void CodeGenerator::GenerateSeqScan(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto n = std::static_pointer_cast<SeqScanNode>(node);
    out.push_back({"SCAN", {n->table_name}});
}

void CodeGenerator::GenerateFilter(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto n = std::static_pointer_cast<FilterNode>(node);
    std::string pred = n->predicate ? n->predicate->ToString() : "?";
    out.push_back({"FILTER", {pred}});
}

void CodeGenerator::GenerateProject(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto n = std::static_pointer_cast<ProjectNode>(node);
    std::vector<std::string> cols;
    for (auto& c : n->columns) cols.push_back(c ? c->ToString() : "?");
    out.push_back({"PROJECT", cols});
}

void CodeGenerator::GenerateJoin(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto n = std::static_pointer_cast<JoinNode>(node);
    const char* jt = "INNER";
    switch (n->join_type) {
        case JoinType::INNER: jt = "INNER"; break;
        case JoinType::LEFT:  jt = "LEFT";  break;
        case JoinType::RIGHT: jt = "RIGHT"; break;
    }
    out.push_back({"JOIN", {jt, n->condition ? n->condition->ToString() : "?"}});
}

void CodeGenerator::GenerateSort(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto n = std::static_pointer_cast<SortNode>(node);
    std::vector<std::string> items;
    for (auto& it : n->order_items) {
        items.push_back((it.expr ? it.expr->ToString() : "?") +
                        std::string(it.ascending ? " ASC" : " DESC"));
    }
    out.push_back({"SORT", items});
}

void CodeGenerator::GenerateLimit(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto n = std::static_pointer_cast<LimitNode>(node);
    out.push_back({"LIMIT", {std::to_string(n->limit_count)}});
}

void CodeGenerator::GenerateAggregate(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto n = std::static_pointer_cast<AggregateNode>(node);
    std::vector<std::string> ops;
    for (auto& e : n->group_by_exprs) ops.push_back("GROUP:" + (e ? e->ToString() : "?"));
    for (auto& e : n->aggregate_exprs) ops.push_back("AGG:" + (e ? e->ToString() : "?"));
    out.push_back({"AGGREGATE", ops});
}

void CodeGenerator::GenerateInsert(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto n = std::static_pointer_cast<InsertNode>(node);
    std::vector<std::string> ops = {n->table_name,
        "rows=" + std::to_string(n->values_list.size())};
    out.push_back({"INSERT", ops});
}

void CodeGenerator::GenerateUpdate(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto n = std::static_pointer_cast<UpdateNode>(node);
    out.push_back({"UPDATE", {n->table_name}});
}

void CodeGenerator::GenerateDelete(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto n = std::static_pointer_cast<DeleteNode>(node);
    out.push_back({"DELETE", {n->table_name}});
}

void CodeGenerator::GenerateCreateTable(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto n = std::static_pointer_cast<CreateTableNode>(node);
    out.push_back({"CREATE_TABLE", {n->table_name,
        "cols=" + std::to_string(n->columns.size())}});
}

void CodeGenerator::GenerateDropTable(const PlanNodePtr& node, std::vector<Instruction>& out) {
    auto n = std::static_pointer_cast<DropTableNode>(node);
    out.push_back({"DROP_TABLE", {n->table_name}});
}

}  // namespace sqlcompiler