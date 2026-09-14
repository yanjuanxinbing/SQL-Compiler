#pragma once

#include <memory>
#include <string>
#include <unordered_map>
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

    std::shared_ptr<IndexScanNode> node_;
    std::unordered_map<std::string, size_t> column_index_map_;

    TableHeap* table_heap_ = nullptr;
    BPlusTree* tree_ = nullptr;
    std::unique_ptr<BPlusTree::Cursor> cursor_;
    std::vector<ValueType> column_types_;
};

}  // namespace sqlcompiler
