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

    // ---- T2 隔离级别：本语句已取得、需按 READ COMMITTED 语句末释放的行读锁 ----
    // 执行算子（SeqScan/IndexScan）在逐行取得 S 锁时若处于 READ COMMITTED 则登记；
    // SERIALIZABLE 的行读锁持有到提交（由 Commit/Rollback 的 UnlockAll 释放），
    // 无需登记。外层 Execute() 在语句结束/异常路径上据此统一回收。
    void RecordRowReadLock(int64_t rid) { statement_row_read_locks_.push_back(rid); }
    const std::vector<int64_t>& GetRowReadLocks() const { return statement_row_read_locks_; }
    void ClearRowReadLocks() { statement_row_read_locks_.clear(); }

    // T2 行级锁获取结果：kOk=已取得；kUnused=未启用（自动提交/无锁管理器/无效RID）；
    // kDeadlock/kTimeout=冲突，调用方应中止本语句。
    enum class RowLockResult { kOk, kUnused, kDeadlock, kTimeout };
    // 行级共享锁（读表逐行）：仅在显式事务+注入 LockManager 且非 READ UNCOMMITTED 时取；
    // READ COMMITTED 登记到本语句行读锁，语句末由外层 Execute() 释放；SERIALIZABLE 持有到提交。
    RowLockResult AcquireRowReadLock(const RID& rid);
    // 行级独占锁（写表逐行）：所有隔离级别在显式事务内都取，持有到提交（Commit/Rollback 释放）。
    RowLockResult AcquireRowWriteLock(const RID& rid);
    // SERIALIZABLE 谓词写前检查：以该表主键建键，若有其他活动事务的读谓词覆盖该
    // 键则阻塞（或 kDeadlock/kTimeout）。仅 SERIALIZABLE 显式事务启用，其余返回 kUnused。
    RowLockResult CheckSerializablePredicate(const std::string& table_name,
                                             const std::vector<Value>& row);

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
    // Phase A：当前事务（nullptr = 隐式 auto-commit）。
    Transaction* txn_ = nullptr;
    // Phase A：所属事务管理器（由 ExecutionEngine 在构造 ctx 时注入）。
    TransactionManager* txn_manager_ = nullptr;
    // T2：当前语句已取得、需按 READ COMMITTED 语句末释放的行读锁。
    std::vector<int64_t> statement_row_read_locks_;
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
