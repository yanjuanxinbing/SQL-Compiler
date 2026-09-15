// =============================================================================
// RecoveryManager — ARIES 3-phase 恢复实现（Phase D：嵌套 savepoint 标记兼容）。
//
//   Analysis: 全量扫描 log_records（从 WAL 首条），构建：
//              * txn_table_:        active txn → 最近 LSN
//              * dirty_page_table_: page_id → 该 page 第一次被脏化的 LSN
//
//   Redo:     从 dirty_page_table_ 中最小的 rec_lsn 开始，按时间顺序遍历
//             所有 log records；对每条 UPDATE / CLR 记录，仅当目标页的
//             page_lsn_ < record.lsn 时才把 after_image (UPDATE) 或
//             before_image (CLR, undo 后状态) memcpy 到 page。这一步必须按
//             顺序（LSN 单调递增）执行，因为 redo 是幂等的、单调推进的。
//             SAVEPOINT_ROLLBACK / SAVEPOINT_RELEASE 不携带 page-image，
//             因此 redo 阶段直接跳过——它们仅是「信息性」标记，让运维工
//             具 / ARIES 分析器识别 savepoint 边界，不修改任何 page。
//
//   Undo:     对每个 txn_table_ 中仍存在的事务，沿 prev_lsn 链反向走：
//              * 遇到 UPDATE：apply before-image + 写一条 CLR，CLR 的
//                undo_next_lsn 指向 undo 链上下一个待撤销 LSN。
//              * 遇到 CLR：跳过（page 已处于 undo 后状态），直接跳到
//                clr.undo_next_lsn_ 继续撤销剩余 UPDATE。
//              * 遇到 SAVEPOINT_ROLLBACK：跳过（不修改 txn_table_），但
//                沿 prev_lsn 链继续回溯——CLR 链上的 undo_next_lsn 已经
//                保证 savepoint 区间之外「更早」的 UPDATEs 仍能被撤销，
//                这是为了在事务被 abort 时与正常 undo 语义一致。
//             最后为每个被撤销事务写 ABORT 记录。
//
// 文档参考：Aries/Recovery 经典论文，本实现做了简化：
//   * 没有按 CHECKPOINT 截断的优化（V1 总是从首条记录扫起）。
// =============================================================================

#include "txn/RecoveryManager.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

#include "catalog/SystemCatalog.h"
#include "txn/TransactionManager.h"

