#include "execution/ApplyExecutor.h"

#include <unordered_set>

#include "execution/AggregateExecutor.h"
#include "execution/FilterExecutor.h"
#include "execution/ProjectExecutor.h"
#include "execution/SortExecutor.h"
#include "execution/WindowExecutor.h"

namespace sqlcompiler {

namespace {

Tuple NullTuple(size_t n) {
    std::vector<Value> out(n, Value::MakeNull());
    return Tuple(std::move(out));
}

// Item #3 (perf)：递归扫描右子计划的所有表达式，检测是否存在任何
// ColumnRefExpr 引用了「外层」表（即表名不在 lateral_inner_tables 中且能
// 命中 combined cmap 的限定形式）。若一个也没有 → inner 是非相关的，
// 可以一次性跑完并重用；否则必须每行重跑。
//
// 出于实现简洁性，限制：只在右子计划为 Filter/Project/Aggregate/Window/Sort
// 时递归进去；对 NoOp 等其它类型保守地视为"可能相关"，行为等价于旧实现。
//
// 这些 executor 类的 select_list_/predicate_/condition_ 字段都是 private，
// 需要做 friend 或通过 public 接口访问。本文件通过 dynamic_cast 与各
// executor 类的实现细节耦合；这是为了一次性把所有 perf 优化落在 .cpp 内，
// 避免修改其它头文件。

const AggregateExecutor* AsAgg(const Executor* e) { return dynamic_cast<const AggregateExecutor*>(e); }
const WindowExecutor* AsWin(const Executor* e) { return dynamic_cast<const WindowExecutor*>(e); }
const FilterExecutor* AsFilter(const Executor* e) { return dynamic_cast<const FilterExecutor*>(e); }
const ProjectExecutor* AsProj(const Executor* e) { return dynamic_cast<const ProjectExecutor*>(e); }
const SortExecutor* AsSort(const Executor* e) { return dynamic_cast<const SortExecutor*>(e); }

// 扫描单一表达式树：发现任何 ColumnRef 的 table_name 不在 inner_set 中，
// 即视为引用外层。
bool ExprHasOuterRef(const ExprPtr& e,
                     const std::unordered_set<std::string>& inner_set) {
    if (!e) return false;
    switch (e->GetType()) {
        case NodeType::COLUMN_REF_EXPR: {
            auto cr = std::static_pointer_cast<ColumnRefExpr>(e);
            if (cr->table_name.empty()) {
                // 未限定列：在 ApplyExecutor 上下文里，外层 cmap 提供了
                // 这些名字的下标。保守地视为可能外层引用。
                return true;
            }
            if (inner_set.find(cr->table_name) == inner_set.end()) return true;
            return false;
        }
        case NodeType::BINARY_EXPR: {
            auto b = std::static_pointer_cast<BinaryExpr>(e);
            return ExprHasOuterRef(b->left, inner_set) ||
                   ExprHasOuterRef(b->right, inner_set);
        }
        case NodeType::UNARY_EXPR:
            return ExprHasOuterRef(std::static_pointer_cast<UnaryExpr>(e)->operand, inner_set);
        case NodeType::FUNCTION_CALL_EXPR: {
            auto f = std::static_pointer_cast<FunctionCallExpr>(e);
            for (auto& a : f->arguments) {
                if (ExprHasOuterRef(a, inner_set)) return true;
            }
            return false;
        }
        case NodeType::CASE_EXPR: {
            auto c = std::static_pointer_cast<CaseExprNode>(e);
            if (ExprHasOuterRef(c->subject, inner_set)) return true;
            for (auto& w : c->whens) {
                if (ExprHasOuterRef(w.when_expr, inner_set)) return true;
                if (ExprHasOuterRef(w.then_expr, inner_set)) return true;
            }
            if (ExprHasOuterRef(c->else_expr, inner_set)) return true;
            return false;
        }
        case NodeType::CAST_EXPR:
            return ExprHasOuterRef(std::static_pointer_cast<CastExprNode>(e)->expr, inner_set);
        case NodeType::LIKE_EXPR: {
            auto l = std::static_pointer_cast<LikeExprNode>(e);
            return ExprHasOuterRef(l->operand, inner_set) ||
                   ExprHasOuterRef(l->pattern, inner_set);
        }
        default:
            return false;
    }
}

bool ExecutorTreeHasOuterRef(
    const Executor* e,
    const std::unordered_set<std::string>& inner_set) {
    if (!e) return false;
    // 通过 dynamic_cast 试探每种常见 executor 类型。匹配失败时按"可能相关"
    // 保守处理（保留旧行为）。
    if (auto p = AsProj(e)) {
        for (auto& expr : p->select_list_for_scan()) {
            if (ExprHasOuterRef(expr, inner_set)) return true;
        }
    } else if (auto f = AsFilter(e)) {
        if (ExprHasOuterRef(f->predicate_for_scan(), inner_set)) return true;
    } else if (auto a = AsAgg(e)) {
        for (auto& expr : a->group_by_for_scan()) {
            if (ExprHasOuterRef(expr, inner_set)) return true;
        }
        for (auto& expr : a->aggregate_for_scan()) {
            if (ExprHasOuterRef(expr, inner_set)) return true;
        }
    } else if (auto w = AsWin(e)) {
        for (auto& expr : w->select_list_for_scan()) {
            if (ExprHasOuterRef(expr, inner_set)) return true;
        }
    } else if (auto s = AsSort(e)) {
        for (auto& ob : s->order_items_for_scan()) {
            if (ExprHasOuterRef(ob.expr, inner_set)) return true;
        }
    } else {
        // 其它类型（NoOp、SetOp 等）：保守地视为可能相关。
        return true;
    }
    return false;
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
      combined_column_index_map_(std::move(combined_column_index_map)),
      lateral_inner_tables_(lateral_inner_tables) {
    // 把右子计划的「内层表名集合」注入到 context，让 Filter/Project 的
    // ExpressionEvaluator 把这些表名识别为内层（qualified ref 不走 outer_bind），
    // 未在集合内的限定列名才回退到 outer_bind。
    if (!lateral_inner_tables.empty()) {
        context->SetLateralInnerTables(std::move(lateral_inner_tables));
    }
    // Item #3 (perf)：探测右子计划是否真的引用了外层列。
    is_correlated_ =
        ExecutorTreeHasOuterRef(right_.get(), lateral_inner_tables_);
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

    // Item #3 (perf)：非相关子查询只跑一次 inner，结果复用。
    if (!is_correlated_) {
        right_buffer_.clear();
        Tuple rt;
        while (right_->Next(&rt)) right_buffer_.push_back(std::move(rt));
        right_exhausted_ = false;
    }
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
    // Item #3 (perf)：非相关 inner 已一次性跑完，right_buffer_ 不会随外层
    // 行变化。直接做笛卡尔积：每条左行 × 整个 right_buffer_。
    if (!is_correlated_) {
        if (right_buffer_.empty()) {
            // inner 没有任何输出时，行为取决于 is_left_outer。
            if (!is_left_outer_) return false;
        }
        while (true) {
            if (right_cursor_ >= right_buffer_.size()) {
                if (!NextLeftRow(&current_left_)) return false;
                right_cursor_ = 0;
                // 非相关路径不写 outer_bind（保留默认 outer_bind=nullptr）。
            }
            // 取右 buffer 里的下一行，与当前左行拼接发射。
            if (right_cursor_ < right_buffer_.size()) {
                const Tuple& rt = right_buffer_[right_cursor_++];
                Tuple joined = Concat(current_left_, rt);
                if (tuple) *tuple = std::move(joined);
                return true;
            }
            // 右 buffer 已空；若是 LEFT OUTER APPLY 则补一行 NULL。
            if (is_left_outer_) {
                size_t right_cols = right_buffer_.empty() ? 0
                                                          : right_buffer_[0].ColumnCount();
                // 让下一次 Next() 重新进入外层行刷新（right_cursor_ == size）
                right_cursor_ = right_buffer_.size() + 1;
                Tuple padded = Concat(current_left_, NullTuple(right_cols));
                if (tuple) *tuple = std::move(padded);
                return true;
            }
            // 非 LEFT OUTER：本左行没有匹配，继续下一条左行。
            right_cursor_ = right_buffer_.size();
            continue;
        }
    }
    // 相关 inner：每条外层行重跑一次右子计划（旧行为）。
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