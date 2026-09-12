#pragma once

#include <cstddef>
#include <cstdint>
#include <shared_mutex>

namespace sqlcompiler {

using page_id_t = int32_t;
constexpr page_id_t INVALID_PAGE_ID = -1;
constexpr size_t PAGE_SIZE = 4096;  // 固定页大小：4KB

// 表示内存中的一个物理页帧（frame），承载磁盘上某一页的数据
class Page {
public:
    Page();

    page_id_t GetPageId() const;
    void SetPageId(page_id_t page_id);

    char* GetData();
    const char* GetData() const;

    bool IsDirty() const;
    void SetDirty(bool dirty);

    int GetPinCount() const;
    void IncPinCount();
    void DecPinCount();

    // ---- Phase B：页级 LSN（Log Sequence Number） ----
    // 每个 page 帧记录最后一次写入该页的 log record 的 LSN。
    // 当 WAL 的 durability 要求「先 log 再 page」时（ARIES 的 WAL-before-data
    // 规则），BufferPool 在 FlushPage 之前必须保证 page.page_lsn_ <= log.durable_lsn_。
    // 反过来，恢复时 redo 阶段用 (page.page_lsn_ < record.lsn) 来判断「这条日志
    // 之后该页没被写过」——避免把同一个 page-image 重放多次。
    uint64_t GetPageLsn() const { return page_lsn_; }
    void SetPageLsn(uint64_t lsn) { page_lsn_ = lsn; }

    // ---- E4：页级读写锁（latch） ----
    // 每个物理帧带一把读写锁（std::shared_mutex），支持共享读 / 独占写两种访问。
    // 它保护「页数据 data_ 的并发访问」：多线程可持读锁共享读同一帧，写修改须持
    // 写锁独占。锁由 RAII 句柄（PageReadGuard / PageWriteGuard）在访问期间持有，
    // 不直接由 Page 自锁（GetData 保持无锁裸访问，向后兼容单线程路径）。
    // 锁序（防死锁）：全局 latch_（BPM）→ 页级锁；持页锁期间绝不允许再请求全局
    // latch_。Guard 释放顺序固定为「先释放页锁，再调用 BPM::UnpinPage」。
    void RLatch() { latch_.lock_shared(); }
    void RUnlatch() { latch_.unlock_shared(); }
    void WLatch() { latch_.lock(); }
    void WUnlatch() { latch_.unlock(); }
    std::shared_mutex& GetLatch() { return latch_; }
    const std::shared_mutex& GetLatch() const { return latch_; }

    // 重置页内容与元信息为初始状态，供缓冲池复用该帧时调用
    void ResetMemory();

private:
    page_id_t page_id_;
    char data_[PAGE_SIZE];
    bool is_dirty_;
    int pin_count_;
    // Phase B：当前帧对应 page 上一次被任何 log record 写入时的 LSN。
    // ResetMemory 中归零表示「这是全新页，未参与过 redo」。
    uint64_t page_lsn_ = 0;
    // E4：页级读写锁。与 data_ 独立存在，不参与 ResetMemory（锁不随帧内容清零）。
    std::shared_mutex latch_;
};

}  // namespace sqlcompiler