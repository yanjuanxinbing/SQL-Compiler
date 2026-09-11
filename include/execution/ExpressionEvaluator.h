#pragma once

#include <string>
#include <unordered_map>

#include "ast/AST.h"
#include "storage_engine/Tuple.h"

namespace sqlcompiler {

class ExecutionContext;

// 表达式求值器：在给定的Tuple与"列名 -> 下标"映射（即该表的Schema）下，
// 对编译器产出的AST表达式树进行求值，供Filter/Project/Update等算子复用
class ExpressionEvaluator {
public:
    explicit ExpressionEvaluator(
        const std::unordered_map<std::string, size_t>& column_index_map);

    // 增强构造：传入执行上下文与"外层行绑定"。
    // - ctx：子查询求值时需要用它驱动内部子计划；为 nullptr 时 SubqueryExprNode 退化为 NULL。
    // - outer_bind：相关子查询中把外层 SELECT 当前行的列暴露给内层表达式。
    //   当表达式里的 ColumnRef 在 column_index_map_ 中找不到且 outer_bind 命中同名时，
    //   使用 outer_bind 中的值；否则原样返回 NULL。
    ExpressionEvaluator(
        const std::unordered_map<std::string, size_t>& column_index_map,
        ExecutionContext* ctx,
        const std::unordered_map<std::string, Value>* outer_bind);

    // 对表达式求值，返回结果Value。
    // 布尔判断约定：Value为INTEGER类型且AsInt()非0表示true（用于WHERE/HAVING条件）
    Value Evaluate(const ExprPtr& expr, const Tuple& tuple) const;

    // 设置外层绑定。Filter/Project 在循环中每行调用一次。
    void SetOuterBind(const std::unordered_map<std::string, Value>* bind) {
        outer_bind_ = bind;
    }

private:
    const std::unordered_map<std::string, size_t>& column_index_map_;
    ExecutionContext* ctx_ = nullptr;
    const std::unordered_map<std::string, Value>* outer_bind_ = nullptr;

    Value EvaluateLiteral(const LiteralExpr& expr) const;
    Value EvaluateColumnRef(const ColumnRefExpr& expr, const Tuple& tuple) const;
    Value EvaluateBinary(const BinaryExpr& expr, const Tuple& tuple) const;
    Value EvaluateUnary(const UnaryExpr& expr, const Tuple& tuple) const;
    Value EvaluateFunctionCall(const FunctionCallExpr& expr, const Tuple& tuple) const;
    Value EvaluateCase(const CaseExprNode& expr, const Tuple& tuple) const;
    Value EvaluateCast(const CastExprNode& expr, const Tuple& tuple) const;
    Value EvaluateSubquery(const SubqueryExprNode& expr, const Tuple& tuple) const;
    // 43_upsert: VALUES(col) —— 通过 ExecutionContext 的 upsert_values_bind 取值
    Value EvaluateUpsertValuesRef(const UpsertValuesRefExpr& expr,
                                  const Tuple& tuple) const;
    // 44_pattern_match: LIKE / ILIKE / REGEXP / RLIKE（含可选 ESCAPE 子句）
    Value EvaluateLike(const LikeExprNode& expr, const Tuple& tuple) const;
    // 45_datetime: EXTRACT(field FROM source) —— 返回 INT（见实现注释）
    Value EvaluateExtract(const ExtractExprNode& expr, const Tuple& tuple) const;
    // 45_datetime: INTERVAL <n> <unit> 节点求值，返回保存计数与单位的
    // FunctionCallExpr 形式（沿用既有"复合结构用函数节点承载"的风格）。
    Value EvaluateInterval(const IntervalExprNode& expr, const Tuple& tuple) const;
};

}  // namespace sqlcompiler
