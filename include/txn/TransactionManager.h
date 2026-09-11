#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "storage/BufferPoolManager.h"
#include "txn/Transaction.h"

namespace sqlcompiler {

// 事务管理器（Phase B：写 WAL 记录 + LSN-aware）。
//
// 职责：
//   * 维护会话级「当前事务」与「事务嵌套深度」；
//   * 提供 Begin / Commit / Rollback / Savepoint / RollbackTo / Release 接口；
//   * 在 Rollback 时按逆序把 txn->undo_log_ 中的 before-image 写回对应页面，
//     实现逻辑回滚。
//   * Phase B：Commit / Rollback 时向 LogManager 追加 BEGIN/COMMIT/ABORT
//     记录；COMMIT 路径在 LogManager::Flush 之后调用 DiskManager::Sync()
//     确保 dirty page 落盘。
//
// 注意：本实现不持久化、不支持并发。Begin 内嵌 BEGIN 是按 depth 累加的；
// COMMIT/ROLLBACK 在最外层才真正清空 undo 日志。
class LogManager;
class DiskManager;

class TransactionManager {
public:
    TransactionManager();

    // Begin: 若当前无显式事务（depth_ == 0），分配新 Transaction；
    // 否则只把 depth_ 加 1（嵌套 BEGIN 视作 no-op，深度仍跟踪以平衡）。
    // 返回当前的 Transaction 指针（嵌套时返回上一层的 txn）。
    Transaction* Begin();

    // Commit: 仅在最外层（depth_ == 1）真正清空 undo 日志并释放 txn；
    // 否则仅 depth_--。Phase B：先 LogManager->AppendRecord(COMMIT) 再
    // LogManager->Flush() 再 DiskManager->Sync()，最后清 txn。
    void Commit();

    // Rollback: 把 undo_log_ 反向应用到 BufferPool，再清空日志；depth 减 1。
    // Phase B：应用 undo 之后再写一条 ABORT 记录并 Flush WAL。
    void Rollback();

    // Savepoint: 把当前 undo_log_ 长度记到栈里。Phase A 限定 depth == 1 才允许。
    void Savepoint(const std::string& name);

    // ROLLBACK TO name: 把 undo_log_ 反向应用到「该保存点 undo_log_offset 之前」
    // 的状态。Phase A 实现为「应用 offset 之后所有记录的逆序」，再截断日志。
    void RollbackToSavepoint(const std::string& name);

    // RELEASE SAVEPOINT name: 仅从栈中移除，保留日志（与 ROLLBACK TO 不同）。
    void ReleaseSavepoint(const std::string& name);

    // 当前事务指针；nullptr 表示当前无显式事务（隐式 auto-commit 上下文）。
    Transaction* GetCurrentTransaction() const { return current_txn_; }

    // 当前嵌套深度。0 = 无显式事务，>= 1 = 在某个 txn 内。
    int GetCurrentDepth() const { return current_txn_depth_; }

    // 在测试 / Shutdown 时强制清空当前事务（不应用 undo）。Phase A 仅在
    // 进程退出前调用，避免悬挂的 txn 句柄。
    void ResetForShutdown();

    // 让 TransactionManager 拿到 BufferPoolManager 以在 Rollback 时写回页面。
    void SetBufferPoolManager(BufferPoolManager* bpm) { buffer_pool_manager_ = bpm; }

    // ---- Phase B：把 LogManager / DiskManager 注入到事务生命周期 ----
    // nullptr 表示 Phase A 兼容（不写 WAL）。
    void SetLogManager(LogManager* lm) { log_manager_ = lm; }
    void SetDiskManager(DiskManager* dm) { disk_manager_ = dm; }

    // 上次分配的 txn_id；Database 用它初始化 commit 时填 prev_lsn。
    int64_t GetNextTxnId() const { return next_txn_id_; }
    // 在 Begin 之后立即记录 BEGIN 日志；Database 在执行栈里显式调一次，
    // 让 BEGIN 顺序先于任何 DML 的 UPDATE 记录。
    void LogBegin(Transaction* txn);

    // ---- Phase C：rollback 中途崩溃注入 ----
    // > 0 时，每次 Rollback 完成一条 undo 步骤后扣减 1；归零立即 _Exit(1)。
    // 由 Database 在收到 \\crash_after_undo_steps N 后调用。
    void SetCrashAfterUndoSteps(int n) { crash_after_undo_steps_ = n > 0 ? n : 0; }

private:
    // 把 undo_log_ 中 from..to 区间（按逆序）逐条写回 page。
    // 调用方负责 txn 的 undo_log_ 在调用前后的语义。
    void ApplyUndoRange(Transaction* txn, size_t from, size_t to);

    BufferPoolManager* buffer_pool_manager_ = nullptr;
    LogManager* log_manager_ = nullptr;
    DiskManager* disk_manager_ = nullptr;
    Transaction* current_txn_ = nullptr;
    int current_txn_depth_ = 0;
    int64_t next_txn_id_ = 1;
    int crash_after_undo_steps_ = 0;  // Phase C：rollback 中途崩溃注入计数
};

}  // namespace sqlcompiler