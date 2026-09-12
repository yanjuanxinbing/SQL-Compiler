#pragma once

#include <cstdio>
#include <memory>
#include <string>
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

}  // namespace sqlcompiler