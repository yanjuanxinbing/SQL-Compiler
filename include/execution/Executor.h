#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "catalog/SystemCatalog.h"
#include "storage_engine/Tuple.h"
#include "storage_engine/Value.h"

namespace sqlcompiler {

// Phase A 前向声明：完整定义在 txn/Transaction.h / TransactionManager.h。
// 各算子只持有指针，避免在 Executor.h 引入事务模块的 <vector> 依赖。
class Transaction;
class TransactionManager;
// Spec 2.3 前向声明：完整定义在 storage/StorageAccess.h，避免在 Executor.h 引入
// 整个存储门面头文件；算子按需在 .cpp 中 include。
class StorageAccess;

// 单条物化的 CTE：执行 CTE_DEFINE 节点时把 children[0] 的子计划跑完，
// 把所有结果行收集在这里。后续同一次查询执行中任何 CteBindNode 都直接
// 从这里读取，不必重新执行子计划。
struct CteMaterialization {
    std::string cte_name;
    std::vector<Tuple> rows;
};

// 执行上下文：贯穿整个查询执行过程，向各算子提供目录与存储访问入口
class ExecutionContext {
public:
    explicit ExecutionContext(SystemCatalog* catalog,
                             TransactionManager* txn_manager = nullptr);

    SystemCatalog* GetCatalog() const;

    // ---- Phase A：事务管理 ----
    TransactionManager* GetTransactionManager() const { return txn_manager_; }
    void SetTransactionManager(TransactionManager* mgr) { txn_manager_ = mgr; }

    // CTE 注册表：CTE_DEFINE 节点负责写入，CteBind 节点负责读取。
    // 同一次 Execute 调用内对相同 cte_name 只允许写一次（递归 CTE 多次追加）。
    void RegisterCte(const std::string& name, std::vector<Tuple> rows);
    void AppendCteRows(const std::string& name, const std::vector<Tuple>& rows);
    const std::vector<Tuple>* GetCteRows(const std::string& name) const;
    bool HasCte(const std::string& name) const;
    // 递归 CTE 的 CTE_BIND 在每次迭代时切换到「本轮新增的工作集」上。
    // 用 push/pop 把外层可见的旧结果保留下来，迭代结束后恢复。
    void PushCteOverride(const std::string& name, std::vector<Tuple> rows);
    void PopCteOverride(const std::string& name);

    // 相关子查询（correlated subquery）：子查询内部的 ColumnRefExpr 可能引用外层 SELECT
    // 的当前行。EvaluateSubquery 在运行子计划前把当前外层行的列名→值映射存入 outer_bind_，
    // 子计划里的 ExpressionEvaluator 通过 ExecutionContext 拿到这个 map 后回退解析外层列引用。
    void SetOuterBind(const std::unordered_map<std::string, Value>* bind) {
        outer_bind_ = bind;
    }
    const std::unordered_map<std::string, Value>* GetOuterBind() const {
        return outer_bind_;
    }

    // 当前子查询 / 子计划可见的「内层表名集合」（含真实表名和别名）。
    // EvaluateColumnRef 在限定列未命中 cmap 时据此判断是否要去 outer_bind 找外层列。
    // nullptr 表示没有子查询上下文，按旧行为回退到无限定列名查找。
    void SetInnerTables(const std::unordered_set<std::string>* tables) {
        inner_tables_ = tables;
    }
    const std::unordered_set<std::string>* GetInnerTables() const {
        return inner_tables_;
    }
    // 55_query: ApplyExecutor 在 LATERAL 路径下需要把右子计划视为「子查询上下文」，
    // 让 EvaluateColumnRef 知道哪些表名是右子计划的内部表，从而把同名 outer
    // 引用回退到 outer_bind。本方法在 ApplyExecutor 构造时被调用一次，
    // 把当前 inner_tables 替换为 stored_lateral_inner_tables_。
    void SetLateralInnerTables(std::unordered_set<std::string> tables) {
        stored_lateral_inner_tables_ = std::move(tables);
    }
    const std::unordered_set<std::string>* GetLateralInnerTables() const {
        return stored_lateral_inner_tables_.empty() ? nullptr
                                                     : &stored_lateral_inner_tables_;
    }

