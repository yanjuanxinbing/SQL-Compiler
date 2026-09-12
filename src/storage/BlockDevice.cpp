#include "storage/BlockDevice.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <io.h>  // _fileno, _commit
#else
#include <unistd.h>  // fileno, fsync
#endif

namespace sqlcompiler {

namespace {

// 将 FILE* 的内容真正落盘（durable），而不仅是刷到 OS 缓冲。
//   Windows: _commit(_fileno(f))  ~= FlushFileBuffers
//   POSIX :  fsync(fileno(f))
void DurableSync(std::FILE* f) {
    if (f == nullptr) return;
    std::fflush(f);
#ifdef _WIN32
    ::_commit(::_fileno(f));
#else
    ::fsync(::fileno(f));
#endif
}

// 返回当前文件字节大小；失败返回 0
// 64 位文件定位（Windows: _fseeki64/_ftelli64；POSIX: fseeko/ftello）。
// 数据文件偏移 = page_id * PAGE_SIZE，DB 超 2GB（约 52 万页）后 32 位 long 会被
// 截断回绕，读写错位；必须 64 位定位。
int FSeek64(std::FILE* f, long long off, int whence) {
#ifdef _WIN32
    return ::_fseeki64(f, off, whence);
#else
    return ::fseeko(f, static_cast<off_t>(off), whence);
#endif
}
long long FTell64(std::FILE* f) {
#ifdef _WIN32
    return ::_ftelli64(f);
#else
    return static_cast<long long>(::ftello(f));
#endif
}

long long TellFileSize(std::FILE* f) {
    if (f == nullptr) return 0;
    long long cur = FTell64(f);
    if (FSeek64(f, 0, SEEK_END) != 0) return 0;
    long long end = FTell64(f);
    if (cur >= 0) FSeek64(f, cur, SEEK_SET);
    return end < 0 ? 0 : end;
}

}  // namespace

FileBlockDevice::FileBlockDevice(const std::string& path) : path_(path) {
    // 先尝试读写打开；若文件不存在则创建（w+b 会截断，仅在不存在的分支走到）。
    f_ = std::fopen(path_.c_str(), "r+b");
    if (f_ == nullptr) {
        f_ = std::fopen(path_.c_str(), "w+b");
    }
    // 无法打开/创建介质：保持 f_ 为空，IsReady() 返回 false，由上层决定降级策略。
}

FileBlockDevice::~FileBlockDevice() {
    if (f_ != nullptr) {
        std::fflush(f_);
        std::fclose(f_);
        f_ = nullptr;
    }
}

long long FileBlockDevice::Size() const {
    return TellFileSize(f_);
}

size_t FileBlockDevice::Read(long long offset, char* buf, size_t len) {
    if (f_ == nullptr) return 0;
    if (buf == nullptr || len == 0) return 0;
    FSeek64(f_, offset, SEEK_SET);
    size_t got = std::fread(buf, 1, len, f_);
    if (got < len && std::ferror(f_)) {
        std::clearerr(f_);
        throw std::runtime_error("FileBlockDevice::Read failed: " + path_);
    }
    return got;
}

size_t FileBlockDevice::Write(long long offset, const char* buf, size_t len) {
    if (f_ == nullptr) return 0;
    if (buf == nullptr || len == 0) return 0;
    FSeek64(f_, offset, SEEK_SET);
    size_t written = std::fwrite(buf, 1, len, f_);
    std::fflush(f_);  // 仅到 OS 缓存，不逐页 fsync（落盘由 Sync 统一负责）
    if (written != len || std::ferror(f_)) {
        std::clearerr(f_);
        throw std::runtime_error("FileBlockDevice::Write failed: " + path_);
    }
    return written;
}

void FileBlockDevice::EnsureCapacity(long long byte_count) {
    if (f_ == nullptr || byte_count <= 0) return;
    long long cur = TellFileSize(f_);
    if (cur >= byte_count) return;
    FSeek64(f_, byte_count - 1, SEEK_SET);
    const char zero = 0;
    if (std::fwrite(&zero, 1, 1, f_) != 1) {
        std::clearerr(f_);
        throw std::runtime_error("FileBlockDevice::EnsureCapacity failed: " + path_);
    }
    std::fflush(f_);
}

void FileBlockDevice::Sync() {
    DurableSync(f_);
}

// ---- 坏块注入装饰器 ----

size_t FaultInjectingBlockDevice::Read(long long offset, char* buf, size_t len) {
    if (fail_read_once_) {
        fail_read_once_ = false;
        throw std::runtime_error(
            "FaultInjectingBlockDevice: injected read failure: " + inner_->Name());
    }
    return inner_->Read(offset, buf, len);
}

size_t FaultInjectingBlockDevice::Write(long long offset, const char* buf,
                                        size_t len) {
    if (fail_write_once_) {
        fail_write_once_ = false;
        throw std::runtime_error(
            "FaultInjectingBlockDevice: injected write failure: " + inner_->Name());
    }
    size_t n = inner_->Write(offset, buf, len);
    // 若本次写入与配置的坏块区域重叠，则在成功写入后用 0xFF 覆写重叠部分，
    // 模拟坏扇区 / 静默损坏（之后读回会被上层 CRC 校验检测到）。
    if (n > 0 && corrupt_from_ >= 0) {
        long long lo = std::max<long long>(corrupt_from_, offset);
        long long hi = std::min<long long>(corrupt_from_ + corrupt_len_,
                                           offset + static_cast<long long>(n));
        if (hi > lo) {
            std::vector<char> bad(static_cast<size_t>(hi - lo),
                                  static_cast<char>(0xFF));
            // 覆写坏块区域（内部再出现介质错误则抛上）。
            inner_->Write(lo, bad.data(), bad.size());
        }
    }
    return n;
}

void FaultInjectingBlockDevice::CorruptWritesForRange(long long from, long long len) {
    if (len == 0) {
        corrupt_from_ = -1;
        corrupt_len_ = 0;
        return;
    }
    corrupt_from_ = from;
    corrupt_len_ = len;
}

}  // namespace sqlcompiler