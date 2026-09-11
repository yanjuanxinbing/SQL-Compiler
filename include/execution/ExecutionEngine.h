#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "catalog/SystemCatalog.h"
#include "execution/Executor.h"
#include "plan/Plan.h"
#include "storage_engine/Tuple.h"

namespace sqlcompiler {

class TransactionManager;

// 执行结果：
//   - 查询类语句（SELECT）：success/column_names/rows 有效
//   - DML/DDL语句（INSERT/UPDATE/DELETE/CREATE TABLE/DROP TABLE）：
//     success/message 有效，message可用于承载"受影响行数"等提示信息
struct ExecutionResult {
    bool success = true;
    std::string message;
    std::vector<std::string> column_names;
    std::vector<Tuple> rows;
};

// 执行引擎：将Planner生成、Optimizer优化后的逻辑执行计划(PlanNodePtr)
// 转换为算子树（Executor Tree）并以火山模型驱动其运行，产出最终结果
class ExecutionEngine {
public:
    explicit ExecutionEngine(SystemCatalog* catalog,
                             TransactionManager* txn_manager = nullptr);

    // 执行入口：输入一棵逻辑计划树，返回执行结果
    ExecutionResult Execute(const PlanNodePtr& plan);

    // 在已有的 ExecutionContext 上跑一棵子计划，并把所有结果行返回。
    // 供子查询 / CTE 物化等需要复用父查询的 CTE 注册表/BufferPool 时使用。
    ExecutionResult ExecuteSubplan(const PlanNodePtr& plan, ExecutionContext* ctx);

    // 把一棵计划子树转换为 Executor，供 CteDefineNode 等需要在内部再次构造
    // 子执行器时复用（作为 public 暴露）。
    ExecutorPtr BuildExecutor(const PlanNodePtr& plan_node, ExecutionContext* context);

    // ---- Phase A ----
    TransactionManager* GetTransactionManager() const { return txn_manager_; }

private:
    SystemCatalog* catalog_;
    TransactionManager* txn_manager_;

    // 根据表结构构建"列名 -> 下标"的映射，供表达式求值使用
    std::unordered_map<std::string, size_t> BuildColumnIndexMap(const std::string& table_name);

    // 根据select_list推导输出结果集列名（用于展示SELECT结果）
    std::vector<std::string> DeriveOutputColumnNames(const PlanNodePtr& plan_node);
};

}  // namespace sqlcompiler
