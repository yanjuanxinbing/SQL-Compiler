#pragma once

#include <string>

namespace sqlcompiler {

// 语义错误细分类（Spec 1.3：「错误类型 + 位置 + 原因」）。
// 用来在错误消息里给出机器友好的分类标签（例如 "[TableNotFound]"），
// 便于用户/工具按类别过滤；不影响错误消息的人类可读部分。
//
// 与 common/Error.h 中的 ErrorStage 区别：
//   - ErrorStage 表示「错误发生在哪个编译阶段」（LEXICAL/SYNTAX/SEMANTIC/...）；
//   - SemanticErrorKind 在 ErrorStage::SEMANTIC 内部再做一次细分。
enum class SemanticErrorKind {
    // 表/列/索引等 catalog 对象查找失败
    TableNotFound,
    ColumnNotFound,
    IndexNotFound,
    ViewNotFound,

    // 名字/对象重复或冲突
    DuplicateName,        // 重复表名 / 重复列名 / CREATE 已存在的对象
    ColumnAlreadyExists,  // ALTER ADD COLUMN 命中已存在列

    // 类型与算子
    TypeMismatch,
    ArityMismatch,        // INSERT VALUES 列数不匹配 / 函数实参个数不对

    // 引用 / 函数
    UnknownFunction,
    InvalidReference,     // 不在任何 FROM 作用域的别名/限定列引用

    // 其他不适合以上分类的错误
    Other,
};

// 把 SemanticErrorKind 格式化为短标签字符串（不带方括号），用于
// "[<Kind>]" 形式的展示。例如 TableNotFound -> "TableNotFound"。
// 故意不做大小写转换 / 不本地化：保持稳定，便于自动化测试断言。
std::string SemanticErrorKindToString(SemanticErrorKind kind);

}  // namespace sqlcompiler
