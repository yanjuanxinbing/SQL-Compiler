#pragma once

#include "storage/BufferPoolManager.h"
#include "storage/DiskManager.h"

namespace sqlcompiler {

// StorageAccess is the single documented entry point for page-level storage
// operations on top of BufferPoolManager + DiskManager.
//
// All database modules (catalog, executors, recovery, txn) should call this
// facade instead of touching BufferPoolManager / DiskManager directly.
// Reasons:
//   (a) single documentation point — one place to learn the storage API;
//   (b) easy to add cross-cutting concerns (tracing, caching, IO scheduling,
//       instrumentation, second-tier cache, alternative replacers) without
//       rewriting call sites;
//   (c) keeps test setup simple (mock the facade).
//
// This class is a thin pass-through wrapper: it does NOT implement caching or
// replacement itself; it merely forwards to the wrapped BufferPoolManager and
// DiskManager. Construction takes raw pointers (not ownership); the caller
// must guarantee the underlying objects outlive this facade.
class StorageAccess {
public:
    StorageAccess(BufferPoolManager* bpm, DiskManager* dm);

    // ---- Page access (cache-first) ----
    Page* GetPage(page_id_t page_id);
    Page* NewPage(page_id_t* new_page_id);
    bool UnpinPage(page_id_t page_id, bool is_dirty);
    bool FlushPage(page_id_t page_id);
    void FlushAllDirtyPages();
    void FlushAllPages();

    // ---- Raw disk IO (no cache) ----
    void ReadPage(page_id_t page_id, char* data);
    void WritePage(page_id_t page_id, const char* data, bool force = false);

    // ---- Page lifecycle ----
    page_id_t AllocatePage();
    void DeallocatePage(page_id_t page_id);
    bool DeletePage(page_id_t page_id);

    // ---- Diagnostics ----
    const BufferPoolStats& GetStats() const;
    const std::vector<ReplacementLogEntry>& GetReplacementLog() const;

    // ---- 低层直接访问（BPlusTree / PageGuard 等需要直接持有 BPM 的模块）----
    // 大多数调用方应该走 GetPage / NewPage 等缓存优先路径，不直接拿 BPM/DM。
    // 仅当模块需要与 BPM 的具体替换策略 / 帧管理耦合时才使用。
    BufferPoolManager* GetBufferPoolManager() const { return buffer_pool_manager_; }
    DiskManager* GetDiskManager() const { return disk_manager_; }

private:
    BufferPoolManager* buffer_pool_manager_;  // not owned
    DiskManager* disk_manager_;                // not owned
};

}  // namespace sqlcompiler