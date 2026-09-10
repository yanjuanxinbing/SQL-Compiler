#include "execution/LimitExecutor.h"

namespace sqlcompiler {

LimitExecutor::LimitExecutor(ExecutionContext* context, ExecutorPtr child,
                              int limit_count, int offset)
    : Executor(context), child_(std::move(child)),
      limit_count_(limit_count), offset_(offset), emitted_(0), skipped_(0) {
}

void LimitExecutor::Init() {
    emitted_ = 0;
    skipped_ = 0;
    if (child_) child_->Init();
}

bool LimitExecutor::Next(Tuple* tuple) {
    if (!child_) return false;
    if (emitted_ >= limit_count_) return false;
    while (skipped_ < offset_) {
        Tuple dummy;
        if (!child_->Next(&dummy)) return false;
        ++skipped_;
    }
    if (!child_->Next(tuple)) return false;
    ++emitted_;
    return true;
}

}  // namespace sqlcompiler
