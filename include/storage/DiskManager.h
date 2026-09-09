#pragma once

#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "storage/Page.h"

namespace sqlcompiler {

// 磁盘管理器：负责数据文件的页级读写，以及页的分配与回收
// 对上层暴露 read_page(page_id) / write_page(page_id, data) 语义的接口
class DiskManager {
public:
    explicit DiskManager(const std::string& db_file);
    ~DiskManager();

    // 分配一个新页，返回其page_id（优先复用已回收的空闲页号）
    page_id_t AllocatePage();

    // 释放一个页，将其归还到空闲页列表，供后续AllocatePage()复用
    void DeallocatePage(page_id_t page_id);

    // 从磁盘文件中读取page_id对应的页内容到data（大小需为PAGE_SIZE）
    void ReadPage(page_id_t page_id, char* data);

    // 将data（大小为PAGE_SIZE）写入磁盘文件中page_id对应的位置
    void WritePage(page_id_t page_id, const char* data);

    // 当前已分配（含已回收）的页数，即下一个全新page_id
    int GetNumPages() const;

private:
    std::string db_file_name_;
    std::fstream db_io_;
    std::mutex db_io_latch_;

    page_id_t next_page_id_;
    std::vector<page_id_t> free_pages_;

    // 确保底层文件大小足以容纳page_id对应的页，不足则扩展文件
    void EnsureFileCapacity(page_id_t page_id);
};

}  // namespace sqlcompiler
