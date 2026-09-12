#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "storage/BufferPoolManager.h"
#include "txn/Transaction.h"

namespace sqlcompiler {

class LockManager;
class LogManager;
class DiskManager;
class CommitTracker;

// 全局事务 id 序列器：跨会话共享的原子计数器，保证在「每会话一个
// TransactionManager」的并发模型下，tns_id 依然全局唯一（WAL/recovery
// 依赖唯一 txn_id 区分不同事务）。默认由单个 TransactionManager 独占，
// 也可在构造时注入共享实例。
struct TxnIdSequencer {
    std::atomic<int64_t> value{0};
    int64_t Next() { return value.fetch_add(1) + 1; }
    int64_t Current() const { return value.load(); }
};

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
    // txn_id 序列器：默认独占一个，或注入跨会话共享的实例（并发会话场景）。
    explicit TransactionManager(TxnIdSequencer* shared_sequencer = nullptr);

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

    // 本会话已发出/当前可用的 txn_id；并发场景下由共享 TxnIdSequencer 提供，
    // 保证各会话 id 全局唯一。Database 用它初始化 commit 时填 prev_lsn。
    int64_t GetNextTxnId() const { return seq_->Current(); }

    // 当前嵌套深度。0 = 无显式事务，>= 1 = 在某个 txn 内。
    int GetCurrentDepth() const { return current_txn_depth_; }
    // 在 Begin 之后立即记录 BEGIN 日志；Database 在执行栈里显式调一次，
    // 让 BEGIN 顺序先于任何 DML 的 UPDATE 记录。
    void LogBegin(Transaction* txn);

    // 在测试 / Shutdown 时强制清空当前事务（不应用 undo）。Phase A 仅在
    // 进程退出前调用，避免悬挂的 txn 句柄。
    void ResetForShutdown();

    // 让 TransactionManager 拿到 BufferPoolManager 以在 Rollback 时写回页面。
    void SetBufferPoolManager(BufferPoolManager* bpm) { buffer_pool_manager_ = bpm; }

    // ---- Phase B：把 LogManager / DiskManager 注入到事务生命周期 ----
    // nullptr 表示 Phase A 兼容（不写 WAL）。
    void SetLogManager(LogManager* lm) { log_manager_ = lm; }
    void SetDiskManager(DiskManager* dm) { disk_manager_ = dm; }

    // ---- Phase C：rollback 中途崩溃注入 ----
    // > 0 时，每次 Rollback 完成一条 undo 步骤后扣减 1；归零立即 _Exit(1)。
    // 由 Database 在收到 \\crash_after_undo_steps N 后调用。
    void SetCrashAfterUndoSteps(int n) { crash_after_undo_steps_ = n > 0 ? n : 0; }

    // ---- T2：隔离级别与事务级锁 ----
    // 注入跨会话共享的 LockManager（并发会话协调同一组表锁）。nullptr = 不启用。
    void SetLockManager(LockManager* lm) { lock_manager_ = lm; }
    LockManager* GetLockManager() const { return lock_manager_; }
    // 本会话默认隔离级别：在一次 BEGIN 分配新 txn 时采样进该事务。
    void SetIsolationLevel(IsolationLevel lv) { default_isolation_ = lv; }
    IsolationLevel GetIsolationLevel() const { return default_isolation_; }

    // ---- MVCC 快照隔离：共享 CommitTracker ----
    // 注入跨会话共享的提交跟踪器（Database 持有，向各会话 TM 注入同实例）。
    // nullptr = 未启用快照特性（kSnapshot 读按无头 legacy 处理）。
    void SetCommitTracker(CommitTracker* ct) { commit_tracker_ = ct; }
    CommitTracker* GetCommitTracker() const { return commit_tracker_; }

private:
    // 把 undo_log_ 中 from..to 区间（按逆序）逐条写回 page。
    // 调用方负责 txn 的 undo_log_ 在调用前后的语义。
    void ApplyUndoRange(Transaction* txn, size_t from, size_t to);

    // ---- MVCC 提交收尾辅助（kSnapshot 最外层 Commit 时使用）----
    // first-committer-wins：重读 txn 写集中每行 head；若已被已提交的其他事务
    // 改写（head.begin_xid 非本事务且已提交）则返回 true（应中止本事务）。
    bool SnapshotWriteConflict(const Transaction* txn);
    // 提交拿到 CSN 后，逐槽位回填 begin_csn/end_csn。
    void BackfillVersionCsn(Transaction* txn, int64_t csn);

    BufferPoolManager* buffer_pool_manager_ = nullptr;
    LogManager* log_manager_ = nullptr;
    DiskManager* disk_manager_ = nullptr;
    Transaction* current_txn_ = nullptr;
    int current_txn_depth_ = 0;
    TxnIdSequencer* seq_ = nullptr;        // txn_id 来源（共享或独占）
    TxnIdSequencer own_seq_;               // 未注入共享序列器时的默认序列器
    int crash_after_undo_steps_ = 0;       // Phase C：rollback 中途崩溃注入计数
    // T2：跨会话共享的锁管理器（DB 注入）；nullptr = 隔离失效（不锁）。
    LockManager* lock_manager_ = nullptr;
    // 本会话默认隔离级别（BEGIN 时采样到新 txn）。
    IsolationLevel default_isolation_ = IsolationLevel::kSerializable;
    // MVCC 快照隔离：跨会话共享的提交跟踪器（DB 注入）；nullptr = 未启用。
    CommitTracker* commit_tracker_ = nullptr;
};

}  // namespace sqlcompiler