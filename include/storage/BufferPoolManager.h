#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage/DiskManager.h"
#include "storage/Page.h"
#include "storage/Replacer.h"

namespace sqlcompiler {

// 可选的替换策略种类
enum class ReplacementPolicy { LRU, FIFO };

// 缓存命中统计信息
struct BufferPoolStats {
    long hit_count = 0;
    long miss_count = 0;
    long replacement_count = 0;

    double HitRate() const;
};

// 一条替换日志记录，便于调试与展示缓存替换过程
struct ReplacementLogEntry {
    page_id_t evicted_page_id;  // 被淘汰的页（INVALID_PAGE_ID表示淘汰的是空槽位）
    page_id_t loaded_page_id;   // 本次换入的页
    bool evicted_was_dirty;     // 被淘汰的页在换出前是否为脏页
};

// 缓冲池管理器：在内存中缓存固定数量的页，减少磁盘IO次数
// 对上层（存储引擎）提供 get_page / flush_page 等接口，
// 并在缓存满时依据Replacer选择的策略（LRU/FIFO）淘汰旧页
class BufferPoolManager {
public:
    BufferPoolManager(size_t pool_size, DiskManager* disk_manager,
                       ReplacementPolicy policy = ReplacementPolicy::LRU);
    ~BufferPoolManager();

    // 获取page_id对应的页（若不在缓存中则从磁盘加载，必要时淘汰旧页）
    // 返回的Page指针在使用完毕后必须调用UnpinPage释放，返回nullptr表示获取失败
    Page* GetPage(page_id_t page_id);

    // 通过DiskManager分配一个新页并将其放入缓冲池，page_id通过输出参数返回
    Page* NewPage(page_id_t* page_id);

    // 释放对某页的占用（pin计数减一），is_dirty表示本次使用是否修改了该页
    bool UnpinPage(page_id_t page_id, bool is_dirty);

    // 将指定页强制写回磁盘（无论是否脏页）
    bool FlushPage(page_id_t page_id);

    // 将缓冲池中所有页写回磁盘
    void FlushAllPages();

    // 删除一个页：从缓冲池中移除并通过DiskManager回收该page_id
    bool DeletePage(page_id_t page_id);

    const BufferPoolStats& GetStats() const;
    const std::vector<ReplacementLogEntry>& GetReplacementLog() const;

private:
    size_t pool_size_;
    DiskManager* disk_manager_;
    std::unique_ptr<Replacer> replacer_;

    std::vector<Page> pages_;                        // 帧数组（frame slots），下标即frame_id
    std::unordered_map<page_id_t, int> page_table_;   // page_id -> frame_id
    std::vector<int> free_list_;                      // 尚未使用的空闲帧id列表

    BufferPoolStats stats_;
    std::vector<ReplacementLogEntry> replacement_log_;

    // 找到一个可用帧：优先从free_list_取空闲帧，否则调用replacer_->Victim()淘汰一页；
    // 若淘汰的页为脏页需先写回磁盘。成功返回true并写入frame_id
    bool FindFreeFrame(int* frame_id);
};

}  // namespace sqlcompiler
