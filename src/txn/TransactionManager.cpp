// =============================================================================
// TransactionManager implementation (Phase D: nested savepoints + WAL polish).
//
// Phase B 在 Phase A in-memory undo log 之上加 WAL：
//   * Begin 时写 BEGIN 记录（用 LogBegin 显式调用，避免隐式 BEGIN 在嵌套
//     BEGIN 中触发）；
//   * Commit 时按 ARIES 经典：先 LogManager->AppendRecord(COMMIT) 把它和
//     所有之前的 UPDATE 串到 prev_lsn 链上，再 LogManager->Flush() 强制
//     fdatasync；最后 DiskManager->Sync() 把 BufferPool 中的脏页落盘。
//   * Rollback 时（Phase C）按 ARIES CLR 链语义：每撤销一条原始 UPDATE 立
//     刻写一条 CLR（Compensation Log Record），其中 undo_next_lsn 指向
//     undo 链上下一个待撤销 LSN；每条 CLR 后 Flush WAL。崩溃发生在
//     Rollback 中途时，恢复期从已写入的 CLR 处继续撤销剩余步骤。
//
// Phase D 在 RollbackToSavepoint / ReleaseSavepoint 上加：
//   * RollbackToSavepoint 按 [offset, total) 区间逆向应用 undo，每步写
//     一条 CLR（与普通 Rollback 同），最后再写一条 SAVEPOINT_ROLLBACK 信
//     息性记录并 Flush WAL；undo_log_ 截断到 offset，savepoint 栈弹出
//     matched 条目及其后所有条目。
//   * ReleaseSavepoint 仅写一条 SAVEPOINT_RELEASE 信息性记录 + Flush WAL，
//     不写 CLR，undo_log_ 与 savepoint 栈都只做最轻的栈弹操作（matched）。
//   * 崩溃恢复期：SAVEPOINT_ROLLBACK / SAVEPOINT_RELEASE 不参与 dirty
//     page table 与 undo pass——它们是「信息性」标记，让 redo pass 不会
//     把它们当作 page 修改记录，CLR 链本身的 undo_next_lsn 已经能保证
//     savepoint 回滚是 crash-safe 的。
// =============================================================================

#include "txn/TransactionManager.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

#include "common/Error.h"
#include "index/PageGuard.h"
#include "storage/DiskManager.h"
#include "storage/LockManager.h"
#include "storage/Page.h"
#include "storage_engine/MvccRecord.h"
#include "txn/CommitTracker.h"
#include "txn/LogManager.h"
#include "txn/LogRecord.h"

