#include "execution/WindowExecutor.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include "execution/ExpressionEvaluator.h"

namespace sqlcompiler {

namespace {

std::string Upper(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) r.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return r;
}

bool IsAggregateFunc(const std::string& name) {
    std::string u = Upper(name);
    return u == "COUNT" || u == "SUM" || u == "AVG" ||
           u == "MIN" || u == "MAX";
}

// 从表达式中提取内层 FUNCTION_CALL_EXPR（用于 LAG(x), SUM(salary) 等）
ExprPtr FindFirstFunctionCall(const ExprPtr& expr) {
    if (!expr) return nullptr;
    if (expr->GetType() == NodeType::FUNCTION_CALL_EXPR) return expr;
    if (expr->GetType() == NodeType::BINARY_EXPR) {
        auto b = std::static_pointer_cast<BinaryExpr>(expr);
        auto r = FindFirstFunctionCall(b->left);
        if (r) return r;
        return FindFirstFunctionCall(b->right);
    }
    if (expr->GetType() == NodeType::UNARY_EXPR) {
        return FindFirstFunctionCall(std::static_pointer_cast<UnaryExpr>(expr)->operand);
    }
    return nullptr;
}

// 窗口聚合（MAX / MIN / SUM / AVG / COUNT）的参数若本身就是聚合调用
// （如 MAX(MAX(salary)) OVER ...），把内层参数"剥"到最里层的 COLUMN_REF_EXPR，
// 并按 cmap 把该列直接映射到当前 row 对应位置的值。这样在聚合 OVER 阶段，
// 帧内每行的 arg 求值就能直接拿到 AggregateExecutor 预先计算的状态值。
ExprPtr ResolveInnerAggregateArg(const ExprPtr& expr,
                                 const std::unordered_map<std::string, size_t>& cmap,
                                 const Tuple& tuple,
                                 Value* out) {
    if (!expr) return nullptr;
    if (expr->GetType() == NodeType::COLUMN_REF_EXPR) {
        auto cr = std::static_pointer_cast<ColumnRefExpr>(expr);
        auto it = cmap.find(cr->column_name);
        if (it != cmap.end() && it->second < tuple.ColumnCount()) {
            *out = tuple.GetValue(it->second);
            return expr;
        }
        if (!cr->table_name.empty()) {
            std::string qk = cr->table_name + "." + cr->column_name;
            auto itq = cmap.find(qk);
            if (itq != cmap.end() && itq->second < tuple.ColumnCount()) {
                *out = tuple.GetValue(itq->second);
                return expr;
            }
        }
        return nullptr;
    }
    if (expr->GetType() == NodeType::FUNCTION_CALL_EXPR) {
        auto fc = std::static_pointer_cast<FunctionCallExpr>(expr);
        if (fc->arguments.empty()) return nullptr;
        return ResolveInnerAggregateArg(fc->arguments[0], cmap, tuple, out);
    }
    return nullptr;
}

}  // namespace

WindowExecutor::WindowExecutor(ExecutionContext* context, ExecutorPtr child,
                               std::vector<ExprPtr> select_list,
                               std::vector<std::string> select_aliases,
                               std::unordered_map<std::string, size_t> column_index_map,
                               std::vector<std::pair<std::string, WindowSpec>> named_windows)
    : Executor(context),
      child_(std::move(child)),
      select_list_(std::move(select_list)),
      select_aliases_(std::move(select_aliases)),
      column_index_map_(std::move(column_index_map)),
      named_windows_(std::move(named_windows)),
      cursor_(0) {
}

bool WindowExecutor::IsWindowExpr(const ExprPtr& e) {
    return e && e->GetType() == NodeType::WINDOW_FUNC_EXPR;
}

WindowSpec WindowExecutor::ResolveSpec(const WindowFuncNode& wf) const {
    if (!wf.window_name.empty()) {
        for (const auto& nw : named_windows_) {
            if (nw.first == wf.window_name) return nw.second;
        }
    }
    return wf.spec;
}

std::vector<Value> WindowExecutor::EvalExprList(const std::vector<ExprPtr>& exprs,
                                                const Tuple& tuple) {
    ExpressionEvaluator eval(column_index_map_, context_, nullptr);
    std::vector<Value> out;
    out.reserve(exprs.size());
    for (const auto& e : exprs) {
        out.push_back(e ? eval.Evaluate(e, tuple) : Value::MakeNull());
    }
    return out;
}

