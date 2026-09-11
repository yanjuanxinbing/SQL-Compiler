#include "execution/ProjectExecutor.h"

#include "execution/ExpressionEvaluator.h"

namespace sqlcompiler {

ProjectExecutor::ProjectExecutor(ExecutionContext* context, ExecutorPtr child,
                                 std::vector<ExprPtr> select_list,
                                 std::unordered_map<std::string, size_t> column_index_map,
                                 std::vector<std::string> aliases)
    : Executor(context),
      child_(std::move(child)),
      select_list_(std::move(select_list)),
      column_index_map_(std::move(column_index_map)),
      aliases_(std::move(aliases)),
      has_emitted_(false) {
}

void ProjectExecutor::Init() {
    has_emitted_ = false;
    if (child_) child_->Init();
}

bool ProjectExecutor::Next(Tuple* tuple) {
    if (!child_) {
        if (has_emitted_) return false;
        has_emitted_ = true;
        if (!tuple) return true;
        ExpressionEvaluator eval(column_index_map_, context_, nullptr);
        std::vector<Value> values;
        values.reserve(select_list_.size());
        for (const auto& e : select_list_) {
            if (e) values.push_back(eval.Evaluate(e, Tuple()));
            else values.push_back(Value::MakeNull());
        }
        *tuple = Tuple(std::move(values));
        return true;
    }
    Tuple in;
    if (!child_->Next(&in)) return false;
    if (!tuple) return true;
    if (select_list_.size() == 1 &&
        select_list_[0]->GetType() == NodeType::FUNCTION_CALL_EXPR) {
        auto fc = std::static_pointer_cast<FunctionCallExpr>(select_list_[0]);
        if (fc->function_name == "*" || fc->function_name == "STAR") {
            *tuple = in;
            return true;
        }
    }
    // SELECT-list alias visibility：第 i 项求值时，允许引用前面已定义的别名。
    // 实现：构造扩展输入 ext_values = in + 已求出的 values[0..i-1]，
    // 让别名映射到「in 长度 + 之前的位置」，评估时使用该扩展 tuple 作为入参。
    std::vector<Value> ext_values;
    ext_values.reserve(in.ColumnCount() + select_list_.size());
    for (const auto& v : in.GetValues()) ext_values.push_back(v);

    std::vector<Value> values;
    values.reserve(select_list_.size());
    for (size_t i = 0; i < select_list_.size(); ++i) {
        const auto& e = select_list_[i];
        std::unordered_map<std::string, size_t> cmap = column_index_map_;
        size_t base = in.ColumnCount();
        for (size_t k = 0; k < i && k < aliases_.size(); ++k) {
            if (!aliases_[k].empty()) cmap[aliases_[k]] = base + k;
        }
        ExpressionEvaluator eval(cmap, context_, nullptr);
        Tuple ext_tuple(ext_values);
        Value v = e ? eval.Evaluate(e, ext_tuple) : Value::MakeNull();
        values.push_back(std::move(v));
        ext_values.push_back(values.back());
    }
    // 把 SELECT list 值附加在 underlying 元组前面：让下游 Sort 算子即使 ORDER BY
    // 引用了未投影的内层列（如 `SELECT cust_id FROM t ORDER BY amount`），
    // 也能在排序元组里读到 amount。PrintResult 仅按 column_names 数量取前几列，
    // 尾部追加的 underlying 列对最终输出无影响。
    std::vector<Value> combined;
    combined.reserve(values.size() + in.ColumnCount());
    for (auto& v : values) combined.push_back(std::move(v));
    for (size_t i = 0; i < in.ColumnCount(); ++i) {
        combined.push_back(in.GetValue(i));
    }
    *tuple = Tuple(std::move(combined));
    return true;
}

}  // namespace sqlcompiler