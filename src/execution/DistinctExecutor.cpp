#include "execution/DistinctExecutor.h"

namespace sqlcompiler {

DistinctExecutor::DistinctExecutor(ExecutionContext* context, ExecutorPtr child,
                                 size_t distinct_column_count)
    : Executor(context), child_(std::move(child)),
      distinct_column_count_(distinct_column_count) {
}

void DistinctExecutor::Init() {
    seen_.clear();
    if (child_) child_->Init();
}

bool DistinctExecutor::Next(Tuple* tuple) {
    if (!child_) return false;
    Tuple t;
    while (child_->Next(&t)) {
        // Build a hash key by concatenating each value's serialized form.
        // 仅对前 `distinct_column_count_` 列哈希：ProjectExecutor 的输出是
        // [select_values ++ underlying_tuple]，underlying 列因 id/name 等
        // 永远不同会让 DISTINCT 永远不命中；按投影列去重才能正确识别重复行。
        const auto& vs = t.GetValues();
        size_t limit = (distinct_column_count_ == 0 || distinct_column_count_ > vs.size())
                           ? vs.size()
                           : distinct_column_count_;
        std::string key;
        key.reserve(limit * 8);
        for (size_t i = 0; i < limit; ++i) {
            key += vs[i].ToString();
            key.push_back('\x1F');  // unit separator
        }
        if (seen_.insert(key).second) {
            if (tuple) *tuple = t;
            return true;
        }
    }
    return false;
}

}  // namespace sqlcompiler