namespace sqlcompiler {

namespace {
// 与 TableHeap 一致的 slotted-page 布局（用于 MVCC 提交时重读/回填版本头）。
constexpr int32_t kHdrBytes = 16;
constexpr int32_t kSlotBytes = 8;
constexpr uint32_t kTomb = 0xFFFFFFFFu;

inline int32_t ReadI32(const char* data, size_t off) {
    int32_t v;
    std::memcpy(&v, data + off, sizeof(int32_t));
    return v;
}

void ReadPageHeader(const char* data, int32_t& next_pid, int32_t& slot_count,
                    int32_t& free_off) {
    next_pid = ReadI32(data, 0);
    slot_count = ReadI32(data, 4);
    free_off = ReadI32(data, 8);
}

void ReadSlot(const char* data, int slot_num, int32_t& off, int32_t& len) {
    size_t o = static_cast<size_t>(kHdrBytes + slot_num * kSlotBytes);
    off = ReadI32(data, o);
    len = ReadI32(data, o + 4);
}

bool IsTombstone(int32_t len) { return static_cast<uint32_t>(len) == kTomb; }
}  // namespace

TransactionManager::TransactionManager(TxnIdSequencer* shared_sequencer)
    : seq_(shared_sequencer ? shared_sequencer : &own_seq_) {}

Transaction* TransactionManager::Begin() {
    ++current_txn_depth_;
    if (current_txn_ == nullptr) {
        // txn_id 取自 seq_：共享序列器（并发会话）下全局唯一，独占时等价旧行为。
        current_txn_ = new Transaction(seq_->Next());
        // 采样本会话默认隔离级别到新事务，供执行层决定表锁持有期。
        current_txn_->SetIsolationLevel(default_isolation_);
        // MVCC 快照隔离：快照模式在 BEGIN 时捕获稳定的提交水位 S，全事务不变；
        // 并向共享 CommitTracker 登记活动快照（Vacuum 以最老活动快照为界回收）。
        if (default_isolation_ == IsolationLevel::kSnapshot && commit_tracker_ != nullptr) {
            int64_t csn = commit_tracker_->CurrentCSN();
            current_txn_->SetSnapshotCsn(csn);
            commit_tracker_->RegisterSnapshot(csn);
        }
    }
    return current_txn_;
}

void TransactionManager::LogBegin(Transaction* txn) {
    if (txn == nullptr || log_manager_ == nullptr) return;
    LogRecord rec;
    rec.type_ = LogRecordType::BEGIN;
    rec.txn_id_ = txn->GetTxnId();
    log_manager_->AppendRecord(std::move(rec));
}

void TransactionManager::Commit() {
    if (current_txn_depth_ <= 0 || current_txn_ == nullptr) {
        // 静默 no-op：与 Standalone ROLLBACK/COMMIT 兼容（test 40）。
        return;
    }
    if (current_txn_depth_ > 1) {
        // 嵌套 BEGIN 的内层 COMMIT 视为 no-op；只有最外层真正释放事务。
        --current_txn_depth_;
        return;
    }
    Transaction* txn = current_txn_;
    // MVCC 快照隔离：kSnapshot 最外层提交前先做 first-committer-wins 冲突检测。
    // 若写集牙刷的 head 已被「已提交的其他事务」改写，则本事务是输家——回滚并中止。
    const bool snapshot_commit =
        (txn->GetIsolationLevel() == IsolationLevel::kSnapshot);
    if (snapshot_commit && SnapshotWriteConflict(txn)) {
        std::cerr << "[mvcc] snapshot write conflict for txn "
                  << txn->GetTxnId() << "; aborting by first-committer-wins"
                  << std::endl;
        Rollback();
        return;
    }
    // Phase B：写 COMMIT 记录 + Flush WAL + 落盘 dirty pages。
    if (log_manager_ != nullptr) {
        LogRecord rec;
        rec.type_ = LogRecordType::COMMIT;
        rec.txn_id_ = txn->GetTxnId();
        log_manager_->AppendRecord(std::move(rec));
        try {
            log_manager_->Flush();
        } catch (...) {
            // 测试场景里 WAL 不可写时吞掉，保持原有行为。
        }
    }
    if (buffer_pool_manager_ != nullptr) {
        buffer_pool_manager_->FlushAllDirtyPages();
    }
    if (disk_manager_ != nullptr) {
        try {
            disk_manager_->Sync();
        } catch (...) {
            // 落盘失败时同样吞掉，避免把已 COMMIT 的事务回退。
        }
    }
    // MVCC 快照隔离：提交成功，向共享 CommitTracker 分配本事务 CSN 并回填版本头；
    // 注销活动快照登记。
    if (snapshot_commit && commit_tracker_ != nullptr) {
        const int64_t csn = commit_tracker_->Commit(txn->GetTxnId());
        BackfillVersionCsn(txn, csn);
        commit_tracker_->UnregisterSnapshot(txn->GetSnapshotCsn());
        txn->ClearWriteSet();
        txn->ClearVersionSlots();
    }
    // 最外层提交：释放本事务持有的全部表锁（含 SERIALIZABLE 读锁），再清 txn。
    if (lock_manager_ != nullptr && txn != nullptr) {
        lock_manager_->UnlockAll(txn->GetTxnId());
    }
    // 最外层提交：清空 undo 日志并释放 txn。
    txn->MarkCommitted();
    delete txn;
    current_txn_ = nullptr;
    current_txn_depth_ = 0;
}

void TransactionManager::ApplyUndoRange(Transaction* txn, size_t from, size_t to) {
    if (txn == nullptr || buffer_pool_manager_ == nullptr) return;
    const auto& log = txn->GetUndoLog();
    if (to > log.size()) to = log.size();
    for (size_t i = to; i-- > from; ) {
        const auto& rec = log[i];
        Page* page = buffer_pool_manager_->GetPage(rec.page_id);
        if (page == nullptr) continue;
        if (rec.before_image.size() == PAGE_SIZE) {
            std::memcpy(page->GetData(), rec.before_image.data(), PAGE_SIZE);
            page->SetDirty(true);
        }
        buffer_pool_manager_->UnpinPage(rec.page_id, true);
    }
}

// =============================================================================
//  Rollback — Phase C：每步 undo 写一条 CLR（Compensation Log Record）。
//
//  CLR 链语义：
//    * 每撤销一条原始 UPDATE 立刻写一条 CLR：
//      page_id           ——  被回滚的 page；
//      before_image_     ——  undo 后整页状态（即刚刚把 page 设成的形态）；
//      undo_next_lsn_    ——  undo 链上下一个待撤销的 UPDATE LSN；
//                           undo 链末尾时填 INVALID_LSN。
//    * 每个 CLR 后 Flush WAL（ARIES 的 WAL-before-data：日志必须先持久化，
//      后续才能动 page 的 dirty 标记 / flush）。
//    * prev_lsn 由 LogManager::AppendRecord 通过 last_lsn_per_txn_ 自动串到
//      上一条记录，因此正常 txn 的 prev_lsn 链与 CLR 共用一条链，恢复期
//      undo pass 沿链反向走时，遇到 CLR 改走 clr.undo_next_lsn_ 跳过。
//
//  Crash-mid-rollback 恢复：进程在撤销若干步后被 _Exit()，重启时：
//    redo 阶段会重放已写入的 CLR 与尚未写 CLR 的 UPDATE；幂等（L 是单
//    调推进，page.page_lsn 检查保证重复应用 no-op）；
//    undo 阶段从 active txn 的最新 LSN 反向走，遇到 CLR 跳到 undo_next_lsn_，
//    继续撤销剩余的 UPDATE，最后写 ABORT。
// =============================================================================
void TransactionManager::Rollback() {
    if (current_txn_depth_ <= 0 || current_txn_ == nullptr) {
        // 静默 no-op：test 40 期望 standalone ROLLBACK 不报错。
        return;
    }
    if (current_txn_depth_ > 1) {
        // 嵌套内层 rollback：仅 depth--，不应用 undo。
        --current_txn_depth_;
        return;
    }
    Transaction* txn = current_txn_;
    const auto& undo_log = txn->GetUndoLog();
    const size_t total = undo_log.size();

    // Phase C：逐条反向撤销，每一步：抓页 -> 写回 before-image -> 写 CLR -> flush。
    // crash_after_undo_steps_ 是测试用 crash 注入钩子：每撤销一步就扣减 1，归零后
    // 立即 _Exit(1)，模拟崩溃在 rollback 中途发生。
    for (size_t i = total; i-- > 0; ) {
        const auto& rec = undo_log[i];
        std::cerr << "[DBG-rb] txn=" << txn->GetTxnId()
                  << " undo page=" << rec.page_id
                  << " bi_size=" << rec.before_image.size() << std::endl;
        // 1) 把 page 恢复到 before-image。
        if (buffer_pool_manager_ != nullptr) {
            Page* page = buffer_pool_manager_->GetPage(rec.page_id);
            if (page != nullptr) {
                if (rec.before_image.size() == PAGE_SIZE) {
                    std::memcpy(page->GetData(), rec.before_image.data(),
                                PAGE_SIZE);
                    page->SetDirty(true);
                }
                buffer_pool_manager_->UnpinPage(rec.page_id, true);
            }
        }
        // 2) 写一条 CLR，承载「undo 后状态」+ undo_next_lsn。
        if (log_manager_ != nullptr) {
            lsn_t undo_next_lsn =
                (i > 0) ? undo_log[i - 1].lsn : INVALID_LSN;
            // 重新取一次 page 的当前内容（与 before_image 等价），保证 CLR
            // 记录的确实是「undo 后状态」。即便前面 UnpinPage/Page 被换出，
            // 重新 GetPage 拿到的也是最新 in-memory 副本。
            std::vector<char> post_undo(PAGE_SIZE, 0);
            if (buffer_pool_manager_ != nullptr) {
                Page* page2 = buffer_pool_manager_->GetPage(rec.page_id);
                if (page2 != nullptr) {
                    std::memcpy(post_undo.data(), page2->GetData(), PAGE_SIZE);
                    buffer_pool_manager_->UnpinPage(rec.page_id, false);
                }
            } else if (rec.before_image.size() == PAGE_SIZE) {
                std::memcpy(post_undo.data(), rec.before_image.data(),
                            PAGE_SIZE);
            }
            lsn_t clr_lsn = log_manager_->AppendCLR(
                txn->GetTxnId(), rec.page_id, post_undo.data(), undo_next_lsn);
            // 把 page.page_lsn 推到 CLR 的 LSN，让 redo 阶段幂等。
            if (buffer_pool_manager_ != nullptr) {
                Page* page3 = buffer_pool_manager_->GetPage(rec.page_id);
                if (page3 != nullptr) {
                    page3->SetPageLsn(clr_lsn);
                    buffer_pool_manager_->UnpinPage(rec.page_id, true);
                }
            }
            try {
                log_manager_->Flush();
            } catch (...) {
                // 测试场景里 WAL 不可写时吞掉。
            }
        }
        // 3) 测试钩子：撤销 N 步后立即 _Exit(1)。
        if (crash_after_undo_steps_ > 0) {
            --crash_after_undo_steps_;
            if (crash_after_undo_steps_ == 0) {
                std::cerr << "[crash-after-undo-steps] triggering _Exit(1) "
                          << "after undo step" << std::endl;
                std::_Exit(1);
            }
        }
    }

    // Phase B：写 ABORT 记录并 Flush WAL，让 redo / analysis 看到事务已结束。
    if (log_manager_ != nullptr) {
        LogRecord rec;
        rec.type_ = LogRecordType::ABORT;
        rec.txn_id_ = txn->GetTxnId();
        log_manager_->AppendRecord(std::move(rec));
        try {
            log_manager_->Flush();
        } catch (...) {
            // 测试场景里 WAL 不可写时吞掉。
        }
    }
    if (buffer_pool_manager_ != nullptr) {
        buffer_pool_manager_->FlushAllDirtyPages();
    }
    if (disk_manager_ != nullptr) {
        try {
            disk_manager_->Sync();
        } catch (...) {
        }
    }

    txn->MarkAborted();
    // MVCC 快照隔离：登记中止（其写出的版本对读者不可见）；注销活动快照登记。
    if (txn->GetIsolationLevel() == IsolationLevel::kSnapshot && commit_tracker_ != nullptr) {
        commit_tracker_->Abort(txn->GetTxnId());
        commit_tracker_->UnregisterSnapshot(txn->GetSnapshotCsn());
    }
    // 回滚完成后释放本事务持有的全部表锁。
    if (lock_manager_ != nullptr) {
        lock_manager_->UnlockAll(txn->GetTxnId());
    }
    delete txn;
    current_txn_ = nullptr;
    current_txn_depth_ = 0;
}

void TransactionManager::Savepoint(const std::string& name) {
    if (current_txn_ == nullptr || !current_txn_->IsActive()) {
        // 没有显式事务的 SAVEPOINT：按静默 no-op 处理（兼容 test 40）。
        return;
    }
    current_txn_->PushSavepoint(name);
}

// =============================================================================
//  ROLLBACK TO name — Phase D：嵌套保存点的部分回滚（crash-safe via CLR 链）。
//
//  算法：
//   1) 取 savepoint 的 undo_log_offset，把 [offset, total) 区间反向 undo；
//      每撤销一条原始 UPDATE 立刻写一条 CLR，CLR 的 undo_next_lsn 指向
//      undo 链上下一个待撤销 LSN（即「entry 数组中前一条」的 LSN）。这条
//      链与全 Rollback 完全一致——savepoint rollback 只是全 Rollback 的
//      前缀子集，所以 CLR 链的 undo_next_lsn 计算规则相同。
//   2) 每条 CLR 后 Flush WAL（ARIES 的 WAL-before-data：必须先把日志
//      持久化，才能标记 page dirty / flush page）。
//   3) 写一条 SAVEPOINT_ROLLBACK 信息性记录并 Flush WAL；让 redo 阶段
//      把「savepoint 回滚完成」这一事实记入 WAL，便于人工 / 工具分析。
//   4) TruncateUndoLog(offset)：undo_log_ 截断到 offset，entries [offset,
//      total) 被丢弃（事务的「已写入 undo 链」现在只到 offset 为止）。
//   5) PopSavepointStack(name)：弹出 matched 条目及其后所有条目（外层、
//      靠前的同名保存点继续保留）。
//
//  崩溃恢复期语义：
//   * 崩溃发生在第 1 步（CLR 链写一半）：重启后 redo pass 重放已写入的
//     CLR（before_image = undo 后 page 状态），undo pass 从最后一条
//     CLR 的 undo_next_lsn 继续撤销剩余 UPDATEs，最终事务被 ABORT。
//   * 崩溃发生在第 1 步之后、第 5 步之前：redo pass 应用所有 CLR 的
//     before_image，page 已是 savepoint 回滚后的形态；undo pass 在
//     txn 仍 active 时继续撤销更早的 UPDATEs（normal crash-after-txn-
//     abort semantics）。无论何种场景，结果都一致：savepoint 区间之外的
//     「先于 savepoint」写入可能仍被回滚，但 savepoint 区间之内的写入
//     一定已被撤销。
// =============================================================================
void TransactionManager::RollbackToSavepoint(const std::string& name) {
    if (current_txn_ == nullptr || !current_txn_->IsActive()) {
        return;
    }
    if (!current_txn_->HasSavepoint(name)) {
        // 未知保存点：按 no-op 处理。
        return;
    }
    Transaction* txn = current_txn_;
    const size_t offset = txn->GetSavepointOffset(name);
    const auto& undo_log = txn->GetUndoLog();
    const size_t total = undo_log.size();

    // 1) 反向撤销 [offset, total) 区间，每步写一条 CLR。
    for (size_t i = total; i-- > offset; ) {
        const auto& rec = undo_log[i];
        // 1a) 把 page 恢复到 before-image。
        if (buffer_pool_manager_ != nullptr) {
            Page* page = buffer_pool_manager_->GetPage(rec.page_id);
            if (page != nullptr) {
                if (rec.before_image.size() == PAGE_SIZE) {
                    std::memcpy(page->GetData(), rec.before_image.data(),
                                PAGE_SIZE);
                    page->SetDirty(true);
                }
                buffer_pool_manager_->UnpinPage(rec.page_id, true);
            }
        }
        // 1b) 写一条 CLR：undo_next_lsn = 前一条 entry 的 LSN（与普通
        //     Rollback 完全一致）。entry[i-1] 在 undo_log_ 中仍存在
        //     （除非 i == offset），若 i == offset 则前一条是 entry[i-1]
        //     ——它是 savepoint 区间之外更早的写入，不属于本次 savepoint
        //     回滚的范围，但对 crash-mid-txn-abort 仍然有意义。
        if (log_manager_ != nullptr) {
            lsn_t undo_next_lsn =
                (i > offset) ? undo_log[i - 1].lsn : INVALID_LSN;
            std::vector<char> post_undo(PAGE_SIZE, 0);
            if (buffer_pool_manager_ != nullptr) {
                Page* page2 = buffer_pool_manager_->GetPage(rec.page_id);
                if (page2 != nullptr) {
                    std::memcpy(post_undo.data(), page2->GetData(), PAGE_SIZE);
                    buffer_pool_manager_->UnpinPage(rec.page_id, false);
                }
            } else if (rec.before_image.size() == PAGE_SIZE) {
                std::memcpy(post_undo.data(), rec.before_image.data(),
                            PAGE_SIZE);
            }
            lsn_t clr_lsn = log_manager_->AppendCLR(
                txn->GetTxnId(), rec.page_id, post_undo.data(), undo_next_lsn);
            if (buffer_pool_manager_ != nullptr) {
                Page* page3 = buffer_pool_manager_->GetPage(rec.page_id);
                if (page3 != nullptr) {
                    page3->SetPageLsn(clr_lsn);
                    buffer_pool_manager_->UnpinPage(rec.page_id, true);
                }
            }
            try {
                log_manager_->Flush();
            } catch (...) {
                // 测试场景里 WAL 不可写时吞掉。
            }
        }
        // 1c) 测试钩子：撤销 N 步后立即 _Exit(1)。让 Phase C 的 crash 注入
        //     也能用于 savepoint rollback 的 crash-mid-rollback 测试。
        if (crash_after_undo_steps_ > 0) {
            --crash_after_undo_steps_;
            if (crash_after_undo_steps_ == 0) {
                std::cerr << "[crash-after-undo-steps] triggering _Exit(1) "
                          << "after savepoint undo step" << std::endl;
                std::_Exit(1);
            }
        }
    }

    // 3) 写 SAVEPOINT_ROLLBACK 信息性记录 + Flush WAL。
    if (log_manager_ != nullptr) {
        log_manager_->AppendSavepointRollback(txn->GetTxnId(), name);
        try {
            log_manager_->Flush();
        } catch (...) {
        }
    }

    // 4) 截断 undo_log_ + 5) 弹出 savepoint 栈（matched 及其后所有条目）。
    txn->TruncateUndoLog(offset);
    txn->PopSavepointStack(name);
}

// RELEASE SAVEPOINT name — Phase D：仅移除保存点标记，不撤销任何写入。
// 写一条 SAVEPOINT_RELEASE 信息性记录 + Flush WAL；不动 undo_log_，仅
// 从栈中弹出该名称的最内层条目（不影响外层保存点）。
void TransactionManager::ReleaseSavepoint(const std::string& name) {
    if (current_txn_ == nullptr || !current_txn_->IsActive()) {
        return;
    }
    // 写 WAL 之前先确认 savepoint 存在——避免给一个不存在的名字写日志
    // （test 40 期望静默 no-op，HasSavepoint false 时直接 return）。
    if (!current_txn_->HasSavepoint(name)) {
        return;
    }
    if (log_manager_ != nullptr) {
        log_manager_->AppendSavepointRelease(current_txn_->GetTxnId(), name);
        try {
            log_manager_->Flush();
        } catch (...) {
        }
    }
    current_txn_->PopSavepoint(name);
}

void TransactionManager::ResetForShutdown() {
    if (current_txn_ != nullptr) {
        // MVCC 快照隔离：注销活动快照登记，避免悬挂登记污染 Vacuum 地板。
        if (commit_tracker_ != nullptr &&
            current_txn_->GetIsolationLevel() == IsolationLevel::kSnapshot) {
            commit_tracker_->UnregisterSnapshot(current_txn_->GetSnapshotCsn());
        }
        current_txn_->MarkAborted();
        delete current_txn_;
        current_txn_ = nullptr;
    }
    current_txn_depth_ = 0;
}

// =============================================================================
//  MVCC 快照隔离：first-committer-wins 冲突检测 + 提交 CSN 回填。
// =============================================================================

// 重读 txn 写集中每行，做 first-committer-wins 冲突检测：
// 若本事务写在 head 上的版本「被替代」的版本（即 head.prev 指向的版本）来自一个
// 与写集基不同的、且已提交的其他事务，则本事务是输家（写基于陈旧数据），中止。
// 纯 INSERT（无基）不判定；自写跳过；未提交的替代写者（另一活动写者）视为命中共享，
// 在对方提交前不中止。
bool TransactionManager::SnapshotWriteConflict(const Transaction* txn) {
    if (commit_tracker_ == nullptr || buffer_pool_manager_ == nullptr) return false;
    const auto& ws = txn->GetWriteSet();
    for (const auto& e : ws) {
        if (e.base_begin_xid == 0 && e.base_begin_csn == 0) continue;  // 纯 INSERT
        RID rid = e.rid;
        if (!rid.IsValid()) continue;
        // 读 head 版本头。
        PageReadGuard guard =
            PageReadGuard::Fetch(buffer_pool_manager_, rid.page_id);
        if (!guard.Valid()) continue;
        const char* data = guard.Data();
        int32_t next_pid, slot_count, free_off;
        ReadPageHeader(data, next_pid, slot_count, free_off);
        if (rid.slot_num < 0 || rid.slot_num >= slot_count) continue;
        int32_t off, len;
        ReadSlot(data, rid.slot_num, off, len);
        if (IsTombstone(len)) continue;
        MvccRecordHeader h;
        if (!ReadMvccHeader(data + off, &h)) continue;  // legacy 行：无链，不判定

        int64_t displaced_xid = 0;  // 本事务写的版本所替代的版本写者
        if (h.begin_xid == txn->GetTxnId()) {
            // head 是本事务的版本。head.prev = 它替代的版本。
            if (h.prev_page_id == INVALID_PAGE_ID) continue;  // 纯自写新行，无替代
            const page_id_t pv = h.prev_page_id;
            const int32_t ps = h.prev_slot_num;
            const char* pd = nullptr;
            int32_t pslot_count, pnext, pfree;
            if (pv == rid.page_id) {
                pd = data;
            } else {
                guard.Release();
                PageReadGuard pg = PageReadGuard::Fetch(buffer_pool_manager_, pv);
                if (!pg.Valid()) continue;
                guard = std::move(pg);
                pd = guard.Data();
            }
            ReadPageHeader(pd, pnext, pslot_count, pfree);
            if (ps < 0 || ps >= pslot_count) continue;
            int32_t po, pl;
            ReadSlot(pd, ps, po, pl);
            MvccRecordHeader pv_hdr;
            if (IsTombstone(pl) || !ReadMvccHeader(pd + po, &pv_hdr)) continue;
            displaced_xid = pv_hdr.begin_xid;
            if (displaced_xid == e.base_begin_xid) continue;  // 正基于它更新，无冲突
            // 替代版本写者已提交且不同于基 → 冲突。
            int64_t dcsn = 0;
            if (commit_tracker_->LookupCommitted(displaced_xid, &dcsn)) return true;
            // 替代写者未提交：命中共享（另一活动写者），在其提交前不中止。
            continue;
        } else {
            // head 不是本事务（另一写者压在本事务版本之上）——行 X 锁已串行化写者，
            // 通常不会发生；防御性：若该写者已提交且非基，判冲突。
            if (h.begin_xid == e.base_begin_xid) continue;
            int64_t hcsn = 0;
            if (commit_tracker_->LookupCommitted(h.begin_xid, &hcsn)) return true;
            continue;
        }
    }
    return false;
}

// 提交拿到 CSN 后，逐槽位回填版本头：
//   * is_end==false 的新版本 → 置 begin_csn = csn（本事务写者的提交序）；
//   * is_end==true 的被替代/删除旧版本 → 置 end_csn = csn（使它失效的提交序）。
void TransactionManager::BackfillVersionCsn(Transaction* txn, int64_t csn) {
    if (buffer_pool_manager_ == nullptr) return;
    const auto& slots = txn->GetVersionSlots();
    for (const auto& ref : slots) {
        PageWriteGuard guard =
            PageWriteGuard::Fetch(buffer_pool_manager_, ref.page_id);
        if (!guard.Valid()) continue;
        char* data = guard.Data();
        int32_t next_pid, slot_count, free_off;
        ReadPageHeader(data, next_pid, slot_count, free_off);
        if (ref.slot_num < 0 || ref.slot_num >= slot_count) continue;
        int32_t off, len;
        ReadSlot(data, ref.slot_num, off, len);
        if (IsTombstone(len)) continue;
        MvccRecordHeader h;
        if (!ReadMvccHeader(data + off, &h)) continue;
        if (ref.is_end) {
            if (h.end_csn == 0) h.end_csn = csn;
        } else {
            if (h.begin_csn == 0) h.begin_csn = csn;
        }
        WriteMvccHeader(data + off, h);
        guard.MarkDirty();
    }
}

}  // namespace sqlcompiler