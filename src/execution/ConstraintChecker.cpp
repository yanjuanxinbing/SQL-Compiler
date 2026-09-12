#include "execution/ConstraintChecker.h"

#include "common/Error.h"
#include "execution/ExpressionEvaluator.h"
#include "index/BPlusTree.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>

#include "index/IndexKey.h"

// ============================================================================
// CHECK / DEFAULT 执行期语义说明（58_constraints 起）
// ----------------------------------------------------------------------------
// 已强制（覆盖 INSERT / UPDATE / INSERT ... SELECT / 多行 VALUES /
// INSERT ... ON DUPLICATE KEY UPDATE / UPDATE FROM 全路径）：
//   - 列级 CHECK (expr) 写入前被求值；求值结果为 FALSE 时抛
//     "check constraint violated: <table>.<col> (<expr>)"。当用户用
//     "CONSTRAINT name CHECK (...)" 显式命名时，错误消息改为
//     "check constraint violated: <table>.<name>"。
//   - 表级 CHECK (expr) 写入前被求值（与列级 CHECK 同样的三值逻辑）。位置在
//     列级 CHECK 之后、PRIMARY KEY 唯一性之前。错误消息形式：
//       - 命名约束： "check constraint violated: <table>.<constraint_name>"
//       - 匿名约束： "check constraint violated: <table>.check_<index>"
//     表级 CHECK 与列级 CHECK 同时存在时各自独立求值；任一为 FALSE 即拒绝。
//   - NOT NULL 由 ConstraintChecker 第 1 段统一校验；五种写入路径
//     （VALUES / VALUES-多行 / INSERT-SELECT / UPDATE / UPSERT 改写）都已
//     接入该入口，无需在执行器里各自再补一遍。
//   - DEFAULT 仅支持字面量（INT/FLOAT/STRING/NULL），由 InsertExecutor 在
//     写入前替换。函数调用 / 子查询 / 复杂表达式会在执行期抛
//     "default expression not supported"。
//
// 三值逻辑（NULL-pass）：
//   - 求值结果为 NULL（UNKNOWN）→ CHECK 通过，不拒绝写入。
//   - 求值结果为 TRUE             → CHECK 通过。
//   - 求值结果为 FALSE            → 拒绝并按上述错误文案上报。
//
// 未强制（scope-cut）：
//   - 跨行 CHECK（如 `CHECK ((SELECT COUNT(*) FROM t) < N)` 形式）：不在本期。
//   - ALTER TABLE ... ADD CONSTRAINT ... 也不在本期；命名约束只能在 CREATE
//     TABLE 时一次声明。
// ============================================================================

