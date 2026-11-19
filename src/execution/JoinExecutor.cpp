#include "execution/JoinExecutor.h"

#include <algorithm>

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

// Item #4 (perf)：检测 on 条件是否为简单的等值谓词 col_a = col_b，
// 同时返回两侧的列下标（在拼接后的 joined tuple 上的下标：左表列数 + 右表列数）。
//
// 策略：
//   - 只在 BinaryExpr(BinaryOperator::EQUAL) 形态下返回 true；AND 连接
//     的多个等值谓词暂不支持（保持原 nested loop）。
//   - 两侧必须是 ColumnRefExpr；不识别常量 / 算术表达式。
//   - 若两侧的表名不同（一个是外层限定，一个是另一个），按"左 col / 右 col"返回。
//   - 若两侧都没限定名 / 同表限定名，则无法确定属于左表还是右表，保守返回 false
//     走 nested loop。
//
// joined_offset_left  / joined_offset_right 分别返回左 col / 右 col 在拼接 tuple 中的位置。
bool DetectEqualityPredicate(const ExprPtr& cond,
                             const std::unordered_map<std::string, size_t>& cmap,
                             const Tuple& sample_left,
                             const Tuple& sample_right,
                             size_t* out_left_idx,
                             size_t* out_right_idx,
                             bool* out_left_is_first) {
    if (!cond || cond->GetType() != NodeType::BINARY_EXPR) return false;
    auto b = std::static_pointer_cast<BinaryExpr>(cond);
    if (b->op != BinaryOperator::EQUAL) return false;
    if (!b->left || b->left->GetType() != NodeType::COLUMN_REF_EXPR) return false;
    if (!b->right || b->right->GetType() != NodeType::COLUMN_REF_EXPR) return false;
    auto cr_l = std::static_pointer_cast<ColumnRefExpr>(b->left);
    auto cr_r = std::static_pointer_cast<ColumnRefExpr>(b->right);
    // 把 column 名限定到唯一下标：先尝试限定名，否则尝试裸列名。
    auto lookup_idx = [&](const ColumnRefExpr& cr) -> size_t {
        std::string qk = cr.table_name + "." + cr.column_name;
        auto it = cmap.find(qk);
        if (it != cmap.end()) return it->second;
        auto it2 = cmap.find(cr.column_name);
        if (it2 != cmap.end()) return it2->second;
        return static_cast<size_t>(-1);
    };
    size_t idx_l = lookup_idx(*cr_l);
    size_t idx_r = lookup_idx(*cr_r);
    if (idx_l == static_cast<size_t>(-1) || idx_r == static_cast<size_t>(-1)) return false;
    size_t left_cols = sample_left.ColumnCount();
    size_t right_cols = sample_right.ColumnCount();
    // 确定哪个属于左表、哪个属于右表。下标 < left_cols 即在 joined tuple 的左半。
    bool l_in_left  = idx_l < left_cols;
    bool r_in_left  = idx_r < left_cols;
    bool l_in_right = idx_l >= left_cols && idx_l < left_cols + right_cols;
    bool r_in_right = idx_r >= left_cols && idx_r < left_cols + right_cols;
    if (!(l_in_left && r_in_right) && !(l_in_right && r_in_left)) {
        // 两个 col 都在同一侧（或者越界）→ 退化为 nested loop
        return false;
    }
    if (l_in_left && r_in_right) {
        *out_left_idx = idx_l;
        *out_right_idx = idx_r - left_cols;
        *out_left_is_first = true;
    } else {
        *out_left_idx = idx_r;
        *out_right_idx = idx_l - left_cols;
        *out_left_is_first = false;
    }
    return true;
}

