#pragma once

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
    virtual ~BlockDevice() = default;

    virtual std::string Name() const = 0;   // 介质名（用于诊断 / 故障报告）
    virtual bool IsReady() const = 0;       // 底层介质是否打开可用
    virtual long long Size() const = 0;     // 当前介质字节大小
    virtual size_t Read(long long offset, char* buf, size_t len) = 0;
    virtual size_t Write(long long offset, const char* buf, size_t len) = 0;
    virtual void EnsureCapacity(long long byte_count) = 0;
    virtual void Sync() = 0;
};

// 基于真实文件的块设备实现：内部用 C 流 FILE*（Windows 下可用 _commit 做真持久化）。
class FileBlockDevice : public BlockDevice {
public:
    explicit FileBlockDevice(const std::string& path);
    ~FileBlockDevice() override;

    std::string Name() const override { return path_; }
    bool IsReady() const override { return f_ != nullptr; }
    long long Size() const override;
    size_t Read(long long offset, char* buf, size_t len) override;
    size_t Write(long long offset, const char* buf, size_t len) override;
    void EnsureCapacity(long long byte_count) override;
    void Sync() override;

private:
    std::string path_;
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
    explicit FaultInjectingBlockDevice(std::unique_ptr<BlockDevice> inner)
        : inner_(std::move(inner)) {}

    std::string Name() const override {
        return inner_->Name() + "(faulty)";
    }
    bool IsReady() const override { return inner_->IsReady(); }
    long long Size() const override { return inner_->Size(); }

    size_t Read(long long offset, char* buf, size_t len) override;
    size_t Write(long long offset, const char* buf, size_t len) override;
    void EnsureCapacity(long long byte_count) override { inner_->EnsureCapacity(byte_count); }
    void Sync() override { inner_->Sync(); }

    void FailNextRead() { fail_read_once_ = true; }
    void FailNextWrite() { fail_write_once_ = true; }
    // 使 [from, from + len) 成为「坏块」：写入重叠区域后会用 0xFF 覆写。
    void CorruptWritesForRange(long long from, long long len);
    void ClearFaults() {
        fail_read_once_ = false;
        fail_write_once_ = false;
        corrupt_from_ = -1;
        corrupt_len_ = 0;
    }

private:
    std::unique_ptr<BlockDevice> inner_;
    bool fail_read_once_ = false;
    bool fail_write_once_ = false;
    long long corrupt_from_ = -1;  // -1 = 未配置坏块区域
    long long corrupt_len_ = 0;
};

// ---- T4 介质扩展：三种可交换块设备（复用 BlockDevice 抽象，交换 FileBlockDevice
//      零业务改动——DiskManager 只依赖本接口，测试可直接注入任一种） ----

// 内存块设备（T4）：以 std::vector<char> 为介质，读写全部驻留内存，Sync 为空操作
// （内存介质无需落盘）。演示「介质可交换」的极端形态：数据文件不落任何持久介质，
// 进程退出即失；用于性能对比（无磁盘 I/O 开销）与故障注入测试的干净底座。
class MemoryBlockDevice : public BlockDevice {
public:
    MemoryBlockDevice() = default;

    std::string Name() const override { return "memory"; }
    bool IsReady() const override { return true; }
    long long Size() const override { return static_cast<long long>(data_.size()); }
    size_t Read(long long offset, char* buf, size_t len) override;
    size_t Write(long long offset, const char* buf, size_t len) override;
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
    explicit SparseFileBlockDevice(const std::string& path);
    ~SparseFileBlockDevice() override;

    std::string Name() const override { return path_ + "(sparse)"; }
    bool IsReady() const override { return f_ != nullptr; }
    long long Size() const override { return logical_size_; }
    size_t Read(long long offset, char* buf, size_t len) override;
    size_t Write(long long offset, const char* buf, size_t len) override;
    void EnsureCapacity(long long byte_count) override;
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
    LoopbackNetworkBlockDevice();
    ~LoopbackNetworkBlockDevice() override;

    std::string Name() const override;
    bool IsReady() const override { return ready_; }
    long long Size() const override;
    size_t Read(long long offset, char* buf, size_t len) override;
    size_t Write(long long offset, const char* buf, size_t len) override;
    void EnsureCapacity(long long byte_count) override;
    void Sync() override;

    // 观测：随机分配的监听端口（0 = 未就绪）。
    int GetPort() const { return port_; }
    // 观测：TCP 请求计数（读 / 写）。
    long long GetReadRequests() const;
    long long GetWriteRequests() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int port_ = 0;
    bool ready_ = false;

    // 服务端主循环（静态成员：需访问私有 Impl，见 .cpp）。
    static void ServerLoop(Impl* impl);
};

}  // namespace sqlcompiler