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
}

}  // namespace sqlcompiler