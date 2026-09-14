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
//
// 54_dml: RETURNING 子句支持。returning_exprs 非空时，每条被删除的行会先按
// pre-image（删除前的内容）评估 returning_exprs 并作为结果集 emit。
class DeleteExecutor : public Executor {
public:
    DeleteExecutor(ExecutionContext* context, std::string table_name, ExprPtr predicate,
                    std::unordered_map<std::string, size_t> column_index_map,
                    std::vector<ExprPtr> returning_exprs = {},
                    std::vector<std::string> returning_aliases = {});

    void Init() override;
    // 无 RETURNING 时：返回 false 一次（并通过 tuple 输出受影响行数）。
    // 有 RETURNING 时：按顺序 emit 每条被删除行的 RETURNING 元组；emit 完毕后
    // 调用方再调用一次 Next，会返回 false（无更多行）。
    bool Next(Tuple* tuple) override;

private:
    std::string table_name_;
    ExprPtr predicate_;  // 可为空，表示无WHERE条件，删除全表
    std::unordered_map<std::string, size_t> column_index_map_;
    std::vector<ExprPtr> returning_exprs_;
    std::vector<std::string> returning_aliases_;

    TableHeap* table_heap_;
    std::vector<ValueType> column_types_;
    std::unique_ptr<TableHeap::Iterator> iterator_;
    bool executed_;
    // RETURNING 缓冲：每条被删除的行评估一次 returning_exprs，结果暂存。
    std::vector<Tuple> pending_returning_;
    size_t pending_pos_ = 0;
};

}  // namespace sqlcompiler
