#include "storage/BufferPoolManager.h"

#include "storage/FIFOReplacer.h"
#include "storage/LRUReplacer.h"
#include "storage/ClockReplacer.h"
#include "storage/LRUKReplacer.h"
#include "txn/LogManager.h"

#include <cstring>
#include <shared_mutex>

namespace sqlcompiler {

// 全局锁序（防止死锁）：
//   本类引入 latch_ 串行化所有帧表操作。关键约束是锁序恒为
//     BPM::latch_  ->  LogManager::mutex_
//     BPM::latch_  ->  DiskManager::db_io_latch_
//   即：BPM 拿锁后可以调用 log->Flush() / disk->WritePage 等（获取子系统的锁），
//   但绝不允许反向嵌套。经核查：
//     * LogManager::mutex_ 是私有成员，LogManager 的所有公开方法自锁自放，不会在持有
//       自己的锁期间回调 BufferPoolManager；
//     * 外层（RecoveryManager / TransactionManager / Database）都是先调用
//       log->Flush()（锁已释放）再调用 BPM 的刷新方法，是两个不嵌套的作用域。
//   因此不存在「持日志/DiskManager 锁再进入 BPM」的路径，加入 BPM 全局锁不会形成环。
//   单线程场景下锁无争用，行为与原先完全一致；多线程时按此约定扩展即可。

double BufferPoolStats::HitRate() const {
    long total = hit_count + miss_count;
    if (total == 0) return 0.0;
    return static_cast<double>(hit_count) / static_cast<double>(total);
}

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager* disk_manager,
                                      ReplacementPolicy policy, size_t lru_k)
    : pool_size_(pool_size), disk_manager_(disk_manager), pages_(pool_size) {
    if (policy == ReplacementPolicy::LRU) {
        replacer_ = std::make_unique<LRUReplacer>(pool_size);
    } else if (policy == ReplacementPolicy::LRUK) {
        replacer_ = std::make_unique<LRUKReplacer>(pool_size, lru_k);
    } else if (policy == ReplacementPolicy::CLOCK) {
        replacer_ = std::make_unique<ClockReplacer>(pool_size);
    } else {
        replacer_ = std::make_unique<FIFOReplacer>(pool_size);
    }
    free_list_.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i) {
        free_list_.push_back(static_cast<int>(pool_size - 1 - i));
    }
}

BufferPoolManager::~BufferPoolManager() {
    // 先把后台刷脏线程干净收尾（join），避免其与析构末尾的 FlushAllPages 竞态。
    StopBackgroundFlush();
    // 析构函数默认 noexcept，写盘若抛异常会直接 terminate。异常通常来自真实磁盘故障
    // （如磁盘满/句柄失效）；此时进程本就在退出，吞掉是比崩溃更稳定的行为，真正的
    // 运行期 I/O 错误已由 ExecuteSQL 的错误处理通道上报。
    try {
        FlushAllPages();
    } catch (...) {
    }
}

Page* BufferPoolManager::GetPage(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(latch_);
    if (page_id < 0) return nullptr;
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        int frame_id = it->second;
        pages_[frame_id].IncPinCount();
        replacer_->Pin(frame_id);
        ++stats_.hit_count;
        return &pages_[frame_id];
    }
    int frame_id = -1;
    try {
        if (!FindFreeFrame(page_id, &frame_id)) {
            return nullptr;
        }
        disk_manager_->ReadPage(page_id, pages_[frame_id].GetData());
    } catch (...) {
        // 读盘/找帧异常：把已取出的帧归还为空闲帧（若 FindFreeFrame 内部失败，其
        // catch 已归还，此处 frame_id 仍为 -1 不会重复归还）。避免缓冲池容量因
        // CRC 校验失败、块设备故障等异常永久收缩。
        if (frame_id >= 0) {
            pages_[frame_id].ResetMemory();
            pages_[frame_id].SetPageId(INVALID_PAGE_ID);
            pages_[frame_id].SetDirty(false);
            free_list_.push_back(frame_id);
        }
        throw;
    }
    pages_[frame_id].SetPageId(page_id);
    pages_[frame_id].SetDirty(false);
    pages_[frame_id].IncPinCount();  // now pin = 1
    // 重新加载磁盘页后该页的 page_lsn 不可知（磁盘格式不带 LSN），归零。
    // 后续 redo 会用「page.page_lsn < record.lsn」判定是否重放，安全。
    pages_[frame_id].SetPageLsn(0);
    page_table_[page_id] = frame_id;
    replacer_->Pin(frame_id);
    ++stats_.miss_count;
    return &pages_[frame_id];
}

