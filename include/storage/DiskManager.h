#pragma once

// =============================================================================
// DiskManager — 数据文件的页级 I/O、页分配/回收与旁路持久化
//
// 职责：把「页」映射到数据文件的固定偏移（offset = page_id * PAGE_SIZE）完成读写；
//       维护「下一个全新页号」与「空闲页列表」；并把空闲页位图（<db>.fpl）与页
//       校验和（<db>.crc）旁路持久化，使重启后仍能复用回收页、并能识别介质损坏。
//
// 关键设计决策：
//   * 页 I/O 已下沉到 BlockDevice 抽象：本类只持有 device_ 句柄，默认用
//     FileBlockDevice 打开 db_file，测试可注入 FaultInjectingBlockDevice 等实现；
//   * .fpl / .crc 属「旁路文件」，仍由本类以 FILE* 直接管理，不走 BlockDevice；
//     两者都惰性创建（无回收 / 无写页则不产生文件），旧库缺文件即按空处理；
//   * 大文件定位统一走 64 位接口（_fseeki64 / fseeko），并用 file_size_ 缓存文件
//     字节大小，避免每次 I/O 都 seek 探测。
//
// 文件布局：
//   <db>      纯页数组，第 i 页位于偏移 i * PAGE_SIZE，无文件头；
//   <db>.fpl  24 字节头（u32 magic / u32 version / u64 pages / u64 size）+ 位图，
//             位 i = 1 表示 page i 空闲可复用；
//   <db>.crc  无头，每页 4 字节小端 u32（该页整页的 CRC32），0 表示无记录。
//
// 必须维持的不变量与约束：
//   * 锁序：上层 BufferPoolManager 的 frame latch 先于本类 db_io_latch_；本类内部
//     不再回调任何上层对象，故不存在反向加锁。
//   * next_page_id_ == 数据文件字节大小 / PAGE_SIZE（装载 .fpl 时以此为一致性判据，
//     不一致则整份位图安全丢弃，只损失空间复用、绝不误复用活页）。
//   * free_pages_ 中不出现重复页号，且每个元素在位图中对应位为 1。
//   * 写页前必须先 EnsureFileCapacity 扩展文件，否则会写到文件末尾之外。
//   * 崩溃安全性：Sync() 必须先持久化 .crc 再持久化数据页（见 Sync 的说明）。
// =============================================================================

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
//
// 线程安全：所有成员函数均以 db_io_latch_ 串行化（含 const 查询方法，故内部用
// const_cast 加锁），可被多线程并发调用。本类不拥有、也不回调上层对象。
class DiskManager {
public:
    // 单参构造：使用默认文件块设备。
    // @param db_file 数据文件路径；据此派生 <db>.fpl 与 <db>.crc 两个旁路文件名。
    // @note 内部委托给双参构造并传入空设备指针；介质打开失败不抛异常，而是置
    //       device_ready_ = false，后续 I/O 降级为空操作或报错（见各方法说明）。
    explicit DiskManager(const std::string& db_file);
    // E7：注入自定义块设备（如 FaultInjectingBlockDevice 做坏块/断电模拟）。
    // 数据文件的 RAM 页 I/O 将走该设备；<db>.fpl / <db>.crc 旁路仍由本类直接管理。
    // 不传时默认用 FileBlockDevice 打开 db_file。
    // @param db_file 数据文件路径；设备不可用时亦用作诊断名，并作为旁路文件名前缀。
    // @param device  自定义块设备；为 nullptr 时内部构造 FileBlockDevice(db_file)。
    //                 非空时所有权转移给本类（析构时随 device_.reset() 释放）。
    // @note 构造末尾依次执行 LoadFreePageBitmap() / LoadPageCrcs()，装载既有旁路文件；
    //       next_page_id_ 初值取「介质字节大小 / PAGE_SIZE」。
    DiskManager(const std::string& db_file, std::unique_ptr<BlockDevice> device);
    // 析构：加锁后先 FinalizeFreePageBitmap() 与 FinalizePageCrcs() 收尾落盘，
    //       再释放 device_（FileBlockDevice 析构负责 fflush + fclose）。
    // @note 收尾时的写盘失败会被静默吞掉，不抛异常；析构后不得再调用本对象方法。
    ~DiskManager();

