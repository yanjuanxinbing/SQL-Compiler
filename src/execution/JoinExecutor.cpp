#include "execution/JoinExecutor.h"

namespace sqlcompiler {

namespace {

Tuple Concat(const Tuple& a, const Tuple& b) {
    std::vector<Value> out;
    out.reserve(a.ColumnCount() + b.ColumnCount());
    for (const auto& v : a.GetValues()) out.push_back(v);
    for (const auto& v : b.GetValues()) out.push_back(v);
    return Tuple(std::move(out));
}

Tuple NullTuple(size_t n) {
    std::vector<Value> out(n, Value::MakeNull());
    return Tuple(std::move(out));
}

}  // namespace

JoinExecutor::JoinExecutor(ExecutionContext* context, ExecutorPtr left, ExecutorPtr right,
                            JoinType join_type, ExprPtr condition,
                            std::unordered_map<std::string, size_t> column_index_map)
    : Executor(context), left_(std::move(left)), right_(std::move(right)),
      join_type_(join_type), condition_(std::move(condition)),
      column_index_map_(std::move(column_index_map)),
      li_(0), ri_(0), cur_left_pushed_(false) {
}

void JoinExecutor::Init() {
    left_buffer_.clear();
    right_buffer_.clear();
    right_matched_.clear();
    li_ = 0;
    ri_ = 0;
    cur_left_pushed_ = false;

    if (left_) {
        left_->Init();
        Tuple t;
        while (left_->Next(&t)) left_buffer_.push_back(t);
    }
    if (right_) {
        right_->Init();
        Tuple t;
        while (right_->Next(&t)) right_buffer_.push_back(t);
    }
    if (join_type_ == JoinType::RIGHT) {
        right_matched_.assign(right_buffer_.size(), false);
    }
}

bool JoinExecutor::Next(Tuple* tuple) {
    ExpressionEvaluator eval(column_index_map_, context_, nullptr);
    while (li_ < left_buffer_.size()) {
        const Tuple& lt = left_buffer_[li_];
        while (ri_ < right_buffer_.size()) {
            const Tuple& rt = right_buffer_[ri_];
            ++ri_;
            Tuple joined = Concat(lt, rt);
            bool match = true;
            if (condition_) {
                Value v = eval.Evaluate(condition_, joined);
                match = !v.IsNull() && v.AsInt() != 0;
            }
            if (match) {
                cur_left_pushed_ = true;  // 当前 li_ 已"产出过"——无论是真匹配还是 NULL 补行
                if (join_type_ == JoinType::RIGHT && ri_ - 1 < right_matched_.size()) {
                    right_matched_[ri_ - 1] = true;
                }
                if (tuple) *tuple = joined;
                return true;
            }
        }
        // 跑完 inner 循环：当前 li_ 没有再多的 right 可匹配。
        // LEFT JOIN：若整个 li_ 一次都没成功匹配过（包括当前这次 inner 循环的初轮），
        // 补一行 (left, NULL right)；否则说明它至少匹配过，直接前进到下一个 li_。
        if (join_type_ == JoinType::LEFT && !cur_left_pushed_) {
            Tuple padded = Concat(lt, NullTuple(right_buffer_.empty() ? 0 :
                                                  right_buffer_[0].ColumnCount()));
            if (tuple) *tuple = padded;
            cur_left_pushed_ = true;  // 标记本 li_ 已处理，避免下一轮 Next() 重复发射
            return true;
        }
        ++li_;
        ri_ = 0;
        cur_left_pushed_ = false;
    }

    // RIGHT JOIN: emit unmatched right tuples with NULL left
    if (join_type_ == JoinType::RIGHT) {
        for (size_t i = 0; i < right_buffer_.size(); ++i) {
            if (!right_matched_[i]) {
                size_t left_cols = left_buffer_.empty() ? 0 : left_buffer_[0].ColumnCount();
                Tuple padded = Concat(NullTuple(left_cols), right_buffer_[i]);
                right_matched_[i] = true;  // avoid re-emit
                if (tuple) *tuple = padded;
                return true;
            }
        }
    }
    return false;
}

}  // namespace sqlcompiler