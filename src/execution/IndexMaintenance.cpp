#include "execution/IndexMaintenance.h"

#include "common/Error.h"
#include "index/BPlusTree.h"

#include <unordered_map>

namespace sqlcompiler {

namespace {

std::unordered_map<std::string, size_t> BuildColumnIndex(const TableInfo& info) {
    std::unordered_map<std::string, size_t> idx;
    for (size_t i = 0; i < info.columns.size(); ++i) idx[info.columns[i].name] = i;
    return idx;
}

std::string DescribeKey(const IndexInfo& index_info, const IndexKey& key) {
    std::string out;
    for (size_t i = 0; i < index_info.key_columns.size() && i < key.values.size(); ++i) {
        if (!out.empty()) out += ", ";
        out += index_info.key_columns[i] + "=" + key.values[i].ToString();
    }
    return out;
}

}  // namespace

bool BuildIndexKeyFromRow(const TableInfo& table_info, const IndexInfo& index_info,
                          const std::vector<Value>& row, IndexKey* out) {
    if (out == nullptr) return false;
    const auto idx = BuildColumnIndex(table_info);
    out->values.clear();
    out->values.reserve(index_info.key_columns.size());
    for (const auto& col : index_info.key_columns) {
        auto it = idx.find(col);
        if (it == idx.end() || it->second >= row.size()) return false;
        const Value& v = row[it->second];
        if (v.IsNull()) return false;  // 索引键不含 NULL
        out->values.push_back(v);
    }
    return true;
}

void CheckUniqueIndexes(SystemCatalog* catalog, const TableInfo& table_info,
                        const std::vector<Value>& row, const RID* exclude_rid) {
    if (catalog == nullptr) return;
    for (const IndexInfo* info : catalog->GetIndexesForTable(table_info.table_name)) {
        if (!info->is_unique) continue;
        IndexKey key;
        if (!BuildIndexKeyFromRow(table_info, *info, row, &key)) continue;
        BPlusTree* tree = catalog->GetIndexTree(info->index_name);
        if (tree == nullptr) continue;
        RID existing = tree->FindFirst(key);
        if (!existing.IsValid()) continue;
        if (exclude_rid != nullptr && existing == *exclude_rid) continue;
        if (info->IsPrimaryKeyIndex()) {
            throw CompilerException(
                ErrorStage::SEMANTIC,
                "PRIMARY KEY constraint violated on table '" +
                    table_info.table_name + "': duplicate key (" +
                    DescribeKey(*info, key) + ")");
        }
        throw CompilerException(
            ErrorStage::SEMANTIC,
            "UNIQUE constraint violated on index '" + info->index_name +
                "': duplicate key (" + DescribeKey(*info, key) + ")");
    }
}

void InsertIntoIndexes(SystemCatalog* catalog, const TableInfo& table_info,
                       const std::vector<Value>& row, const RID& rid) {
    if (catalog == nullptr) return;
    for (const IndexInfo* info : catalog->GetIndexesForTable(table_info.table_name)) {
        IndexKey key;
        if (!BuildIndexKeyFromRow(table_info, *info, row, &key)) continue;
        BPlusTree* tree = catalog->GetIndexTree(info->index_name);
        if (tree == nullptr) continue;
        if (!tree->Insert(key, rid)) {
            // 唯一性冲突本应在 CheckUniqueIndexes 阶段就被拦下；走到这里说明
            // 键太长之类的结构性失败，此时堆已写入而索引没写进去，必须报错让
            // 用户知道，而不是静默留下一个不完整的索引。
            throw CompilerException(
                ErrorStage::SEMANTIC,
                "failed to insert into index '" + info->index_name +
                    "': key (" + DescribeKey(*info, key) + ") rejected");
        }
    }
}

void DeleteFromIndexes(SystemCatalog* catalog, const TableInfo& table_info,
                       const std::vector<Value>& row, const RID& rid) {
    if (catalog == nullptr) return;
    for (const IndexInfo* info : catalog->GetIndexesForTable(table_info.table_name)) {
        IndexKey key;
        if (!BuildIndexKeyFromRow(table_info, *info, row, &key)) continue;
        BPlusTree* tree = catalog->GetIndexTree(info->index_name);
        if (tree == nullptr) continue;
        tree->Delete(key, rid);  // 找不到不算错误
    }
}

}  // namespace sqlcompiler
