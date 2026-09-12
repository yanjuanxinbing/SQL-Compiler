#include "execution/ValuesExecutor.h"

#include "execution/ExpressionEvaluator.h"

namespace sqlcompiler {

ValuesExecutor::ValuesExecutor(ExecutionContext* context,
                               std::vector<std::vector<ExprPtr>> rows,
                               std::vector<std::string> column_aliases,
                               std::string derived_alias)
    : Executor(context),
      rows_(std::move(rows)),
      column_aliases_(std::move(column_aliases)),
      derived_alias_(std::move(derived_alias)) {
    // 取最大行宽作为 row_width_（不规则矩阵按最大行宽度；空行视为零列）。
    row_width_ = 0;
    for (const auto& r : rows_) {
        if (r.size() > row_width_) row_width_ = r.size();
    }
}

void ValuesExecutor::Init() {
    next_index_ = 0;
}

bool ValuesExecutor::Next(Tuple* tuple) {
    if (next_index_ >= rows_.size()) return false;
    const auto& row = rows_[next_index_++];
    // 55_query: VALUES 行构造器执行。row 表达式按当前列下标映射求值；
    // 当前 V1 假设 VALUES 内只含字面量 / UDF / 不引用外部列，所以传空 cmap。
    std::unordered_map<std::string, size_t> cmap;
    ExpressionEvaluator eval(cmap, context_, nullptr);
    std::vector<Value> values;
    values.reserve(row.size());
    for (const auto& e : row) {
        if (e) values.push_back(eval.Evaluate(e, Tuple()));
        else values.push_back(Value::MakeNull());
    }
    if (tuple) *tuple = Tuple(std::move(values));
    return true;
}

}  // namespace sqlcompiler