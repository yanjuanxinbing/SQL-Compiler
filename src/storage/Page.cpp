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
    is_dirty_ = dirty;
}

// T4：从干净变脏时记录一次变脏时刻（幂等；重复标脏不覆盖）。
void Page::MarkDirtyFromClean(int64_t op_tick) {
    if (!is_dirty_) {
        is_dirty_ = true;
        dirty_since_tick_ = op_tick;
    }
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
    // Phase 4：访问温度清零——新帧重新积累温度。
    access_count_ = 0;
    // T4：脏页年龄基准清零——新帧视为从未变脏。
    dirty_since_tick_ = 0;
}

}  // namespace sqlcompiler