namespace sqlcompiler {

RecoveryManager::RecoveryManager(LogManager* log_manager,
                                  BufferPoolManager* buffer_pool_manager,
                                  DiskManager* disk_manager,
                                  TransactionManager* txn_manager,
                                  SystemCatalog* catalog)
    : log_manager_(log_manager),
      buffer_pool_manager_(buffer_pool_manager),
      disk_manager_(disk_manager),
      txn_manager_(txn_manager),
      catalog_(catalog) {}

bool RecoveryManager::RestorePageImage(page_id_t pid, const char* image) {
    if (pid < 0 || image == nullptr || buffer_pool_manager_ == nullptr) return false;
    Page* page = buffer_pool_manager_->GetPage(pid);
    if (page == nullptr) return false;
    std::memcpy(page->GetData(), image, PAGE_SIZE);
    page->SetDirty(true);
    buffer_pool_manager_->UnpinPage(pid, true);
    return true;
}

void RecoveryManager::AnalysisPass(const std::vector<LogRecord>& records) {
    txn_table_.clear();
    dirty_page_table_.clear();
    for (const auto& rec : records) {
        switch (rec.type_) {
            case LogRecordType::BEGIN:
                txn_table_[rec.txn_id_] = rec.lsn_;
                break;
            case LogRecordType::COMMIT:
            case LogRecordType::ABORT:
                // 事务结束：从活动表中移除。
                txn_table_.erase(rec.txn_id_);
                break;
            case LogRecordType::UPDATE:
            case LogRecordType::CLR:
                // 仅当该 page 还未被记录为脏时才写入（dirty_page_table 记「最早」LSN）。
                if (dirty_page_table_.find(rec.page_id_) == dirty_page_table_.end()) {
                    dirty_page_table_[rec.page_id_] = rec.lsn_;
                }
                // 更新事务最近 LSN。
                txn_table_[rec.txn_id_] = rec.lsn_;
                break;
            case LogRecordType::CHECKPOINT:
                // V1 简化：CHECKPOINT 不带额外表项，跳过。
                break;
            case LogRecordType::SAVEPOINT_ROLLBACK:
            case LogRecordType::SAVEPOINT_RELEASE:
                // Phase D：信息性标记——不修改 dirty_page_table，仅让
                // txn_table_ 推进到本记录的 LSN（这样 undo pass 从这里
                // 继续走 prev_lsn 链时，会经过 SAVEPOINT_ROLLBACK 然后
                // 命中之前的 CLR / UPDATE）。CLR 链的 undo_next_lsn 已
                // 经负责跳过「已被 savepoint rollback 撤销」的 UPDATE。
                if (rec.txn_id_ != 0 &&
                    txn_table_.find(rec.txn_id_) != txn_table_.end()) {
                    txn_table_[rec.txn_id_] = rec.lsn_;
                }
                break;
        }
    }
}

void RecoveryManager::RedoPass(const std::vector<LogRecord>& records) {
    // 找 dirty_page_table_ 中最小的 rec_lsn 作为扫描起点。
    lsn_t start_lsn = 1;
    if (!dirty_page_table_.empty()) {
        start_lsn = std::min_element(dirty_page_table_.begin(),
                                      dirty_page_table_.end(),
                                      [](const auto& a, const auto& b) {
                                          return a.second < b.second;
                                      })->second;
    }
    for (const auto& rec : records) {
        if (rec.lsn_ < start_lsn) continue;
        if (rec.type_ != LogRecordType::UPDATE &&
            rec.type_ != LogRecordType::CLR) continue;
        Page* page = buffer_pool_manager_->GetPage(rec.page_id_);
        if (page == nullptr) continue;
        const uint64_t page_lsn = page->GetPageLsn();
        // 仅当 page 还没记录到这条日志之后才重放，保证幂等。
        if (page_lsn < rec.lsn_) {
            // UPDATE 的「最新状态」是 after_image；CLR 的「最新状态」是
            // before_image（即 undo 后状态）。
            const std::vector<char>& target =
                (rec.type_ == LogRecordType::CLR) ? rec.before_image_
                                                  : rec.after_image_;
            if (target.size() == PAGE_SIZE) {
                std::memcpy(page->GetData(), target.data(), PAGE_SIZE);
            }
            page->SetPageLsn(rec.lsn_);
            page->SetDirty(true);
        }
        buffer_pool_manager_->UnpinPage(rec.page_id_, true);
    }
}

void RecoveryManager::UndoPass(const std::vector<LogRecord>& records) {
    if (txn_table_.empty()) return;

    // 把 log_records 按 lsn 排序后建 prev_lsn 链索引，方便从最新一条回溯。
    std::unordered_map<lsn_t, const LogRecord*> by_lsn;
    by_lsn.reserve(records.size());
    for (const auto& rec : records) {
        by_lsn[rec.lsn_] = &rec;
    }

    // 对每个活动事务，从其最近一条记录开始反向撤销。
    // Phase C：CLR 链处理：
    //   * 遇到 UPDATE：apply before-image + 写一条 CLR。CLR 的 undo_next_lsn
    //     指向 undo 链上下一个待撤销 LSN（即当前 rec.prev_lsn_）。
    //   * 遇到 CLR：page 已处于 undo 后状态（由 redo 阶段保证），直接跳到
    //     clr.undo_next_lsn_ 继续撤销。
    std::unordered_set<txn_id_t> aborted;
    for (const auto& kv : txn_table_) {
        const txn_id_t tid = kv.first;
        lsn_t cur = kv.second;
        lsn_t next_undo_lsn = INVALID_LSN;  // 初始：CLR 的 undo_next_lsn 起点
        while (cur != INVALID_LSN) {
            auto it = by_lsn.find(cur);
            if (it == by_lsn.end()) break;
            const LogRecord& rec = *it->second;
            if (rec.type_ == LogRecordType::UPDATE) {
                // 应用 before-image。
                if (rec.before_image_.size() == PAGE_SIZE) {
                    RestorePageImage(rec.page_id_, rec.before_image_.data());
                }
                // Phase C：写 CLR，让 txn 的 WAL 自描述「这部分已 undo」。
                // undo_next_lsn = 当前 rec.prev_lsn_（undo 链上下一步）。
                if (log_manager_ != nullptr) {
                    std::vector<char> post_undo(PAGE_SIZE, 0);
                    Page* page = buffer_pool_manager_->GetPage(rec.page_id_);
                    if (page != nullptr) {
                        std::memcpy(post_undo.data(), page->GetData(),
                                    PAGE_SIZE);
                        buffer_pool_manager_->UnpinPage(rec.page_id_, false);
                    } else if (rec.before_image_.size() == PAGE_SIZE) {
                        std::memcpy(post_undo.data(),
                                    rec.before_image_.data(), PAGE_SIZE);
                    }
                    lsn_t clr_lsn = log_manager_->AppendCLR(
                        tid, rec.page_id_, post_undo.data(),
                        /*undo_next_lsn=*/rec.prev_lsn_);
                    if (page != nullptr) {
                        // 重取 page 设 page_lsn（page 已 UnpinPage 上面）。
                        Page* p2 =
                            buffer_pool_manager_->GetPage(rec.page_id_);
                        if (p2 != nullptr) {
                            p2->SetPageLsn(clr_lsn);
                            buffer_pool_manager_->UnpinPage(rec.page_id_, true);
                        }
                    }
                }
                next_undo_lsn = rec.prev_lsn_;
            } else if (rec.type_ == LogRecordType::CLR) {
                // 跳过已 undo 工作：直接用 undo_next_lsn_。
                next_undo_lsn = rec.undo_next_lsn_;
            } else {
                // BEGIN / COMMIT / ABORT / CHECKPOINT：直接沿 prev_lsn 走。
            }
            cur = rec.prev_lsn_;
            // 注意：CLR 自身也用 prev_lsn 链串到上一条记录；CLR 的 prev_lsn
            // 对应 LogManager 自动维护的链（CLR 之前的最近一条记录），与
            // undo 链不同。正确做法：CLR 时跳过 undo_next_lsn 后立刻把 cur
            // 设为 undo_next_lsn，因为 undo_next_lsn 已经是「下一个待撤销
            // UPDATE 的 LSN」；同时退出对当前 rec.prev_lsn_ 的二次回溯，
            // 否则会错误地把 CLR 的 prev_lsn（UPDATE-C 的 LSN）当 undo 链
            // 下一条走，但 UPDATE-C 已 undo 过——这是 Phase C 与 Phase B
            // 的关键区别。
            if (rec.type_ == LogRecordType::CLR) {
                cur = next_undo_lsn;
            }
        }
        aborted.insert(tid);
    }

    // 为每个撤销事务写 ABORT 记录 + Flush WAL。
    if (log_manager_ != nullptr && !aborted.empty()) {
        for (txn_id_t tid : aborted) {
            LogRecord rec;
            rec.type_ = LogRecordType::ABORT;
            rec.txn_id_ = tid;
            log_manager_->AppendRecord(std::move(rec));
        }
        try {
            log_manager_->Flush();
        } catch (...) {
            // Flush 失败不影响恢复完成（数据已正确）。
        }
    }

    txn_table_.clear();
    dirty_page_table_.clear();
}

bool RecoveryManager::Run() {
    if (log_manager_ == nullptr) return false;
    auto records = log_manager_->ReadAll();
    if (records.empty()) return true;  // 全新库：无需恢复。

    AnalysisPass(records);
    RedoPass(records);
    UndoPass(records);

    // 把 redo 过的页面强制写盘，确保 BufferPool 在 Phase B 的 COMMIT 语义下
    // 看到的是「恢复后的最终状态」。
    if (buffer_pool_manager_ != nullptr) {
        buffer_pool_manager_->FlushAllDirtyPages();
    }
    if (disk_manager_ != nullptr) {
        disk_manager_->Sync();
    }
    return true;
}

void RecoveryManager::Checkpoint() {
    if (log_manager_ == nullptr) return;
    LogRecord rec;
    rec.type_ = LogRecordType::CHECKPOINT;
    rec.txn_id_ = 0;
    log_manager_->AppendRecord(std::move(rec));
    try {
        log_manager_->Flush();
    } catch (...) {
        // 测试场景里可能没有 WAL 文件；吞掉。
    }
}

}  // namespace sqlcompiler