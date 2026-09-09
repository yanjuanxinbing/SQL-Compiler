#pragma once

#include <memory>
#include <string>
#include <vector>

#include "execution/Executor.h"
#include "storage_engine/TableHeap.h"

namespace sqlcompiler {

// 顺序扫描算子：遍历指定表的所有（未被删除的）记录，
// 对应逻辑计划中的 SeqScanNode，是查询执行树的叶子节点
class SeqScanExecutor : public Executor {
public:
    SeqScanExecutor(ExecutionContext* context, std::string table_name);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    std::string table_name_;
    TableHeap* table_heap_;
    std::vector<ValueType> column_types_;
    std::unique_ptr<TableHeap::Iterator> iterator_;
};

}  // namespace sqlcompiler