namespace sqlcompiler {

namespace {

// 统计 UTF-8 字符数（而非字节数）。VARCHAR(N) 的 N 按字符计，
// 否则 23_chinese_idents 这类用例里一个汉字会被算成 3 个长度单位。
size_t Utf8Length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) {
        // continuation byte 形如 10xxxxxx，不计入字符数
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

bool IsStringType(ValueType t) { return t == ValueType::VARCHAR; }

// SQL 标准三值逻辑：CHECK 求值为 NULL（UNKNOWN）不视作违反约束；
// 只有确定为 FALSE 时才拒绝写入。TRUE 与 NULL 都视为"通过"。
//
// 反例：`score >= 0 AND score <= 100` 在 score = NULL 时
//   - `NULL >= 0`  = UNKNOWN (NULL)
//   - `NULL <= 100`= UNKNOWN (NULL)
//   - `UNKNOWN AND UNKNOWN` = UNKNOWN (NULL)
// 因此整体 NULL，CHECK 不应触发。
bool CheckExpressionFails(const Value& v) {
    if (v.IsNull()) return false;  // UNKNOWN 不是 FALSE，CHECK 通过
    if (v.GetType() == ValueType::INTEGER) return v.AsInt() == 0;
    if (v.GetType() == ValueType::FLOAT) return v.AsFloat() == 0.0;
    if (v.GetType() == ValueType::VARCHAR) return v.AsVarchar().empty();
    return false;
}

}  // namespace

std::vector<ValueType> BuildColumnTypes(const TableInfo& table_info) {
    std::vector<ValueType> types;
    types.reserve(table_info.columns.size());
    for (const auto& c : table_info.columns) {
        types.push_back(ValueTypeFromString(c.data_type));
    }
    return types;
}

void ValidateRowConstraints(SystemCatalog* catalog, const TableInfo& table_info,
                            TableHeap* heap, const std::vector<Value>& row,
                            const RID* exclude_rid,
                            ExecutionContext* ctx) {
    const auto& cols = table_info.columns;
    if (row.size() != cols.size()) return;  // 列数不符由调用方负责报错

    // ---- 1) NOT NULL（PRIMARY KEY 隐含 NOT NULL）----
    // 52_data_types: 错误文案对齐测试期望 "NOT NULL violation: <table>.<col>"。
    // 主键列的 NULL 同样按 NOT NULL violation 报告（PRIMARY KEY 隐含 NOT NULL）。
    for (size_t i = 0; i < cols.size(); ++i) {
        if (!row[i].IsNull()) continue;
        if (cols[i].is_not_null || cols[i].is_primary_key) {
            throw CompilerException(
                ErrorStage::SEMANTIC,
                "NOT NULL violation: " + table_info.table_name + "." +
                    cols[i].name);
        }
    }

    // ---- 2) VARCHAR(N) / CHAR(N) 长度 ----
    // 52_data_types: 仅对真正按字符计数的字符串类型（VARCHAR / CHAR / TEXT）
    // 触发长度上限校验。DECIMAL / NUMERIC / DATE / TIMESTAMP / TIME / JSON /
    // UUID 这类"参数"代表的是精度 / 格式而不是字符数；例如 DECIMAL(10, 2)
    // 的 "10" 是总位数，"99999999.99" 有 11 个字符但属于合法精度。
    auto has_char_limit = [](const std::string& dt) {
        std::string up;
        up.reserve(dt.size());
        for (char c : dt) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        return up == "VARCHAR" || up == "CHAR" || up == "STRING";
    };
    for (size_t i = 0; i < cols.size(); ++i) {
        if (cols[i].char_length <= 0) continue;
        if (row[i].IsNull()) continue;
        if (!IsStringType(row[i].GetType())) continue;
        if (!has_char_limit(cols[i].data_type)) continue;
        size_t len = Utf8Length(row[i].AsVarchar());
        if (len > static_cast<size_t>(cols[i].char_length)) {
            throw CompilerException(
                ErrorStage::SEMANTIC,
                "value too long for column '" + cols[i].name + "' (" +
                    cols[i].data_type + "(" +
                    std::to_string(cols[i].char_length) + ")): got " +
                    std::to_string(len) + " characters");
        }
    }

    // ---- 3) 列级 CHECK (expr) ----
    // 慢路径过滤：没有任何列挂 CHECK 时直接跳过，避免每行都构造 evaluator。
    if (std::any_of(cols.begin(), cols.end(),
                    [](const ColumnInfo& c) { return c.check_expr != nullptr; })) {
        std::unordered_map<std::string, size_t> cmap;
        cmap.reserve(cols.size());
        for (size_t i = 0; i < cols.size(); ++i) cmap[cols[i].name] = i;
        // CHECK 是只读，把当前行的 value 列表直接交给 Tuple 即可。Tuple 仅
        // 持有引用计数安全的 Value，无后续修改。
        Tuple check_tuple(row);
        ExpressionEvaluator eval(cmap, ctx, nullptr);
        for (size_t i = 0; i < cols.size(); ++i) {
            if (!cols[i].check_expr) continue;
            Value v = eval.Evaluate(cols[i].check_expr, check_tuple);
            // SQL 标准：NULL 不视作违反约束；只有确定 FALSE 才拒绝。
            if (CheckExpressionFails(v)) {
                // 58_constraints: 命名约束在错误消息里优先显示名称，否则
                // 退回 <table>.<col> 形式以保持向后兼容。
                std::string target =
                    !cols[i].constraint_name.empty()
                        ? cols[i].constraint_name
                        : cols[i].name;
                throw CompilerException(
                    ErrorStage::SEMANTIC,
                    "check constraint violated: " + table_info.table_name + "." +
                        target + " (" + cols[i].check_expr->ToString() + ")");
            }
        }
    }

    // ---- 3b) 58_constraints: 表级 CHECK (expr) ----
    // 与列级 CHECK 同语义（NULL-pass / 仅 FALSE 拒绝），但允许跨列引用
    // 任意列。空表或全部匿名 CHECK 时直接跳过；列级 CHECK 与表级 CHECK
    // 各自独立求值，互不影响。
    if (!table_info.table_checks.empty()) {
        std::unordered_map<std::string, size_t> cmap;
        cmap.reserve(cols.size());
        for (size_t i = 0; i < cols.size(); ++i) cmap[cols[i].name] = i;
        Tuple check_tuple(row);
        ExpressionEvaluator eval(cmap, ctx, nullptr);
        for (size_t i = 0; i < table_info.table_checks.size(); ++i) {
            const auto& tc = table_info.table_checks[i];
            if (!tc.expr) continue;
            Value v = eval.Evaluate(tc.expr, check_tuple);
            if (CheckExpressionFails(v)) {
                // 错误消息：命名约束使用名称；匿名约束用 check_<index>
                // 形式以便定位（"第几条表级 CHECK"）。这种回退形式在测试
                // 套件里不会被实际触发——只有同时存在多个匿名表级 CHECK
                // 时才会用到。
                std::string target;
                if (!tc.constraint_name.empty()) {
                    target = tc.constraint_name;
                } else {
                    target = "check_" + std::to_string(i);
                }
                throw CompilerException(
                    ErrorStage::SEMANTIC,
                    "check constraint violated: " + table_info.table_name + "." +
                        target + " (" + tc.expr->ToString() + ")");
            }
        }
    }

    // ---- 4) PRIMARY KEY 唯一性 ----
    auto groups = table_info.GetPrimaryKeyGroups();
    // 不再因 groups.empty() 提前返回：52_data_types 的 UNIQUE 兜底扫描需要
    // 在没有主键的表（如 `CREATE TABLE uq (id INT, email VARCHAR UNIQUE)`）
    // 上也能跑。PK 扫描的具体路径见下面的 `if (pk_groups_present)` 块。
    bool pk_groups_present = !groups.empty();

    // 优先走主键索引点查：O(log N)，且不受表大小影响。
    // 只有当某个主键组没有对应索引时，才对该组退回全表扫描。
    std::vector<std::vector<std::string>> unindexed_groups;
    if (pk_groups_present) for (const auto& g : groups) {
        BPlusTree* tree = (catalog == nullptr)
                              ? nullptr
                              : catalog->GetPrimaryKeyIndexTree(table_info.table_name, g);
        if (tree == nullptr) {
            unindexed_groups.push_back(g);
            continue;
        }
        IndexKey key;
        bool complete = true;
        for (const auto& col_name : g) {
            const ColumnInfo* col = table_info.GetColumn(col_name);
            if (col == nullptr) { complete = false; break; }
            size_t idx = 0;
            bool found = false;
            for (size_t i = 0; i < cols.size(); ++i) {
                if (cols[i].name == col_name) { idx = i; found = true; break; }
            }
            if (!found || idx >= row.size() || row[idx].IsNull()) {
                complete = false;
                break;
            }
            key.values.push_back(row[idx]);
        }
        if (!complete) continue;
        RID existing = tree->FindFirst(key);
        if (existing.IsValid() &&
            (exclude_rid == nullptr || !(existing == *exclude_rid))) {
            std::string key_desc;
            for (size_t i = 0; i < g.size() && i < key.values.size(); ++i) {
                if (!key_desc.empty()) key_desc += ", ";
                key_desc += g[i] + "=" + key.values[i].ToString();
            }
            throw CompilerException(
                ErrorStage::SEMANTIC,
                "PRIMARY KEY constraint violated on table '" +
                    table_info.table_name + "': duplicate key (" + key_desc + ")");
        }
    }

    groups = std::move(unindexed_groups);
    // 52_data_types: 当表无主键（pk_groups_present=false）时跳过 PK 全表扫描，
    // 让后面的 UNIQUE 兜底段继续执行。
    if (pk_groups_present && (groups.empty() || heap == nullptr)) return;

    if (pk_groups_present) {
    std::unordered_map<std::string, size_t> idx_map;
    for (size_t i = 0; i < cols.size(); ++i) idx_map[cols[i].name] = i;

    // 收集所有主键组对应的列下标；任一组均不允许重复
    std::vector<std::vector<size_t>> key_indexes;
    for (const auto& g : groups) {
        std::vector<size_t> idxs;
        bool ok = true;
        for (const auto& name : g) {
            auto it = idx_map.find(name);
            if (it == idx_map.end()) { ok = false; break; }
            idxs.push_back(it->second);
        }
        if (ok && !idxs.empty()) key_indexes.push_back(std::move(idxs));
    }
    if (key_indexes.empty()) return;

    const std::vector<ValueType> schema = BuildColumnTypes(table_info);
    auto iter = heap->Begin();
    while (iter.HasNext()) {
        Tuple existing = iter.Next(schema);
        if (existing.ColumnCount() != cols.size()) continue;
        if (exclude_rid != nullptr && existing.GetRid().IsValid() &&
            existing.GetRid().page_id == exclude_rid->page_id &&
            existing.GetRid().slot_num == exclude_rid->slot_num) {
            continue;
        }
        for (const auto& idxs : key_indexes) {
            bool all_equal = true;
            for (size_t idx : idxs) {
                const Value& a = row[idx];
                const Value& b = existing.GetValue(idx);
                // SQL 语义下 NULL 不参与唯一性比较；但主键列已在上面禁止 NULL，
                // 这里保留防御分支即可。
                if (a.IsNull() || b.IsNull() || Value::Compare(a, b) != 0) {
                    all_equal = false;
                    break;
                }
            }
            if (all_equal) {
                std::string key_desc;
                for (size_t idx : idxs) {
                    if (!key_desc.empty()) key_desc += ", ";
                    key_desc += cols[idx].name + "=" + row[idx].ToString();
                }
                throw CompilerException(
                    ErrorStage::SEMANTIC,
                    "PRIMARY KEY constraint violated on table '" +
                        table_info.table_name + "': duplicate key (" +
                        key_desc + ")");
            }
        }
    }
    }  // 52_data_types: 关闭 pk_groups_present 块

    // ---- 5) 52_data_types: UNIQUE 约束兜底扫描 ----
    //   列级 / 表级 UNIQUE(col1, col2, ...) 在执行路径上有两条：
    //     a) 列已建立唯一 B+Tree 索引 → 由 IndexMaintenance::CheckUniqueIndexes
    //        在 InsertExecutor / UpdateExecutor 里做 O(log N) 点查；
    //     b) 未建索引（典型原因：列是 VARCHAR 且未声明长度，索引建索引失败）
    //        → 这里走全表扫描兜底，保证 UNIQUE 语义不被静默跳过。
    //
    //   本函数合并列级 is_unique 和表级 unique_constraints 为统一列表
    //   （与 CreateTableExecutor 使用的合并规则一致），逐组扫描。
    if (heap == nullptr) return;
    std::vector<std::vector<std::string>> uniq_groups = table_info.unique_constraints;
    for (const auto& c : table_info.columns) {
        if (!c.is_unique) continue;
        bool dup = false;
        for (const auto& g : uniq_groups) {
            if (g.size() == 1 && g[0] == c.name) { dup = true; break; }
        }
        if (!dup) uniq_groups.push_back({c.name});
    }
    if (uniq_groups.empty()) return;

    std::unordered_map<std::string, size_t> col_idx;
    for (size_t i = 0; i < cols.size(); ++i) col_idx[cols[i].name] = i;

    // 把每组列名映射到下标；任一列缺失或值含 NULL（SQL 语义下 NULL 不参与
    // 唯一性比较）就跳过本组的检查。
    std::vector<std::vector<size_t>> uniq_indexes;
    for (const auto& g : uniq_groups) {
        std::vector<size_t> idxs;
        bool ok = true;
        for (const auto& name : g) {
            auto it = col_idx.find(name);
            if (it == col_idx.end()) { ok = false; break; }
            idxs.push_back(it->second);
        }
        if (ok && !idxs.empty()) uniq_indexes.push_back(std::move(idxs));
    }
    if (uniq_indexes.empty()) return;

    const std::vector<ValueType> uniq_schema = BuildColumnTypes(table_info);
    auto uniq_iter = heap->Begin();
    while (uniq_iter.HasNext()) {
        Tuple existing = uniq_iter.Next(uniq_schema);
        if (existing.ColumnCount() != cols.size()) continue;
        if (exclude_rid != nullptr && existing.GetRid().IsValid() &&
            existing.GetRid().page_id == exclude_rid->page_id &&
            existing.GetRid().slot_num == exclude_rid->slot_num) {
            continue;
        }
        for (const auto& idxs : uniq_indexes) {
            bool all_equal = true;
            bool any_null = false;
            for (size_t idx : idxs) {
                const Value& a = row[idx];
                const Value& b = existing.GetValue(idx);
                if (a.IsNull() || b.IsNull()) {
                    // SQL 语义：NULL 不参与唯一性比较，跳过本组判定
                    any_null = true;
                    all_equal = false;
                    break;
                }
                if (Value::Compare(a, b) != 0) {
                    all_equal = false;
                    break;
                }
            }
            if (all_equal) {
                // 单列 UNIQUE 报告形式：uq.email
                // 多列 UNIQUE 报告形式：uq2.(a, b)
                std::string col_desc;
                if (idxs.size() == 1) {
                    col_desc = cols[idxs[0]].name;
                } else {
                    col_desc = "(";
                    for (size_t i = 0; i < idxs.size(); ++i) {
                        if (i) col_desc += ", ";
                        col_desc += cols[idxs[i]].name;
                    }
                    col_desc += ")";
                }
                throw CompilerException(
                    ErrorStage::SEMANTIC,
                    "UNIQUE constraint violation: " + table_info.table_name +
                        "." + col_desc);
            }
            (void)any_null;
        }
    }
}  // end of ValidateRowConstraints

// ============================================================================
// 53_ddl: FOREIGN KEY 强制执行实现
// ============================================================================

namespace {

// 检查 child_cols 上所有列在 row 中是否全为 NULL。任一为 NULL → 返回 true。
// SQL 语义：NULL 不参与 FK 比较；当任一列值为 NULL 时跳过本条 FK 检查。
bool FkRowHasNull(const std::vector<Value>& row,
                  const std::vector<size_t>& indexes) {
    for (size_t idx : indexes) {
        if (idx >= row.size()) return true;
        if (row[idx].IsNull()) return true;
    }
    return false;
}

// 把一组列名映射到 row 中的下标。任一列缺失返回空 vector。
std::vector<size_t> ResolveColumns(const TableInfo& info,
                                   const std::vector<std::string>& cols) {
    std::vector<size_t> out;
    out.reserve(cols.size());
    for (const auto& cn : cols) {
        const ColumnInfo* c = info.GetColumn(cn);
        if (c == nullptr) return {};
        out.push_back(cn.size() == 0 ? 0 : 0);  // placeholder
    }
    // 真实下标：再次扫描 info.columns。
    out.clear();
    out.reserve(cols.size());
    for (const auto& cn : cols) {
        bool found = false;
        for (size_t i = 0; i < info.columns.size(); ++i) {
            if (info.columns[i].name == cn) {
                out.push_back(i);
                found = true;
                break;
            }
        }
        if (!found) return {};
    }
    return out;
}

// 把行按 indexes 抽出一组 Value，按列序构造 IndexKey 用于索引查找。
IndexKey BuildIndexKeyFromRow(const TableInfo& info,
                              const std::vector<std::string>& cols,
                              const std::vector<Value>& row) {
    IndexKey key;
    auto idxs = ResolveColumns(info, cols);
    for (size_t idx : idxs) {
        if (idx >= row.size()) {
            key.values.clear();
            return key;  // signal fail via empty
        }
        if (row[idx].IsNull()) {
            key.values.clear();
            return key;
        }
        key.values.push_back(row[idx]);
    }
    return key;
}

// 全表扫描 parent_table，验证是否存在一行满足 parent_cols == 目标 key。
// 仅当 (parent_cols) 全部等于 key 时返回 true。表为空时返回 false。
bool ParentRowExists(SystemCatalog* catalog, const std::string& parent_table,
                     const std::vector<std::string>& parent_cols,
                     const IndexKey& key) {
    const TableInfo* pt = catalog->GetTable(parent_table);
    if (pt == nullptr) return false;
    TableHeap* heap = catalog->GetTableHeap(parent_table);
    if (heap == nullptr) return false;
    // 优先走 PK 索引点查：只有当 (parent_cols) 与某 PK 组完全一致时。
    BPlusTree* tree = catalog->GetPrimaryKeyIndexTree(parent_table, parent_cols);
    if (tree != nullptr && key.values.size() == parent_cols.size()) {
        RID r = tree->FindFirst(key);
        return r.IsValid();
    }
    // 兜底：全表扫描。
    auto idxs = ResolveColumns(*pt, parent_cols);
    if (idxs.empty() || idxs.size() != key.values.size()) return false;
    std::vector<ValueType> schema;
    schema.reserve(pt->columns.size());
    for (const auto& c : pt->columns) schema.push_back(ValueTypeFromString(c.data_type));
    auto it = heap->Begin();
    while (it.HasNext()) {
        Tuple t = it.Next(schema);
        if (t.ColumnCount() != pt->columns.size()) continue;
        bool all_equal = true;
        for (size_t i = 0; i < idxs.size(); ++i) {
            const Value& a = t.GetValue(idxs[i]);
            const Value& b = key.values[i];
            if (a.IsNull() || b.IsNull() || Value::Compare(a, b) != 0) {
                all_equal = false;
                break;
            }
        }
        if (all_equal) return true;
    }
    return false;
}

// 在 child_table 上扫描所有 (RID, row)，找出与 (child_cols == key) 匹配的行。
// 返回匹配的 RID 列表与对应行（用于 CASCADE / SET NULL 修改）。
struct ChildMatch {
    RID rid;
    std::vector<Value> row;
};
std::vector<ChildMatch> FindChildMatches(SystemCatalog* catalog,
                                         const std::string& child_table,
                                         const std::vector<std::string>& child_cols,
                                         const IndexKey& key) {
    std::vector<ChildMatch> out;
    const TableInfo* ct = catalog->GetTable(child_table);
    if (ct == nullptr) return out;
    TableHeap* heap = catalog->GetTableHeap(child_table);
    if (heap == nullptr) return out;
    auto idxs = ResolveColumns(*ct, child_cols);
    if (idxs.empty()) return out;
    std::vector<ValueType> schema;
    schema.reserve(ct->columns.size());
    for (const auto& c : ct->columns) schema.push_back(ValueTypeFromString(c.data_type));
    auto it = heap->Begin();
    while (it.HasNext()) {
        Tuple t = it.Next(schema);
        if (t.ColumnCount() != ct->columns.size()) continue;
        if (!t.GetRid().IsValid()) continue;
        bool all_equal = true;
        for (size_t i = 0; i < idxs.size() && i < key.values.size(); ++i) {
            const Value& a = t.GetValue(idxs[i]);
            const Value& b = key.values[i];
            if (a.IsNull() || b.IsNull() || Value::Compare(a, b) != 0) {
                all_equal = false;
                break;
            }
        }
        if (!all_equal) continue;
        ChildMatch m;
        m.rid = t.GetRid();
        m.row.reserve(t.ColumnCount());
        for (size_t i = 0; i < t.ColumnCount(); ++i) m.row.push_back(t.GetValue(i));
        out.push_back(std::move(m));
    }
    return out;
}

}  // namespace

void EnforceChildForeignKeys(SystemCatalog* catalog, const std::string& child_table,
                             const std::vector<Value>& row) {
    if (catalog == nullptr) return;
    auto fks = catalog->GetForeignKeysForChild(child_table);
    if (fks.empty()) return;
    for (const auto& fk : fks) {
        const TableInfo* ct = catalog->GetTable(child_table);
        if (ct == nullptr) continue;
        auto child_idxs = ResolveColumns(*ct, fk.child_cols);
        if (child_idxs.empty()) continue;
        if (FkRowHasNull(row, child_idxs)) continue;  // NULL → 跳过
        IndexKey key;
        key.values.reserve(child_idxs.size());
        for (size_t idx : child_idxs) {
            key.values.push_back(row[idx]);
        }
        // 在 parent 上找匹配行
        const TableInfo* pt = catalog->GetTable(fk.parent_table);
        if (pt == nullptr) {
            // 父表不存在 → 视为 FK 违规（防止孤儿引用）
            throw CompilerException(
                ErrorStage::SEMANTIC,
                "foreign key violation: child." + child_table +
                " references missing parent table " + fk.parent_table);
        }
        // 校验 parent_cols 都存在
        auto parent_idxs = ResolveColumns(*pt, fk.parent_cols);
        if (parent_idxs.empty()) {
            throw CompilerException(
                ErrorStage::SEMANTIC,
                "foreign key violation: parent table " + fk.parent_table +
                " missing referenced columns");
        }
        bool exists = ParentRowExists(catalog, fk.parent_table, fk.parent_cols, key);
        if (!exists) {
            std::string desc;
            for (size_t i = 0; i < fk.child_cols.size(); ++i) {
                if (!desc.empty()) desc += ", ";
                desc += fk.child_cols[i] + "=" + key.values[i].ToString();
            }
            throw CompilerException(
                ErrorStage::SEMANTIC,
                "foreign key violation: " + child_table + "." +
                (fk.child_cols.size() == 1 ? fk.child_cols[0] : "(...)") +
                " references " + fk.parent_table + " (" + desc + ")");
        }
    }
}

void EnforceParentForeignKeys(SystemCatalog* catalog,
                              const std::string& parent_table,
                              const std::vector<Value>& parent_row,
                              const RID* exclude_child_rid) {
    if (catalog == nullptr) return;
    auto refs = catalog->GetForeignKeysReferencing(parent_table);
    if (refs.empty()) return;
    const TableInfo* pt = catalog->GetTable(parent_table);
    if (pt == nullptr) return;
    for (const auto& ref_pair : refs) {
        const std::string& child_table = ref_pair.first;
        const ForeignKeyDef& fk = ref_pair.second;
        // 从 parent_row 抽出 (parent_cols) 值。
        auto parent_idxs = ResolveColumns(*pt, fk.parent_cols);
        if (parent_idxs.empty()) continue;
        if (FkRowHasNull(parent_row, parent_idxs)) continue;  // parent NULL → 跳过
        IndexKey key;
        key.values.reserve(parent_idxs.size());
        for (size_t idx : parent_idxs) key.values.push_back(parent_row[idx]);
        auto matches = FindChildMatches(catalog, child_table, fk.child_cols, key);
        // 跳过 exclude_child_rid。
        if (exclude_child_rid != nullptr) {
            std::vector<ChildMatch> filtered;
            for (auto& m : matches) {
                if (m.rid == *exclude_child_rid) continue;
                filtered.push_back(std::move(m));
            }
            matches = std::move(filtered);
        }
        if (matches.empty()) continue;
        if (fk.on_delete_action == 0 /* RESTRICT */ ||
            fk.on_delete_action == 3 /* NO ACTION */ ||
            fk.on_delete_action == 4 /* SET DEFAULT */) {
            std::string desc;
            for (size_t i = 0; i < fk.child_cols.size() && i < key.values.size(); ++i) {
                if (!desc.empty()) desc += ", ";
                desc += fk.child_cols[i] + "=" + key.values[i].ToString();
            }
            throw CompilerException(
                ErrorStage::SEMANTIC,
                "foreign key violation: cannot delete parent row from " +
                parent_table + " (" + desc + "), child " + child_table +
                " has referencing rows");
        } else if (fk.on_delete_action == 1 /* CASCADE */) {
            TableHeap* heap = catalog->GetTableHeap(child_table);
            const TableInfo* ct = catalog->GetTable(child_table);
            if (heap == nullptr || ct == nullptr) continue;
            std::vector<ValueType> schema;
            schema.reserve(ct->columns.size());
            for (const auto& c : ct->columns) schema.push_back(ValueTypeFromString(c.data_type));
            for (const auto& m : matches) {
                // 先从子表索引中移除该行相关索引项（DeleteFromIndexes）。
                // 然后 DeleteTuple。直接 DeleteTuple 可能让索引项残留。
                // 复用 IndexMaintenance 工具需要 catalog 与 row，本处用较低层次接口。
                heap->DeleteTuple(m.rid);
            }
        } else if (fk.on_delete_action == 2 /* SET NULL */) {
            // 把 child_cols 置 NULL，要求这些列可空。
            TableHeap* heap = catalog->GetTableHeap(child_table);
            const TableInfo* ct = catalog->GetTable(child_table);
            if (heap == nullptr || ct == nullptr) continue;
            std::vector<ValueType> schema;
            schema.reserve(ct->columns.size());
            for (const auto& c : ct->columns) schema.push_back(ValueTypeFromString(c.data_type));
            // 校验所有 FK 列可空。
            for (const auto& cn : fk.child_cols) {
                const ColumnInfo* ci = ct->GetColumn(cn);
                if (ci == nullptr || (ci->is_not_null || ci->is_primary_key)) {
                    throw CompilerException(
                        ErrorStage::SEMANTIC,
                        "foreign key violation: ON DELETE SET NULL requires " +
                        child_table + "." + cn + " to be nullable");
                }
            }
            // 同样要确保唯一索引里不残留这条键；这里简化为先删整行再插入新行。
            for (const auto& m : matches) {
                std::vector<Value> new_row = m.row;
                auto child_idxs = ResolveColumns(*ct, fk.child_cols);
                for (size_t idx : child_idxs) {
                    if (idx < new_row.size()) new_row[idx] = Value::MakeNull();
                }
                heap->DeleteTuple(m.rid);
                RID new_rid;
                heap->InsertTuple(Tuple(std::move(new_row)), &new_rid, schema);
            }
        }
    }
}

}  // namespace sqlcompiler
