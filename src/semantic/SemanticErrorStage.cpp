#include "semantic/SemanticErrorStage.h"

namespace sqlcompiler {

// 把 SemanticErrorKind 格式化为短标签（与枚举名一致的 PascalCase）。
// 故意不做翻译 / 不本地化：标签稳定，便于测试断言。
std::string SemanticErrorKindToString(SemanticErrorKind kind) {
    switch (kind) {
        case SemanticErrorKind::TableNotFound:      return "TableNotFound";
        case SemanticErrorKind::ColumnNotFound:     return "ColumnNotFound";
        case SemanticErrorKind::IndexNotFound:      return "IndexNotFound";
        case SemanticErrorKind::ViewNotFound:       return "ViewNotFound";
        case SemanticErrorKind::DuplicateName:      return "DuplicateName";
        case SemanticErrorKind::ColumnAlreadyExists:return "ColumnAlreadyExists";
        case SemanticErrorKind::TypeMismatch:       return "TypeMismatch";
        case SemanticErrorKind::ArityMismatch:      return "ArityMismatch";
        case SemanticErrorKind::UnknownFunction:    return "UnknownFunction";
        case SemanticErrorKind::InvalidReference:   return "InvalidReference";
        case SemanticErrorKind::Other:              return "Other";
    }
    return "Other";
}

}  // namespace sqlcompiler
