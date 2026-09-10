#include "execution/SortExecutor.h"

#include <algorithm>

namespace sqlcompiler {

SortExecutor::SortExecutor(ExecutionContext* context, ExecutorPtr child,
                            std::vector<OrderByItem> order_items,
                            std::unordered_map<std::string, size_t> column_index_map)
    : Executor(context), child_(std::move(child)), order_items_(std::move(order_items)),
      column_index_map_(std::move(column_index_map)), cursor_(0) {
}

void SortExecutor::Init() {
    cursor_ = 0;
    materialized_.clear();
    sorted_indices_.clear();
    if (!child_) return;
    child_->Init();

    ExpressionEvaluator eval(column_index_map_);
    Tuple t;
    while (child_->Next(&t)) {
        SortedEntry entry;
        entry.tuple = t;
        entry.keys.reserve(order_items_.size());
        entry.ascending.reserve(order_items_.size());
        for (const auto& ob : order_items_) {
            if (!ob.expr) {
                entry.keys.push_back(Value::MakeNull());
            } else {
                entry.keys.push_back(eval.Evaluate(ob.expr, t));
            }
            entry.ascending.push_back(ob.ascending);
        }
        materialized_.push_back(std::move(entry));
    }

    sorted_indices_.resize(materialized_.size());
    for (size_t i = 0; i < sorted_indices_.size(); ++i) sorted_indices_[i] = i;

    std::sort(sorted_indices_.begin(), sorted_indices_.end(),
              [this](size_t a, size_t b) {
                  const auto& ka = materialized_[a].keys;
                  const auto& kb = materialized_[b].keys;
                  for (size_t i = 0; i < ka.size() && i < kb.size(); ++i) {
                      int cmp = Value::Compare(ka[i], kb[i]);
                      if (cmp != 0) {
                          return materialized_[a].ascending[i] ? cmp < 0 : cmp > 0;
                      }
                  }
                  return a < b;
              });
}

bool SortExecutor::Next(Tuple* tuple) {
    if (cursor_ >= sorted_indices_.size()) return false;
    if (tuple) *tuple = materialized_[sorted_indices_[cursor_]].tuple;
    ++cursor_;
    return true;
}

}  // namespace sqlcompiler