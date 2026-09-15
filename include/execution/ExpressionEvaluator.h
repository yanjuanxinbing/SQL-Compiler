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

    // 59_procs (Category 8): 设置 procedure 当前局部变量绑定。当 NULL 时
    // 关闭回退（默认）。该绑定在 ColumnRef 解析时与 outer_bind 并列使用：
    // outer_bind 优先；若 outer_bind 未命中且 proc_locals 非空，再回退到
    // proc_locals；都未命中返回 NULL。
    void SetProcLocals(const std::unordered_map<std::string, Value>* locals) {
        proc_locals_ = locals;
    }

private:
    const std::unordered_map<std::string, size_t>& column_index_map_;
    ExecutionContext* ctx_ = nullptr;
    const std::unordered_map<std::string, Value>* outer_bind_ = nullptr;
    // 59_procs (Category 8): procedure 局部变量绑定回退。
    const std::unordered_map<std::string, Value>* proc_locals_ = nullptr;

    // Item #14 (perf): 大小写不敏感的 column_index_map 副本。第一次出现
    // case-miss 时构建一次，后续 lookup 走 O(1) 哈希。不可变（const 成员
    // 在 lambda 里通过 mutable 一次性填充）。单 evaluator 实例共享同一张表。
    mutable std::unordered_map<std::string, size_t> ci_cmap_;
    mutable bool ci_cmap_built_ = false;
    // 大小写不敏感 outer_bind 副本。结构同上。
    mutable std::unordered_map<std::string, Value> ci_outer_bind_;
    mutable bool ci_outer_bind_built_ = false;
    mutable const std::unordered_map<std::string, Value>* ci_outer_bind_src_ = nullptr;

    Value EvaluateLiteral(const LiteralExpr& expr) const;

    // Item #14 (perf)：惰性构建 outer_bind 的 lowercase 副本；outer_bind 指针
    // 会随 SetOuterBind 切换，因此这里用 src 指针 + flag 来识别「已缓存过当前
    // outer_bind」。如果 src 指针变了（指向另一张表），就重建 ci_outer_bind_。
    void EnsureOuterBindCi() const {
        if (!outer_bind_) return;
        if (ci_outer_bind_built_ && ci_outer_bind_src_ == outer_bind_) return;
        ci_outer_bind_.clear();
        ci_outer_bind_.reserve(outer_bind_->size());
        for (const auto& kv : *outer_bind_) {
            std::string lc;
            lc.reserve(kv.first.size());
            for (char c : kv.first) {
                lc.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            ci_outer_bind_[lc] = kv.second;
        }
        ci_outer_bind_src_ = outer_bind_;
        ci_outer_bind_built_ = true;
    }
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
    // 53_ddl: NEXTVAL FOR sequence_name —— 推进序列并返回当前值。
    Value EvaluateNextval(const NextvalExpr& expr) const;
};

}  // namespace sqlcompiler
