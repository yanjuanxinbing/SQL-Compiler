#pragma once

#include <string>
#include <unordered_map>

#include "ast/AST.h"
#include "storage_engine/Tuple.h"

namespace sqlcompiler {

// 表达式求值器：在给定的Tuple与"列名 -> 下标"映射（即该表的Schema）下，
// 对编译器产出的AST表达式树进行求值，供Filter/Project/Update等算子复用
class ExpressionEvaluator {
public:
    explicit ExpressionEvaluator(
        const std::unordered_map<std::string, size_t>& column_index_map);

    // 对表达式求值，返回结果Value。
    // 布尔判断约定：Value为INTEGER类型且AsInt()非0表示true（用于WHERE/HAVING条件）
    Value Evaluate(const ExprPtr& expr, const Tuple& tuple) const;

private:
    const std::unordered_map<std::string, size_t>& column_index_map_;

    Value EvaluateLiteral(const LiteralExpr& expr) const;
    Value EvaluateColumnRef(const ColumnRefExpr& expr, const Tuple& tuple) const;
    Value EvaluateBinary(const BinaryExpr& expr, const Tuple& tuple) const;
    Value EvaluateUnary(const UnaryExpr& expr, const Tuple& tuple) const;
    Value EvaluateFunctionCall(const FunctionCallExpr& expr, const Tuple& tuple) const;
};

}  // namespace sqlcompiler
