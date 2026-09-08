#include "common/Error.h"

namespace sqlcompiler
{
    std::string ErrorStageToString(ErrorStage stage)
    {
        // TODO: 根据stage返回对应的可读字符串，例如 "Lexical", "Syntax" 等
        return "";
    }

    CompilerException::CompilerException(ErrorStage stage,
                                         const std::string &message,
                                         int line,
                                         int column)
        : std::runtime_error(message), stage_(stage), line_(line), column_(column) {}

    ErrorStage CompilerException::GetStage() const
    {
        return stage_;
    }

    int CompilerException::GetLine() const
    {
        return line_;
    }

    int CompilerException::GetColumn() const
    {
        return column_;
    }

    std::string FormatError(const CompilerException &ex)
    {
        // TODO: 组合 阶段 + 行号 + 列号 + 错误信息，生成统一格式的字符串
        return "";
    }

} // namespace sqlcompiler
