#pragma once

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
    // 提前归还（幂等）。析构时不会重复 Unpin。
    void Release();

private:
    PageGuard(BufferPoolManager* bpm, Page* page, page_id_t page_id);

    BufferPoolManager* bpm_ = nullptr;
    Page* page_ = nullptr;
    page_id_t page_id_ = INVALID_PAGE_ID;
    bool dirty_ = false;
};

}  // namespace sqlcompiler
