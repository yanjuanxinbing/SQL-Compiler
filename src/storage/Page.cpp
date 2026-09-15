#include "storage/Page.h"

#include <cstring>

namespace sqlcompiler {

Page::Page() : page_id_(INVALID_PAGE_ID), is_dirty_(false), pin_count_(0) {
    ResetMemory();
}

page_id_t Page::GetPageId() const {
    return page_id_;
}

void Page::SetPageId(page_id_t page_id) {
    page_id_ = page_id;
}

char* Page::GetData() {
    return data_;
}

const char* Page::GetData() const {
    return data_;
}

bool Page::IsDirty() const {
    return is_dirty_;
}

void Page::SetDirty(bool dirty) {
    if (is_dirty_ == dirty) {
        // 状态未变：跳过回调，避免 dirty_frames_ 集合的冗余 insert/erase。
        // 这一点对热路径（每次 UnpinPage 都可能 SetDirty(true)）很关键。
        return;
    }
    is_dirty_ = dirty;
    if (dirty_cb_) dirty_cb_(dirty);
}

int Page::GetPinCount() const {
    return pin_count_;
}

void Page::IncPinCount() {
    ++pin_count_;
}

void Page::DecPinCount() {
    if (pin_count_ > 0) {
        --pin_count_;
    }
}

void Page::ResetMemory() {
    std::memset(data_, 0, PAGE_SIZE);
    page_id_ = INVALID_PAGE_ID;
    is_dirty_ = false;
    pin_count_ = 0;
    // Phase B：新帧视为未参与 redo，page_lsn_ 必须归零，否则回收后的页面
    // 会被错误地认成「已经被某条 lsn 写过」。
    page_lsn_ = 0;
    // 走 SetDirty(false) 触发回调，保持 dirty_frames_ 同步。
    // 直接置 is_dirty_ = false 已完成，但回调不会被通知——所以这里显式调用。
    // 注：上面已经把 is_dirty_ 置 false，SetDirty 会因为状态未变而跳过回调，
    // 但 dirty_cb_(false) 仍需要发一次「重置」通知，否则旧脏标记会残留。
    if (dirty_cb_) dirty_cb_(false);
}

}  // namespace sqlcompiler