#pragma once

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "storage/BlockDevice.h"
#include "storage/Page.h"

namespace sqlcompiler {

// 磁盘管理器：负责数据文件的页级读写，以及页的分配与回收
// 对上层暴露 read_page(page_id) / write_page(page_id, data) 语义的接口
//
// 采用 C 标准 I/O (FILE*) 而非 std::fstream：
//  - fstream 不暴露文件描述符，无法在 Windows 上做真正的 fsync（FlushFileBuffers 等价物）；
//  - 使用 FILE* 后可通过 _commit / fsync 拿到真正的持久化语义。
class DiskManager {
public:
    explicit DiskManager(const std::string& db_file);
    // E7：注入自定义块设备（如 FaultInjectingBlockDevice 做坏块/断电模拟）。
    // 数据文件的 RAM 页 I/O 将走该设备；<db>.fpl / <db>.crc 旁路仍由本类直接管理。
    // 不传时默认用 FileBlockDevice 打开 db_file。
    DiskManager(const std::string& db_file, std::unique_ptr<BlockDevice> device);
    ~DiskManager();

    // 分配一个新页，返回其page_id（优先复用已回收的空闲页号）
    page_id_t AllocatePage();

    // 释放一个页，将其归还到空闲页列表，供后续AllocatePage()复用。
    // 空闲页号会持久化到 <db>.fpl 位图，保证程序重启后仍可复用（指导书『管理
    // 空闲页列表…回收』）。
    void DeallocatePage(page_id_t page_id);

    // 从磁盘文件中读取page_id对应的页内容到data（大小需为PAGE_SIZE）
    void ReadPage(page_id_t page_id, char* data);

    // 将data（大小为PAGE_SIZE）写入磁盘文件中page_id对应的位置。
    // force=true 时调用 OS fsync 把数据落到磁盘；默认 false（依赖 Shutdown
    // 或显式 Sync 兜底），用于 Phase B 的 COMMIT 路径确保 dirty page 落盘。
    void WritePage(page_id_t page_id, const char* data, bool force = false);

    // 强制把数据文件刷新到磁盘（fdatasync / FlushFileBuffers）。Phase B 的
    // COMMIT 路径在 LogManager::Flush 之后调用本接口，保证 redo 数据先于
    // WAL 的 CHECKPOINT 可见。
    void Sync();

    // 当前已分配（含已回收）的页数，即下一个全新page_id
    int GetNumPages() const;

    // 当前待回收的空闲页数量（含启动时从 <db>.fpl 装载的持久化空闲页与本次会话
    // DeallocatePage 累计，尚未被 AllocatePage 复用的部分）。供 \stats 输出。
    int GetNumFreePages() const;

    // 物理磁盘 I/O 计数（页级读写次数），供 \stats 输出 IO 统计，
    // 对齐指导书「输出日志与统计信息 / I/O 统计监控」。
    long long GetIOReadCount() const;
    long long GetIOWriteCount() const;

private:
    std::string db_file_name_;
    // 数据文件的页块设备（默认为 FileBlockDevice；测试可注入故障设备）。
    std::unique_ptr<BlockDevice> device_;
    bool device_ready_ = false;  // 底层介质是否打开可用（open 失败时置 false，I/O 降级为空操作）
    std::mutex db_io_latch_;

    page_id_t next_page_id_;
    std::vector<page_id_t> free_pages_;

    // 缓存文件字节大小，避免每次 I/O 都做 seek 探测（原实现每次读页额外 3 次 seek）。
    // 只在构造与扩展文件时更新。
    long long file_size_ = 0;

    // 物理磁盘 I/O 计数：仅在真正触达磁盘的读/写处累加。
    long long io_read_count_ = 0;   // 物理读页次数（页面从磁盘加载）
    long long io_write_count_ = 0;  // 物理写页次数（页面写回磁盘）

