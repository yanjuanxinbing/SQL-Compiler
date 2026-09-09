#pragma once

#include <cstddef>
#include <cstdint>

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

    // 重置页内容与元信息为初始状态，供缓冲池复用该帧时调用
    void ResetMemory();

private:
    page_id_t page_id_;
    char data_[PAGE_SIZE];
    bool is_dirty_;
    int pin_count_;
};

}  // namespace sqlcompiler
