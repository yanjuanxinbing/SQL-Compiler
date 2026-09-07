#include <iostream>
#include <string>

#include "codegen/CodeGenerator.h"
#include "common/Error.h"
#include "lexer/Lexer.h"
#include "optimizer/Optimizer.h"
#include "parser/Parser.h"
#include "plan/Planner.h"
#include "semantic/SemanticAnalyzer.h"
#include "semantic/SymbolTable.h"

// SQL编译器命令行入口
//
// 整体编译流程（TODO：由你补充实现）：
//   1. 读取SQL语句（可来自命令行参数、标准输入或文件）
//   2. 词法分析：sqlcompiler::Lexer -> std::vector<Token>
//   3. 语法分析：sqlcompiler::Parser -> AST (StatementPtr)
//   4. 语义分析：sqlcompiler::SemanticAnalyzer -> 校验AST是否合法
//   5. 生成逻辑计划：sqlcompiler::Planner -> PlanNodePtr
//   6. 优化：sqlcompiler::Optimizer -> 优化后的PlanNodePtr
//   7. 生成目标指令：sqlcompiler::CodeGenerator -> std::vector<Instruction>
//   8. 输出/交由执行引擎执行（执行器本身不在本框架范围内，可自行扩展）
int main(int argc, char** argv) {
    // TODO: 实现一个简单的REPL或文件输入模式，串联上述编译流程
    // 例如：
    //   sqlcompiler::SymbolTable symbol_table;
    //   std::string sql;
    //   while (std::getline(std::cin, sql)) {
    //       try {
    //           sqlcompiler::Lexer lexer(sql);
    //           auto tokens = lexer.Tokenize();
    //
    //           sqlcompiler::Parser parser(tokens);
    //           auto statement = parser.Parse();
    //
    //           sqlcompiler::SemanticAnalyzer analyzer(symbol_table);
    //           if (!analyzer.Analyze(statement)) {
    //               // 输出analyzer.GetErrors()
    //               continue;
    //           }
    //
    //           sqlcompiler::Planner planner(symbol_table);
    //           auto plan = planner.CreatePlan(statement);
    //
    //           sqlcompiler::Optimizer optimizer;
    //           plan = optimizer.Optimize(plan);
    //
    //           sqlcompiler::CodeGenerator codegen;
    //           auto instructions = codegen.Generate(plan);
    //
    //           // 输出instructions，或交给自行实现的执行引擎运行
    //       } catch (const sqlcompiler::CompilerException& ex) {
    //           std::cerr << sqlcompiler::FormatError(ex) << std::endl;
    //       }
    //   }

    return 0;
}
