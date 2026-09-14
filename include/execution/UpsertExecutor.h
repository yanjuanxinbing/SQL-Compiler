#pragma once

#include <string>
#include <utility>
#include <vector>

#include "ast/AST.h"
#include "execution/Executor.h"
#include "plan/Plan.h"

namespace sqlcompiler {

// 43_upsert：ON DUPLICATE KEY UPDATE 执行器。
//
// =============================================================================
// 实现范围 / 未实现范围
// =============================================================================
// 已实现：
//   - PRIMARY KEY 冲突时按 upsert_assignments 改写该已有行；
//     改写后再次跑 CHECK / NOT NULL / VARCHAR 长度约束（路径与 UpdateExecutor
//     共享），并维护 UNIQUE INDEX（先摘旧键再写新键，写堆失败回滚）。
//   - VALUES(col) 引用：解析为 UpsertValuesRefExpr；本执行器在评估 assignments
//     时把候选行的列值推到 ExecutionContext.upsert_values_bind，ExpressionEvaluator
//     据此取出对应列值。
//   - DEFAULT 与已有 INSERT 路径一致：INSERT 路径已自动填补 DEFAULT；
//     本路径复用 InsertExecutor::InsertRow（不含 upsert_assignments 的候选行
//     直接走普通 INSERT）。
//   - 与 INSERT ... SELECT（query 非空）的互斥：query 非空时拒绝 upsert，
//     因为 VALUES 候选行在 SELECT 数据源下不可枚举。
//
// 未实现（后续可补）：
//   - UNIQUE INDEX（非主键）冲突时也走 upsert 改写。本任务仅 PRIMARY KEY
//     路径覆盖；其他 UNIQUE 约束冲突按 MySQL 语义也应该走 upsert，但当前
//     实现不会触发（唯一索引已在 InsertExecutor 写堆前抛错）。
//   - 复合主键下的"行级 last_insert_id()"等 MySQL 扩展副作用。
//
// 与已有路径的边界：
//   - 普通 INSERT / INSERT ... SELECT 行为完全不变（走 InsertNode → InsertExecutor）。
//   - UPDATE 路径不变。UpsertExecutor 仅在 UpsertNode 下被构造。
class UpsertExecutor : public Executor {
 public:
    UpsertExecutor(ExecutionContext* context, std::string table_name,
                   std::vector<std::string> columns,
                   std::vector<std::vector<ExprPtr>> values_list,
                   std::vector<std::pair<std::string, ExprPtr>> upsert_assignments,
                   std::vector<ExprPtr> returning_exprs = {},
                   std::vector<std::string> returning_aliases = {});

    void Init() override;
    // 无 RETURNING 时返回一次（受影响行数）。有 RETURNING 时按顺序 emit 每条
    // upsert 后的 RETURNING 行；emit 完毕后再调一次 Next 返回 false。
    bool Next(Tuple* tuple) override;

 private:
    // 把一行候选值按表 schema 规整化：补 DEFAULT、补 AUTO_INCREMENT、类型强转。
    // 失败抛 CompilerException（NOT NULL / 长度 / CHECK 等）。
    void PrepareCandidateRow(std::vector<Value>& row_values);

    // 把候选行写入表堆（无冲突路径），返回受影响行数（始终 1）。
    // 复用 InsertExecutor 的 InsertRow：NOT NULL / 长度 / CHECK / PRIMARY KEY 唯一性
    // / UNIQUE INDEX 全部沿用既有语义。upsert 后若 returning_exprs_ 非空，会把
    // 评估结果追加到 pending_returning_。
    void InsertCandidateRow(std::vector<Value>& row_values);

    // 用 PRIMARY KEY 在主键索引里探测候选行是否冲突。
    // 返回 true 表示存在其它行使用相同主键值；false 表示无冲突。
    bool HasPrimaryKeyConflict(const std::vector<Value>& row_values,
                               RID* existing_rid_out) const;

    // 冲突路径：取出 existing_rid 对应行的现有列值，按 upsert_assignments 改写，
    // 重新跑约束，写回堆并同步索引。改写后若 returning_exprs_ 非空，会把评估结果
    // 追加到 pending_returning_。
    void UpdateConflictingRow(const std::vector<Value>& candidate_values,
                              const RID& existing_rid);

    // 54_dml: 评估 returning_exprs_ 并追加到 pending_returning_。
    void EmitReturning(const Tuple& row);

    std::string table_name_;
    std::vector<std::string> columns_;
    std::vector<std::vector<ExprPtr>> values_list_;
    std::vector<std::pair<std::string, ExprPtr>> upsert_assignments_;

    // 54_dml: RETURNING 缓冲。
    std::vector<ExprPtr> returning_exprs_;
    std::vector<std::string> returning_aliases_;
    std::vector<Tuple> pending_returning_;
    size_t pending_pos_ = 0;

    // 表 schema 缓存（Init 时填充）
    std::vector<ValueType> column_types_;
    std::unordered_map<std::string, size_t> column_index_map_;

    bool executed_ = false;
    int affected_rows_ = 0;
};

}  // namespace sqlcompiler