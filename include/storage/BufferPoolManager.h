#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage/DiskManager.h"
#include "storage/Page.h"
#include "storage/Replacer.h"

namespace sqlcompiler {

// 前向声明：避免 BufferPoolManager.h 引入事务模块的 <mutex> 与 LogRecord 头。
class LogManager;

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

    // 将指定页强制写回磁盘（无论是否脏页）。
    // Phase B：若 page.page_lsn_ > log.durable_lsn_，必须先 Flush 日志，
    // 否则磁盘上的 page-image 会「领先」日志，导致重启时 redo 不到原始写入。
    // 没设置 LogManager 时回退到 Phase A 行为（仅写盘）。
    bool FlushPage(page_id_t page_id);

    // 将缓冲池中所有脏页写回磁盘（不写干净页）。
    // Phase B：COMMIT 时调用此方法把脏页快速落盘，路径上自动尊重 WAL 规则。
    // 配合 DiskManager::Sync() 实现 COMMIT 语义。
    void FlushAllDirtyPages();

    // 将缓冲池中所有页写回磁盘（包括干净页）。Phase A 测试依赖此方法做
    // 测试收尾清理；保留接口。
    void FlushAllPages();

    // 删除一个页：从缓冲池中移除并通过DiskManager回收该page_id
    bool DeletePage(page_id_t page_id);

    // ---- Phase B：注入 LogManager，让 FlushPage 在写盘前尊重 WAL 规则 ----
    void SetLogManager(LogManager* lm) { log_manager_ = lm; }
    LogManager* GetLogManager() const { return log_manager_; }

    // 收集当前缓冲池中的脏页（page_id, page_lsn），供 COMMIT 路径强制落盘用。
    // 返回顺序无定义。
    std::vector<std::pair<page_id_t, uint64_t>> CollectDirtyPages();

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

    // Phase B：可空；非空时 FlushPage / FlushAllDirtyPages 走 LSN 检查。
    LogManager* log_manager_ = nullptr;

    // 找到一个可用帧：优先从 free_list_ 取空闲帧，否则调用 replacer_->Victim()
    // 淘汰一页；若淘汰的页为脏页需先写回磁盘。成功返回 true 并写入 *frame_id。
    // 同时把一条 ReplacementLogEntry 追加到 replacement_log_。
    //
    // to_load：调用方即将换入该帧的 page_id（GetPage 是请求加载的 page_id；
    //          NewPage 是 DiskManager 刚分配的新 page_id）。该值会原样写入
    //          ReplacementLogEntry.loaded_page_id，便于外部观测"换入什么页"。
    //          若调用方尚未决定 page_id（例如 NewPage 的 pid 在分配后才得到），
    //          可以传 INVALID_PAGE_ID，并在之后用 PatchLastReplacementLog()
    //          补填。
    bool FindFreeFrame(int* frame_id, page_id_t to_load);

    // 补填最近一次 FindFreeFrame 写入的 ReplacementLogEntry.loaded_page_id。
    // 适用于调用方在 FindFreeFrame 之后才确定 page_id 的场景（如 NewPage 先
    // 拿到 frame 再调 DiskManager::AllocatePage 的旧路径）。无副作用时
    // （日志为空）安全 no-op。
    void PatchLastReplacementLog(page_id_t loaded_page_id);
};

}  // namespace sqlcompiler