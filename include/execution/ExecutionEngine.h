#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "catalog/SystemCatalog.h"
#include "execution/Executor.h"
#include "plan/Plan.h"
#include "storage_engine/Tuple.h"
#include "storage_engine/Value.h"  // 71_proc_out_params: SessionVars uses Value

namespace sqlcompiler {

class TransactionManager;
class StorageAccess;

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
    //
    // wrap_timing == true 时，每个构造出的算子会被 TimingProxyExecutor 包一层，
    // 用于 EXPLAIN ANALYZE 收集 per-node 统计。默认 false 不改变现有行为。
    ExecutorPtr BuildExecutor(const PlanNodePtr& plan_node, ExecutionContext* context,
                              bool wrap_timing = false);

    // ---- Phase A ----
    TransactionManager* GetTransactionManager() const { return txn_manager_; }

    // ---- Spec 2.3：统一的存储访问门面 ----
    // Database 在构造完 StorageAccess 后调用本接口注入；每次 Execute() 创建
    // ExecutionContext 时再把同一指针挂到 ctx 上，算子便可通过
    // ctx.GetStorage()->GetPage(...) 调用 BPM / DM。
    void SetStorageAccess(StorageAccess* storage) { storage_access_ = storage; }

    // 71_proc_out_params：把 Database 持有的 session_vars_ 注入；每次 Execute()
    // 都会把它挂到新构造的 ExecutionContext 上，让 UdfExecutor / Trigger /
    // ExpressionEvaluator 直接读写同一张表，跨 ExecuteSQL 调用持久。
    void SetSessionVars(std::unordered_map<std::string, Value>* vars) {
        session_vars_ = vars;
    }

private:
    SystemCatalog* catalog_;
    TransactionManager* txn_manager_;
    StorageAccess* storage_access_ = nullptr;  // not owned
    // 71_proc_out_params：会话变量表指针（非所有权），指向 Database 内的同一张表。
    std::unordered_map<std::string, Value>* session_vars_ = nullptr;

    // 根据表结构构建"列名 -> 下标"的映射，供表达式求值使用
    std::unordered_map<std::string, size_t> BuildColumnIndexMap(const std::string& table_name);

    // 根据select_list推导输出结果集列名（用于展示SELECT结果）
    std::vector<std::string> DeriveOutputColumnNames(const PlanNodePtr& plan_node);
};

}  // namespace sqlcompiler
