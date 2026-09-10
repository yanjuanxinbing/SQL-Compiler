#include "index/PageGuard.h"

#include <utility>

namespace sqlcompiler {

PageGuard::PageGuard(BufferPoolManager* bpm, Page* page, page_id_t page_id)
    : bpm_(bpm), page_(page), page_id_(page_id) {
}

PageGuard PageGuard::Fetch(BufferPoolManager* bpm, page_id_t page_id) {
    if (bpm == nullptr || page_id < 0) return PageGuard();
    Page* p = bpm->GetPage(page_id);
    if (p == nullptr) return PageGuard();
    return PageGuard(bpm, p, page_id);
}

PageGuard PageGuard::New(BufferPoolManager* bpm) {
    if (bpm == nullptr) return PageGuard();
    page_id_t pid = INVALID_PAGE_ID;
    Page* p = bpm->NewPage(&pid);
    if (p == nullptr) return PageGuard();
    return PageGuard(bpm, p, pid);
}

PageGuard::~PageGuard() {
    Release();
}

PageGuard::PageGuard(PageGuard&& other) noexcept
    : bpm_(other.bpm_), page_(other.page_), page_id_(other.page_id_),
      dirty_(other.dirty_) {
    other.bpm_ = nullptr;
    other.page_ = nullptr;
    other.page_id_ = INVALID_PAGE_ID;
    other.dirty_ = false;
}

PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
    if (this != &other) {
        Release();
        bpm_ = other.bpm_;
        page_ = other.page_;
        page_id_ = other.page_id_;
        dirty_ = other.dirty_;
        other.bpm_ = nullptr;
        other.page_ = nullptr;
        other.page_id_ = INVALID_PAGE_ID;
        other.dirty_ = false;
    }
    return *this;
}

char* PageGuard::Data() const {
    return page_ != nullptr ? page_->GetData() : nullptr;
}

void PageGuard::Release() {
    if (page_ != nullptr && bpm_ != nullptr) {
        bpm_->UnpinPage(page_id_, dirty_);
    }
    bpm_ = nullptr;
    page_ = nullptr;
    page_id_ = INVALID_PAGE_ID;
    dirty_ = false;
}

}  // namespace sqlcompiler