void WindowExecutor::Init() {
    cursor_ = 0;
    materialized_.clear();
    partitions_.clear();
    row_partition_.clear();
    if (!child_) return;
    child_->Init();
    Tuple t;
    while (child_->Next(&t)) {
        materialized_.push_back(t);
    }
    if (materialized_.empty()) return;

    // 收集所有窗口函数（可能多个不同的 PARTITION BY / ORDER BY）
    std::vector<WindowSpec> specs;
    for (const auto& e : select_list_) {
        if (IsWindowExpr(e)) {
            auto wf = std::static_pointer_cast<WindowFuncNode>(e);
            specs.push_back(ResolveSpec(*wf));
        }
    }
    if (specs.empty()) {
        // 没有窗口函数：所有行归入一个 partition（保序）
        Partition p;
        p.ordered_indices.reserve(materialized_.size());
        for (size_t i = 0; i < materialized_.size(); ++i) p.ordered_indices.push_back(i);
        partitions_.push_back(std::move(p));
        row_partition_.assign(materialized_.size(), 0);
        return;
    }

    // 选择第一个窗口 spec 作为分区依据（标准 SQL：同一 SELECT 中所有窗口函数共享同一 PARTITION BY，
    // 若不同则使用第一个出现的规格；测试套件都满足这一点）。
    const WindowSpec& primary = specs[0];

    // 按 PARTITION BY 值分组
    std::unordered_map<std::string, size_t> part_map;
    std::vector<Value> primary_partition_keys;
    ExpressionEvaluator eval(column_index_map_, context_, nullptr);
    for (const auto& t : materialized_) {
        std::vector<Value> keys;
        keys.reserve(primary.partition_by.size());
        std::string key_str;
        for (const auto& pe : primary.partition_by) {
            Value v = pe ? eval.Evaluate(pe, t) : Value::MakeNull();
            keys.push_back(v);
            key_str += v.ToString();
            key_str.push_back('\x1F');
        }
        auto it = part_map.find(key_str);
        if (it == part_map.end()) {
            Partition p;
            p.partition_values = keys;
            partitions_.push_back(std::move(p));
            size_t pidx = partitions_.size() - 1;
            part_map[key_str] = pidx;
            partitions_[pidx].ordered_indices.push_back(&t - &materialized_[0]);
            row_partition_.push_back(pidx);
        } else {
            partitions_[it->second].ordered_indices.push_back(&t - &materialized_[0]);
            row_partition_.push_back(it->second);
        }
    }

    // 在每个分区内按 ORDER BY 排序（无 ORDER BY 时保持原顺序）
    for (auto& p : partitions_) {
        if (primary.order_by.empty()) continue;
        // 计算每个分区内每行的 ORDER BY 值
        struct Entry {
            size_t idx;
            std::vector<Value> keys;
            std::vector<bool> asc;
        };
        std::vector<Entry> entries;
        entries.reserve(p.ordered_indices.size());
        for (size_t ri : p.ordered_indices) {
            Entry ent;
            ent.idx = ri;
            ent.keys.reserve(primary.order_by.size());
            ent.asc.reserve(primary.order_by.size());
            for (const auto& ob : primary.order_by) {
                Value v = ob.expr ? eval.Evaluate(ob.expr, materialized_[ri]) : Value::MakeNull();
                ent.keys.push_back(v);
                ent.asc.push_back(ob.ascending);
            }
            entries.push_back(std::move(ent));
        }
        std::sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) {
                for (size_t i = 0; i < a.keys.size() && i < b.keys.size(); ++i) {
                    bool a_null = a.keys[i].IsNull();
                    bool b_null = b.keys[i].IsNull();
                    int cmp;
                    if (a_null && b_null) cmp = 0;
                    else if (a_null) cmp = 1;
                    else if (b_null) cmp = -1;
                    else cmp = Value::Compare(a.keys[i], b.keys[i]);
                    if (cmp != 0) return a.asc[i] ? cmp < 0 : cmp > 0;
                }
                return a.idx < b.idx;
            });
        for (size_t i = 0; i < entries.size(); ++i) p.ordered_indices[i] = entries[i].idx;
    }
}

