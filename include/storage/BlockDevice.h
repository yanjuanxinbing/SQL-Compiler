#pragma once

// =============================================================================
// BlockDevice — 底层持久化介质抽象（E7）
//
// 职责：把「原始字节读写 + 扩容 + 落盘」抽象成一个接口，让 DiskManager 只依赖介质
//       能力、不感知介质形态；从而可以零业务改动地交换实现，并用装饰器注入故障。
//
// 关键设计决策：
//   * 接口只暴露偏移 + 字节数的无结构读写，页语义（page_id * PAGE_SIZE）由
//     DiskManager 负责，介质层不解释内容；
//   * 故障注入用装饰器而非分支开关：FaultInjectingBlockDevice 包住任意实现，
//     未配置故障时逐字节透传，对被测代码完全透明；
//   * 具体实现覆盖四种介质形态：真实文件（FileBlockDevice）、纯内存（MemoryBlockDevice）、
//     稀疏文件（SparseFileBlockDevice）、回环网络（LoopbackNetworkBlockDevice）。
//
// 必须维持的不变量与约束：
//   * 所有实现的 Size() 返回「逻辑大小」，读越界（offset ≥ 逻辑大小）一律返回 0
//     而不报错，由调用方按「补零」语义处理；
//   * Read / Write 允许短读短写（返回实际字节数）；介质硬错误才抛异常；
//   * Sync 是实现 durability 的唯一入口（fsync / _commit 等价物），内存介质为空操作；
//   * IsReady() == false 表示介质不可用，调用方需自行降级，不要求实现再抛异常；
//   * 本层不含并发控制（DiskManager 的 db_io_latch_ 已串行化调用），故实现无需线程安全，
//     例外是 LoopbackNetworkBlockDevice 的服务端线程另行加锁保护自己的内存介质。
//
// 跨平台差异：
//   * Windows 用 _fseeki64/_ftelli64/_commit/_get_osfhandle（<io.h>），稀疏文件需
//     FSCTL_SET_SPARSE + SetEndOfFile；
//   * POSIX 用 fseeko/ftello/fsync/ftruncate（<unistd.h>）。
// =============================================================================

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sqlcompiler {

// 块设备抽象层（E7）：把「底层持久化介质」的原始字节读写抽象成一个接口，
// 让 DiskManager 只依赖本接口而非直接封装 FILE*。这样既能：
//   1) 换成不同的介质实现（真实文件、模拟裸盘、内存设备）；
//   2) 通过装饰器做坏块注入 / 断电模拟（贴近课件「模拟裸裸块设备」主题），
//      而无需改动 DiskManager / BufferPoolManager 上层。
//
// 语义约定：
//   * Read / Write 以起始偏移 + 字节数进行，返回实际读/写字节数（可能小于请求；
//     读在介质末尾时返回不足，不报错）。
//   * 介质硬错误（如系统调用失败）应抛 std::runtime_error（由上层统一 I/O 错误通道）。
//   * EnsureCapacity 保证介质能容纳 byte_count 字节（不足时扩展；新增区行为由实现定义，
//     通常为全 0）。
//   * Sync 把已写内容真正落盘（fsync / _commit），实现 durability。
//   * IsReady 表示底层介质句柄是否打开可用；不可用时上层应降级为「无介质」（I/O 空操作或报错）。
class BlockDevice {
public:
    // 虚析构：允许通过基类指针销毁具体设备（DiskManager 持有 unique_ptr<BlockDevice>）。
    virtual ~BlockDevice() = default;

    // 介质名，用于诊断输出（\analyze）与故障报告；装饰器会在内层名后追加标记。
    // @return 人类可读的介质标识，如 "memory"、"<path>(sparse)"、"loopback-tcp://..."。
    virtual std::string Name() const = 0;   // 介质名（用于诊断 / 故障报告）
    // 底层介质是否已打开且可用。
    // @return true  —— 可正常读写；false —— 打开/创建失败，调用方应降级（不要求抛异常）。
    virtual bool IsReady() const = 0;       // 底层介质是否打开可用
    // 当前介质逻辑字节大小。
    // @return 逻辑大小（字节），恒 ≥ 0；内存/稀疏实现按逻辑大小而非物理占用计算。
    // @note 每次调用可能触发一次介质探测（文件实现需 seek 到末尾），故上层应缓存结果。
    virtual long long Size() const = 0;     // 当前介质字节大小
    // 从偏移 offset 读取最多 len 字节。
    // @param offset 起始字节偏移，须 ≥ 0（实现按 < 0 直接返回 0 处理）。
    // @param buf    输出缓冲，须非空且至少 len 字节。
    // @param len    请求字节数；为 0 时直接返回 0。
    // @return 实际读入字节数，可小于 len（读越介质末尾时不报错，调用方自行补零）；
    //         介质未就绪 / 参数非法时返回 0。
    // @note 系统调用层面失败（而非读尽）时抛 std::runtime_error。
    virtual size_t Read(long long offset, char* buf, size_t len) = 0;
    // 把 buf 的 len 字节写入偏移 offset 处。
    // @param offset 起始字节偏移，须 ≥ 0。
    // @param buf    源缓冲，须非空且至少 len 字节。
    // @param len    写入字节数；为 0 时直接返回 0。
    // @return 实际写入字节数；正常实现要么返回 len，要么抛异常；介质未就绪时返回 0。
    // @note 不隐含落盘：内容仅到 OS 缓存，durability 需显式调用 Sync()。
    virtual size_t Write(long long offset, const char* buf, size_t len) = 0;
    // 确保介质能容纳 byte_count 字节（不足时扩展）。
    // @param byte_count 目标逻辑字节数；≤ 0 时无操作。
    // @note 新增区域的内容由实现定义（文件/内存实现为 0，稀疏实现为「洞」读回 0）；
    //       扩展失败抛 std::runtime_error。
    virtual void EnsureCapacity(long long byte_count) = 0;
    // 把已写入内容真正持久化（Windows _commit ≈ FlushFileBuffers；POSIX fsync）。
    // @note 内存介质为空操作；介质未就绪时实现应安全跳过（不抛异常）。
    virtual void Sync() = 0;
};

// 基于真实文件的块设备实现：内部用 C 流 FILE*（Windows 下可用 _commit 做真持久化）。
// 语义：Read/Write 在介质末尾不足时短返回；EnsureCapacity 用「写末尾 1 字节」扩展；
// Sync 走 DurableSync（fflush + _commit / fsync）。方法契约同 BlockDevice 基类。
class FileBlockDevice : public BlockDevice {
public:
    // 打开（必要时创建）path 指向的文件作为介质。
    // @param path 数据文件路径；已存在用 "r+b" 打开，不存在则 "w+b" 新建。
    // @note 两次打开都失败时不抛异常，只保持 f_ 为空（IsReady() 返回 false），
    //       由 DiskManager 决定降级策略。
    explicit FileBlockDevice(const std::string& path);
    // 析构：fflush + fclose 释放句柄（不额外 fsync，持久化由 Sync/上层负责）。
    ~FileBlockDevice() override;

    std::string Name() const override { return path_; }  // 介质名即文件路径
    bool IsReady() const override { return f_ != nullptr; }  // 句柄非空即可用
    // @return 当前文件字节大小；探测失败返回 0（见 .cpp 的 TellFileSize）。
    long long Size() const override;
    // @return 实际读入字节数（可能短读）；介质未就绪或参数非法返回 0；
    //         fread 出错（ferror）时抛 std::runtime_error。
    size_t Read(long long offset, char* buf, size_t len) override;
    // @return 实际写入字节数（正常等于 len）；介质未就绪或参数非法返回 0；
    //         短写或 ferror 时抛 std::runtime_error。
    size_t Write(long long offset, const char* buf, size_t len) override;
    // 扩展文件到 byte_count 字节（不足时写末尾 1 个 0 字节）。
    // @param byte_count 目标字节数；≤ 0 或句柄为空时无操作。
    // @note 写失败抛 std::runtime_error；不清零扩展区的中间内容。
    void EnsureCapacity(long long byte_count) override;
    // 落盘：fflush 后 _commit / fsync；句柄为空时安全跳过。
    void Sync() override;

private:
    std::string path_;        // 介质文件路径；同名用于诊断与错误消息
    std::FILE* f_ = nullptr;  // r+b；不存在则尝试 w+b 创建；均失败则为空
};

// 坏块 / 断电模拟装饰器（E7）：包住另一个 BlockDevice，可按需注入故障。
// 默认行为是「完全透传」，与不套装饰器逐字节一致；注入策略由调用方配置：
//   * FailNextRead / FailNextWrite：下一次读/写抛 std::runtime_error（模拟介质故障，
//     走统一 I/O 错误通道）。
//   * CorruptWritesForRange：凡写入与 [from, from+len) 重叠的字节，在成功写入后再
//     用 0xFF 覆写，模拟坏扇区 / 静默损坏（后续读回其页码会被上层 CRC 校验发现）。
class FaultInjectingBlockDevice : public BlockDevice {
public:
    // @param inner 被装饰的内层设备；所有权转移给本对象，不得为空
    //              （Name/IsReady/Size 等直接解引用 inner_，空指针会崩溃）。
    // @note 构造后默认不注入任何故障，行为与裸设备逐字节一致。
    explicit FaultInjectingBlockDevice(std::unique_ptr<BlockDevice> inner)
        : inner_(std::move(inner)) {}

    std::string Name() const override {
        return inner_->Name() + "(faulty)";  // 名字带标记，便于诊断区分是否套了装饰器
    }
    bool IsReady() const override { return inner_->IsReady(); }
    long long Size() const override { return inner_->Size(); }

    // @return 内层实际读入字节数；若恰有「下一次读失败」待触发，则抛
    //         std::runtime_error 并消费掉该标志（一次性故障）。
    size_t Read(long long offset, char* buf, size_t len) override;
    // 先按「下一次写失败」标志决定是否抛 std::runtime_error；否则透传写入，
    // 若写入区间与坏块区域重叠，再用 0xFF 覆写重叠部分（静默损坏模拟）。
    // @return 内层实际写入字节数（返回值报告的是原始写入量，不含后续覆写）。
    size_t Write(long long offset, const char* buf, size_t len) override;
    void EnsureCapacity(long long byte_count) override { inner_->EnsureCapacity(byte_count); }
    void Sync() override { inner_->Sync(); }

    // 令下一次 Read 抛异常（一次性；触发后标志自动清除）。
    void FailNextRead() { fail_read_once_ = true; }
    // 令下一次 Write 抛异常（一次性；触发后标志自动清除）。
    void FailNextWrite() { fail_write_once_ = true; }
    // 使 [from, from + len) 成为「坏块」：写入重叠区域后会用 0xFF 覆写。
    // @param from 坏块起始偏移（字节），照原样保存，可为任意值。
    // @param len  坏块长度（字节）；传 0 表示取消坏块配置（等价 ClearFaults 的该部分）。
    // @note 覆写发生在内层成功写入之后，因此数据页内容被破坏；上层读回时由
    //       DiskManager 的 CRC32 校验发现。
    void CorruptWritesForRange(long long from, long long len);
    // 清除全部已配置故障（读写一次性标志与坏块区间），恢复纯透传。
    void ClearFaults() {
        fail_read_once_ = false;
        fail_write_once_ = false;
        corrupt_from_ = -1;
        corrupt_len_ = 0;
    }

private:
    std::unique_ptr<BlockDevice> inner_;  // 被装饰设备；本对象独占持有
    bool fail_read_once_ = false;         // true = 下一次 Read 抛异常（消费即清）
    bool fail_write_once_ = false;        // true = 下一次 Write 抛异常（消费即清）
    long long corrupt_from_ = -1;  // -1 = 未配置坏块区域
    long long corrupt_len_ = 0;    // 坏块长度；仅在 corrupt_from_ >= 0 时生效
};

// ---- T4 介质扩展：三种可交换块设备（复用 BlockDevice 抽象，交换 FileBlockDevice
//      零业务改动——DiskManager 只依赖本接口，测试可直接注入任一种） ----

// 内存块设备（T4）：以 std::vector<char> 为介质，读写全部驻留内存，Sync 为空操作
// （内存介质无需落盘）。演示「介质可交换」的极端形态：数据文件不落任何持久介质，
// 进程退出即失；用于性能对比（无磁盘 I/O 开销）与故障注入测试的干净底座。
class MemoryBlockDevice : public BlockDevice {
public:
    // 默认构造：构造即就绪（无外部资源），介质为空（Size() == 0）。
    MemoryBlockDevice() = default;

    std::string Name() const override { return "memory"; }
    bool IsReady() const override { return true; }  // 无外部资源，恒可用
    // @return 已写入并扩容到的逻辑字节数（vector 大小），而非物理占用。
    long long Size() const override { return static_cast<long long>(data_.size()); }
    // @return 实际读入字节数；读越逻辑末尾返回 0 或不足值（参数非法同样返回 0）。
    size_t Read(long long offset, char* buf, size_t len) override;
    // 先按需扩容再写入；写入恒为全量（返回 len），不会失败。
    // @return 写入字节数（等于 len）；参数非法（offset < 0 / buf 空 / len 0）返回 0。
    size_t Write(long long offset, const char* buf, size_t len) override;
    // 按需把 data_ 扩容到 byte_count（新增区为 0）；≤ 0 或已足够时无操作。
    void EnsureCapacity(long long byte_count) override;
    void Sync() override {}  // 内存介质无需落盘

private:
    std::vector<char> data_;  // 逻辑介质内容；未写区域恒为 0
};

// 稀疏文件块设备（T4）：底层是真实文件，但以「稀疏文件」方式管理——逻辑大小可以
// 很大，物理上只为已写入的区域分配簇（Windows FSCTL_SET_SPARSE；POSIX 写越 EOF 的
// 洞天然稀疏）。未分配区（洞）读回 0，无需真实磁盘空间。内部跟踪已分配区间集合，
// 提供 AllocatedBytes() 展示「逻辑 vs 物理占用」的稀疏节省量。
class SparseFileBlockDevice : public BlockDevice {
public:
    // 打开（必要时创建）path 并尝试标记为稀疏文件（Windows 用 FSCTL_SET_SPARSE）。
    // @param path 介质文件路径。
    // @note 打开失败不抛异常，只保持 f_ 为空（IsReady() 返回 false）；稀疏标记失败
    //       亦不影响正确性（退化为普通文件，洞读 0 由读取时的 memset 兜底）。
    explicit SparseFileBlockDevice(const std::string& path);
    // 析构：fflush + fclose（不额外 fsync）。
    ~SparseFileBlockDevice() override;

    std::string Name() const override { return path_ + "(sparse)"; }  // 名字标记为稀疏介质
    bool IsReady() const override { return f_ != nullptr; }
    // @return 逻辑大小 logical_size_（可远大于物理占用）；不探测文件系统实际块数。
    long long Size() const override { return logical_size_; }
    // @return 请求的 n 字节（读洞由 memset 补零，故不短返回）；
    //         参数非法或句柄为空返回 0；fread 出错抛 std::runtime_error。
    size_t Read(long long offset, char* buf, size_t len) override;
    // 写入 [offset, offset+len)；写越 EOF 会把逻辑大小推进到写入末尾（中间留洞）。
    // @return 实际写入字节数（等于 len）；参数非法或句柄为空返回 0；
    //         短写 / ferror 抛 std::runtime_error。
    // @note 同时把该区间并入已分配区间统计（仅观测用）。
    size_t Write(long long offset, const char* buf, size_t len) override;
    // 扩展逻辑大小到 byte_count，不写数据字节（扩展区保持稀疏、不分配簇）。
    // @param byte_count 目标逻辑字节数；≤ 0 或已足够时无操作。
    // @note 扩展失败（SetEndOfFile / ftruncate 报错）时静默保持原大小，不抛异常。
    void EnsureCapacity(long long byte_count) override;
    // 落盘：fflush + _commit / fsync；句柄为空时安全跳过。
    void Sync() override;

    // 观测：物理已分配字节数（稀疏节省 = Size() - AllocatedBytes()）。
    long long AllocatedBytes() const { return allocated_bytes_; }
    // 观测：已分配区间数（诊断：碎片程度）。
    size_t AllocatedRegionCount() const { return regions_.size(); }

private:
    std::string path_;
    std::FILE* f_ = nullptr;            // r+b；不存在则创建
    long long logical_size_ = 0;        // 逻辑大小（可远大于物理占用）
    // 已分配区间集合 [lo, hi)，按 lo 有序且互不重叠（连续写合并）；仅供观测统计，
    // 读/写正确性由稀疏文件本身的语义保证（洞读 0、写越 EOF 自动成洞）。
    std::vector<std::pair<long long, long long>> regions_;
    long long allocated_bytes_ = 0;     // 已分配区间总长（物理占用近似）
    bool sparse_ok_ = false;            // 稀疏标记是否成功（仅 Windows 需显式标记）

    // 把 [lo, hi) 并入已分配区间集合（合并重叠/相邻区间并维护 allocated_bytes_）。
    // @param lo 区间起点（字节，含）。
    // @param hi 区间终点（字节，不含）；hi <= lo 时无操作。
    // @note 纯内存统计，不影响介质内容；O(区间数)。
    void MergeRegion(long long lo, long long hi);
};

// 回环网络块设备（T4）：进程内自建 TCP 服务端（监听 127.0.0.1 随机端口），客户端经
// 回环 TCP 按块设备语义读写——所有数据实际落在服务端内存里。演示「介质可交换」的
// 第三种形态：把 I/O 从「本地文件」抽象成「网络请求」，DiskManager 零业务改动即可
// 切换；也顺带验证回环网络路径的读写正确性。真实远程网络块设备（iSCSI/NVMe-oF）的
// 协议远复杂于本实现；此处用进程内回环 + 简单长度前缀协议演示「网络介质」的接入方式。
// 实现细节（WinSock/POSIX 套接字）在 src/storage/LoopbackNetworkBlockDevice.cpp，
// 头文件用 PIMPL 隐藏，避免公共头引入 <winsock2.h>。
class LoopbackNetworkBlockDevice : public BlockDevice {
public:
    // 构造：WSAStartup（仅 Windows）→ 建监听套接字并 bind 到 127.0.0.1:0（随机端口）
    // → listen → 启动服务端线程 → 建客户端套接字 connect 回环地址。
    // @note 任一步失败都不抛异常，仅 ready_ 保持 false（IsReady() == false）；
    //       此时所有 I/O 方法安全降级（返回 0 / 直接返回）。
    // @note 构造顺序刻意「先起线程、再 connect」：connect 与 accept 之间由内核缓冲，
    //       因此不存在「先连接后监听」的竞态。
    LoopbackNetworkBlockDevice();
    // 析构：置 stop 标志 → shutdown 两端套接字让服务线程的 recv 立即返回 →
    //       join 线程 → 关闭套接字 → WSACleanup。全程幂等，构造不完整也可安全执行。
    ~LoopbackNetworkBlockDevice() override;

    // @return "loopback-tcp://127.0.0.1:<port>"；未就绪时端口为 0。
    std::string Name() const override;
    bool IsReady() const override { return ready_; }  // 构造完整走通才为 true
    // 发一条 Size 请求并等响应。
    // @return 服务端内存介质的逻辑字节数；未就绪或信道失败返回 0（不抛异常）。
    long long Size() const override;
    // 发一条 Read 请求并等响应。
    // @param offset 起始偏移（须 ≥ 0）；@param buf 输出缓冲（须非空）；@param len 字节数。
    // @return 服务端实际返回的字节数；未就绪 / 参数非法返回 0；读负载不足部分已 memset 补零。
    // @note 发送/接收失败或服务端返回错误状态时抛 std::runtime_error（与文件介质一致）。
    // @note 成功后才累加 GetReadRequests() 计数。
    size_t Read(long long offset, char* buf, size_t len) override;
    // 发一条 Write 请求（头部 + 负载）并等响应。
    // @return 写入字节数（等于 len）；未就绪 / 参数非法返回 0；信道或服务端错误抛异常。
    // @note 成功后才累加 GetWriteRequests() 计数。
    size_t Write(long long offset, const char* buf, size_t len) override;
    // 请求服务端把内存介质扩展到 byte_count（≤ 0 或未就绪时无操作）。
    // @note 信道失败或服务端报错时抛 std::runtime_error。
    void EnsureCapacity(long long byte_count) override;
    // 请求服务端 Sync（其内存介质无需落盘，服务端仅回响应）。
    // @note 未就绪时无操作；发送失败抛 std::runtime_error。
    void Sync() override;

    // 观测：随机分配的监听端口（0 = 未就绪）。
    // @return 本设备服务端的监听端口号（主机字节序），仅诊断用。
    int GetPort() const { return port_; }
    // 观测：TCP 请求计数（读 / 写）。
    // @return 成功的 Read 请求累计次数；impl_ 为空（构造失败）时返回 0。
    long long GetReadRequests() const;
    // @return 成功的 Write 请求累计次数；impl_ 为空（构造失败）时返回 0。
    long long GetWriteRequests() const;

private:
    struct Impl;                        // 实现体（套接字/线程/内存介质），定义在 .cpp
    std::unique_ptr<Impl> impl_;        // 独占持有；构造首行创建，析构最后一步释放
    int port_ = 0;                      // 监听端口（0 = 未就绪）
    bool ready_ = false;                // 构造是否完整走通（客户端可用的唯一判据）

    // 服务端主循环（静态成员：需访问私有 Impl，见 .cpp）。
    // @param impl 本对象的实现体，生命周期由构造/析构保证长于该线程；
    //             select 每 100ms 醒来检查 impl->stop_，故停止延迟有界。
    static void ServerLoop(Impl* impl);
};

}  // namespace sqlcompiler