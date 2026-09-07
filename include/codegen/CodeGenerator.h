#pragma once

#include <string>
#include <vector>

#include "plan/Plan.h"

namespace sqlcompiler {

// 简化的三地址式执行指令，供后续执行引擎解释执行
struct Instruction {
    std::string opcode;                  // 操作码，如 SCAN / FILTER / PROJECT / EMIT
    std::vector<std::string> operands;   // 操作数（表名/列名/表达式文本等）

    std::string ToString() const;
};

// 代码生成器：将优化后的逻辑执行计划编译为线性指令序列
class CodeGenerator {
public:
    CodeGenerator();

    // 生成入口
    std::vector<Instruction> Generate(const PlanNodePtr& plan);

private:
    void GenerateNode(const PlanNodePtr& node, std::vector<Instruction>& out);

    // 针对各类计划节点的指令生成
    void GenerateSeqScan(const PlanNodePtr& node, std::vector<Instruction>& out);
    void GenerateFilter(const PlanNodePtr& node, std::vector<Instruction>& out);
    void GenerateProject(const PlanNodePtr& node, std::vector<Instruction>& out);
    void GenerateJoin(const PlanNodePtr& node, std::vector<Instruction>& out);
    void GenerateSort(const PlanNodePtr& node, std::vector<Instruction>& out);
    void GenerateLimit(const PlanNodePtr& node, std::vector<Instruction>& out);
    void GenerateAggregate(const PlanNodePtr& node, std::vector<Instruction>& out);
    void GenerateInsert(const PlanNodePtr& node, std::vector<Instruction>& out);
    void GenerateUpdate(const PlanNodePtr& node, std::vector<Instruction>& out);
    void GenerateDelete(const PlanNodePtr& node, std::vector<Instruction>& out);
    void GenerateCreateTable(const PlanNodePtr& node, std::vector<Instruction>& out);
    void GenerateDropTable(const PlanNodePtr& node, std::vector<Instruction>& out);
};

}  // namespace sqlcompiler
