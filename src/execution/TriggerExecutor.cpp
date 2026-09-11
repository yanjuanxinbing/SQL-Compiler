#include "execution/TriggerExecutor.h"

#include "execution/ExpressionEvaluator.h"

#include <iostream>

namespace sqlcompiler {

namespace {

// 把 lhs 拆成 (qualifier, col)：
//   - "NEW.col"  → {"NEW", "col"}  可写
//   - "OLD.col"  → {"OLD", "col"}  只读
//   - "col"      → {"",   "col"}   普通变量；触发器体内不视为 NEW/OLD 引用
// 返回 false 表示 lhs 解析失败（异常前缀等）。
bool ParseTriggerLhs(const std::string& target,
                     std::string* qualifier,
                     std::string* column) {
    auto dot = target.find('.');
    if (dot == std::string::npos) {
        qualifier->clear();
        column->assign(target);
        return true;
    }
    qualifier->assign(target.substr(0, dot));
    column->assign(target.substr(dot + 1));
    return !qualifier->empty() && !column->empty();
}

}  // namespace

void TriggerExecutor::FireBefore(SystemCatalog* catalog,
                                 ExecutionContext* context,
                                 const std::string& table_name,
                                 TriggerTiming timing,
                                 TriggerEvent event,
                                 const std::unordered_map<std::string, size_t>& column_index_map,
                                 const std::vector<Value>* old_row,
                                 std::vector<Value>& row_values) {
    if (catalog == nullptr) return;
    auto triggers = catalog->LookupTriggers(table_name, timing, event);
    if (triggers.empty()) return;

    // 构造 frame：map<key, Value>
    // - "NEW.<col>" / "OLD.<col>" 是限定引用；
    // - 触发器内表达式可以引用未限定的 <col>，把它解析为 NEW.<col>。
    std::unordered_map<std::string, Value> frame;
    for (const auto& kv : column_index_map) {
        const std::string& key = kv.first;
        size_t idx = kv.second;
        // key 可能是 "<col>" 或 "<table>.<col>"。无论哪种都登记到裸列名 / 限定列名两个键。
        auto dot = key.find('.');
        std::string col;
        std::string tbl;
        if (dot == std::string::npos) {
            col = key;
        } else {
            tbl = key.substr(0, dot);
            col = key.substr(dot + 1);
        }
        Value v = (idx < row_values.size()) ? row_values[idx] : Value::MakeNull();
        frame["NEW." + col] = v;
        if (tbl.empty()) {
            frame[col] = v;  // 未限定别名也指向 NEW
        }
    }
    if (old_row != nullptr) {
        for (const auto& kv : column_index_map) {
            const std::string& key = kv.first;
            size_t idx = kv.second;
            auto dot = key.find('.');
            std::string col;
            if (dot == std::string::npos) {
                col = key;
            } else {
                col = key.substr(dot + 1);
            }
            Value v = (idx < old_row->size()) ? (*old_row)[idx] : Value::MakeNull();
            frame["OLD." + col] = v;
        }
    } else {
        // INSERT：OLD 各列视为 NULL
        for (const auto& kv : column_index_map) {
            auto dot = kv.first.find('.');
            std::string col = (dot == std::string::npos) ? kv.first
                                                          : kv.first.substr(dot + 1);
            frame["OLD." + col] = Value::MakeNull();
        }
    }

    // 用 frame 作 outer_bind 评估各 assignment。column_index_map 留空让所有
    // ColumnRefExpr 都走 outer_bind 路径（NEW.col / OLD.col / 裸 col 都命中 frame）。
    std::unordered_map<std::string, size_t> empty_cmap;
    ExpressionEvaluator eval(empty_cmap, context, &frame);

    for (const auto* trigger : triggers) {
        if (trigger == nullptr) continue;
        for (const auto& asg : trigger->assignments) {
            std::string q, col;
            if (!ParseTriggerLhs(asg.first, &q, &col)) continue;
            Value v = eval.Evaluate(asg.second, Tuple());
            // 只允许写回 NEW.col；OLD.col 写回 frame 但不传回 row。
            if (q == "NEW" || q.empty()) {
                std::string key = "NEW." + col;
                frame[key] = v;
                // 未限定别名也同步
                frame[col] = v;
            } else if (q == "OLD") {
                frame["OLD." + col] = v;
            }
        }
    }

    // 把 frame["NEW.<col>"] 写回 row_values，覆盖对应列下标。
    for (const auto& kv : column_index_map) {
        const std::string& key = kv.first;
        size_t idx = kv.second;
        if (idx >= row_values.size()) continue;
        auto dot = key.find('.');
        std::string col = (dot == std::string::npos) ? key : key.substr(dot + 1);
        auto it = frame.find("NEW." + col);
        if (it != frame.end()) {
            row_values[idx] = it->second;
        }
    }
}

void TriggerExecutor::FireAfter(SystemCatalog* catalog,
                                const std::string& table_name,
                                TriggerEvent event) {
    if (catalog == nullptr) return;
    auto triggers = catalog->LookupTriggers(table_name, TriggerTiming::AFTER, event);
    if (triggers.empty()) return;
    for (const auto* t : triggers) {
        if (t == nullptr) continue;
        // AFTER 触发器本期视为 no-op；只输出触发器名称便于调试。
        std::cout << "trigger fired: " << t->trigger_name << std::endl;
    }
}

}  // namespace sqlcompiler