Page* BufferPoolManager::NewPage(page_id_t* page_id) {
    std::lock_guard<std::mutex> lock(latch_);
    // 先分配页号再找帧，确保 FindFreeFrame 记录的替换日志的 loaded 字段为真实新页号。
    page_id_t new_pid = disk_manager_->AllocatePage();
    int frame_id = -1;
    try {
        if (!FindFreeFrame(new_pid, &frame_id)) {
            // 无空闲帧可用：回滚已分配的页号，避免该页号永久丢失（否则从 free_pages_
            // 复用的页位已置 0、从 next_page_id_ 增长的计数已递增，都回不来）。
            disk_manager_->DeallocatePage(new_pid);
            return nullptr;
        }
    } catch (...) {
        // 找帧过程异常（如淘汰写回失败）：同样回滚页号后重抛。帧由 FindFreeFrame
        // 的 catch 负责归还。
        disk_manager_->DeallocatePage(new_pid);
        throw;
    }
    pages_[frame_id].ResetMemory();
    pages_[frame_id].SetPageId(new_pid);
    pages_[frame_id].IncPinCount();  // pin = 1
    pages_[frame_id].SetDirty(false);
    // ResetMemory 已把 page_lsn_ 置 0；显式再次提醒意图。
    pages_[frame_id].SetPageLsn(0);
    page_table_[new_pid] = frame_id;
    replacer_->Pin(frame_id);
    if (page_id) *page_id = new_pid;
    ++stats_.miss_count;
    return &pages_[frame_id];
}

bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
    std::lock_guard<std::mutex> lock(latch_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return false;
    int frame_id = it->second;
    if (is_dirty) {
        pages_[frame_id].SetDirty(true);
    }
    pages_[frame_id].DecPinCount();
    if (pages_[frame_id].GetPinCount() == 0) {
        replacer_->Unpin(frame_id);
    }
    return true;
}

bool BufferPoolManager::FlushPage(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(latch_);
    FlushPageUnlocked(page_id);
    return true;
}

// FlushPage 的免锁主体。调用前提：调用方已持有 latch_。
// 仅内部（FlushPage / FlushAllPages）复用，避免非递归锁重入死锁。
void BufferPoolManager::FlushPageUnlocked(page_id_t page_id) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return;
    int frame_id = it->second;
    // Phase B：WAL-before-data 规则。
    // 若该页的 page_lsn 尚未被日志持久化，必须先 LogManager::Flush，
    // 否则磁盘上的 page 会"领先"日志，导致崩溃后 redo 看不到原始写入。
    // 锁序：latch_ -> LogManager::mutex_（见文件头注释）。
    if (log_manager_ != nullptr) {
        uint64_t page_lsn = pages_[frame_id].GetPageLsn();
        uint64_t durable = log_manager_->durable_lsn();
        if (page_lsn > durable) {
            log_manager_->Flush();
        }
    }
    // 统计脏页写回：仅当该页写回前确为脏页才累加。
    if (pages_[frame_id].IsDirty()) ++stats_.writeback_count;
    // E4：写盘前取该帧页级**读锁**，防止并发持写锁修改页数据的线程把正在落盘的内容
    // 改掉（写盘撕裂）。读锁允许多个写回线程共享并发，但会与独占写者互斥。
    std::shared_lock<std::shared_mutex> data_lock(pages_[frame_id].GetLatch());
    disk_manager_->WritePage(page_id, pages_[frame_id].GetData());
    pages_[frame_id].SetDirty(false);
}

void BufferPoolManager::FlushAllDirtyPages() {
    std::lock_guard<std::mutex> lock(latch_);
    FlushAllDirtyUnlocked();
}

