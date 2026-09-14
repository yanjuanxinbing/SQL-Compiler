#pragma once

#include "execution/Executor.h"
#include "plan/Plan.h"

namespace sqlcompiler {

// 事务控制语句执行器：处理 BEGIN / COMMIT / ROLLBACK / SAVEPOINT /
// ROLLBACK TO / RELEASE SAVEPOINT。
//
// 真正的 BEGIN / COMMIT / ROLLBACK 副作用由 TransactionManager 承担：
// 本执行器只把请求转给 TransactionManager 并把新创建的 txn 句柄挂到
// ExecutionContext 上，供后续 DML 算子在 InsertTuple/UpdateTuple/
// DeleteTuple 等写路径上把 undo log 写进去。
//
// 计划节点形态：5 个独立 PlanNode 子类（BeginNode/CommitNode/...），
// 每个对应一个 Executor 字段，由 BuildExecutor 在工厂里按类型分派。
class BeginExecutor : public Executor {
public:
    explicit BeginExecutor(ExecutionContext* context);
    void Init() override;
    bool Next(Tuple* tuple) override;
};

class CommitExecutor : public Executor {
public:
    explicit CommitExecutor(ExecutionContext* context);
    void Init() override;
    bool Next(Tuple* tuple) override;
};

class RollbackExecutor : public Executor {
public:
    explicit RollbackExecutor(ExecutionContext* context);
    void Init() override;
    bool Next(Tuple* tuple) override;
};

class SavepointExecutor : public Executor {
public:
    SavepointExecutor(ExecutionContext* context, std::string name);
    void Init() override;
    bool Next(Tuple* tuple) override;
private:
    std::string name_;
};

class RollbackToSavepointExecutor : public Executor {
public:
    RollbackToSavepointExecutor(ExecutionContext* context, std::string name);
    void Init() override;
    bool Next(Tuple* tuple) override;
private:
    std::string name_;
};

class ReleaseSavepointExecutor : public Executor {
public:
    ReleaseSavepointExecutor(ExecutionContext* context, std::string name);
    void Init() override;
    bool Next(Tuple* tuple) override;
private:
    std::string name_;
};

}  // namespace sqlcompiler