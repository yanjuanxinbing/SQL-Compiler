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
    // is_default_values=true 时插入一行所有列用 DEFAULT 表达式（无 DEFAULT
    // 时为 NULL），对应 SQL 标准 INSERT ... DEFAULT VALUES。
    InsertExecutor(ExecutionContext* context, std::string table_name,
                    std::vector<std::string> columns,
                    std::vector<std::vector<ExprPtr>> values_list,
                    bool is_replace = false,
                    bool is_default_values = false,
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
    // SELECT 路径：构造时把 query_plan 全量执行一次得到结果缓冲，
    // 避免 INSERT INTO t SELECT ... FROM t 这类「目标表 = 源表」在
    // 边读边写场景下产生无限循环——读端看到本语句刚插入的行后再写，
    // 下一轮再读到、再写，循环不止。先在构造期把源表快照固定下来，
    // 后续 Next() 仅消费缓冲，不再触碰源 SeqScan。
    ExecutorPtr source_;
    std::vector<Tuple> materialized_rows_;
    // BUG-19：源 SELECT 的"声明输出列数"。ProjectExecutor 会把底层元组追加在
    // SELECT 值之后（ORDER BY 隐藏列设计），物化元组的实际宽度可能大于源
    // SELECT 的列数；INSERT 只应消费前 source_width_ 列。0 = 未知（不裁剪）。
    size_t source_width_ = 0;
    // 是否走 REPLACE 语义。
    bool is_replace_ = false;
    // INSERT ... DEFAULT VALUES：插入一行所有列用 DEFAULT 表达式（无
    // DEFAULT 时为 NULL）。当 is_default_values_ 为 true 时，values_list_
    // 会被忽略（执行期按 info->columns.size() 构造一行 DefaultExprNode）。
    bool is_default_values_ = false;
    // 54_dml: RETURNING 缓冲与游标。
    std::vector<ExprPtr> returning_exprs_;
    std::vector<std::string> returning_aliases_;
    std::vector<Tuple> pending_returning_;
    size_t pending_pos_ = 0;
};

}  // namespace sqlcompiler
