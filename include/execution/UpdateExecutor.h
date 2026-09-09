#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ast/AST.h"
#include "execution/Executor.h"
#include "storage_engine/TableHeap.h"

namespace sqlcompiler {

// 更新算子（可选扩展语法 UPDATE）：顺序扫描目标表，
// 对满足predicate的记录按assignments计算新值后写回，
// 对应逻辑计划中的 UpdateNode
class UpdateExecutor : public Executor {
public:
    UpdateExecutor(ExecutionContext* context, std::string table_name,
                    std::vector<std::pair<std::string, ExprPtr>> assignments, ExprPtr predicate,
                    std::unordered_map<std::string, size_t> column_index_map);

    void Init() override;
    // 可选地通过tuple返回一个仅含"受影响行数"的特殊Tuple
    bool Next(Tuple* tuple) override;

private:
    std::string table_name_;
    std::vector<std::pair<std::string, ExprPtr>> assignments_;
    ExprPtr predicate_;  // 可为空
    std::unordered_map<std::string, size_t> column_index_map_;

    TableHeap* table_heap_;
    std::vector<ValueType> column_types_;
    std::unique_ptr<TableHeap::Iterator> iterator_;
    bool executed_;
};

}  // namespace sqlcompiler