    // 分配一个新页，返回其page_id（优先复用已回收的空闲页号）
    // @return 新页号，恒 ≥ 0：优先弹出 free_pages_ 栈顶的回收页；栈空时返回
    //         next_page_id_ 的当前值并使其自增（即返回自增前的旧值）。
    // @note 复用回收页前必须 SetPageFree(pid, false) 持久化清零位图位，否则崩溃重启
    //       后该页会被再次分配，导致两个对象共享同一物理页（数据损坏）。
    // @note 本方法不触碰介质、不扩展文件：首次写入该页时由 WritePage 扩容。
    page_id_t AllocatePage();

    // 释放一个页，将其归还到空闲页列表，供后续AllocatePage()复用。
    // 空闲页号会持久化到 <db>.fpl 位图，保证程序重启后仍可复用（指导书『管理
    // 空闲页列表…回收』）。
    // @param page_id 待回收的页号；< 0 时直接忽略（无操作）。
    // @note 幂等：若位图中该页已标记为空闲则直接返回，避免 free_pages_ 出现重复项。
    // @note 副作用：写入 .fpl（首次调用时惰性创建该文件）并清除该页的 .crc 记录；
    //       页内数据不清零，复用前由上层自行初始化。
    // @note 从未分配 / 从未释放过的页首次调用不会被幂等检查拦截（位图未覆盖位视为
    //       「使用中」），故仍会入栈。
    void DeallocatePage(page_id_t page_id);

    // 从磁盘文件中读取page_id对应的页内容到data（大小需为PAGE_SIZE）
    // @param page_id 目标页号；< 0 时将 data 清零后直接返回。
    // @param data    输出缓冲，调用方须保证非空且至少 PAGE_SIZE 字节。
    // @note 进入函数即 memset(data, 0, PAGE_SIZE)：未分配页（偏移 ≥ file_size_）或
    //       介质不可用时返回全零页，属合法语义而非错误。
    // @note 页尾不足 PAGE_SIZE 时只读可用字节，其余保持为零（介质末尾容错，不报错）。
    // @note 若该页在 .crc 中有记录（非 0）且 CRC32 不匹配，则累计 crc_error_count_
    //       并抛 std::runtime_error，交由上层统一 I/O 错误通道处理。
    // @note io_read_count_ 仅在真正触达介质时自增（未分配页 / 介质不可用不计）。
    void ReadPage(page_id_t page_id, char* data);

    // 将data（大小为PAGE_SIZE）写入磁盘文件中page_id对应的位置。
    // force=true 时调用 OS fsync 把数据落到磁盘；默认 false（依赖 Shutdown
    // 或显式 Sync 兜底），用于 Phase B 的 COMMIT 路径确保 dirty page 落盘。
    // @param page_id 目标页号，调用方须保证 ≥ 0（本方法不做负值校验）。
    // @param data    长度为 PAGE_SIZE 的页镜像，不得为空指针；按原样写入介质。
    // @param force   true 时写完立即 device_->Sync() 落盘；false（默认）只留在 OS
    //                缓存，由 Sync() 或析构统一兜底，以减少同步开销。
    // @note 写入前先 EnsureFileCapacity 扩容；成功写满 PAGE_SIZE 后自增
    //       io_write_count_，并刷新该页在 .crc 中的记录（仅 fflush，不逐页 fsync）。
    // @note 介质不可用（device_ready_ == false）或出现短写时抛 std::runtime_error。
    void WritePage(page_id_t page_id, const char* data, bool force = false);