    // 43_upsert: ON DUPLICATE KEY UPDATE 的赋值右侧可能出现 VALUES(col) 形式
    // 引用「本次候选行」的列值。该绑定由 UpsertExecutor 在评估冲突路径上的
    // assignments 时推入；ExpressionEvaluator 看到 UpsertValuesRefExpr 时
    // 据此查找列名对应的候选行值。nullptr 表示当前不在 upsert 上下文中。
    void SetUpsertValuesBind(const std::unordered_map<std::string, Value>* bind) {
        upsert_values_bind_ = bind;
    }
    const std::unordered_map<std::string, Value>* GetUpsertValuesBind() const {
        return upsert_values_bind_;
    }

    // ---- Phase A：当前事务 ----
    // nullptr 表示当前没有显式事务（隐式 auto-commit）；DML 算子据此判断
    // 是否要把写入记录到事务的 undo log 中。TransactionExecutor 负责
    // BEGIN/COMMIT/ROLLBACK 时机的设置。
    void SetTransaction(Transaction* txn) { txn_ = txn; }
    Transaction* GetTransaction() const { return txn_; }

    // ---- Spec 2.3：统一的存储访问门面 ----
    // 由 ExecutionEngine 在 Execute() 创建 ctx 时挂上；算子可调用
    // ctx.GetStorage()->GetPage(...) 访问 BPM，绕过对 BufferPoolManager 的直接
    // 依赖。nullptr 表示当前 ctx 没有关联到 StorageAccess（极少发生在测试场景）。
    void SetStorage(StorageAccess* storage) { storage_ = storage; }
    StorageAccess* GetStorage() const { return storage_; }

    // ---- 59_procs (Category 8)：OUT 参数返回值表 ----
    //
    // CALL name(...) 执行后，procedure 的 OUT / INOUT 参数被回写到本字典
    // （key = 参数名）。下游 SQL 通过 CALL 后立即 SELECT * FROM
    // session_out_args（或在 V1 简化为 INSERT 持有表的方式）来读取结果。
    // 该结构仅在 Database 同一会话内有效，跨会话不保留。
    void SetOutArg(const std::string& name, Value value) {
        out_args_[name] = std::move(value);
    }
    const std::unordered_map<std::string, Value>& GetOutArgs() const {
        return out_args_;
    }
    void ClearOutArgs() { out_args_.clear(); }

    // 59_procs (Category 8)：procedure 当前局部变量绑定。
    // 当 CALL 在执行 procedure body 时，UdfExecutor 把当前 frame.locals
    // 暴露给 ExecutionContext，让 procedure 内嵌 DML（INSERT/UPDATE/DELETE）
    // 在解析表达式时能引用"局部变量 x"等。该绑定由 UdfExecutor 在
    // ExecuteStatement 中按"单语句"粒度推入/弹出。
    void SetProcLocals(const std::unordered_map<std::string, Value>* locals) {
        proc_locals_ = locals;
    }
    const std::unordered_map<std::string, Value>* GetProcLocals() const {
        return proc_locals_;
    }

