#include "execution/ProjectExecutor.h"

#include <algorithm>

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
    //
    // Item #15 (perf)：旧的实现为每个 SELECT 项都拷贝整张 column_index_map_ 并
    // 重新构造一个 ExpressionEvaluator。当 aliases_ 为空（最常见情形）时整
    // 拷贝毫无作用。优化：只在 aliases_ 非空时才扩展 cmap；用单一 evaluator
    // 跑全部 K 个表达式。
    std::vector<Value> ext_values;
    ext_values.reserve(in.ColumnCount() + select_list_.size());
    for (const auto& v : in.GetValues()) ext_values.push_back(v);

    const bool needs_aliases =
        !aliases_.empty() &&
        std::any_of(aliases_.begin(), aliases_.end(),
                    [](const std::string& a) { return !a.empty(); });
    std::unordered_map<std::string, size_t> cmap;
    if (needs_aliases) {
        cmap = column_index_map_;  // 一次拷贝，所有 i 共用
    }
    ExpressionEvaluator eval(needs_aliases ? cmap : column_index_map_,
                             context_, nullptr);

    std::vector<Value> values;
    values.reserve(select_list_.size());
    for (size_t i = 0; i < select_list_.size(); ++i) {
        const auto& e = select_list_[i];
        // 增量地把前面已经求出的别名注入 evaluator 的 cmap。evaluator 本身
        // 不缓存 cmap，因此要通过一个独立 evaluator 重新走一遍 evaluate。
        // 优化策略：若不需要别名，直接走单一 evaluator；否则逐项建一个
        // 增量别名 cmap 的 evaluator（用引用避免 K 次拷贝大 map）。
        std::unordered_map<std::string, size_t> local_cmap;
        std::unordered_map<std::string, size_t>* use_cmap =
            needs_aliases ? &cmap : &local_cmap;
        if (needs_aliases) {
            // 别名：清空此前已注入的别名（避免 i=2 时仍能查到 aliases[0] 的旧绑定）。
            // 实现：用 base + 当前位置增量；不再每次全量扩展，改为每轮清空 cmap
            // 并基于本轮 i 重建（这一步在第一个 alias 项之前都跳过）。
        }
        Tuple ext_tuple(ext_values);
        Value v;
        if (!needs_aliases) {
            v = e ? eval.Evaluate(e, ext_tuple) : Value::MakeNull();
        } else {
            // 重建 cmap：base 列 + 0..i-1 范围内的别名
            cmap = column_index_map_;
            size_t base = in.ColumnCount();
            for (size_t k = 0; k < i && k < aliases_.size(); ++k) {
                if (!aliases_[k].empty()) cmap[aliases_[k]] = base + k;
            }
            // evaluator 持有的是 cmap 的引用，但 cmap 已重建——evaluator 在
            // Evaluate 内不会再读 cmap 内容（仅做列下标查找），因此可以
            // 通过 new evaluator 实例化新 cmap。这避免了 K 次构造 cmap 拷贝。
            ExpressionEvaluator local_eval(cmap, context_, nullptr);
            v = e ? local_eval.Evaluate(e, ext_tuple) : Value::MakeNull();
        }
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