    // 强制把数据文件刷新到磁盘（fdatasync / FlushFileBuffers）。Phase B 的
    // COMMIT 路径在 LogManager::Flush 之后调用本接口，保证 redo 数据先于
    // WAL 的 CHECKPOINT 可见。
    // @note 顺序关键：先 FlushCrcFile() + DurableSync(crc_)，后 device_->Sync()，
    //       即「校验先落、数据后落」；顺序颠倒会在两次 fsync 之间断电时产生
    //       「数据已是新值、校验仍是旧值」的状态，被恢复逻辑误报为介质损坏
    //       （完整推导见 .cpp 中的注释）。
    // @note .crc 未创建时跳过校验一侧，介质不可用时跳过数据一侧，均不抛异常。
    void Sync();

    // 当前已分配（含已回收）的页数，即下一个全新page_id
    // @return next_page_id_：已「诞生」过的页号总数，不扣减已回收页，
    //         因此与「活跃页数」不同，不可用作用户数据的页计数。
    int GetNumPages() const;

    // 当前待回收的空闲页数量（含启动时从 <db>.fpl 装载的持久化空闲页与本次会话
    // DeallocatePage 累计，尚未被 AllocatePage 复用的部分）。供 \stats 输出。
    // @return free_pages_.size()，即当前可供复用的页号个数。
    int GetNumFreePages() const;

    // 物理磁盘 I/O 计数（页级读写次数），供 \stats 输出 IO 统计，
    // 对齐指导书「输出日志与统计信息 / I/O 统计监控」。
    // @return 自构造以来「真正触达介质的读页次数」，进程内累计、不持久化。
    long long GetIOReadCount() const;
    // @return 自构造以来「成功写满一页的写页次数」，进程内累计、不持久化。
    long long GetIOWriteCount() const;

    // ---- T4 诊断：CRC 校验累计计数 ----
    // 每次读页时若该页存在持久化 CRC 记录且核对失败（磁盘损坏/位翻转/部分写），
    // 在抛出 I/O 错误前累计一次。供 \analyze / 故障诊断观察「介质损坏率」。
    // @return 校验失败累计次数（进程内，不持久化）。
    long long GetCrcErrorCount() const;

    // ---- T4 诊断：底层介质名 ----
    // 透传当前块设备名称（如 "memory" / "<path>(sparse)"），供 \analyze 输出。
    // @return device_->Name()；device_ 为空时退化为数据文件路径 db_file_name_。
    std::string GetDeviceName() const;

private:
    std::string db_file_name_;  // 数据文件路径；派生 .fpl/.crc 文件名，亦是降级时的诊断名
    // 数据文件的页块设备（默认为 FileBlockDevice；测试可注入故障设备）。
    // 独占所有权：非空时由本类持有，析构函数调用 device_.reset() 释放。
    std::unique_ptr<BlockDevice> device_;
    bool device_ready_ = false;  // 底层介质是否打开可用（open 失败时置 false，I/O 降级为空操作）
    // 串行化本类全部内部状态与介质访问；const 查询方法通过 const_cast 获取。
    // 锁序从上到下：BufferPoolManager 的 frame latch → 本锁，绝不反向回调。
    std::mutex db_io_latch_;

    page_id_t next_page_id_;      // 下一个全新页号；不变量：等于介质字节大小 / PAGE_SIZE
    // 待复用的空闲页号（LIFO 栈，AllocatePage 取栈顶）。元素互不重复，且在 fbit_
    // 中对应位为 1；仅在 DeallocatePage 入栈、AllocatePage 出栈。
    std::vector<page_id_t> free_pages_;

    // 缓存文件字节大小，避免每次 I/O 都做 seek 探测（原实现每次读页额外 3 次 seek）。
    // 只在构造与扩展文件时更新。
    long long file_size_ = 0;

    // 物理磁盘 I/O 计数：仅在真正触达磁盘的读/写处累加。
    long long io_read_count_ = 0;   // 物理读页次数（页面从磁盘加载）
    long long io_write_count_ = 0;  // 物理写页次数（页面写回磁盘）