std::string KeyOf(const Tuple& t, size_t col_idx) {
    if (col_idx >= t.ColumnCount()) return std::string();
    return t.GetValue(col_idx).ToString();
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
    left_matched_.clear();
    li_ = 0;
    ri_ = 0;
    cur_left_pushed_ = false;
    use_hash_join_ = false;
    hj_hash_.clear();
    hj_probe_idx_ = 0;
    hj_cur_bucket_pos_ = 0;
    hj_cur_bucket_.clear();

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
    if (join_type_ == JoinType::RIGHT || join_type_ == JoinType::FULL_OUTER) {
        right_matched_.assign(right_buffer_.size(), false);
    }
    if (join_type_ == JoinType::FULL_OUTER) {
        left_matched_.assign(left_buffer_.size(), false);
    }

    // Item #4 (perf)：当 join 类型为 INNER 且 on 条件为简单等值 col=col 时，
    // 用 hash join 替代 O(N·M) 嵌套循环。LEFT/RIGHT/FULL OUTER 仍走 nested loop
    // （FULL OUTER 不容易在 hash 上表达；保留 nested loop 实现，避免引入新 bug）。
    //
    // 为了保持与 nested loop 一致的行序（按 left 行序遍历，每行按 right 桶顺序
    // 输出），**始终** build 在 right、probe 在 left：hash 表键为右行 r_idx 列，
    // 探测时按左行 l_idx 列查桶。这样无论 left/right 大小如何，输出都与嵌套
    // 循环的 left-outer × right-inner 顺序一致。代价是当 right 比 left 大很多
    // 时 hash 表占用空间较大，但仍优于 O(N·M) 时间。
    if (join_type_ == JoinType::INNER && condition_ &&
        !left_buffer_.empty() && !right_buffer_.empty()) {
        size_t l_idx = 0, r_idx = 0;
        bool left_first = true;
        if (DetectEqualityPredicate(condition_, column_index_map_,
                                    left_buffer_[0], right_buffer_[0],
                                    &l_idx, &r_idx, &left_first)) {
            use_hash_join_ = true;
            // build = right_buffer_（键值取 r_idx 列），probe = left_buffer_（键值取 l_idx 列）。
            hj_hash_.reserve(right_buffer_.size() * 2);
            for (size_t i = 0; i < right_buffer_.size(); ++i) {
                std::string k = KeyOf(right_buffer_[i], r_idx);
                hj_hash_[k].push_back(i);
            }
            hj_probe_left_col_ = l_idx;  // probe (left) 取 l_idx 列
            hj_build_right_col_ = r_idx; // build (right) 取 r_idx 列
            hj_probe_is_left_ = true;    // probe = left, build = right
        }
    }
}

bool JoinExecutor::Next(Tuple* tuple) {
    // CROSS JOIN: 直接嵌套循环，无条件。
    if (join_type_ == JoinType::CROSS) {
        if (left_buffer_.empty() || right_buffer_.empty()) return false;
        while (li_ < left_buffer_.size()) {
            const Tuple& lt = left_buffer_[li_];
            while (ri_ < right_buffer_.size()) {
                const Tuple& rt = right_buffer_[ri_];
                ++ri_;
                if (tuple) *tuple = Concat(lt, rt);
                return true;
            }
            ++li_;
            ri_ = 0;
        }
        return false;
    }

    // Item #4 (perf)：hash join 路径仅用于 INNER + 简单等值。
    if (use_hash_join_) {
        const auto& probe_buf = hj_probe_is_left_ ? left_buffer_ : right_buffer_;
        const auto& build_buf = hj_probe_is_left_ ? right_buffer_ : left_buffer_;
        while (hj_probe_idx_ < probe_buf.size()) {
            const Tuple& pt = probe_buf[hj_probe_idx_];
            std::string k = KeyOf(pt, hj_probe_is_left_ ? hj_probe_left_col_
                                                         : hj_probe_left_col_);
            if (hj_cur_bucket_pos_ == 0) {
                auto it = hj_hash_.find(k);
                if (it != hj_hash_.end()) {
                    hj_cur_bucket_ = it->second;
                } else {
                    hj_cur_bucket_.clear();
                }
            }
            while (hj_cur_bucket_pos_ < hj_cur_bucket_.size()) {
                size_t build_idx = hj_cur_bucket_[hj_cur_bucket_pos_++];
                const Tuple& bt = build_buf[build_idx];
                if (hj_probe_is_left_) {
                    if (tuple) *tuple = Concat(pt, bt);
                } else {
                    if (tuple) *tuple = Concat(bt, pt);
                }
                return true;
            }
            ++hj_probe_idx_;
            hj_cur_bucket_pos_ = 0;
        }
        return false;
    }

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
                if ((join_type_ == JoinType::RIGHT || join_type_ == JoinType::FULL_OUTER)
                    && ri_ - 1 < right_matched_.size()) {
                    right_matched_[ri_ - 1] = true;
                }
                if (join_type_ == JoinType::FULL_OUTER
                    && li_ < left_matched_.size()) {
                    left_matched_[li_] = true;
                }
                if (tuple) *tuple = joined;
                return true;
            }
        }
        // 跑完 inner 循环：当前 li_ 没有再多的 right 可匹配。
        // LEFT JOIN / FULL OUTER JOIN：若整个 li_ 一次都没成功匹配过，
        // 补一行 (left, NULL right)；否则说明它至少匹配过，直接前进到下一个 li_。
        if ((join_type_ == JoinType::LEFT || join_type_ == JoinType::FULL_OUTER)
            && !cur_left_pushed_) {
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

    // RIGHT JOIN / FULL OUTER JOIN: emit unmatched right tuples with NULL left
    if (join_type_ == JoinType::RIGHT || join_type_ == JoinType::FULL_OUTER) {
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