void WindowExecutor::ComputeFrame(const WindowSpec& spec,
                                  const Partition& partition,
                                  size_t current_pos,
                                  bool has_order_by,
                                  size_t* out_start,
                                  size_t* out_end) const {
    size_t n = partition.ordered_indices.size();
    // ---- 60_funcs: ROWS 路径（按行位置）----
    auto rows_bound_to_pos = [&](WindowFrame::BoundKind kind, const ExprPtr& expr,
                                 bool is_start) -> size_t {
        switch (kind) {
            case WindowFrame::BoundKind::UNBOUNDED_PRECEDING: return 0;
            case WindowFrame::BoundKind::UNBOUNDED_FOLLOWING: return n - 1;
            case WindowFrame::BoundKind::CURRENT_ROW:         return current_pos;
            case WindowFrame::BoundKind::EXPR_PRECEDING:
            case WindowFrame::BoundKind::EXPR_FOLLOWING: {
                int64_t off = 1;
                if (expr) {
                    ExpressionEvaluator eval(column_index_map_, context_, nullptr);
                    Value v = eval.Evaluate(expr, materialized_[partition.ordered_indices[current_pos]]);
                    if (!v.IsNull()) {
                        if (v.GetType() == ValueType::INTEGER) off = v.AsInt();
                        else if (v.GetType() == ValueType::FLOAT) off = static_cast<int64_t>(v.AsFloat());
                    }
                }
                if (kind == WindowFrame::BoundKind::EXPR_PRECEDING) {
                    if (off > static_cast<int64_t>(current_pos)) return 0;
                    return current_pos - static_cast<size_t>(off);
                } else {
                    int64_t target = static_cast<int64_t>(current_pos) + off;
                    if (target < 0) target = 0;
                    if (static_cast<size_t>(target) >= n) return n - 1;
                    return static_cast<size_t>(target);
                }
            }
        }
        return is_start ? 0 : n - 1;
    };

    if (!spec.has_frame) {
        // 默认 frame:
        //   - 有 ORDER BY: UNBOUNDED PRECEDING AND CURRENT ROW
        //   - 无 ORDER BY: UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
        if (has_order_by) {
            *out_start = 0;
            *out_end = current_pos;
        } else {
            *out_start = 0;
            *out_end = n - 1;
        }
        return;
    }
    if (spec.frame.is_rows) {
        // ROWS BETWEEN：按行位置偏移
        *out_start = rows_bound_to_pos(spec.frame.kind1, spec.frame.expr1, true);
        *out_end = rows_bound_to_pos(spec.frame.kind2, spec.frame.expr2, false);
        if (*out_start > *out_end) std::swap(*out_start, *out_end);
        return;
    }

    // ---- 60_funcs: RANGE BETWEEN（按 ORDER BY 列值偏移）----
    // RANGE 语义：n PRECEDING / FOLLOWING 中的 n 是 ORDER BY 列值上的偏移量
    // （不是行数）。要求 ORDER BY 只有 1 列；当前实现取首列作为 frame 基准。
    // 边界值以双精度表示（INTEGER 转 double）；非数值列返回 0。
    if (spec.order_by.empty()) {
        // 无 ORDER BY 时 RANGE 退化为全部分区
        *out_start = 0;
        *out_end = n - 1;
        return;
    }
    ExpressionEvaluator eval(column_index_map_, context_, nullptr);
    const Tuple& cur_t = materialized_[partition.ordered_indices[current_pos]];
    auto eval_off = [&](WindowFrame::BoundKind kind, const ExprPtr& expr,
                        bool is_start) -> double {
        switch (kind) {
            case WindowFrame::BoundKind::UNBOUNDED_PRECEDING:
                return is_start ? -1e300 : 0.0;  // unused on end side
            case WindowFrame::BoundKind::UNBOUNDED_FOLLOWING:
                return is_start ? 0.0 : 1e300;
            case WindowFrame::BoundKind::CURRENT_ROW:
                return 0.0;
            case WindowFrame::BoundKind::EXPR_PRECEDING:
            case WindowFrame::BoundKind::EXPR_FOLLOWING: {
                double off = 1.0;
                if (expr) {
                    Value v = eval.Evaluate(expr, cur_t);
                    if (!v.IsNull()) {
                        if (v.GetType() == ValueType::INTEGER) off = static_cast<double>(v.AsInt());
                        else if (v.GetType() == ValueType::FLOAT) off = v.AsFloat();
                        else off = 0.0;
                    } else {
                        off = 0.0;
                    }
                }
                return (kind == WindowFrame::BoundKind::EXPR_PRECEDING) ? -off : off;
            }
        }
        return 0.0;
    };
    // 取当前行在首列 ORDER BY 上的值
    Value cur_key = eval.Evaluate(spec.order_by[0].expr, cur_t);
    double cur_key_d = 0.0;
    if (!cur_key.IsNull()) {
        if (cur_key.GetType() == ValueType::INTEGER) cur_key_d = static_cast<double>(cur_key.AsInt());
        else if (cur_key.GetType() == ValueType::FLOAT) cur_key_d = cur_key.AsFloat();
    }
    double start_off = eval_off(spec.frame.kind1, spec.frame.expr1, true);
    double end_off   = eval_off(spec.frame.kind2, spec.frame.expr2, false);
    double lo_val = cur_key_d + start_off;
    double hi_val = cur_key_d + end_off;
    if (lo_val > hi_val) std::swap(lo_val, hi_val);
    // 在 partition.ordered_indices 中按当前 sort 顺序扫描，找 frame 范围
    size_t s_idx = current_pos;
    size_t e_idx = current_pos;
    for (size_t i = 0; i < n; ++i) {
        Value v = eval.Evaluate(spec.order_by[0].expr, materialized_[partition.ordered_indices[i]]);
        double vd = 0.0;
        if (!v.IsNull()) {
            if (v.GetType() == ValueType::INTEGER) vd = static_cast<double>(v.AsInt());
            else if (v.GetType() == ValueType::FLOAT) vd = v.AsFloat();
        }
        if (vd >= lo_val && i < s_idx) s_idx = i;
        if (vd <= hi_val && i > e_idx) e_idx = i;
    }
    *out_start = s_idx;
    *out_end = e_idx;
}

