#include "storage/BufferPoolManager.h"

#include "storage/FIFOReplacer.h"
#include "storage/LRUReplacer.h"
#include "txn/LogManager.h"

#include <cstring>

namespace sqlcompiler {

double BufferPoolStats::HitRate() const {
    long total = hit_count + miss_count;
    if (total == 0) return 0.0;
    return static_cast<double>(hit_count) / static_cast<double>(total);
}

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager* disk_manager,
                                      ReplacementPolicy policy)
    : pool_size_(pool_size), disk_manager_(disk_manager), pages_(pool_size) {
    if (policy == ReplacementPolicy::LRU) {
        replacer_ = std::make_unique<LRUReplacer>(pool_size);
    } else {
        replacer_ = std::make_unique<FIFOReplacer>(pool_size);
    }
    free_list_.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i) {
        free_list_.push_back(static_cast<int>(pool_size - 1 - i));
    }
}

BufferPoolManager::~BufferPoolManager() {
    FlushAllPages();
}

Page* BufferPoolManager::GetPage(page_id_t page_id) {
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
    if (!FindFreeFrame(&frame_id, page_id)) {
        return nullptr;
    }
    disk_manager_->ReadPage(page_id, pages_[frame_id].GetData());
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
    int frame_id = -1;
    // NewPage 调用时 page_id 尚未分配，因此先取一个 frame 占位并把替换日志
    // 的 loaded_page_id 暂时记为 INVALID_PAGE_ID；分配完新 pid 后再
    // PatchLastReplacementLog 补填，保持替换日志语义自洽。
    if (!FindFreeFrame(&frame_id, INVALID_PAGE_ID)) {
        return nullptr;
    }
    page_id_t new_pid = disk_manager_->AllocatePage();
    if (new_pid == INVALID_PAGE_ID) {
        // 分配失败：把 frame 退回 free_list_，让外部看起来像"什么都没发生"。
        free_list_.push_back(frame_id);
        return nullptr;
    }
    PatchLastReplacementLog(new_pid);
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
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return false;
    int frame_id = it->second;
    // Phase B：WAL-before-data 规则。
    // 若该页的 page_lsn 尚未被日志持久化，必须先 LogManager::Flush，
    // 否则磁盘上的 page 会"领先"日志，导致崩溃后 redo 看不到原始写入。
    if (log_manager_ != nullptr) {
        uint64_t page_lsn = pages_[frame_id].GetPageLsn();
        uint64_t durable = log_manager_->durable_lsn();
        if (page_lsn > durable) {
            log_manager_->Flush();
        }
    }
    disk_manager_->WritePage(page_id, pages_[frame_id].GetData());
    pages_[frame_id].SetDirty(false);
    return true;
}

void BufferPoolManager::FlushAllDirtyPages() {
    // Phase B：仅刷脏页；Lsn-aware 的 FlushPage 保证 WAL 顺序。
    for (const auto& kv : page_table_) {
        int frame_id = kv.second;
        if (pages_[frame_id].IsDirty()) {
            FlushPage(kv.first);
        }
    }
}

void BufferPoolManager::FlushAllPages() {
    for (const auto& kv : page_table_) {
        FlushPage(kv.first);
    }
}

bool BufferPoolManager::DeletePage(page_id_t page_id) {
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
        disk_manager_->WritePage(page_id, pages_[frame_id].GetData());
    }
    page_table_.erase(it);
    pages_[frame_id].ResetMemory();
    free_list_.push_back(frame_id);
    disk_manager_->DeallocatePage(page_id);
    return true;
}

std::vector<std::pair<page_id_t, uint64_t>> BufferPoolManager::CollectDirtyPages() {
    std::vector<std::pair<page_id_t, uint64_t>> out;
    for (const auto& kv : page_table_) {
        int frame_id = kv.second;
        if (pages_[frame_id].IsDirty()) {
            out.emplace_back(kv.first, pages_[frame_id].GetPageLsn());
        }
    }
    return out;
}

const BufferPoolStats& BufferPoolManager::GetStats() const {
    return stats_;
}

const std::vector<ReplacementLogEntry>& BufferPoolManager::GetReplacementLog() const {
    return replacement_log_;
}

bool BufferPoolManager::FindFreeFrame(int* frame_id, page_id_t to_load) {
    if (!free_list_.empty()) {
        int fid = free_list_.back();
        free_list_.pop_back();
        // free_list_ 路径不淘汰任何页，不写 replacement_log_，
        // 这样可以避免日志被"无意义"的换入事件污染。
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
        // Phase B：victim 写出也需尊重 WAL 顺序。
        if (log_manager_ != nullptr) {
            uint64_t page_lsn = pages_[victim].GetPageLsn();
            uint64_t durable = log_manager_->durable_lsn();
            if (page_lsn > durable) {
                log_manager_->Flush();
            }
        }
        disk_manager_->WritePage(evicted_pid, pages_[victim].GetData());
    }
    page_table_.erase(evicted_pid);
    ReplacementLogEntry entry;
    entry.evicted_page_id = evicted_pid;
    entry.evicted_was_dirty = evicted_dirty;
    // 直接由调用方提供 to_load，避免过去那种 "filled by caller" 但没人来填
    // 的死代码。NewPage 路径若一时拿不到 pid，会传 INVALID_PAGE_ID 并在
    // AllocatePage 之后调用 PatchLastReplacementLog 补填。
    entry.loaded_page_id = to_load;
    replacement_log_.push_back(entry);
    ++stats_.replacement_count;
    if (frame_id) *frame_id = victim;
    return true;
}

void BufferPoolManager::PatchLastReplacementLog(page_id_t loaded_page_id) {
    // 仅有真正的"换出→换入"事件才记录日志；free_list_ 取出的帧不会留下条目。
    if (replacement_log_.empty()) return;
    replacement_log_.back().loaded_page_id = loaded_page_id;
}

}  // namespace sqlcompiler