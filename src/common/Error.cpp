#include "common/Error.h"

#include <sstream>

namespace sqlcompiler {

std::string ErrorStageToString(ErrorStage stage) {
    switch (stage) {
        case ErrorStage::LEXICAL:      return "Lexical";
        case ErrorStage::SYNTAX:       return "Syntax";
        case ErrorStage::SEMANTIC:     return "Semantic";
        case ErrorStage::OPTIMIZATION: return "Optimization";
        case ErrorStage::CODEGEN:      return "CodeGen";
    }
    return "Unknown";
}

CompilerException::CompilerException(ErrorStage stage,
                                      const std::string& message,
                                      int line,
                                      int column)
    : std::runtime_error(message), stage_(stage), line_(line), column_(column) {
}

ErrorStage CompilerException::GetStage() const {
    return stage_;
}

int CompilerException::GetLine() const {
    return line_;
}

int CompilerException::GetColumn() const {
    return column_;
}

std::string FormatError(const CompilerException& ex) {
    std::ostringstream oss;
    oss << "[" << ErrorStageToString(ex.GetStage()) << "]";
    int line = ex.GetLine();
    int col = ex.GetColumn();
    if (line >= 0) {
        oss << " line=" << line;
        if (col >= 0) {
            oss << ", col=" << col;
        }
        oss << ":";
    }
    oss << " " << ex.what();
    return oss.str();
}

}  // namespace sqlcompiler