// FlushAllDirtyPages 的免锁主体。
void BufferPoolManager::FlushAllDirtyUnlocked() {
    // Phase B：组提交优化。原先逐脏页调 FlushPage 会对每个 page_lsn > durable 的
    // 页各触发一次日志 Flush；这里改为一次性把日志刷到所有脏页中最大的 page_lsn，
    // 只需一遍 LogManager::Flush。数据页仍逐页写回，但 WAL-before-data 保证不变。
    // 注意：本方法不调用 DiskManager::Sync——调用方（TransactionManager/RecoveryManager）
    // 会在刷脏页后统一 Sync，避免重复 fsync。
    if (log_manager_ != nullptr) {
        bool need_flush = false;
        for (const auto& kv : page_table_) {
            int frame_id = kv.second;
            if (!pages_[frame_id].IsDirty()) continue;
            if (pages_[frame_id].GetPageLsn() > log_manager_->durable_lsn()) {
                need_flush = true;
                break;
            }
        }
        if (need_flush) {
            log_manager_->Flush();
        }
    }
    for (const auto& kv : page_table_) {
        int frame_id = kv.second;
        if (!pages_[frame_id].IsDirty()) continue;
        ++stats_.writeback_count;  // 全量刷脏：每个真脏页记一次写回
        // E4：写盘加帧读锁防撕裂（见 FlushPageUnlocked 说明）。
        std::shared_lock<std::shared_mutex> data_lock(pages_[frame_id].GetLatch());
        disk_manager_->WritePage(kv.first, pages_[frame_id].GetData());
        pages_[frame_id].SetDirty(false);
    }
}

void BufferPoolManager::FlushAllPages() {
    std::lock_guard<std::mutex> lock(latch_);
    for (const auto& kv : page_table_) {
        FlushPageUnlocked(kv.first);
    }
}

bool BufferPoolManager::DeletePage(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(latch_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        disk_manager_->DeallocatePage(page_id);
        return true;
    }
    int frame_id = it->second;
    if (pages_[frame_id].GetPinCount() > 0) return false;
    if (pages_[frame_id].IsDirty()) {
        // 同样走 WAL-before-data 规则。
        if (log_manager_ != nullptr) {
            uint64_t page_lsn = pages_[frame_id].GetPageLsn();
            uint64_t durable = log_manager_->durable_lsn();
            if (page_lsn > durable) {
                log_manager_->Flush();
            }
        }
        ++stats_.writeback_count;  // 删除退页：脏页先写回再回收
        // E4：删除退页写回加帧读锁防撕裂（见 FlushPageUnlocked 说明）。
        std::shared_lock<std::shared_mutex> data_lock(pages_[frame_id].GetLatch());
        disk_manager_->WritePage(page_id, pages_[frame_id].GetData());
    }
    page_table_.erase(it);
    pages_[frame_id].ResetMemory();
    free_list_.push_back(frame_id);
    disk_manager_->DeallocatePage(page_id);
    return true;
}

std::vector<std::pair<page_id_t, uint64_t>> BufferPoolManager::CollectDirtyPages() {
    std::lock_guard<std::mutex> lock(latch_);
    std::vector<std::pair<page_id_t, uint64_t>> out;
    for (const auto& kv : page_table_) {
        int frame_id = kv.second;
        if (pages_[frame_id].IsDirty()) {
            out.emplace_back(kv.first, pages_[frame_id].GetPageLsn());
        }
    }
    return out;
}

BufferPoolStats BufferPoolManager::GetStats() const {
    std::lock_guard<std::mutex> lock(latch_);
    return stats_;  // 返回副本：调用方读的是快照，无需再持锁
}

std::vector<ReplacementLogEntry> BufferPoolManager::GetReplacementLog() const {
    std::lock_guard<std::mutex> lock(latch_);
    return replacement_log_;  // 返回副本
}

size_t BufferPoolManager::GetMemoryUsageFrames() const {
    // 已映射逻辑页的帧数 = 总帧数 - 空闲帧数。free_list_ 由全局 latch_ 保护。
    std::lock_guard<std::mutex> lock(latch_);
    return pool_size_ >= free_list_.size() ? (pool_size_ - free_list_.size()) : 0;
}

// ---- E5 后台异步刷脏页线程 ----

void BufferPoolManager::StartBackgroundFlush(std::chrono::milliseconds interval) {
    if (interval.count() <= 0) return;  // 禁用：保持与旧版逐字节一致
    std::lock_guard<std::mutex> lock(bg_mutex_);
    if (bg_running_) return;  // 已在运行：幂等
    bg_interval_ = interval;
    bg_stop_ = false;
    bg_running_ = true;
    background_flusher_ =
        std::thread(&BufferPoolManager::BackgroundFlushLoop, this);
}

void BufferPoolManager::StopBackgroundFlush() {
    std::thread to_join;
    {
        std::lock_guard<std::mutex> lock(bg_mutex_);
        if (!bg_running_) return;  // 幂等
        bg_stop_ = true;
        bg_running_ = false;
        to_join = std::move(background_flusher_);  // 移出后另行 join，避免持锁 join 死锁
    }
    bg_cv_.notify_all();
    if (to_join.joinable()) to_join.join();
}

