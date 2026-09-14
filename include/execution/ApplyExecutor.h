#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "execution/Executor.h"
#include "storage_engine/Value.h"

namespace sqlcompiler {

// 55_query: APPLY 执行器（LATERAL / CROSS APPLY）。
//
// 对每条外层（左）行：
//   1) 把当前左行的列名 → Value 推到 ExecutionContext.outer_bind；
//   2) 初始化右子计划并把所有输出行收集到本地 buffer_；
//   3) 把每条右行与当前左行拼接发射出去。
//
// 必要时（外层行没匹配到右行）按 is_left_outer 决定是否发左行 + NULL 行。
// 当前 V1 仅支持 CROSS APPLY：is_left_outer == false 时不发射 NULL 补行。
//
// 由于 LATERAL 子查询被识别为相关子查询（IsSubqueryCorrelated），ExpressionEvaluator
// 在 EvaluateSubquery 时会基于 column_index_map / outer_bind 推导外层列引用。
// 本执行器直接 push 完整外层行到 outer_bind，让右计划在每次 Init 时能拿到。
//
// lateral_inner_tables 是右子计划 FROM/JOIN 的表名 + 别名集合（含 from_table_alias）。
// 在构造时一次性写入 ExecutionContext，供右子计划的 ExpressionEvaluator 识别
// 「限定列是内层表名还是外层引用」。
class ApplyExecutor : public Executor {
public:
    ApplyExecutor(ExecutionContext* context,
                  ExecutorPtr left,
                  ExecutorPtr right,
                  bool is_left_outer,
                  std::unordered_map<std::string, size_t> combined_column_index_map,
                  std::unordered_set<std::string> lateral_inner_tables);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    // 拉取一条新左行（并在 first_left_row 上把 outer_bind 设为本行）；返回 false 表示没有更多。
    bool NextLeftRow(Tuple* out);
    // 拼接一条左行 + 一条右行。
    static Tuple Concat(const Tuple& a, const Tuple& b);

    ExecutorPtr left_;
    ExecutorPtr right_;
    bool is_left_outer_;
    // 外层所有列（含左表的真实列 + 限定别名）→ 下标映射。
    // 由 BuildExecutor 在构造时基于 PlanNode 计算出（左侧各表的列下标）。
    std::unordered_map<std::string, size_t> combined_column_index_map_;
    // 当前左行：保存供 outer_bind 关联。
    Tuple current_left_;
    bool has_left_ = false;
    // 当前左行对应的 right 行 buffer。
    std::vector<Tuple> right_buffer_;
    size_t right_cursor_ = 0;
    // 已为 current_left_ 取过左行（避免重复 Init）。
    bool left_pulled_ = false;
    // 当前左行的 outer_bind（生命周期内有效）。
    std::unordered_map<std::string, Value> current_bind_;
};

}  // namespace sqlcompiler