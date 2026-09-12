#pragma once

#include <shared_mutex>
#include <type_traits>
#include <utility>

#include "storage/BufferPoolManager.h"
#include "storage/Page.h"

namespace sqlcompiler {

// 缓冲池页面的 RAII 句柄。
//
// 存在的理由：B+Tree 的插入/分裂路径会在一次调用里同时持有多个页面（叶子、
// 新兄弟、父节点、乃至新根），任何一条 early-return 都可能漏掉 UnpinPage，
// 而 pin 泄漏的表现是「缓冲池逐渐耗尽 → NewPage 返回 nullptr」这类极难定位的
// 远端故障。用 RAII 把 Unpin 绑定到作用域，从类型层面消除这类错误。
//
// 约定：B+Tree 内部禁止裸调用 BufferPoolManager::GetPage/UnpinPage。
class PageGuard {
public:
    PageGuard() = default;

    // 取已有页面；页面不存在或缓冲池耗尽时返回 Valid() == false 的空句柄
    static PageGuard Fetch(BufferPoolManager* bpm, page_id_t page_id);
    // 分配新页面（内容已由 BufferPoolManager::NewPage 清零）
    static PageGuard New(BufferPoolManager* bpm);

    ~PageGuard();

    PageGuard(PageGuard&& other) noexcept;
    PageGuard& operator=(PageGuard&& other) noexcept;
    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;

    bool Valid() const { return page_ != nullptr; }
    char* Data() const;
    page_id_t PageId() const { return page_id_; }
    // 标记该页已修改；析构时会以 is_dirty=true 归还
    void MarkDirty() { dirty_ = true; }
    // Phase B：把 LSN 转发到底层 Page。仅 WAL 写出器调一次，避免每次都绕回
    // BufferPool 找帧。
    void SetPageLsn(uint64_t lsn) {
        if (page_ != nullptr) page_->SetPageLsn(lsn);
    }
    // 提前归还（幂等）。析构时不会重复 Unpin。
    void Release();

private:
    PageGuard(BufferPoolManager* bpm, Page* page, page_id_t page_id);

    BufferPoolManager* bpm_ = nullptr;
    Page* page_ = nullptr;
    page_id_t page_id_ = INVALID_PAGE_ID;
    bool dirty_ = false;
};

// ---- E4：页级读写锁的 RAII 句柄 ----
//
// 与 PageGuard（只管理 pin 生命周期、不持任何页锁）不同，LatchedPageGuard 在拿到
// 帧后还持有一把页级读写锁：Exclusive=true 的 PageWriteGuard 持写（独占）锁，
// Exclusive=false 的 PageReadGuard 持读（共享）锁。因此多线程可对**同一页**并发
// 读（共享锁），写修改则串行（独占锁）——这正是「页级读写并发」的支撑。
//
// 锁序（防死锁）约定，见 Page::RLatch 注释：
//   1) Fetch/New：先 bpm->GetPage()（全局 latch_ 在方法内短暂持有后即释放），
//      返回后对帧上页锁。绝不「持全局锁再等页锁」之外的顺序；
//   2) Release：**必须先释放页锁，再调 bpm->UnpinPage()（内部取全局 latch_）**，
//      即持页锁期间绝不请求全局 latch_，从而不可能与「BPM 写回持全局锁取页锁」
//      形成等待环。
// 约定：持本句柄访问期间只能调用 Data()/PageId()/MarkDirty()/SetPageLsn() 这类
// 只触碰当前帧、不经缓冲池的方法；持页锁期间调用任何 BufferPoolManager 方法会违反锁序。
template <bool Exclusive>
class LatchedPageGuard {
public:
    LatchedPageGuard() = default;

    static LatchedPageGuard Fetch(BufferPoolManager* bpm, page_id_t page_id) {
        if (bpm == nullptr || page_id < 0) return LatchedPageGuard();
        Page* p = bpm->GetPage(page_id);
        if (p == nullptr) return LatchedPageGuard();
        if constexpr (Exclusive)
            p->WLatch();
        else
            p->RLatch();
        return LatchedPageGuard(bpm, p, page_id);
    }

    static LatchedPageGuard New(BufferPoolManager* bpm) {
        if (bpm == nullptr) return LatchedPageGuard();
        page_id_t pid = INVALID_PAGE_ID;
        Page* p = bpm->NewPage(&pid);
        if (p == nullptr) return LatchedPageGuard();
        if constexpr (Exclusive)
            p->WLatch();
        else
            p->RLatch();
        return LatchedPageGuard(bpm, p, pid);
    }

    ~LatchedPageGuard() { Release(); }

    LatchedPageGuard(LatchedPageGuard&& o) noexcept { *this = std::move(o); }
    LatchedPageGuard& operator=(LatchedPageGuard&& o) noexcept {
        if (this != &o) {
            Release();
            bpm_ = o.bpm_;
            page_ = o.page_;
            page_id_ = o.page_id_;
            dirty_ = o.dirty_;
            o.bpm_ = nullptr;
            o.page_ = nullptr;
            o.page_id_ = INVALID_PAGE_ID;
            o.dirty_ = false;
        }
        return *this;
    }
    LatchedPageGuard(const LatchedPageGuard&) = delete;
    LatchedPageGuard& operator=(const LatchedPageGuard&) = delete;

    bool Valid() const { return page_ != nullptr; }
    page_id_t PageId() const { return page_id_; }

    // 读模式返回 const 数据（只读），写模式返回可变数据（独占修改）。
    using DataPtr = std::conditional_t<Exclusive, char*, const char*>;
    DataPtr Data() { return page_ != nullptr ? page_->GetData() : nullptr; }

    void MarkDirty() { dirty_ = true; }
    void SetPageLsn(uint64_t lsn) {
        if (page_ != nullptr) page_->SetPageLsn(lsn);
    }

    // 提前归还：先释放页锁，再 Unpin（遵守锁序）。幂等。
    void Release() {
        if (page_ != nullptr && bpm_ != nullptr) {
            if constexpr (Exclusive)
                page_->WUnlatch();
            else
                page_->RUnlatch();
            bpm_->UnpinPage(page_id_, dirty_);
        }
        bpm_ = nullptr;
        page_ = nullptr;
        page_id_ = INVALID_PAGE_ID;
        dirty_ = false;
    }

private:
    LatchedPageGuard(BufferPoolManager* bpm, Page* page, page_id_t page_id)
        : bpm_(bpm), page_(page), page_id_(page_id) {}

    BufferPoolManager* bpm_ = nullptr;
    Page* page_ = nullptr;
    page_id_t page_id_ = INVALID_PAGE_ID;
    bool dirty_ = false;
};

using PageReadGuard = LatchedPageGuard<false>;   // 页级共享读锁句柄
using PageWriteGuard = LatchedPageGuard<true>;   // 页级独占写锁句柄

}  // namespace sqlcompiler
