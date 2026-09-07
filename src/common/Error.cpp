#include "common/Error.h"

namespace sqlcompiler {

std::string ErrorStageToString(ErrorStage stage) {
    // TODO: 根据stage返回对应的可读字符串，例如 "Lexical", "Syntax" 等
    return "";
}

CompilerException::CompilerException(ErrorStage stage,
                                      const std::string& message,
                                      int line,
                                      int column)
    : std::runtime_error(message), stage_(stage), line_(line), column_(column) {
    // TODO: 如有需要，可在此补充额外的初始化逻辑
}

ErrorStage CompilerException::GetStage() const {
    // TODO: 返回stage_
    return stage_;
}

int CompilerException::GetLine() const {
    // TODO: 返回line_
    return 0;
}

int CompilerException::GetColumn() const {
    // TODO: 返回column_
    return 0;
}

std::string FormatError(const CompilerException& ex) {
    // TODO: 组合 阶段 + 行号 + 列号 + 错误信息，生成统一格式的字符串
    return "";
}

}  // namespace sqlcompiler
