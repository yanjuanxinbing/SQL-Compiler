#include "storage/DiskManager.h"

#include <cstring>

namespace sqlcompiler {

namespace {

// Helper: get current file size in bytes
long long GetFileSize(std::fstream& fs) {
    auto cur = fs.tellg();
    fs.seekg(0, std::ios::end);
    auto end = fs.tellg();
    fs.seekg(cur);
    return static_cast<long long>(end);
}

}  // namespace

DiskManager::DiskManager(const std::string& db_file)
    : db_file_name_(db_file), next_page_id_(0) {
    db_io_.open(db_file_name_,
                std::ios::in | std::ios::out | std::ios::binary);
    if (!db_io_.is_open()) {
        // File doesn't exist yet — create it
        db_io_.clear();
        db_io_.open(db_file_name_,
                    std::ios::out | std::ios::binary);
        db_io_.close();
        db_io_.open(db_file_name_,
                    std::ios::in | std::ios::out | std::ios::binary);
    }
    long long sz = GetFileSize(db_io_);
    next_page_id_ = static_cast<page_id_t>(sz / PAGE_SIZE);
}

DiskManager::~DiskManager() {
    std::lock_guard<std::mutex> lock(db_io_latch_);
    if (db_io_.is_open()) {
        db_io_.flush();
        db_io_.close();
    }
}

page_id_t DiskManager::AllocatePage() {
    std::lock_guard<std::mutex> lock(db_io_latch_);
    if (!free_pages_.empty()) {
        page_id_t pid = free_pages_.back();
        free_pages_.pop_back();
        return pid;
    }
    return next_page_id_++;
}

void DiskManager::DeallocatePage(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(db_io_latch_);
    free_pages_.push_back(page_id);
}

void DiskManager::ReadPage(page_id_t page_id, char* data) {
    std::lock_guard<std::mutex> lock(db_io_latch_);
    std::memset(data, 0, PAGE_SIZE);
    if (page_id < 0) return;
    long long offset = static_cast<long long>(page_id) * PAGE_SIZE;
    long long file_size = GetFileSize(db_io_);
    if (offset >= file_size) return;
    db_io_.seekg(offset);
    db_io_.read(data, PAGE_SIZE);
    std::streamsize got = db_io_.gcount();
    if (got < static_cast<std::streamsize>(PAGE_SIZE)) {
        std::memset(data + got, 0, PAGE_SIZE - static_cast<size_t>(got));
    }
}

void DiskManager::WritePage(page_id_t page_id, const char* data, bool force) {
    std::lock_guard<std::mutex> lock(db_io_latch_);
    EnsureFileCapacity(page_id);
    long long offset = static_cast<long long>(page_id) * PAGE_SIZE;
    db_io_.seekp(offset);
    db_io_.write(data, PAGE_SIZE);
    db_io_.flush();
    // Phase B：COMMIT 路径调用 force=true，把 dirty page 强制刷到磁盘；
    // 默认 false 保留 Phase A 行为以减少同步开销。
    if (force) {
        db_io_.sync();  // std::fstream::sync 调用 OS fsync
    }
}

void DiskManager::Sync() {
    std::lock_guard<std::mutex> lock(db_io_latch_);
    db_io_.flush();
    db_io_.sync();  // 调用 OS fsync/FlushFileBuffers
}

int DiskManager::GetNumPages() const {
    // next_page_id_ is conceptually mutable, but the lock is needed for
    // consistent read. Take it briefly.
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(db_io_latch_));
    return next_page_id_;
}

void DiskManager::EnsureFileCapacity(page_id_t page_id) {
    if (page_id < 0) return;
    long long need = static_cast<long long>(page_id + 1) * PAGE_SIZE;
    long long cur = GetFileSize(db_io_);
    if (cur >= need) return;
    db_io_.seekp(need - 1);
    char zero = 0;
    db_io_.write(&zero, 1);
    db_io_.flush();
}

}  // namespace sqlcompiler