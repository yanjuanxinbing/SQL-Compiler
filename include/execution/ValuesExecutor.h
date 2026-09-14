#pragma once

#include <vector>

#include "ast/AST.h"
#include "execution/Executor.h"

namespace sqlcompiler {

// 55_query: VALUES 行构造器执行器。
//
// 用作 (VALUES (1,'a'), (2,'b'), (3,'c')) AS t(id, name) 这类 FROM 派生表的
// 物理算子。每行（每条 row tuple）通过 ExpressionEvaluator 在当前空绑定下
// 求值（row 表达式中的列引用按 column_aliases 解析；若有 outer bind，
// 通过上下文传递）。本执行器不依赖 catalog，因此可以放在 FROM 第一位（无须表）。
//
// 派生表别名（derived_alias）与列别名（column_aliases）传给后续 ProjectNode
// 构造列下标映射。
class ValuesExecutor : public Executor {
public:
    ValuesExecutor(ExecutionContext* context,
                   std::vector<std::vector<ExprPtr>> rows,
                   std::vector<std::string> column_aliases,
                   std::string derived_alias);

    void Init() override;
    bool Next(Tuple* tuple) override;

    const std::vector<std::string>& column_aliases() const { return column_aliases_; }
    const std::string& derived_alias() const { return derived_alias_; }
    size_t row_width() const { return row_width_; }

private:
    std::vector<std::vector<ExprPtr>> rows_;
    std::vector<std::string> column_aliases_;
    std::string derived_alias_;
    size_t row_width_ = 0;
    size_t next_index_ = 0;
};

}  // namespace sqlcompiler