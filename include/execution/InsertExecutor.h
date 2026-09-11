#pragma once

#include <string>
#include <vector>

#include "ast/AST.h"
#include "execution/Executor.h"
#include "plan/Plan.h"

namespace sqlcompiler {

// 插入算子：将VALUES中的每一行表达式求值为Tuple，写入目标表的TableHeap，
// 对应逻辑计划中的 InsertNode
class InsertExecutor : public Executor {
public:
    // 常规 INSERT INTO ... VALUES (...) 路径：直接对字面表达式求值后写入。
    InsertExecutor(ExecutionContext* context, std::string table_name,
                    std::vector<std::string> columns,
                    std::vector<std::vector<ExprPtr>> values_list);

    // INSERT INTO ... SELECT ... 路径：先把 query_plan 物化到子执行器，
    // 再按行把结果插入目标表。columns 含义与 VALUES 路径一致（空=按表定义列序）。
    InsertExecutor(ExecutionContext* context, std::string table_name,
                    std::vector<std::string> columns,
                    PlanNodePtr query_plan);

    void Init() override;
    // 每调用一次插入一行；全部插入完成后返回false。
    // 可选地通过tuple返回一个仅含"受影响行数"的特殊Tuple，供上层CLI展示
    bool Next(Tuple* tuple) override;

private:
    // 共用：将一行（已经按列映射好的 Value 序列）写入目标表。
    bool InsertRow(const std::vector<Value>& row_values);

    std::string table_name_;
    std::vector<std::string> columns_;
    // VALUES 路径：预先准备好的字面量表达式集合。
    std::vector<std::vector<ExprPtr>> values_list_;
    size_t current_row_ = 0;
    // SELECT 路径：物化后的子执行器（query_plan 在 Init 时被构建一次）。
    ExecutorPtr source_;
};

}  // namespace sqlcompiler
