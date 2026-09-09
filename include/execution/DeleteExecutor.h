#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/AST.h"
#include "execution/Executor.h"
#include "storage_engine/TableHeap.h"

namespace sqlcompiler {

// 删除算子：内部顺序扫描目标表，对满足predicate（WHERE条件）的记录
// 调用TableHeap::DeleteTuple()删除，对应逻辑计划中的 DeleteNode
class DeleteExecutor : public Executor {
public:
    DeleteExecutor(ExecutionContext* context, std::string table_name, ExprPtr predicate,
                    std::unordered_map<std::string, size_t> column_index_map);

    void Init() override;
    // 可选地通过tuple返回一个仅含"受影响行数"的特殊Tuple
    bool Next(Tuple* tuple) override;

private:
    std::string table_name_;
    ExprPtr predicate_;  // 可为空，表示无WHERE条件，删除全表
    std::unordered_map<std::string, size_t> column_index_map_;

    TableHeap* table_heap_;
    std::vector<ValueType> column_types_;
    std::unique_ptr<TableHeap::Iterator> iterator_;
    bool executed_;
};

}  // namespace sqlcompiler