    // T4 诊断：CRC 校验失败累计次数（见 GetCrcErrorCount）。
    long long crc_error_count_ = 0;

    // ---- 空闲页持久化（<db>.fpl 位图，见 LoadFreePageBitmap 注释）----
    // <db>.fpl 布局：24 字节头部 + 页位图；位 i = 1 表示 page i 空闲可复用。
    static constexpr uint32_t kFplMagic = 0x46504C31u;  // "FPL1"
    static constexpr uint32_t kFplVersion = 1u;         // 布局版本；不匹配即丢弃位图
    static constexpr size_t kFplHeader = 24;  // u32 magic + u32 version + u64 pages + u64 size
    static constexpr size_t kFplMaxLoadBytes = 1u << 22;  // 位图装载上限（4M bit ≈ 32K 页），防损坏头 OOM

    std::string fpl_path_;              // <db>.fpl
    std::FILE* fpl_ = nullptr;          // 位图文件句柄（惰性创建）
    std::vector<uint8_t> fbit_;         // 位图的内存副本（位 i = page i 空闲）
    size_t fbit_bytes_ = 0;             // fbit_ 有效字节数

    // 确保底层文件大小足以容纳page_id对应的页，不足则扩展文件
    // @param page_id 需要写入的页号；< 0 或介质不可用时直接返回（不扩展）。
    // @note 扩展后同步更新 file_size_ 缓存；仅扩容、不清零/不写页内容。
    void EnsureFileCapacity(page_id_t page_id);

    // 启动时若存在 <db>.fpl 且格式/页数一致则装载空闲列表；否则视为空
    // （旧库向后兼容：无 .fpl 时行为与原先完全一致）。
    // @note 一致性判据：文件长度 ≥ 24、magic/version 匹配、头部 pages 字段等于刚从
    //       数据文件推导出的 next_page_id_、位图字节数不超过 kFplMaxLoadBytes 且未越界；
    //       任一不满足即整份丢弃（安全优先：宁可损失空间复用，绝不误复用活页）。
    // @note 成功后 fpl_ 复用已打开的句柄，free_pages_ 按位重建（位 1 且页号 <
    //       next_page_id_）。不抛异常。旧库向后兼容：无 .fpl 时行为与原先完全一致。
    void LoadFreePageBitmap();
    // 惰性创建 .fpl 文件（首次 DeallocatePage 时才落盘，避免无回收场景产生多余文件）
    // @note 以 "w+b" 截断新建，清空 fbit_/fbit_bytes_；创建失败则静默降级为
    //       「不持久化空闲列表」（仅会话内复用），fpl_ 保持 nullptr，下次仍会重试。
    void EnsureFplFile();
    // 将位图扩展至至少 need_bytes 字节（不足补零，0 = 已分配）
    // @param need_bytes 需要的位图字节数；fpl_ 为空或已足够时直接返回。
    // @note 通过「写最后一个字节」把文件扩到 24 + need_bytes，再用 0 覆盖该字节
    //       （写穿缓存不保证为零，故必须显式写一次）；同时按需 resize fbit_。
    void EnsureBitCapacity(size_t need_bytes);
    // 把 fbit_[byte_index] 写回文件并持久化
    // @param byte_index 位图字节下标；越界（≥ fbit_bytes_）或 fpl_ 为空时返回。
    // @note 写单字节后 fflush + DurableSync，保证崩溃后该位不丢失（复用页前必须落地）。
    void FlushBitByte(size_t byte_index);
    // 回写 24 字节头部（pages = next_page_id_，size = fbit_bytes_）并持久化
    // @note 头部字段用于下次启动时的一致性校验；fpl_ 为空时无操作，不抛异常。
    void FlushFplHeader();
    // 将某个页置为空闲(1)/已分配(0)，持久化后再返回（分配复用前必须先置 0 防复用活页）
    // @param page_id 目标页号；< 0 时忽略。
    // @param free    true = 置为空闲位 1；false = 置为已分配位 0。
    // @note 副作用：按需创建 .fpl、扩展位图区，并依次回写该字节与头部（均同步落盘）。
    void SetPageFree(page_id_t page_id, bool free);
    // 析构前收尾：把位图对齐到 next_page_id_ 并写头，保证优雅重启后可装载
    // @note 把位图补齐到 ceil(next_page_id_ / 8) 字节（新页默认位 0 = 已分配），
    //       回写头部后 fflush + DurableSync + fclose；写盘失败不抛出。
    void FinalizeFreePageBitmap();

