// 回环网络块设备（T4）：进程内 TCP 服务端 + 回环客户端。
//
// 设计要点：
//   * 复用 BlockDevice 抽象：本类与 FileBlockDevice / MemoryBlockDevice /
//     SparseFileBlockDevice 平级，DiskManager 不感知介质差异（「交换零业务改动」）。
//   * 进程内自建服务端（监听 127.0.0.1 随机端口，后台线程 accept + 处理），
//     客户端套接字直连回环地址；所有数据实际落在服务端内存（vector<char>）里。
//   * 协议：简单长度前缀帧——
//       请求 = op(1B) + offset u64 LE(8B) + len u64 LE(8B) [+ 写负载]
//       响应 = status(1B) + len u64 LE(8B) [+ 读负载 / Size 的 8B 值]
//     op: 1=读 2=写 3=EnsureCapacity 4=Sync 5=Size
//   * 由于 DiskManager 以 db_io_latch_ 串行化所有设备调用，客户端请求天然单线程，
//     服务端存储另加 mutex 保护（防御直接设备级并发使用）。
//
// 说明：真实远程网络块设备（iSCSI/NVMe-oF 等）的协议与可靠性远复杂于此；本实现
// 只演示「网络介质」的接入方式与回环路径读写正确性。

#include "storage/BlockDevice.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(_MSC_VER)
// 仅 MSVC 需要该链接指令；MinGW 由 CMake 的 target_link_libraries 提供 ws2_32。
#pragma comment(lib, "ws2_32.lib")
#endif
using SockHandle = SOCKET;
constexpr SockHandle kInvalidSock = INVALID_SOCKET;
using SockLenT = int;
inline int SockClose(SockHandle s) { return ::closesocket(s); }
inline int SockShutdown(SockHandle s, int how) { return ::shutdown(s, how); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SockHandle = int;
constexpr SockHandle kInvalidSock = -1;
using SockLenT = socklen_t;
inline int SockClose(SockHandle s) { return ::close(s); }
inline int SockShutdown(SockHandle s, int how) { return ::shutdown(s, how); }
#endif

namespace sqlcompiler {

// 服务端/客户端套接字与内存介质（PIMPL 实现体）。
// 必须在 ServerLoop 之前完整定义（ServerLoop 以 Impl* 访问其成员）。
struct LoopbackNetworkBlockDevice::Impl {
    SockHandle listen_ = kInvalidSock;  // 服务端监听套接字（服务线程关闭）
    SockHandle conn_ = kInvalidSock;    // 客户端套接字（本设备持有）
    SockHandle peer_ = kInvalidSock;    // 服务端 accept 的连接（服务线程关闭）
    std::thread server_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;       // 保护 data_
    std::vector<char> data_;  // 服务端内存介质
    std::atomic<long long> read_req_{0};
    std::atomic<long long> write_req_{0};
#ifdef _WIN32
    bool ws_started_ = false;  // WSAStartup 是否成功（析构时对应 WSACleanup）
#endif
};

namespace {

inline void PutU64(char* p, uint64_t v) { std::memcpy(p, &v, 8); }
inline uint64_t GetU64(const char* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

// 全量发送（处理部分写）；失败返回 false。
bool SendAll(SockHandle s, const char* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const int n = ::send(s, buf + sent, static_cast<int>(len - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// 全量接收（处理部分读）；失败返回 false。
bool RecvAll(SockHandle s, char* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        const int n = ::recv(s, buf + got, static_cast<int>(len - got), 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

// 服务端主循环：select 监听（100ms 超时响应 stop）→ accept → 处理请求直至断开。
// 实现为类的静态成员函数，以访问私有 Impl（PIMPL 类型）。
void LoopbackNetworkBlockDevice::ServerLoop(Impl* impl) {
    while (!impl->stop_.load(std::memory_order_acquire)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(impl->listen_, &rfds);
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  // 100ms：让 stop 置位后至多 100ms 内退出
        const int sel =
            ::select(static_cast<int>(impl->listen_) + 1, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;  // 超时/被打断：回循环检查 stop
        sockaddr_in caddr;
        SockLenT clen = sizeof(caddr);
        SockHandle c = ::accept(impl->listen_, reinterpret_cast<sockaddr*>(&caddr), &clen);
        if (c == kInvalidSock) continue;
        impl->peer_ = c;
        // 响应帧固定 9 字节：status(1B) + 长度或值 u64 LE(8B)，其后可选跟负载。
        char resp[9];
        while (!impl->stop_.load(std::memory_order_acquire)) {
            // 请求帧固定 17 字节：op(1B) + offset u64 LE(8B) + len u64 LE(8B)。
            char hdr[17];
            if (!RecvAll(c, hdr, sizeof(hdr))) break;  // 对端关闭/出错
            const uint8_t op = static_cast<uint8_t>(hdr[0]);
            const long long off = static_cast<long long>(GetU64(hdr + 1));
            const long long len = static_cast<long long>(GetU64(hdr + 9));
            resp[0] = 0;  // 默认成功状态；仅「未知 opcode」分支改写为 1
            if (op == 1) {  // 读：返回可用字节（介质末尾不足）
                std::vector<char> payload;
                {
                    std::lock_guard<std::mutex> lock(impl->mu_);
                    long long n = 0;
                    if (off >= 0 && off < static_cast<long long>(impl->data_.size())) {
                        n = std::min<long long>(
                            len, static_cast<long long>(impl->data_.size()) - off);
                    }
                    payload.assign(static_cast<size_t>(n), 0);
                    if (n > 0) {
                        std::memcpy(payload.data(), impl->data_.data() + off,
                                    static_cast<size_t>(n));
                    }
                }
                PutU64(resp + 1, static_cast<uint64_t>(payload.size()));
                if (!SendAll(c, resp, 9)) break;
                if (!payload.empty() && !SendAll(c, payload.data(), payload.size())) break;
            } else if (op == 2) {  // 写：扩展介质并写入
                std::vector<char> payload(static_cast<size_t>(len));
                if (!RecvAll(c, payload.data(), payload.size())) break;
                {
                    std::lock_guard<std::mutex> lock(impl->mu_);
                    if (off + len > static_cast<long long>(impl->data_.size())) {
                        impl->data_.resize(static_cast<size_t>(off + len), 0);
                    }
                    if (len > 0) {
                        std::memcpy(impl->data_.data() + off, payload.data(),
                                    static_cast<size_t>(len));
                    }
                }
                PutU64(resp + 1, static_cast<uint64_t>(len));
                if (!SendAll(c, resp, 9)) break;
            } else if (op == 3) {  // EnsureCapacity：复用 offset 字段承载目标字节数
                {
                    std::lock_guard<std::mutex> lock(impl->mu_);
                    if (off > static_cast<long long>(impl->data_.size())) {
                        impl->data_.resize(static_cast<size_t>(off), 0);
                    }
                }
                PutU64(resp + 1, 0);
                if (!SendAll(c, resp, 9)) break;
            } else if (op == 4) {  // Sync：内存介质无需落盘
                PutU64(resp + 1, 0);
                if (!SendAll(c, resp, 9)) break;
            } else if (op == 5) {  // Size：返回逻辑大小
                uint64_t sz;
                {
                    std::lock_guard<std::mutex> lock(impl->mu_);
                    sz = static_cast<uint64_t>(impl->data_.size());
                }
                // Size 的响应额外跟 8 字节负载（Read 的响应则在固定 9 字节之后跟读到的数据）。
                PutU64(resp + 1, 8);
                if (!SendAll(c, resp, 9)) break;
                char szb[8];
                PutU64(szb, sz);
                if (!SendAll(c, szb, 8)) break;
            } else {  // 未知 opcode
                resp[0] = 1;
                PutU64(resp + 1, 0);
                if (!SendAll(c, resp, 9)) break;
            }
        }
        SockClose(c);
        impl->peer_ = kInvalidSock;
    }
    SockClose(impl->listen_);
    impl->listen_ = kInvalidSock;
}

LoopbackNetworkBlockDevice::LoopbackNetworkBlockDevice() {
    impl_ = std::make_unique<Impl>();
#ifdef _WIN32
    WSADATA wsa;
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;  // ready_ 保持 false
    impl_->ws_started_ = true;
#endif
    impl_->listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->listen_ == kInvalidSock) return;
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1
    addr.sin_port = 0;                              // 随机端口
    if (::bind(impl_->listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        return;
    if (::listen(impl_->listen_, 4) != 0) return;
    SockLenT alen = sizeof(addr);
    if (::getsockname(impl_->listen_, reinterpret_cast<sockaddr*>(&addr), &alen) != 0)
        return;
    port_ = ntohs(addr.sin_port);
    // 启动服务端线程后再建客户端连接：connect 与 accept 之间由内核缓冲，无竞态。
    impl_->server_ = std::thread(&LoopbackNetworkBlockDevice::ServerLoop, impl_.get());
    impl_->conn_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->conn_ == kInvalidSock) return;
    if (::connect(impl_->conn_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        return;
    ready_ = true;
}

// 析构幂等：无论构造是否完整，都按「已就绪」路径收尾；未创建的套接字/线程自动跳过。
LoopbackNetworkBlockDevice::~LoopbackNetworkBlockDevice() {
    if (!impl_) return;
    impl_->stop_.store(true, std::memory_order_release);
    // 断开连接让服务端 RecvAll 立即返回 0 退出（select 100ms 超时兜底）。
    // shutdown 的 how=2 在 Windows 为 SD_BOTH、POSIX 为 SHUT_RDWR：收发双向都停，
    // 使双方正在阻塞的 recv 立刻返回，避免只靠 100ms 轮询等待。
    if (impl_->peer_ != kInvalidSock) SockShutdown(impl_->peer_, 2);
    if (impl_->conn_ != kInvalidSock) SockShutdown(impl_->conn_, 2);
    if (impl_->server_.joinable()) impl_->server_.join();
    if (impl_->conn_ != kInvalidSock) {
        SockClose(impl_->conn_);
        impl_->conn_ = kInvalidSock;
    }
    if (impl_->listen_ != kInvalidSock) {
        SockClose(impl_->listen_);  // 服务线程正常退出时已置 kInvalidSock，此处幂等
        impl_->listen_ = kInvalidSock;
    }
#ifdef _WIN32
    if (impl_->ws_started_) ::WSACleanup();
#endif
}

std::string LoopbackNetworkBlockDevice::Name() const {
    return "loopback-tcp://127.0.0.1:" + std::to_string(port_);
}

long long LoopbackNetworkBlockDevice::Size() const {
    if (!ready_ || impl_->conn_ == kInvalidSock) return 0;
    char hdr[17];
    hdr[0] = 5;  // op 5 = Size；请求帧另两个字段无意义，置 0
    PutU64(hdr + 1, 0);
    PutU64(hdr + 9, 0);
    if (!SendAll(impl_->conn_, hdr, sizeof(hdr))) return 0;
    char resp[9];
    if (!RecvAll(impl_->conn_, resp, sizeof(resp))) return 0;
    char szb[8];  // 固定 9 字节响应之后跟 8 字节大小值
    if (!RecvAll(impl_->conn_, szb, sizeof(szb))) return 0;
    return static_cast<long long>(GetU64(szb));
}

size_t LoopbackNetworkBlockDevice::Read(long long offset, char* buf, size_t len) {
    if (!ready_ || impl_->conn_ == kInvalidSock || offset < 0 || buf == nullptr ||
        len == 0)
        return 0;
    char hdr[17];
    hdr[0] = 1;  // op 1 = Read
    PutU64(hdr + 1, static_cast<uint64_t>(offset));
    PutU64(hdr + 9, static_cast<uint64_t>(len));
    if (!SendAll(impl_->conn_, hdr, sizeof(hdr))) {
        throw std::runtime_error("LoopbackNetworkBlockDevice::Read: send failed");
    }
    char resp[9];
    if (!RecvAll(impl_->conn_, resp, sizeof(resp))) {
        throw std::runtime_error("LoopbackNetworkBlockDevice::Read: recv failed");
    }
    if (resp[0] != 0) {
        throw std::runtime_error("LoopbackNetworkBlockDevice::Read: server error");
    }
    const size_t n = static_cast<size_t>(GetU64(resp + 1));
    std::memset(buf, 0, len);  // 介质末尾不足部分补零（与 FileBlockDevice 语义一致）
    if (n > 0 && !RecvAll(impl_->conn_, buf, n)) {
        throw std::runtime_error("LoopbackNetworkBlockDevice::Read: payload recv failed");
    }
    ++impl_->read_req_;
    return n;
}

size_t LoopbackNetworkBlockDevice::Write(long long offset, const char* buf, size_t len) {
    if (!ready_ || impl_->conn_ == kInvalidSock || offset < 0 || buf == nullptr ||
        len == 0)
        return 0;
    char hdr[17];
    hdr[0] = 2;  // op 2 = Write（头部之后紧跟 len 字节负载）
    PutU64(hdr + 1, static_cast<uint64_t>(offset));
    PutU64(hdr + 9, static_cast<uint64_t>(len));
    if (!SendAll(impl_->conn_, hdr, sizeof(hdr)) || !SendAll(impl_->conn_, buf, len)) {
        throw std::runtime_error("LoopbackNetworkBlockDevice::Write: send failed");
    }
    char resp[9];
    if (!RecvAll(impl_->conn_, resp, sizeof(resp))) {
        throw std::runtime_error("LoopbackNetworkBlockDevice::Write: recv failed");
    }
    if (resp[0] != 0) {
        throw std::runtime_error("LoopbackNetworkBlockDevice::Write: server error");
    }
    ++impl_->write_req_;
    return len;
}

void LoopbackNetworkBlockDevice::EnsureCapacity(long long byte_count) {
    if (!ready_ || impl_->conn_ == kInvalidSock || byte_count <= 0) return;
    char hdr[17];
    hdr[0] = 3;  // op 3 = EnsureCapacity（offset 字段承载目标字节数）
    PutU64(hdr + 1, static_cast<uint64_t>(byte_count));
    PutU64(hdr + 9, 0);
    if (!SendAll(impl_->conn_, hdr, sizeof(hdr))) {
        throw std::runtime_error("LoopbackNetworkBlockDevice::EnsureCapacity: send failed");
    }
    char resp[9];
    if (!RecvAll(impl_->conn_, resp, sizeof(resp))) {
        throw std::runtime_error("LoopbackNetworkBlockDevice::EnsureCapacity: recv failed");
    }
    if (resp[0] != 0) {
        throw std::runtime_error("LoopbackNetworkBlockDevice::EnsureCapacity: server error");
    }
}

void LoopbackNetworkBlockDevice::Sync() {
    if (!ready_ || impl_->conn_ == kInvalidSock) return;
    char hdr[17];
    hdr[0] = 4;  // op 4 = Sync
    PutU64(hdr + 1, 0);
    PutU64(hdr + 9, 0);
    if (!SendAll(impl_->conn_, hdr, sizeof(hdr))) {
        throw std::runtime_error("LoopbackNetworkBlockDevice::Sync: send failed");
    }
    char resp[9];
    if (!RecvAll(impl_->conn_, resp, sizeof(resp))) {
        throw std::runtime_error("LoopbackNetworkBlockDevice::Sync: recv failed");
    }
}

long long LoopbackNetworkBlockDevice::GetReadRequests() const {
    return impl_ ? impl_->read_req_.load() : 0;
}

long long LoopbackNetworkBlockDevice::GetWriteRequests() const {
    return impl_ ? impl_->write_req_.load() : 0;
}

}  // namespace sqlcompiler