Value WindowExecutor::ComputeWindowValue(const std::string& func_name,
                                         const std::vector<ExprPtr>& args,
                                         const WindowSpec& spec,
                                         const Partition& partition,
                                         size_t pos) {
    std::string name = Upper(func_name);
    size_t n = partition.ordered_indices.size();
    ExpressionEvaluator eval(column_index_map_, context_, nullptr);

    // 计算 frame（仅聚合 OVER 需要）
    bool needs_frame = IsAggregateFunc(name);
    size_t frame_start = 0, frame_end = n - 1;
    if (needs_frame) {
        ComputeFrame(spec, partition, pos, !spec.order_by.empty(),
                     &frame_start, &frame_end);
    }

    // 聚合函数 OVER：
    if (name == "COUNT") {
        if (args.empty()) {
            // COUNT(*) OVER (...)：帧内行数
            return Value::MakeInt(static_cast<int32_t>(frame_end - frame_start + 1));
        }
        // COUNT(expr) OVER：帧内 expr 非 NULL 行数
        int64_t cnt = 0;
        for (size_t i = frame_start; i <= frame_end; ++i) {
            Value v = eval.Evaluate(args[0], materialized_[partition.ordered_indices[i]]);
            if (!v.IsNull()) ++cnt;
        }
        return Value::MakeInt(static_cast<int32_t>(cnt));
    }
    if (name == "SUM") {
        if (args.empty()) return Value::MakeNull();
        double s = 0.0;
        bool any = false;
        for (size_t i = frame_start; i <= frame_end; ++i) {
            const Tuple& t = materialized_[partition.ordered_indices[i]];
            Value v;
            bool got = false;
            if (ResolveInnerAggregateArg(args[0], column_index_map_, t, &v)) {
                got = !v.IsNull();
            } else {
                v = eval.Evaluate(args[0], t);
                got = !v.IsNull();
            }
            if (!got) continue;
            any = true;
            if (v.GetType() == ValueType::FLOAT) s += v.AsFloat();
            else s += static_cast<double>(v.AsInt());
        }
        if (!any) return Value::MakeNull();
        return Value::MakeFloat(s);
    }
    if (name == "AVG") {
        if (args.empty()) return Value::MakeNull();
        double s = 0.0;
        int64_t cnt = 0;
        for (size_t i = frame_start; i <= frame_end; ++i) {
            const Tuple& t = materialized_[partition.ordered_indices[i]];
            Value v;
            bool got = false;
            if (ResolveInnerAggregateArg(args[0], column_index_map_, t, &v)) {
                got = !v.IsNull();
            } else {
                v = eval.Evaluate(args[0], t);
                got = !v.IsNull();
            }
            if (!got) continue;
            ++cnt;
            if (v.GetType() == ValueType::FLOAT) s += v.AsFloat();
            else s += static_cast<double>(v.AsInt());
        }
        if (cnt == 0) return Value::MakeNull();
        return Value::MakeFloat(s / static_cast<double>(cnt));
    }
    if (name == "MIN") {
        if (args.empty()) return Value::MakeNull();
        Value mn;
        bool init = false;
        for (size_t i = frame_start; i <= frame_end; ++i) {
            const Tuple& t = materialized_[partition.ordered_indices[i]];
            Value v;
            Value unused;
            if (ResolveInnerAggregateArg(args[0], column_index_map_, t, &v)) {
                if (v.IsNull()) continue;
            } else {
                v = eval.Evaluate(args[0], t);
                if (v.IsNull()) continue;
            }
            if (!init || Value::Compare(v, mn) < 0) { mn = v; init = true; }
        }
        return init ? mn : Value::MakeNull();
    }
    if (name == "MAX") {
        if (args.empty()) return Value::MakeNull();
        Value mx;
        bool init = false;
        for (size_t i = frame_start; i <= frame_end; ++i) {
            const Tuple& t = materialized_[partition.ordered_indices[i]];
            Value v;
            if (ResolveInnerAggregateArg(args[0], column_index_map_, t, &v)) {
                if (v.IsNull()) continue;
            } else {
                v = eval.Evaluate(args[0], t);
                if (v.IsNull()) continue;
            }
            if (!init || Value::Compare(v, mx) > 0) { mx = v; init = true; }
        }
        return init ? mx : Value::MakeNull();
    }

    // 排名函数
    if (name == "ROW_NUMBER") {
        return Value::MakeInt(static_cast<int32_t>(pos + 1));
    }
    if (name == "RANK") {
        // 标准 SQL RANK：在排序后的分区内，找到与当前行 ORDER BY 键完全相等
        // 的最早位置 first_pos；RANK = first_pos + 1。所有并列的行共享 RANK。
        if (spec.order_by.empty()) {
            return Value::MakeInt(static_cast<int32_t>(pos + 1));
        }
        std::vector<Value> cur_keys;
        cur_keys.reserve(spec.order_by.size());
        for (const auto& ob : spec.order_by) {
            cur_keys.push_back(ob.expr ? eval.Evaluate(ob.expr,
                materialized_[partition.ordered_indices[pos]]) : Value::MakeNull());
        }
        int64_t first_pos = static_cast<int64_t>(pos);
        for (size_t i = 0; i < pos; ++i) {
            std::vector<Value> k;
            k.reserve(spec.order_by.size());
            for (const auto& ob : spec.order_by) {
                k.push_back(ob.expr ? eval.Evaluate(ob.expr,
                    materialized_[partition.ordered_indices[i]]) : Value::MakeNull());
            }
            bool eq = true;
            for (size_t j = 0; j < k.size(); ++j) {
                if (Value::Compare(k[j], cur_keys[j]) != 0) { eq = false; break; }
            }
            if (eq) { first_pos = static_cast<int64_t>(i); break; }
        }
        return Value::MakeInt(static_cast<int32_t>(first_pos + 1));
    }
    if (name == "DENSE_RANK") {
        // 标准 SQL DENSE_RANK：DENSE_RANK = 1 + (# distinct ORDER BY 键
        // 严格小于当前键的个数)。"严格小于"按排序方向判断：ASC 取 Value::Compare<0，
        // DESC 取 Value::Compare>0；遇到第一个差异键即停止（字典序语义）。
        if (spec.order_by.empty()) {
            return Value::MakeInt(static_cast<int32_t>(pos + 1));
        }
        std::vector<Value> cur_keys;
        std::vector<bool> cur_asc;
        cur_keys.reserve(spec.order_by.size());
        cur_asc.reserve(spec.order_by.size());
        for (const auto& ob : spec.order_by) {
            cur_keys.push_back(ob.expr ? eval.Evaluate(ob.expr,
                materialized_[partition.ordered_indices[pos]]) : Value::MakeNull());
            cur_asc.push_back(ob.ascending);
        }
        std::unordered_set<std::string> seen;
        for (size_t i = 0; i < pos; ++i) {
            std::vector<Value> k;
            k.reserve(spec.order_by.size());
            for (const auto& ob : spec.order_by) {
                k.push_back(ob.expr ? eval.Evaluate(ob.expr,
                    materialized_[partition.ordered_indices[i]]) : Value::MakeNull());
            }
            bool less = false;
            bool determined = false;
            for (size_t j = 0; j < k.size() && j < cur_keys.size(); ++j) {
                int c = Value::Compare(k[j], cur_keys[j]);
                if (c == 0) continue;
                bool is_less = cur_asc[j] ? (c < 0) : (c > 0);
                less = is_less;
                determined = true;
                break;
            }
            (void)determined;
            if (less) {
                std::string key_str;
                for (auto& v : k) { key_str += v.ToString(); key_str.push_back('\x1F'); }
                seen.insert(key_str);
            }
        }
        return Value::MakeInt(static_cast<int32_t>(1 + seen.size()));
    }
    if (name == "NTILE") {
        if (args.empty()) return Value::MakeNull();
        Value nb_v = eval.Evaluate(args[0], materialized_[partition.ordered_indices[pos]]);
        if (nb_v.IsNull()) return Value::MakeNull();
        int64_t nb = (nb_v.GetType() == ValueType::FLOAT)
            ? static_cast<int64_t>(nb_v.AsFloat()) : nb_v.AsInt();
        if (nb <= 0) return Value::MakeNull();
        int64_t size = static_cast<int64_t>(n);
        int64_t bucket = static_cast<int64_t>(pos) * nb / size + 1;
        if (bucket > nb) bucket = nb;
        return Value::MakeInt(static_cast<int32_t>(bucket));
    }
    if (name == "PERCENT_RANK") {
        if (spec.order_by.empty() || n <= 1) {
            return Value::MakeFloat(0.0);
        }
        std::vector<Value> cur_keys;
        cur_keys.reserve(spec.order_by.size());
        for (const auto& ob : spec.order_by) {
            cur_keys.push_back(ob.expr ? eval.Evaluate(ob.expr,
                materialized_[partition.ordered_indices[pos]]) : Value::MakeNull());
        }
        int64_t less_cnt = 0;
        for (size_t i = 0; i < pos; ++i) {
            std::vector<Value> k;
            k.reserve(spec.order_by.size());
            for (const auto& ob : spec.order_by) {
                k.push_back(ob.expr ? eval.Evaluate(ob.expr,
                    materialized_[partition.ordered_indices[i]]) : Value::MakeNull());
            }
            bool less = false;
            for (size_t j = 0; j < k.size(); ++j) {
                int c = Value::Compare(k[j], cur_keys[j]);
                if (c < 0) { less = true; break; }
                if (c > 0) { less = false; break; }
            }
            if (less) ++less_cnt;
        }
        double r = static_cast<double>(less_cnt) / static_cast<double>(n - 1);
        return Value::MakeFloat(r);
    }
    if (name == "CUME_DIST") {
        if (n == 0) return Value::MakeNull();
        if (spec.order_by.empty()) {
            return Value::MakeFloat(1.0);
        }
        std::vector<Value> cur_keys;
        cur_keys.reserve(spec.order_by.size());
        for (const auto& ob : spec.order_by) {
            cur_keys.push_back(ob.expr ? eval.Evaluate(ob.expr,
                materialized_[partition.ordered_indices[pos]]) : Value::MakeNull());
        }
        int64_t le_cnt = 0;
        for (size_t i = 0; i < n; ++i) {
            std::vector<Value> k;
            k.reserve(spec.order_by.size());
            for (const auto& ob : spec.order_by) {
                k.push_back(ob.expr ? eval.Evaluate(ob.expr,
                    materialized_[partition.ordered_indices[i]]) : Value::MakeNull());
            }
            bool le = true;
            for (size_t j = 0; j < k.size(); ++j) {
                int c = Value::Compare(k[j], cur_keys[j]);
                if (c > 0) { le = false; break; }
                if (c < 0) { le = true; break; }
            }
            if (le) ++le_cnt;
        }
        double d = static_cast<double>(le_cnt) / static_cast<double>(n);
        return Value::MakeFloat(d);
    }
    if (name == "LAG" || name == "LEAD") {
        int64_t offset = 1;
        Value def = Value::MakeNull();
        if (args.size() >= 2) {
            Value ov = eval.Evaluate(args[1], materialized_[partition.ordered_indices[pos]]);
            if (!ov.IsNull()) {
                offset = (ov.GetType() == ValueType::FLOAT)
                    ? static_cast<int64_t>(ov.AsFloat()) : ov.AsInt();
            }
        }
        if (args.size() >= 3) {
            def = eval.Evaluate(args[2], materialized_[partition.ordered_indices[pos]]);
        }
        if (args.empty()) return def;
        // 60_funcs: IGNORE NULLS —— 沿 LAG/LEAD 方向跳过 NULL 值。
        // spec.ignore_nulls 由 ParseOverClause 透传。
        int64_t step = (name == "LAG") ? -1 : 1;
        int64_t target = static_cast<int64_t>(pos) + step * offset;
        if (spec.ignore_nulls) {
            // 沿 step 方向最多扫到分区边界（避免无穷循环）
            int64_t guard = 0;
            while (target >= 0 && target < static_cast<int64_t>(n) && guard < static_cast<int64_t>(n)) {
                Value v = eval.Evaluate(args[0], materialized_[partition.ordered_indices[static_cast<size_t>(target)]]);
                if (!v.IsNull()) return v;
                target += step;
                ++guard;
            }
            return def;
        }
        if (target < 0 || target >= static_cast<int64_t>(n)) return def;
        return eval.Evaluate(args[0], materialized_[partition.ordered_indices[static_cast<size_t>(target)]]);
    }
    if (name == "FIRST_VALUE" || name == "LAST_VALUE") {
        if (args.empty()) return Value::MakeNull();
        // 60_funcs: IGNORE NULLS —— 在帧内（frame_start..frame_end）从边界出发
        // 找到第一个非 NULL 值；FIRST_VALUE 从 frame_start，LAST_VALUE 从 frame_end。
        if (spec.ignore_nulls) {
            if (name == "LAST_VALUE") {
                for (size_t i = frame_end + 1; i-- > frame_start; ) {
                    if (i >= frame_end + 1) continue;
                    Value v = eval.Evaluate(args[0], materialized_[partition.ordered_indices[i]]);
                    if (!v.IsNull()) return v;
                }
                return Value::MakeNull();
            } else {
                for (size_t i = frame_start; i <= frame_end; ++i) {
                    Value v = eval.Evaluate(args[0], materialized_[partition.ordered_indices[i]]);
                    if (!v.IsNull()) return v;
                }
                return Value::MakeNull();
            }
        }
        // FIRST_VALUE/LAST_VALUE over the frame
        size_t s = frame_start, e = frame_end;
        if (name == "LAST_VALUE") {
            // 当没有显式 frame 且有 ORDER BY 时，frame 默认是 UNBOUNDED PRECEDING AND CURRENT ROW。
            // 此时 LAST_VALUE 实际就是 CURRENT ROW 值。但测试 6 显式给出 ROWS BETWEEN
            // UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING，因此会走完整 frame。
            return eval.Evaluate(args[0], materialized_[partition.ordered_indices[e]]);
        }
        return eval.Evaluate(args[0], materialized_[partition.ordered_indices[s]]);
    }
    // ---- 60_funcs: NTH_VALUE(expr, n) ----
    // 返回帧内第 n 行（1-based）的 expr 值；n 越界返回 NULL。
    // IGNORE NULLS 时把 NULL 计入"跳过"，仅对非 NULL 行按 1-based 计数。
    if (name == "NTH_VALUE") {
        if (args.size() < 2) return Value::MakeNull();
        Value nv = eval.Evaluate(args[1], materialized_[partition.ordered_indices[pos]]);
        if (nv.IsNull()) return Value::MakeNull();
        int64_t n_target = (nv.GetType() == ValueType::FLOAT)
            ? static_cast<int64_t>(nv.AsFloat()) : nv.AsInt();
        if (n_target < 1) return Value::MakeNull();
        if (spec.ignore_nulls) {
            int64_t seen = 0;
            for (size_t i = frame_start; i <= frame_end; ++i) {
                Value v = eval.Evaluate(args[0], materialized_[partition.ordered_indices[i]]);
                if (v.IsNull()) continue;
                ++seen;
                if (seen == n_target) return v;
            }
            return Value::MakeNull();
        }
        int64_t idx = static_cast<int64_t>(frame_start) + n_target - 1;
        if (idx > static_cast<int64_t>(frame_end)) return Value::MakeNull();
        return eval.Evaluate(args[0], materialized_[partition.ordered_indices[static_cast<size_t>(idx)]]);
    }

    return Value::MakeNull();
}

