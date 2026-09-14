// =============================================================================
// Phase A 事务控制语句执行器。
//
// 所有 BEGIN / COMMIT / ROLLBACK / SAVEPOINT / ROLLBACK TO / RELEASE SAVEPOINT
// 都通过 ExecutionContext::GetTransactionManager() 转发（由 ExecutionEngine
// 在构造 ctx 时注入 Database 持有的 TransactionManager）。
//
// 副作用总结：
//   * BeginExecutor         → TransactionManager::Begin, ctx->txn_ = 新 txn
//   * CommitExecutor        → TransactionManager::Commit, ctx->txn_ = nullptr
//   * RollbackExecutor      → TransactionManager::Rollback, ctx->txn_ = nullptr
//   * SavepointExecutor     → TransactionManager::Savepoint(name)
//   * RollbackToSavepoint   → TransactionManager::RollbackToSavepoint(name)
//   * ReleaseSavepoint      → TransactionManager::ReleaseSavepoint(name)
// =============================================================================

#include "execution/TransactionExecutor.h"

#include "txn/Transaction.h"
#include "txn/TransactionManager.h"

namespace sqlcompiler {

BeginExecutor::BeginExecutor(ExecutionContext* context) : Executor(context) {}

void BeginExecutor::Init() {
    TransactionManager* mgr = context_ ? context_->GetTransactionManager() : nullptr;
    if (mgr == nullptr) return;
    Transaction* txn = mgr->Begin();
    if (mgr->GetCurrentDepth() == 1 && txn != nullptr) {
        // 第一次 BEGIN：写 BEGIN 日志，让 redo 看到完整事务边界。
        mgr->LogBegin(txn);
    }
    if (context_ != nullptr) context_->SetTransaction(txn);
}

bool BeginExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

CommitExecutor::CommitExecutor(ExecutionContext* context) : Executor(context) {}

void CommitExecutor::Init() {
    TransactionManager* mgr = context_ ? context_->GetTransactionManager() : nullptr;
    if (mgr == nullptr) return;
    mgr->Commit();
    if (context_ != nullptr) context_->SetTransaction(nullptr);
}

bool CommitExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

RollbackExecutor::RollbackExecutor(ExecutionContext* context) : Executor(context) {}

void RollbackExecutor::Init() {
    TransactionManager* mgr = context_ ? context_->GetTransactionManager() : nullptr;
    if (mgr == nullptr) return;
    mgr->Rollback();
    if (context_ != nullptr) context_->SetTransaction(nullptr);
}

bool RollbackExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

SavepointExecutor::SavepointExecutor(ExecutionContext* context, std::string name)
    : Executor(context), name_(std::move(name)) {}

void SavepointExecutor::Init() {
    TransactionManager* mgr = context_ ? context_->GetTransactionManager() : nullptr;
    if (mgr == nullptr) return;
    mgr->Savepoint(name_);
}

bool SavepointExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

RollbackToSavepointExecutor::RollbackToSavepointExecutor(ExecutionContext* context,
                                                       std::string name)
    : Executor(context), name_(std::move(name)) {}

void RollbackToSavepointExecutor::Init() {
    TransactionManager* mgr = context_ ? context_->GetTransactionManager() : nullptr;
    if (mgr == nullptr) return;
    mgr->RollbackToSavepoint(name_);
}

bool RollbackToSavepointExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

ReleaseSavepointExecutor::ReleaseSavepointExecutor(ExecutionContext* context,
                                                   std::string name)
    : Executor(context), name_(std::move(name)) {}

void ReleaseSavepointExecutor::Init() {
    TransactionManager* mgr = context_ ? context_->GetTransactionManager() : nullptr;
    if (mgr == nullptr) return;
    mgr->ReleaseSavepoint(name_);
}

bool ReleaseSavepointExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

}  // namespace sqlcompiler