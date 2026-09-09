#pragma once

#include <string>
#include <vector>

#include "ast/AST.h"
#include "execution/Executor.h"

namespace sqlcompiler {

// 插入算子：将VALUES中的每一行表达式求值为Tuple，写入目标表的TableHeap，
// 对应逻辑计划中的 InsertNode
class InsertExecutor : public Executor {
public:
    InsertExecutor(ExecutionContext* context, std::string table_name,
                    std::vector<std::string> columns,
                    std::vector<std::vector<ExprPtr>> values_list);

    void Init() override;
    // 每调用一次插入一行；全部插入完成后返回false。
    // 可选地通过tuple返回一个仅含"受影响行数"的特殊Tuple，供上层CLI展示
    bool Next(Tuple* tuple) override;

private:
    std::string table_name_;
    std::vector<std::string> columns_;
    std::vector<std::vector<ExprPtr>> values_list_;
    size_t current_row_;
};

}  // namespace sqlcompiler
