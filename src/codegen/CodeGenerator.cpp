#include "codegen/CodeGenerator.h"

namespace sqlcompiler {

std::string Instruction::ToString() const {
    // TODO: 返回形如 "SCAN users" 的可读指令字符串（opcode + operands拼接）
    return "";
}

CodeGenerator::CodeGenerator() {
    // TODO: 如有需要可补充初始化逻辑
}

std::vector<Instruction> CodeGenerator::Generate(const PlanNodePtr& plan) {
    // TODO: 调用GenerateNode()递归生成指令序列并返回
    std::vector<Instruction> instructions;
    return instructions;
}

void CodeGenerator::GenerateNode(const PlanNodePtr& node, std::vector<Instruction>& out) {
    // TODO: 先递归处理node->children，再根据node->GetType()分派到具体的Generate*函数
    // （后序遍历：先生成子节点指令，再生成当前节点指令）
}

void CodeGenerator::GenerateSeqScan(const PlanNodePtr& node, std::vector<Instruction>& out) {
    // TODO: 生成形如 {"SCAN", {table_name}} 的指令
}

void CodeGenerator::GenerateFilter(const PlanNodePtr& node, std::vector<Instruction>& out) {
    // TODO: 生成形如 {"FILTER", {predicate_text}} 的指令
}

void CodeGenerator::GenerateProject(const PlanNodePtr& node, std::vector<Instruction>& out) {
    // TODO: 生成形如 {"PROJECT", {col1, col2, ...}} 的指令
}

void CodeGenerator::GenerateJoin(const PlanNodePtr& node, std::vector<Instruction>& out) {
    // TODO: 生成形如 {"JOIN", {join_type, condition_text}} 的指令
}

void CodeGenerator::GenerateSort(const PlanNodePtr& node, std::vector<Instruction>& out) {
    // TODO: 生成形如 {"SORT", {expr1 ASC/DESC, ...}} 的指令
}

void CodeGenerator::GenerateLimit(const PlanNodePtr& node, std::vector<Instruction>& out) {
    // TODO: 生成形如 {"LIMIT", {count}} 的指令
}

void CodeGenerator::GenerateAggregate(const PlanNodePtr& node, std::vector<Instruction>& out) {
    // TODO: 生成形如 {"AGGREGATE", {group_by列, 聚合表达式}} 的指令
}

void CodeGenerator::GenerateInsert(const PlanNodePtr& node, std::vector<Instruction>& out) {
    // TODO: 生成形如 {"INSERT", {table_name, ...}} 的指令
}

void CodeGenerator::GenerateUpdate(const PlanNodePtr& node, std::vector<Instruction>& out) {
    // TODO: 生成形如 {"UPDATE", {table_name, ...}} 的指令
}

void CodeGenerator::GenerateDelete(const PlanNodePtr& node, std::vector<Instruction>& out) {
    // TODO: 生成形如 {"DELETE", {table_name, ...}} 的指令
}

void CodeGenerator::GenerateCreateTable(const PlanNodePtr& node, std::vector<Instruction>& out) {
    // TODO: 生成形如 {"CREATE_TABLE", {table_name, ...}} 的指令
}

void CodeGenerator::GenerateDropTable(const PlanNodePtr& node, std::vector<Instruction>& out) {
    // TODO: 生成形如 {"DROP_TABLE", {table_name}} 的指令
}

}  // namespace sqlcompiler
