#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/AST.h"
#include "execution/Executor.h"
#include "storage_engine/TableHeap.h"

namespace sqlcompiler {

// 顺序扫描算子：遍历指定表所有（未被删除的）记录，
// 对应逻辑计划中的 SeqScanNode，是查询执行树的叶子节点
//
// 可选 predicate 由 Optimizer::PushDownPredicates 注入：原本位于上层
// Filter 的单表合取项下沉到 scan，让本算子在拿到 Tuple 后立即用
// ExpressionEvaluator 求值并跳过不满足的行。语义上等价于
// `Filter(predicate) -> SeqScan(table)`，但省掉一层算子间传递。
//
// table_alias 用于解析下推谓词中带别名的 ColumnRefExpr（如 `u.id = 1`
// 出现在 `FROM users u WHERE u.id = 1`）。下推谓词里的列引用会按
// `table_name` 或 `table_alias` 限定，所以两者都要登记到 column_index_map_。
class SeqScanExecutor : public Executor {
public:
    SeqScanExecutor(ExecutionContext* context, std::string table_name,
                    std::string table_alias = "",
                    ExprPtr predicate = nullptr);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    std::string table_name_;
    std::string table_alias_;
    ExprPtr predicate_;  // 下推谓词；nullptr 时退化为纯顺序扫描
    TableHeap* table_heap_;
    std::vector<ValueType> column_types_;
    std::unordered_map<std::string, size_t> column_index_map_;
    std::unique_ptr<TableHeap::Iterator> iterator_;
};

}  // namespace sqlcompiler
