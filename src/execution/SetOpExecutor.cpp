#include "execution/SetOpExecutor.h"

#include <utility>

namespace sqlcompiler {

SetOpExecutor::SetOpExecutor(ExecutionContext* context, ExecutorPtr left,
                             ExecutorPtr right, std::string kind)
    : Executor(context), left_(std::move(left)), right_(std::move(right)),
      kind_(std::move(kind)) {
}

std::string SetOpExecutor::TupleKey(const Tuple& t) {
    std::string key;
    for (const auto& v : t.GetValues()) {
        key += v.ToString();
        key.push_back('\x1F');  // 单元分隔符，避免「ab|c」与「a|bc」等串接碰撞
    }
    return key;
}

void SetOpExecutor::Init() {
    seen_union_.clear();
    right_set_.clear();
    right_set_built_ = false;
    phase_ = Phase::kLeft;
    if (left_) left_->Init();
    if (right_) right_->Init();
}

bool SetOpExecutor::Next(Tuple* tuple) {
    if (!left_ && !right_) return false;

    if (kind_ == "UNION ALL") {
        // 先扫 LHS，再扫 RHS；不去重。
        if (phase_ == Phase::kLeft) {
            if (left_ && left_->Next(tuple)) return true;
            phase_ = Phase::kRight;
            if (right_) right_->Init();
        }
        if (phase_ == Phase::kRight) {
            if (right_ && right_->Next(tuple)) return true;
        }
        return false;
    }

    if (kind_ == "UNION") {
        // 先扫 LHS 并记录输出过的键；再扫 RHS，遇到 LHS 已有的就跳过。
        if (phase_ == Phase::kLeft) {
            Tuple t;
            while (left_ && left_->Next(&t)) {
                auto key = TupleKey(t);
                if (seen_union_.insert(key).second) {
                    if (tuple) *tuple = t;
                    return true;
                }
            }
            phase_ = Phase::kRight;
            if (right_) right_->Init();
        }
        if (phase_ == Phase::kRight) {
            Tuple t;
            while (right_ && right_->Next(&t)) {
                auto key = TupleKey(t);
                if (seen_union_.insert(key).second) {
                    if (tuple) *tuple = t;
                    return true;
                }
            }
        }
        return false;
    }

    if (kind_ == "INTERSECT" || kind_ == "EXCEPT") {
        // 先物化 RHS 的去重集合视图，再扫 LHS。
        if (!right_set_built_) {
            right_set_.clear();
            Tuple t;
            while (right_ && right_->Next(&t)) {
                right_set_.insert(TupleKey(t));
            }
            right_set_built_ = true;
        }
        Tuple t;
        while (left_ && left_->Next(&t)) {
            auto key = TupleKey(t);
            bool in_rhs = right_set_.count(key) > 0;
            if (kind_ == "INTERSECT") {
                // LHS ∩ RHS：保留同时出现在两侧的，输出一次。
                if (in_rhs) {
                    if (seen_union_.insert(key).second) {
                        if (tuple) *tuple = t;
                        return true;
                    }
                }
            } else {
                // EXCEPT (LHS \ RHS)：跳过出现在 RHS 中的；剩余按 LHS 顺序（去重后）。
                if (!in_rhs && seen_union_.insert(key).second) {
                    if (tuple) *tuple = t;
                    return true;
                }
            }
        }
        return false;
    }

    // 未知 kind：按 UNION ALL 的安全默认处理
    if (phase_ == Phase::kLeft) {
        if (left_ && left_->Next(tuple)) return true;
        phase_ = Phase::kRight;
        if (right_) right_->Init();
    }
    if (phase_ == Phase::kRight) {
        if (right_ && right_->Next(tuple)) return true;
    }
    return false;
}

}  // namespace sqlcompiler
