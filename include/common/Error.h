#pragma once

#include <stdexcept>
#include <string>

namespace sqlcompiler {

// 编译各阶段标识，用于定位错误发生在哪个阶段
enum class ErrorStage {
    LEXICAL,       // 词法分析阶段
    SYNTAX,        // 语法分析阶段
    SEMANTIC,      // 语义分析阶段
    OPTIMIZATION,  // 优化阶段
    CODEGEN        // 代码/计划生成阶段
};

// 将阶段枚举转换为可读字符串
std::string ErrorStageToString(ErrorStage stage);

// 编译器统一异常类型
class CompilerException : public std::runtime_error {
public:
    CompilerException(ErrorStage stage,
                       const std::string& message,
                       int line = -1,
                       int column = -1);

    ErrorStage GetStage() const;
    int GetLine() const;
    int GetColumn() const;

private:
    ErrorStage stage_;
    int line_;
    int column_;
};

// 统一格式化错误信息，便于打印给用户
std::string FormatError(const CompilerException& ex);

}  // namespace sqlcompiler
