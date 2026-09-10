#include "execution/ConstraintChecker.h"

#include "common/Error.h"
#include "index/BPlusTree.h"

#include <cstddef>
#include <string>
#include <unordered_map>

#include "index/IndexKey.h"

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
                            const RID* exclude_rid) {
    const auto& cols = table_info.columns;
    if (row.size() != cols.size()) return;  // 列数不符由调用方负责报错

    // ---- 1) NOT NULL（PRIMARY KEY 隐含 NOT NULL）----
    for (size_t i = 0; i < cols.size(); ++i) {
        if (!row[i].IsNull()) continue;
        if (cols[i].is_not_null) {
            throw CompilerException(
                ErrorStage::SEMANTIC,
                "NOT NULL constraint violated: column '" + cols[i].name +
                    "' of table '" + table_info.table_name + "'");
        }
        if (cols[i].is_primary_key) {
            throw CompilerException(
                ErrorStage::SEMANTIC,
                "PRIMARY KEY column cannot be NULL: '" + cols[i].name +
                    "' of table '" + table_info.table_name + "'");
        }
    }

    // ---- 2) VARCHAR(N) / CHAR(N) 长度 ----
    for (size_t i = 0; i < cols.size(); ++i) {
        if (cols[i].char_length <= 0) continue;
        if (row[i].IsNull()) continue;
        if (!IsStringType(row[i].GetType())) continue;
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

    // ---- 3) PRIMARY KEY 唯一性 ----
    auto groups = table_info.GetPrimaryKeyGroups();
    if (groups.empty()) return;

    // 优先走主键索引点查：O(log N)，且不受表大小影响。
    // 只有当某个主键组没有对应索引时，才对该组退回全表扫描。
    std::vector<std::vector<std::string>> unindexed_groups;
    for (const auto& g : groups) {
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
    if (groups.empty() || heap == nullptr) return;

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
}

}  // namespace sqlcompiler
