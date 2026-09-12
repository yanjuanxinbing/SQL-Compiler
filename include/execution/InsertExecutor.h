#pragma once

#include <string>
#include <utility>
#include <vector>

#include "ast/AST.h"
#include "execution/Executor.h"
#include "plan/Plan.h"

namespace sqlcompiler {

// 插入算子：将VALUES中的每一行表达式求值为Tuple，写入目标表的TableHeap，
// 对应逻辑计划中的 InsertNode
//
// 54_dml: 扩展：
//   - is_replace：true 时按 MySQL REPLACE 语义——主键/唯一冲突先删旧行再插新行。
//   - returning_exprs：INSERT 成功后，对新行评估并以结果集形式 emit。
class InsertExecutor : public Executor {
public:
    // 常规 INSERT INTO ... VALUES (...) 路径：直接对字面表达式求值后写入。
    InsertExecutor(ExecutionContext* context, std::string table_name,
                    std::vector<std::string> columns,
                    std::vector<std::vector<ExprPtr>> values_list,
                    bool is_replace = false,
                    std::vector<ExprPtr> returning_exprs = {},
                    std::vector<std::string> returning_aliases = {});

    // INSERT INTO ... SELECT ... 路径：先把 query_plan 物化到子执行器，
    // 再按行把结果插入目标表。columns 含义与 VALUES 路径一致（空=按表定义列序）。
    InsertExecutor(ExecutionContext* context, std::string table_name,
                    std::vector<std::string> columns,
                    PlanNodePtr query_plan,
                    std::vector<ExprPtr> returning_exprs = {},
                    std::vector<std::string> returning_aliases = {});

    void Init() override;
    // 无 RETURNING 时：每次 Next 插入一行；返回 true 直到所有行处理完。
    // 有 RETURNING 时：每次 Next 在插入成功后 emit 一条 RETURNING 元组；
    // emit 完毕后再调一次 Next 返回 false。
    bool Next(Tuple* tuple) override;

private:
    // 共用：将一行（已经按列映射好的 Value 序列）写入目标表。
    // insert_or_replace：true 时按 REPLACE 语义（PK/UNIQUE 冲突先删旧行再插新行）。
    // 用于 REPLACE INTO 路径；其他场景下与 INSERT 等价。
    bool InsertRow(const std::vector<Value>& row_values, bool is_replace);

    std::string table_name_;
    std::vector<std::string> columns_;
    // VALUES 路径：预先准备好的字面量表达式集合。
    std::vector<std::vector<ExprPtr>> values_list_;
    size_t current_row_ = 0;
    // SELECT 路径：物化后的子执行器（query_plan 在 Init 时被构建一次）。
    ExecutorPtr source_;
    // 是否走 REPLACE 语义。
    bool is_replace_ = false;
    // 54_dml: RETURNING 缓冲与游标。
    std::vector<ExprPtr> returning_exprs_;
    std::vector<std::string> returning_aliases_;
    std::vector<Tuple> pending_returning_;
    size_t pending_pos_ = 0;
};

}  // namespace sqlcompiler
