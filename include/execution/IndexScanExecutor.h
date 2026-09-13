#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "execution/Executor.h"
#include "index/BPlusTree.h"
#include "plan/Plan.h"
#include "storage_engine/TableHeap.h"

namespace sqlcompiler {

// 索引扫描算子：沿 B+Tree 叶子链扫 [low_key, high_key] 区间，用 RID 回表取整行。
//
// 与 SeqScan 的差别只在「产出哪些行」，输出的 Tuple 结构完全一致，因此上层的
// Project/Sort/Aggregate 算子无需感知访问路径的变化。
class IndexScanExecutor : public Executor {
public:
    IndexScanExecutor(ExecutionContext* context,
                      std::shared_ptr<IndexScanNode> node,
                      std::unordered_map<std::string, size_t> column_index_map);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    // 当前索引项是否已越过上界
    bool BeyondUpperBound(const IndexKey& key) const;

    // MVCC 精确可见性（t4）：把回表得到的「本快照可见版本」重新构建成索引键，
    // 校验其落在扫描区间 [low_key, high_key] 内。索引中可能残留快照写者键改写/
    // 逻辑删除延迟保留的陈旧条目，其可见版本键与条目键不同，据此精确过滤。
    bool InScanBounds(const IndexKey& key) const;

    // 索引叶子条目按 (key, rid) 存储；RID 无默认哈希，就地补一个（页号+槽位）。
    struct RidHash {
        size_t operator()(const RID& r) const {
            size_t h = std::hash<page_id_t>()(r.page_id);
            h ^= std::hash<int32_t>()(r.slot_num) + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };

    std::shared_ptr<IndexScanNode> node_;
    std::unordered_map<std::string, size_t> column_index_map_;

    TableHeap* table_heap_ = nullptr;
    BPlusTree* tree_ = nullptr;
    std::unique_ptr<BPlusTree::Cursor> cursor_;
    std::vector<ValueType> column_types_;
    // 被扫描索引的键列（Init 时从目录解析），用于回表后的键重检。
    std::vector<std::string> index_key_columns_;
    // 键区间跨新旧条目时同一逻辑行（稳定 RID）可能命中多条目，按 RID 去重。
    std::unordered_set<RID, RidHash> seen_rids_;
};

}  // namespace sqlcompiler
