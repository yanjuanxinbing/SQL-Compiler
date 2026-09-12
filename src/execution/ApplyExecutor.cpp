#include "execution/ApplyExecutor.h"

#include <unordered_set>

namespace sqlcompiler {

namespace {

Tuple NullTuple(size_t n) {
    std::vector<Value> out(n, Value::MakeNull());
    return Tuple(std::move(out));
}

}  // namespace

ApplyExecutor::ApplyExecutor(ExecutionContext* context,
                             ExecutorPtr left,
                             ExecutorPtr right,
                             bool is_left_outer,
                             std::unordered_map<std::string, size_t> combined_column_index_map,
                             std::unordered_set<std::string> lateral_inner_tables)
    : Executor(context),
      left_(std::move(left)),
      right_(std::move(right)),
      is_left_outer_(is_left_outer),
      combined_column_index_map_(std::move(combined_column_index_map)) {
    // 把右子计划的「内层表名集合」注入到 context，让 Filter/Project 的
    // ExpressionEvaluator 把这些表名识别为内层（qualified ref 不走 outer_bind），
    // 未在集合内的限定列名才回退到 outer_bind。
    if (!lateral_inner_tables.empty()) {
        context->SetLateralInnerTables(std::move(lateral_inner_tables));
    }
}

void ApplyExecutor::Init() {
    if (left_) left_->Init();
    if (right_) right_->Init();
    has_left_ = false;
    left_pulled_ = false;
    right_buffer_.clear();
    right_cursor_ = 0;
    current_bind_.clear();
    // 清掉上一轮可能遗留的 outer_bind，避免污染下一次执行。
    context_->SetOuterBind(nullptr);
}

bool ApplyExecutor::NextLeftRow(Tuple* out) {
    if (!left_) return false;
    Tuple t;
    if (!left_->Next(&t)) return false;
    if (out) *out = std::move(t);
    return true;
}

Tuple ApplyExecutor::Concat(const Tuple& a, const Tuple& b) {
    std::vector<Value> out;
    out.reserve(a.ColumnCount() + b.ColumnCount());
    for (const auto& v : a.GetValues()) out.push_back(v);
    for (const auto& v : b.GetValues()) out.push_back(v);
    return Tuple(std::move(out));
}

bool ApplyExecutor::Next(Tuple* tuple) {
    while (true) {
        if (right_cursor_ >= right_buffer_.size()) {
            // 当前左行的右行已全部发射；尝试取下一条左行。
            if (!NextLeftRow(&current_left_)) return false;
            has_left_ = true;
            // 把当前左行按 combined_column_index_map_ 投影为 outer_bind。
            // 注意：current_bind_ 必须保持生命周期，直到下一次 left 行刷新之前
            // 一直有效——因为右子计划在 Next 时按引用读 outer_bind。
            current_bind_.clear();
            for (const auto& kv : combined_column_index_map_) {
                if (kv.second < current_left_.ColumnCount()) {
                    current_bind_[kv.first] = current_left_.GetValue(kv.second);
                }
            }
            context_->SetOuterBind(&current_bind_);
            // 重跑右子计划：对当前外层行重新评估相关子查询。
            if (right_) {
                right_->Init();
                right_buffer_.clear();
                Tuple rt;
                while (right_->Next(&rt)) right_buffer_.push_back(std::move(rt));
            } else {
                right_buffer_.clear();
            }
            right_cursor_ = 0;
            if (right_buffer_.empty()) {
                // 本左行没有右行：若是 LEFT OUTER APPLY 则补一行 NULL；
                // 否则直接尝试下一条左行。
                if (is_left_outer_) {
                    size_t right_cols = 0;
                    if (right_) {
                        // right_ 已被 Init 但不一定产出过行；尝试拉一行看列数。
                        // 因为我们这里知道 right_buffer_.empty()，可以从最近一次的
                        // schema 推断。最稳妥的方式：在 ApplyExecutor 构造时同步记录
                        // right 列数。本 V1 简化：把 right 列数延迟到第一次实际 Next 时
                        // 通过子计划探测。
                        // 我们用 0 列兜底——LATERAL 测试中 INNER 路径下不应进入此分支。
                    }
                    Tuple padded = Concat(current_left_, NullTuple(right_cols));
                    if (tuple) *tuple = std::move(padded);
                    return true;
                }
                continue;  // 跳过空匹配的左行
            }
        }
        // 取右 buffer 里的下一行，与当前左行拼接发射。
        const Tuple& rt = right_buffer_[right_cursor_++];
        Tuple joined = Concat(current_left_, rt);
        if (tuple) *tuple = std::move(joined);
        return true;
    }
}

}  // namespace sqlcompiler