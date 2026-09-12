#pragma once

#include <memory>
#include <string>
#include <vector>

#include "execution/Executor.h"
#include "plan/Plan.h"

namespace sqlcompiler {

// =============================================================================
// 60_view_trigger (Category 9): MaterializedViewExecutor
//
// CREATE MATERIALIZED VIEW / ALTER MATERIALIZED VIEW ... REFRESH 的执行算子。
//
// 数据流：
//   1) Init 阶段：构造 children[0] 子执行器，跑一遍 SELECT 收集全部行；
//   2) 第一种形态（CREATE_MATERIALIZED_VIEW）：在 catalog 中建一张 backing table
//      （表名 "__mv_<view_name>"），把收集到的行批量 InsertTuple 进 backing table。
//   3) 第二种形态（ALTER_MATERIALIZED_VIEW）：truncate backing table 后重新执行
//      SELECT 并覆盖内容。
//
// 后续 SELECT FROM <view_name> 时由 Planner 把 from_table 替换为 backing table 名，
// 直接走 SeqScanExecutor 路径读取物化结果，无需重新执行原 SELECT。
// =============================================================================

class MaterializedViewExecutor : public Executor {
public:
    // kind 决定执行语义：CREATE 时建表+物化，REFRESH 时先 truncate 后重新物化。
    enum class Kind { CREATE, REFRESH };

    MaterializedViewExecutor(ExecutionContext* context,
                             Kind kind,
                             const PlanNode* plan_node);
    ~MaterializedViewExecutor() override = default;

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    Kind kind_;
    std::string view_name_;
    const PlanNode* plan_node_;  // CREATE / ALTER 节点；children[0] 是 SELECT 子计划
    // 由 Planner 通过 InferSelectOutputSchema 静态计算出的输出列定义。
    // - CREATE 路径：直接用作 backing table 的列定义。
    // - REFRESH 路径：与 catalog 中已存的 MaterializedViewInfo.columns 对比，
    //   不一致时报「schema drift detected」错误（V1 行为与 PostgreSQL 一致：
    //   schema 变更需要 DROP + CREATE）。
    std::vector<ColumnDefinition> cols_;
    bool done_ = false;
};

}  // namespace sqlcompiler
