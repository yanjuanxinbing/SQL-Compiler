#include "storage/BlockDevice.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <io.h>       // _fileno, _commit, _get_osfhandle
#include <windows.h>  // FSCTL_SET_SPARSE / SetEndOfFile（稀疏文件）
#else
#include <unistd.h>  // fileno, fsync, ftruncate
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

// ---- T4：内存块设备 ----

size_t MemoryBlockDevice::Read(long long offset, char* buf, size_t len) {
    if (offset < 0 || buf == nullptr || len == 0) return 0;
    const long long avail = Size() - offset;
    if (avail <= 0) return 0;  // 介质末尾：返回不足（上层补零）
    const size_t n = std::min<size_t>(len, static_cast<size_t>(avail));
    std::memcpy(buf, data_.data() + static_cast<size_t>(offset), n);
    return n;
}

size_t MemoryBlockDevice::Write(long long offset, const char* buf, size_t len) {
    if (offset < 0 || buf == nullptr || len == 0) return 0;
    EnsureCapacity(offset + static_cast<long long>(len));
    std::memcpy(data_.data() + static_cast<size_t>(offset), buf, len);
    return len;
}

void MemoryBlockDevice::EnsureCapacity(long long byte_count) {
    if (byte_count <= 0) return;
    if (static_cast<long long>(data_.size()) >= byte_count) return;
    data_.resize(static_cast<size_t>(byte_count), 0);  // 新增区恒为 0
}

// ---- T4：稀疏文件块设备 ----

SparseFileBlockDevice::SparseFileBlockDevice(const std::string& path) : path_(path) {
    f_ = std::fopen(path_.c_str(), "r+b");
    if (f_ == nullptr) {
        f_ = std::fopen(path_.c_str(), "w+b");
    }
    if (f_ == nullptr) return;
#ifdef _WIN32
    // 显式标记为稀疏文件（FSCTL_SET_SPARSE）：之后写越 EOF / 跳过区域会自动成洞，
    // 物理上只为已写数据分配簇。标记失败不影响正确性（退化为普通文件，洞读 0 仍
    // 由上层 memset 兜底），只是稀疏节省量可能受限。
    const HANDLE h = reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(f_)));
    if (h != INVALID_HANDLE_VALUE) {
        DWORD dummy = 0;
        if (::DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                              &dummy, nullptr)) {
            sparse_ok_ = true;
        }
    }
#endif
    logical_size_ = TellFileSize(f_);
}

SparseFileBlockDevice::~SparseFileBlockDevice() {
    if (f_ != nullptr) {
        std::fflush(f_);
        std::fclose(f_);
        f_ = nullptr;
    }
}

size_t SparseFileBlockDevice::Read(long long offset, char* buf, size_t len) {
    if (f_ == nullptr || offset < 0 || buf == nullptr || len == 0) return 0;
    const long long avail = logical_size_ - offset;
    if (avail <= 0) return 0;  // 逻辑末尾：返回不足
    const size_t n = std::min<size_t>(len, static_cast<size_t>(avail));
    // 洞区域（未分配区）先清零：稀疏文件读洞本身返回 0，此处显式 memset 双保险。
    std::memset(buf, 0, n);
    FSeek64(f_, offset, SEEK_SET);
    size_t got = std::fread(buf, 1, n, f_);
    if (got < n && std::ferror(f_)) {
        std::clearerr(f_);
        throw std::runtime_error("SparseFileBlockDevice::Read failed: " + path_);
    }
    return n;
}

size_t SparseFileBlockDevice::Write(long long offset, const char* buf, size_t len) {
    if (f_ == nullptr || offset < 0 || buf == nullptr || len == 0) return 0;
    const long long end = offset + static_cast<long long>(len);
    if (end > logical_size_) {
        logical_size_ = end;  // 写越 EOF：产生洞（不物理分配中间区域）
    }
    FSeek64(f_, offset, SEEK_SET);
    size_t written = std::fwrite(buf, 1, len, f_);
    std::fflush(f_);
    if (written != len || std::ferror(f_)) {
        std::clearerr(f_);
        throw std::runtime_error("SparseFileBlockDevice::Write failed: " + path_);
    }
    MergeRegion(offset, end);  // 跟踪已分配区间（仅观测，不影响正确性）
    return written;
}

void SparseFileBlockDevice::EnsureCapacity(long long byte_count) {
    if (f_ == nullptr || byte_count <= 0) return;
    if (logical_size_ >= byte_count) return;
#ifdef _WIN32
    // 用 SetEndOfFile 扩展逻辑大小：不写任何字节 → 扩展区保持稀疏（不分配簇）。
    const HANDLE h = reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(f_)));
    LARGE_INTEGER off;
    off.QuadPart = byte_count;
    if (h != INVALID_HANDLE_VALUE && ::SetFilePointerEx(h, off, nullptr, FILE_BEGIN) &&
        ::SetEndOfFile(h)) {
        logical_size_ = byte_count;
    }
#else
    if (::ftruncate(::fileno(f_), static_cast<off_t>(byte_count)) == 0) {
        logical_size_ = byte_count;
    }
#endif
}

void SparseFileBlockDevice::Sync() {
    DurableSync(f_);
}

// 把 [lo, hi) 合并进已分配区间集合（保持有序、不重叠；相邻连续写合并为同一区间），
// 并同步更新 allocated_bytes_（物理占用近似）。
void SparseFileBlockDevice::MergeRegion(long long lo, long long hi) {
    if (hi <= lo) return;
    size_t i = 0;
    while (i < regions_.size() && regions_[i].second < lo) ++i;  // 第一个可能重叠/相邻的区间
    long long nlo = lo, nhi = hi;
    size_t j = i;
    while (j < regions_.size() && regions_[j].first <= nhi) {
        nlo = std::min(nlo, regions_[j].first);
        nhi = std::max(nhi, regions_[j].second);
        ++j;
    }
    for (size_t k = i; k < j; ++k) {
        allocated_bytes_ -= (regions_[k].second - regions_[k].first);
    }
    regions_.erase(regions_.begin() + i, regions_.begin() + j);
    regions_.insert(regions_.begin() + i, std::make_pair(nlo, nhi));
    allocated_bytes_ += (nhi - nlo);
}

}  // namespace sqlcompiler