    // ---- 空闲页持久化（<db>.fpl 位图，见 LoadFreePageBitmap 注释）----
    // <db>.fpl 布局：24 字节头部 + 页位图；位 i = 1 表示 page i 空闲可复用。
    static constexpr uint32_t kFplMagic = 0x46504C31u;  // "FPL1"
    static constexpr uint32_t kFplVersion = 1u;
    static constexpr size_t kFplHeader = 24;  // u32 magic + u32 version + u64 pages + u64 size
    static constexpr size_t kFplMaxLoadBytes = 1u << 22;  // 位图装载上限（4M bit ≈ 32K 页），防损坏头 OOM

    std::string fpl_path_;              // <db>.fpl
    std::FILE* fpl_ = nullptr;          // 位图文件句柄（惰性创建）
    std::vector<uint8_t> fbit_;         // 位图的内存副本（位 i = page i 空闲）
    size_t fbit_bytes_ = 0;             // fbit_ 有效字节数

    // 确保底层文件大小足以容纳page_id对应的页，不足则扩展文件
    void EnsureFileCapacity(page_id_t page_id);

    // 启动时若存在 <db>.fpl 且格式/页数一致则装载空闲列表；否则视为空
    // （旧库向后兼容：无 .fpl 时行为与原先完全一致）。
    void LoadFreePageBitmap();
    // 惰性创建 .fpl 文件（首次 DeallocatePage 时才落盘，避免无回收场景产生多余文件）
    void EnsureFplFile();
    // 将位图扩展至至少 need_bytes 字节（不足补零，0 = 已分配）
    void EnsureBitCapacity(size_t need_bytes);
    // 把 fbit_[byte_index] 写回文件并持久化
    void FlushBitByte(size_t byte_index);
    // 回写 24 字节头部（pages = next_page_id_，size = fbit_bytes_）并持久化
    void FlushFplHeader();
    // 将某个页置为空闲(1)/已分配(0)，持久化后再返回（分配复用前必须先置 0 防复用活页）
    void SetPageFree(page_id_t page_id, bool free);
    // 析构前收尾：把位图对齐到 next_page_id_ 并写头，保证优雅重启后可装载
    void FinalizeFreePageBitmap();

    // ---- 页 CRC32 校验（<db>.crc 旁路，见 LoadPageCrcs 注释）----
    // 布局：无头，每页 4 字节小端 u32（该页整页 PAGE_SIZE 的 CRC32）；0 = 无记录。
    // 向后兼容：旧库无 .crc → pcrc_ 为空 → GetPageCrc 返回 false → 跳过校验，行为不变。
    std::string crc_path_;              // <db>.crc
    std::FILE* crc_ = nullptr;          // CRC 文件句柄（惰性创建）
    std::vector<uint32_t> pcrc_;        // 内存缓存：pcrc_[page_id]；0 = 无记录

    // 启动时装载既有 <db>.crc（缺文件/损坏按空处理，向后兼容）。
    void LoadPageCrcs();
    // 惰性创建 .crc 并保证至少容纳 need_count 个页的 CRC（不足用 0 补齐）。
    void EnsureCrcFile(size_t need_count);
    // 记录某页 CRC 并回写（fflush 到 OS 缓存即可，不逐页 fsync，避免破坏组提交）。
    void SetPageCrc(page_id_t page_id, const char* data);
    // 清空某页 CRC（回收页复用前调用，防残留旧 CRC 造成误报）。
    void ClearPageCrc(page_id_t page_id);
    // 读取某页已记录的 CRC；不存在（未分配/旧页）返回 false。
    bool GetPageCrc(page_id_t page_id, uint32_t* out) const;
    // 把 pcrc_ 全部按页刷写进文件（不 fsync，配合 Sync 统一落盘）。
    void FlushCrcFile();
    // 析构收尾：FlushCrcFile + 关文件。
    void FinalizePageCrcs();
};

}  // namespace sqlcompiler