// 后台循环：每隔 interval 用免锁 FlushAllDirtyUnlocked 刷脏。
// 关键（保持 D7 锁序与组提交不变式）：
//   * 仅调 WritePage（写到 OS 缓存），不调 DiskManager::Sync() —— 真正落盘由
//     COMMIT 路径的 Sync() 统一负责，故每事务 fsync 次数不变；
//   * FlushAllDirtyUnlocked 内部在 page_lsn > durable 时先 log->Flush()，
//     满足 WAL-before-data，脏页不可能"领先"日志；
//   * 实际刷脏在 bg_cv_ 超时唤醒后、释放 bg_mutex_ 期间执行，只持有 latch_
//     （与 D7 锁序 BPM -> LogManager/DiskManager 一致，无死锁环）。
// I/O 错误吞掉：与 ~BufferPoolManager 一致，退路交给 ExecuteSQL 错误通道。
void BufferPoolManager::BackgroundFlushLoop() {
    while (true) {
        std::unique_lock<std::mutex> lock(bg_mutex_);
        bg_cv_.wait_for(lock, bg_interval_, [this] { return bg_stop_; });
        if (bg_stop_) break;
        ++bg_flush_ticks_;
        // 释放 bg_mutex_ 后再刷，让 StopBackgroundFlush 能在刷盘期间置位 stop。
        lock.unlock();
        try {
            FlushAllDirtyPages();
        } catch (...) {
            // 吸入磁盘错误，后台线程不得因单次失败而退出或 terminate。
        }
    }
}

bool BufferPoolManager::IsBackgroundFlushEnabled() const {
    std::lock_guard<std::mutex> lock(bg_mutex_);
    return bg_running_;
}

long BufferPoolManager::GetBackgroundFlushTicks() const {
    return bg_flush_ticks_.load();
}

std::chrono::milliseconds BufferPoolManager::GetBackgroundFlushInterval() const {
    std::lock_guard<std::mutex> lock(bg_mutex_);
    return bg_interval_;
}

bool BufferPoolManager::FindFreeFrame(page_id_t loaded_page_id, int* frame_id) {
    if (!free_list_.empty()) {
        int fid = free_list_.back();
        free_list_.pop_back();
        if (frame_id) *frame_id = fid;
        return true;
    }
    int victim = -1;
    if (!replacer_->Victim(&victim)) {
        return false;
    }
    page_id_t evicted_pid = pages_[victim].GetPageId();
    bool evicted_dirty = pages_[victim].IsDirty();
    if (evicted_dirty) {
        try {
            // Phase B：victim 写出也需尊重 WAL 顺序。
            if (log_manager_ != nullptr) {
                uint64_t page_lsn = pages_[victim].GetPageLsn();
                uint64_t durable = log_manager_->durable_lsn();
                if (page_lsn > durable) {
                    log_manager_->Flush();
                }
            }
            ++stats_.writeback_count;  // 淘汰换出脏页
            // E4：淘汰换出写回加帧读锁防撕裂（见 FlushPageUnlocked 说明）。
            std::shared_lock<std::shared_mutex> data_lock(pages_[victim].GetLatch());
            disk_manager_->WritePage(evicted_pid, pages_[victim].GetData());
        } catch (...) {
            // 淘汰写回失败：该帧无法淘汰。把它恢复为空闲帧（清 page_table_ 映射、
            // 复位内容、压回 free_list_），避免缓冲池容量因异常永久收缩——否则反复
            // 触发会让池逐渐瘦身，最终 NewPage 无帧可用。上层 GetPage/NewPage 的
            // catch 会据此归还从 free_list_ 取出的帧，二者不会重复归还。
            page_table_.erase(evicted_pid);
            pages_[victim].ResetMemory();
            pages_[victim].SetPageId(INVALID_PAGE_ID);
            pages_[victim].SetDirty(false);
            free_list_.push_back(victim);
            throw;
        }
    }
    page_table_.erase(evicted_pid);
    ReplacementLogEntry entry;
    entry.evicted_page_id = evicted_pid;
    entry.evicted_was_dirty = evicted_dirty;
    entry.loaded_page_id = loaded_page_id;  // 记录本次换入的页号
    replacement_log_.push_back(entry);
    // 环形上限：超过阈值时丢弃最旧的日志，保证长会话内存受控。
    if (replacement_log_.size() > kMaxReplacementLog) {
        replacement_log_.erase(replacement_log_.begin());
    }
    ++stats_.replacement_count;
    if (frame_id) *frame_id = victim;
    return true;
}

}  // namespace sqlcompiler