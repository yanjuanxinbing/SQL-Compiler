#include "execution/IndexMaintenance.h"

#include "common/Error.h"
#include "index/BPlusTree.h"
#include "index/IndexKey.h"
#include "txn/Transaction.h"

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

// MVCC 二级索引精确可见性（t4）：快照写者的逻辑删除/键改写对非唯一二级索引
// 「延迟摘除」——旧条目保留，让快照读者能经索引定位该行、再沿版本链精确过滤。
// 唯一/主键索引不延迟（唯一性预检依赖条目的存在性；延迟会让同键出现两份）。
bool IndexDeleteDeferred(const IndexInfo* info, Transaction* txn) {
    return info != nullptr && !info->is_unique && txn != nullptr &&
           txn->IsActive() &&
           txn->GetIsolationLevel() == IsolationLevel::kSnapshot;
}

// 该索引在两行上的键是否相同（都成功构建时才比较）。
bool IndexKeysEqual(const TableInfo& table_info, const IndexInfo* info,
                    const std::vector<Value>& a, const std::vector<Value>& b) {
    if (info == nullptr) return false;
    IndexKey ka, kb;
    if (!BuildIndexKeyFromRow(table_info, *info, a, &ka)) return false;
    if (!BuildIndexKeyFromRow(table_info, *info, b, &kb)) return false;
    return CompareKeyOnly(ka, kb) == 0;
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
                       const std::vector<Value>& row, const RID& rid,
                       Transaction* txn, const std::vector<Value>* old_row) {
    if (catalog == nullptr) return;
    for (const IndexInfo* info : catalog->GetIndexesForTable(table_info.table_name)) {
        IndexKey key;
        if (!BuildIndexKeyFromRow(table_info, *info, row, &key)) continue;
        BPlusTree* tree = catalog->GetIndexTree(info->index_name);
        if (tree == nullptr) continue;
        // 键未变的更新：旧条目仍指向稳定 RID 且键未变，直接复用；重复插入会让
        // 非唯一索引出现同 (key,rid) 的两份条目，范围扫描按 RID 去重前先避免。
        if (old_row != nullptr && IndexKeysEqual(table_info, info, *old_row, row)) {
            continue;
        }
        // Phase A：把 txn 挂到树上，让 Insert 内部的写路径捕获 undo。
        tree->SetActiveTransaction(txn);
        if (!tree->Insert(key, rid)) {
            tree->SetActiveTransaction(nullptr);
            throw CompilerException(
                ErrorStage::SEMANTIC,
                "failed to insert into index '" + info->index_name +
                    "': key (" + DescribeKey(*info, key) + ") rejected");
        }
        tree->SetActiveTransaction(nullptr);
    }
}

void DeleteFromIndexes(SystemCatalog* catalog, const TableInfo& table_info,
                       const std::vector<Value>& row, const RID& rid,
                       Transaction* txn, const std::vector<Value>* new_row) {
    if (catalog == nullptr) return;
    for (const IndexInfo* info : catalog->GetIndexesForTable(table_info.table_name)) {
        IndexKey key;
        if (!BuildIndexKeyFromRow(table_info, *info, row, &key)) continue;
        BPlusTree* tree = catalog->GetIndexTree(info->index_name);
        if (tree == nullptr) continue;
        // 键未变的更新：旧条目本身就是新版本的正确索引项，无需摘除。
        if (new_row != nullptr && IndexKeysEqual(table_info, info, row, *new_row)) {
            continue;
        }
        // MVCC 二级索引精确可见性（t4）：快照写者的逻辑删除/键改写对非唯一二级
        // 索引延迟摘除——旧条目保留，由快照读者回表 + 版本链与键重检精确过滤；
        // 唯一/主键索引始终急切维护。
        if (IndexDeleteDeferred(info, txn)) continue;
        tree->SetActiveTransaction(txn);
        tree->Delete(key, rid);  // 找不到不算错误
        tree->SetActiveTransaction(nullptr);
    }
}

void RestoreDeletedIndexEntries(SystemCatalog* catalog, const TableInfo& table_info,
                                const std::vector<Value>& row, const RID& rid,
                                Transaction* txn) {
    if (catalog == nullptr) return;
    for (const IndexInfo* info : catalog->GetIndexesForTable(table_info.table_name)) {
        IndexKey key;
        if (!BuildIndexKeyFromRow(table_info, *info, row, &key)) continue;
        BPlusTree* tree = catalog->GetIndexTree(info->index_name);
        if (tree == nullptr) continue;
        // 快照+非唯一索引的旧条目在 DeleteFromIndexes 中被保留，无需放回
        //（放回会产生同 (key,rid) 重复条目）。
        if (IndexDeleteDeferred(info, txn)) continue;
        tree->SetActiveTransaction(txn);
        tree->Insert(key, rid);  // 恢复被摘掉的旧键
        tree->SetActiveTransaction(nullptr);
    }
}

}  // namespace sqlcompiler
