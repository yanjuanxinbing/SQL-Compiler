#include "execution/DistinctExecutor.h"

namespace sqlcompiler {

DistinctExecutor::DistinctExecutor(ExecutionContext* context, ExecutorPtr child)
    : Executor(context), child_(std::move(child)) {
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
        std::string key;
        for (const auto& v : t.GetValues()) {
            key += v.ToString();
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