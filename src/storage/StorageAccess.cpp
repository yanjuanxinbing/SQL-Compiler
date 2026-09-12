#include "storage/StorageAccess.h"

namespace sqlcompiler {

StorageAccess::StorageAccess(BufferPoolManager* bpm, DiskManager* dm)
    : buffer_pool_manager_(bpm), disk_manager_(dm) {
}

// ---- Page access (cache-first) ----

Page* StorageAccess::GetPage(page_id_t page_id) {
    return buffer_pool_manager_->GetPage(page_id);
}

Page* StorageAccess::NewPage(page_id_t* new_page_id) {
    return buffer_pool_manager_->NewPage(new_page_id);
}

bool StorageAccess::UnpinPage(page_id_t page_id, bool is_dirty) {
    return buffer_pool_manager_->UnpinPage(page_id, is_dirty);
}

bool StorageAccess::FlushPage(page_id_t page_id) {
    return buffer_pool_manager_->FlushPage(page_id);
}

void StorageAccess::FlushAllDirtyPages() {
    buffer_pool_manager_->FlushAllDirtyPages();
}

void StorageAccess::FlushAllPages() {
    buffer_pool_manager_->FlushAllPages();
}

// ---- Raw disk IO (no cache) ----

void StorageAccess::ReadPage(page_id_t page_id, char* data) {
    disk_manager_->ReadPage(page_id, data);
}

void StorageAccess::WritePage(page_id_t page_id, const char* data, bool force) {
    disk_manager_->WritePage(page_id, data, force);
}

// ---- Page lifecycle ----

page_id_t StorageAccess::AllocatePage() {
    return disk_manager_->AllocatePage();
}

void StorageAccess::DeallocatePage(page_id_t page_id) {
    disk_manager_->DeallocatePage(page_id);
}

bool StorageAccess::DeletePage(page_id_t page_id) {
    return buffer_pool_manager_->DeletePage(page_id);
}

// ---- Diagnostics ----

const BufferPoolStats& StorageAccess::GetStats() const {
    return buffer_pool_manager_->GetStats();
}

const std::vector<ReplacementLogEntry>& StorageAccess::GetReplacementLog() const {
    return buffer_pool_manager_->GetReplacementLog();
}

}  // namespace sqlcompiler