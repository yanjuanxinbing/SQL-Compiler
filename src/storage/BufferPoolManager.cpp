#include "storage/BufferPoolManager.h"

#include "storage/FIFOReplacer.h"
#include "storage/LRUReplacer.h"

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
    if (!FindFreeFrame(&frame_id)) {
        return nullptr;
    }
    disk_manager_->ReadPage(page_id, pages_[frame_id].GetData());
    pages_[frame_id].SetPageId(page_id);
    pages_[frame_id].SetDirty(false);
    pages_[frame_id].IncPinCount();  // now pin = 1
    page_table_[page_id] = frame_id;
    replacer_->Pin(frame_id);
    ++stats_.miss_count;
    return &pages_[frame_id];
}

Page* BufferPoolManager::NewPage(page_id_t* page_id) {
    int frame_id = -1;
    if (!FindFreeFrame(&frame_id)) {
        return nullptr;
    }
    page_id_t new_pid = disk_manager_->AllocatePage();
    pages_[frame_id].ResetMemory();
    pages_[frame_id].SetPageId(new_pid);
    pages_[frame_id].IncPinCount();  // pin = 1
    pages_[frame_id].SetDirty(false);
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
    disk_manager_->WritePage(page_id, pages_[frame_id].GetData());
    pages_[frame_id].SetDirty(false);
    return true;
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
        disk_manager_->WritePage(page_id, pages_[frame_id].GetData());
    }
    page_table_.erase(it);
    pages_[frame_id].ResetMemory();
    free_list_.push_back(frame_id);
    disk_manager_->DeallocatePage(page_id);
    return true;
}

const BufferPoolStats& BufferPoolManager::GetStats() const {
    return stats_;
}

const std::vector<ReplacementLogEntry>& BufferPoolManager::GetReplacementLog() const {
    return replacement_log_;
}

bool BufferPoolManager::FindFreeFrame(int* frame_id) {
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
        disk_manager_->WritePage(evicted_pid, pages_[victim].GetData());
    }
    page_table_.erase(evicted_pid);
    ReplacementLogEntry entry;
    entry.evicted_page_id = evicted_pid;
    entry.evicted_was_dirty = evicted_dirty;
    entry.loaded_page_id = INVALID_PAGE_ID;  // filled by caller
    replacement_log_.push_back(entry);
    ++stats_.replacement_count;
    if (frame_id) *frame_id = victim;
    return true;
}

}  // namespace sqlcompiler