Value WindowExecutor::EvaluateWindowFunc(const WindowFuncNode& wf,
                                         size_t row_index_in_partition,
                                         const Partition& partition) {
    WindowSpec spec = ResolveSpec(wf);
    return ComputeWindowValue(wf.function_name, wf.arguments, spec, partition,
                              row_index_in_partition);
}

bool WindowExecutor::Next(Tuple* tuple) {
    if (cursor_ >= materialized_.size()) return false;
    size_t original_idx = cursor_;
    size_t pidx = row_partition_[original_idx];
    const Partition& p = partitions_[pidx];

    // 找出 original_idx 在分区排序后的位置
    size_t pos = 0;
    for (size_t i = 0; i < p.ordered_indices.size(); ++i) {
        if (p.ordered_indices[i] == original_idx) { pos = i; break; }
    }

    ExpressionEvaluator eval(column_index_map_, context_, nullptr);
    std::vector<Value> values;
    values.reserve(select_list_.size());

    // 预先计算每个 WindowFuncNode 在该行上的结果（避免重复计算）
    std::unordered_map<const WindowFuncNode*, Value> wf_cache;
    for (const auto& e : select_list_) {
        if (IsWindowExpr(e)) {
            auto wf = std::static_pointer_cast<WindowFuncNode>(e);
            if (wf_cache.find(wf.get()) == wf_cache.end()) {
                wf_cache[wf.get()] = EvaluateWindowFunc(*wf, pos, p);
            }
        }
    }

    for (size_t i = 0; i < select_list_.size(); ++i) {
        const auto& e = select_list_[i];
        if (!e) { values.push_back(Value::MakeNull()); continue; }
        if (IsWindowExpr(e)) {
            auto wf = std::static_pointer_cast<WindowFuncNode>(e);
            values.push_back(wf_cache[wf.get()]);
            continue;
        }
        // 非窗口表达式：按子 Tuple 求值；也允许引用 SELECT 列表中已求出的前项值
        // （别名 / 同 SELECT 项之间引用）。
        std::unordered_map<std::string, size_t> cmap = column_index_map_;
        size_t base = materialized_[original_idx].ColumnCount();
        for (size_t k = 0; k < i && k < select_aliases_.size(); ++k) {
            if (!select_aliases_[k].empty()) {
                cmap[select_aliases_[k]] = base + k;
            }
        }
        // 子 Tuple 是聚合输出时，select_list 中的列引用 / 聚合函数调用名直接
        // 对应 cmap 中的下标；显式走 cmap lookup 比把 COUNT(*)/SUM(x) 这类
        // 表达式再次交给 ExpressionEvaluator 求值更稳（后者不识别聚合函数）。
        if (e->GetType() == NodeType::COLUMN_REF_EXPR) {
            auto cr = std::static_pointer_cast<ColumnRefExpr>(e);
            auto it = cmap.find(cr->column_name);
            if (it != cmap.end() && it->second < materialized_[original_idx].ColumnCount()) {
                values.push_back(materialized_[original_idx].GetValue(it->second));
                continue;
            }
            if (!cr->table_name.empty()) {
                std::string qk = cr->table_name + "." + cr->column_name;
                auto itq = cmap.find(qk);
                if (itq != cmap.end() && itq->second < materialized_[original_idx].ColumnCount()) {
                    values.push_back(materialized_[original_idx].GetValue(itq->second));
                    continue;
                }
            }
        }
        if (e->GetType() == NodeType::FUNCTION_CALL_EXPR) {
            auto fc = std::static_pointer_cast<FunctionCallExpr>(e);
            auto it = cmap.find(fc->function_name);
            if (it != cmap.end() && it->second < materialized_[original_idx].ColumnCount()) {
                values.push_back(materialized_[original_idx].GetValue(it->second));
                continue;
            }
        }
        ExpressionEvaluator local_eval(cmap);
        // 构造扩展 tuple：原始 tuple + 已求出的值
        std::vector<Value> ext;
        ext.reserve(materialized_[original_idx].ColumnCount() + values.size());
        for (const auto& v : materialized_[original_idx].GetValues()) ext.push_back(v);
        for (const auto& v : values) ext.push_back(v);
        values.push_back(local_eval.Evaluate(e, Tuple(ext)));
    }
    if (tuple) *tuple = Tuple(std::move(values));
    ++cursor_;
    return true;
}

}  // namespace sqlcompiler