    // 60_view_trigger (Category 9): AFTER 触发器 / STATEMENT 级触发器共享的会话状态。
    // - session_log_：AFTER 触发器把"会话变量"以 name → value 的形式写入；
    //   后续 SQL 可以通过 SET @var = ... 等路径回读。V1 主要用于记录 AFTER 副作用。
    //   71_proc_out_params 后改为非所有权指针，指向 Database 持有的同一张表；
    //   这样 CALL/SELECT @var / SET @var 跨 ExecuteSQL 调用都能看到对方的写入。
    // - statement_fired_：STATEMENT 级触发器避免在每行重复执行的标记。
    //   InsertExecutor / UpdateExecutor / DeleteExecutor 入口处清空，
    //   TriggerExecutor::FireAfter 在已 fire 时直接返回。
    void SetSessionVars(std::unordered_map<std::string, Value>* vars) {
        session_log_ = vars;
    }
    std::unordered_map<std::string, Value>* GetSessionVars() const {
        return session_log_;
    }
    void SetSessionVar(const std::string& name, Value v);
    Value GetSessionVar(const std::string& name) const;
    const std::unordered_map<std::string, Value>& GetSessionLog() const;
    bool MarkStatementFired(const std::string& key) {
        return !statement_fired_.insert(key).second;  // true 表示已 fire 过
    }
    void ClearStatementFired() { statement_fired_.clear(); }

private:
    SystemCatalog* catalog_;
    std::unordered_map<std::string, CteMaterialization> cte_results_;
    // 临时覆盖层：把 cte_results_[name] 暂存到 vector 首部，新行插入时使用。
    // pop 时还原。这样递归 CTE 既能保留累计结果，又能迭代时让子查询看到 delta。
    std::unordered_map<std::string, std::vector<std::vector<Tuple>>> cte_overrides_;
    // 相关子查询的外层行绑定：nullptr 表示当前不在子查询求值上下文中。
    const std::unordered_map<std::string, Value>* outer_bind_ = nullptr;
    // 当前子查询的内层表名集合（含别名）。
    const std::unordered_set<std::string>* inner_tables_ = nullptr;
    // 43_upsert: ON DUPLICATE KEY UPDATE 中 VALUES(col) 的候选行绑定。
    const std::unordered_map<std::string, Value>* upsert_values_bind_ = nullptr;
    // 55_query: LATERAL 内层表集合（在 ApplyExecutor 启动时一次性设置，
    // 让后续 Filter/Project 的 evaluator 把 inner_tables 当作子查询上下文）。
    std::unordered_set<std::string> stored_lateral_inner_tables_;
    // Phase A：当前事务（nullptr = 隐式 auto-commit）。
    Transaction* txn_ = nullptr;
    // Phase A：所属事务管理器（由 ExecutionEngine 在构造 ctx 时注入）。
    TransactionManager* txn_manager_ = nullptr;
    // Spec 2.3：统一的存储访问门面（非所有权裸指针）。
    StorageAccess* storage_ = nullptr;
    // 59_procs (Category 8): CALL 返回的 OUT / INOUT 参数值。
    std::unordered_map<std::string, Value> out_args_;
    // 59_procs (Category 8): procedure 当前局部变量绑定。
    const std::unordered_map<std::string, Value>* proc_locals_ = nullptr;
    // 60_view_trigger (Category 9): AFTER 触发器会话变量 + 语句级 fire 标记。
    // 71_proc_out_params：session_log_ 由"本地上 map"改为"非所有权指针"，
    // 由 ExecutionEngine::Execute 把 Database::session_vars_ 注入到 ctx 上；
    // SetSessionVar / GetSessionVar 通过该指针读写，跨 ExecuteSQL 调用持久。
    std::unordered_map<std::string, Value>* session_log_ = nullptr;
    std::unordered_set<std::string> statement_fired_;
};

// 执行算子基类，采用火山模型（Volcano / Iterator Model）：
//   Init() 完成准备工作（如打开表迭代器）；
//   Next() 每次产出一条Tuple，返回false表示没有更多数据（DDL/DML语句
//   可以不产出Tuple，Next()恒定返回false，副作用在Init()中完成）
class Executor {
public:
    explicit Executor(ExecutionContext* context);
    virtual ~Executor() = default;

    virtual void Init() = 0;
    virtual bool Next(Tuple* tuple) = 0;

protected:
    ExecutionContext* context_;
};
using ExecutorPtr = std::unique_ptr<Executor>;

}  // namespace sqlcompiler