    // ---- 页 CRC32 校验（<db>.crc 旁路，见 LoadPageCrcs 注释）----
    // 布局：无头，每页 4 字节小端 u32（该页整页 PAGE_SIZE 的 CRC32）；0 = 无记录。
    // 向后兼容：旧库无 .crc → pcrc_ 为空 → GetPageCrc 返回 false → 跳过校验，行为不变。
    std::string crc_path_;              // <db>.crc
    std::FILE* crc_ = nullptr;          // CRC 文件句柄（惰性创建）
    std::vector<uint32_t> pcrc_;        // 内存缓存：pcrc_[page_id]；0 = 无记录

    // 启动时装载既有 <db>.crc（缺文件/损坏按空处理，向后兼容）。
    // @note 每页 4 字节，读取条数取 min(文件长度 / 4, 2^20) 防异常超大文件 OOM；
    //       仅当整份完整读入才接受，否则清空并关闭（避免半写文件导致错误校验值）。
    // @note 成功后 crc_ 复用已打开的句柄；不抛异常。
    void LoadPageCrcs();
    // 惰性创建 .crc 并保证至少容纳 need_count 个页的 CRC（不足用 0 补齐）。
    // @param need_count 需要的 CRC 条目数（页数）；为 0 时直接返回。
    // @note 创建失败则静默降级为「无 CRC」（等同旧版行为）；扩展时同样用「写最后一个
    //       字节」把文件撑到 need_count * 4 字节，保证后续可直接按偏移定位写。
    void EnsureCrcFile(size_t need_count);
    // 记录某页 CRC 并回写（fflush 到 OS 缓存即可，不逐页 fsync，避免破坏组提交）。
    // @param page_id 目标页号；< 0 时忽略。
    // @param data    长度为 PAGE_SIZE 的页镜像，非空（为空会导致越界读）。
    // @note 写入偏移 page_id * 4；crc_ 为空（创建失败）时静默跳过。
    void SetPageCrc(page_id_t page_id, const char* data);
    // 清空某页 CRC（回收页复用前调用，防残留旧 CRC 造成误报）。
    // @param page_id 目标页号；< 0、句柄为空、无记录（pcrc_ 中为 0）时均直接返回。
    void ClearPageCrc(page_id_t page_id);
    // 读取某页已记录的 CRC；不存在（未分配/旧页）返回 false。
    // @param page_id 页号；< 0 时返回 false。
    // @param out     输出参数，非空指针；仅返回 true 时被写入。
    // @return true  —— pcrc_ 中存在该页且值非 0（0 表示无记录）；
    //         false —— 指针为空 / 页号非法 / 下标越界 / 记录为 0。
    bool GetPageCrc(page_id_t page_id, uint32_t* out) const;
    // 把 pcrc_ 全部按页刷写进文件（不 fsync，配合 Sync 统一落盘）。
    // @note 当前实现只做 fflush（pcrc_ 各项在 SetPageCrc 时已即时写文件）；句柄为空或
    //       缓存为空时无操作。
    void FlushCrcFile();
    // 析构收尾：FlushCrcFile + 关文件。
    // @note 落盘用 DurableSync（fflush + _commit / fsync）；失败不抛出。
    void FinalizePageCrcs();
};

}  // namespace sqlcompiler
