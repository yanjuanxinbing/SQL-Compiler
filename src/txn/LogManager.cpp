// =============================================================================
// LogManager — Phase B 持久化 WAL 文件。
//
// Durability semantics:
//   * WAL is appended to `<db_file>.wal` as length-prefixed binary records
//     (LogRecord.cpp 编码).
//   * AppendRecord 把记录写入文件但不强制刷盘；返回的 LSN 仅表示「该记录已交给
//     OS 层写入队列」，不保证可恢复。
//   * Flush() 调用 fdatasync（POSIX）或 FlushFileBuffers（Windows），把已写入
//     的所有字节强制落盘；之后 durable_lsn() 返回「最远可恢复 LSN」，
//     BufferPoolManager 据此判断能否 FlushPage（WAL-before-data 规则）。
//   * ReadAll() 启动期扫描：RecoveryManager::Run() 会调用它做 ARIES
//     analysis -> redo -> undo 三阶段恢复。
//
// Cross-platform:
//   * Windows  下：_open + _write + FlushFileBuffers（HANDLE 从 _get_osfhandle 拿到）。
//   * POSIX 下：open(O_DSYNC) + pwrite + fdatasync。
//
// Phase B intentionally keeps it simple: no log archival, no group commit,
// no log truncation beyond CHECKPOINT. Out of scope items live in Phase C/D.
// =============================================================================

#include "txn/LogManager.h"

#include <cstring>
#include <stdexcept>

#if defined(_WIN32)
// _open / _read / _write are flagged C4996 by MSVC; we deliberately use the
// low-level POSIX-style API to talk to FlushFileBuffers directly. Silence the
// "use _sopen_s" hint that doesn't apply here.
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace sqlcompiler {

namespace {

inline void WriteU32(char* dst, uint32_t v) {
    std::memcpy(dst, &v, sizeof(uint32_t));
}
inline uint32_t ReadU32(const char* src) {
    uint32_t v;
    std::memcpy(&v, src, sizeof(uint32_t));
    return v;
}

}  // namespace

LogManager::LogManager(const std::string& wal_file) : wal_file_(wal_file) {
    if (!OpenForAppend()) {
        throw std::runtime_error("LogManager: cannot open WAL file: " + wal_file_);
    }
    // 启动期：扫描整个 WAL 文件，按 lsn 顺序重建 last_lsn_per_txn_ 与
    // next_lsn_。durable_lsn_ 保守设为 0，由第一次 Flush() 推进。
    std::vector<char> raw;
    ScanFile(&raw);
    next_lsn_ = 1;
    durable_lsn_ = 0;
    last_lsn_per_txn_.clear();
    size_t pos = 0;
    while (pos + sizeof(uint32_t) <= raw.size()) {
        uint32_t len = ReadU32(raw.data() + pos);
        pos += sizeof(uint32_t);
        if (pos + len > raw.size()) break;
        LogRecord rec;
        if (!DeserializeLogRecord(raw.data() + pos, len, &rec)) break;
        if (rec.lsn_ >= next_lsn_) next_lsn_ = rec.lsn_ + 1;
        if (rec.txn_id_ != 0) {
            last_lsn_per_txn_[rec.txn_id_] = rec.lsn_;
        }
        pos += len;
    }
    // 已扫描到的最大 lsn 视为已持久化（durable），让后续 FlushPage 不再因
    // LSN 检查无谓刷盘。
    if (next_lsn_ > 1) {
        durable_lsn_ = next_lsn_ - 1;
    }
}

LogManager::~LogManager() {
    CloseFile();
}

bool LogManager::OpenForAppend() {
#if defined(_WIN32)
    // 用 _open 拿到 OS 句柄，再转成 HANDLE 走 FlushFileBuffers。
    int fd = _open(wal_file_.c_str(),
                   _O_BINARY | _O_APPEND | _O_RDWR | _O_CREAT, _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        // 文件可能存在但只读 / 权限不足；尝试 R+W 显式重试一次。
        fd = _open(wal_file_.c_str(),
                   _O_BINARY | _O_APPEND | _O_RDWR | _O_CREAT, _S_IREAD | _S_IWRITE);
        if (fd < 0) return false;
    }
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) {
        _close(fd);
        return false;
    }
    posix_fd_ = fd;
    file_handle_ = h;
    return true;
#else
    int fd = ::open(wal_file_.c_str(),
                    O_APPEND | O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    posix_fd_ = fd;
    file_handle_ = nullptr;
    return true;
#endif
}

void LogManager::CloseFile() {
#if defined(_WIN32)
    if (posix_fd_ >= 0) {
        _close(posix_fd_);
        posix_fd_ = -1;
    }
    file_handle_ = nullptr;
#else
    if (posix_fd_ >= 0) {
        ::close(posix_fd_);
        posix_fd_ = -1;
    }
#endif
}

bool LogManager::WriteBytes(const char* data, size_t length) {
    if (length == 0) return true;
#if defined(_WIN32)
    if (posix_fd_ < 0) return false;
    size_t written = 0;
    while (written < length) {
        int n = _write(posix_fd_, data + written,
                       static_cast<unsigned int>(length - written));
        if (n <= 0) return false;
        written += static_cast<size_t>(n);
    }
    return true;
#else
    if (posix_fd_ < 0) return false;
    size_t written = 0;
    while (written < length) {
        ssize_t n = ::write(posix_fd_, data + written, length - written);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
#endif
}

bool LogManager::SyncOs() {
#if defined(_WIN32)
    if (file_handle_ == nullptr || file_handle_ == INVALID_HANDLE_VALUE) return false;
    return FlushFileBuffers(static_cast<HANDLE>(file_handle_)) != 0;
#else
    if (posix_fd_ < 0) return false;
    // fdatasync：只刷数据，不刷元数据。在 Linux 上 WAL 追加场景里 inode mtime
    // 不参与正确性，因此 fdatasync 比 fsync 略快。如果系统不支持 fdatasync，
    // 退回到 fsync。
#if defined(__APPLE__) || defined(__FreeBSD__)
    return ::fsync(posix_fd_) == 0;
#else
    return ::fdatasync(posix_fd_) == 0;
#endif
#endif
}

lsn_t LogManager::AppendRecord(LogRecord&& rec) {
    std::lock_guard<std::mutex> lock(mutex_);
    rec.lsn_ = next_lsn_++;
    // 自动维护 per-txn 链：把 rec.prev_lsn_ 设为该 txn 上一条记录的 LSN。
    // BEGIN/COMMIT/ABORT/UPDATE/CLR 都需要串到 prev_lsn 链上，undo pass
    // 才能反向走完。CLR 也走 prev_lsn 链，但 undo pass 遇到 CLR 时跳过，
    // 改用 clr.undo_next_lsn_ 跳到下一条待撤销记录。
    if (rec.txn_id_ != 0) {
        auto it = last_lsn_per_txn_.find(rec.txn_id_);
        if (it != last_lsn_per_txn_.end()) {
            rec.prev_lsn_ = it->second;
        } else if (rec.prev_lsn_ == INVALID_LSN) {
            // 调用方没显式填 prev_lsn_ 时，默认填 0（INVALID_LSN）即可。
            rec.prev_lsn_ = INVALID_LSN;
        }
        last_lsn_per_txn_[rec.txn_id_] = rec.lsn_;
    }

    // 序列化：长度前缀 + 头部 + before/after image。
    const size_t body_size = LogRecordSize(rec);
    std::vector<char> buf(sizeof(uint32_t) + body_size);
    WriteU32(buf.data(), static_cast<uint32_t>(body_size));
    SerializeLogRecord(rec, buf.data() + sizeof(uint32_t), body_size);

    if (!WriteBytes(buf.data(), buf.size())) {
        // 写入失败：恢复 next_lsn_，抛异常让上层感知。
        --next_lsn_;
        throw std::runtime_error("LogManager: WAL write failed for " + wal_file_);
    }
    // 成功：不更新 durable_lsn_ —— 它只反映「已经 SyncOs 过」的范围。
    return rec.lsn_;
}

lsn_t LogManager::AppendCLR(txn_id_t txn_id, page_id_t page_id,
                            const char* page_bytes, lsn_t undo_next_lsn) {
    LogRecord rec;
    rec.type_ = LogRecordType::CLR;
    rec.txn_id_ = txn_id;
    rec.page_id_ = page_id;
    // Phase C：CLR 的 before_image_ 字段承载「undo 后 page 状态」，让 redo
    // pass 在崩溃后能完整重做到 undo 后形态。after_image_ 在 CLR 下保持空。
    if (page_bytes != nullptr) {
        rec.before_image_.assign(page_bytes, page_bytes + PAGE_SIZE);
    }
    rec.undo_next_lsn_ = undo_next_lsn;
    return AppendRecord(std::move(rec));
}

// Phase D：SAVEPOINT_ROLLBACK / SAVEPOINT_RELEASE 信息性记录。
// 不携带 page-image，只携带 savepoint 名称；prev_lsn 由 AppendRecord 自动
// 串到本 txn 上一条记录（通常是最后一个 CLR）。恢复期 AnalysisPass 与
// RedoPass 都直接忽略它们。
lsn_t LogManager::AppendSavepointRollback(txn_id_t txn_id,
                                          const std::string& name) {
    LogRecord rec;
    rec.type_ = LogRecordType::SAVEPOINT_ROLLBACK;
    rec.txn_id_ = txn_id;
    rec.savepoint_name_ = name;
    return AppendRecord(std::move(rec));
}

lsn_t LogManager::AppendSavepointRelease(txn_id_t txn_id,
                                         const std::string& name) {
    LogRecord rec;
    rec.type_ = LogRecordType::SAVEPOINT_RELEASE;
    rec.txn_id_ = txn_id;
    rec.savepoint_name_ = name;
    return AppendRecord(std::move(rec));
}

void LogManager::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!SyncOs()) {
        throw std::runtime_error("LogManager: WAL flush failed for " + wal_file_);
    }
    // Flush 成功：把「next_lsn_-1」标为已持久化（next_lsn_ 是下一个待分配 LSN）。
    durable_lsn_ = next_lsn_ > 0 ? next_lsn_ - 1 : 0;
}

void LogManager::ScanFile(std::vector<char>* out) {
    out->clear();
#if defined(_WIN32)
    if (posix_fd_ < 0) return;
    // 重新打开读取，避免影响写入端的 fd 位置。
    int rd = _open(wal_file_.c_str(), _O_BINARY | _O_RDONLY);
    if (rd < 0) return;
    std::vector<char> chunk(64 * 1024);
    while (true) {
        int n = _read(rd, chunk.data(), static_cast<unsigned int>(chunk.size()));
        if (n <= 0) break;
        out->insert(out->end(), chunk.data(), chunk.data() + n);
    }
    _close(rd);
#else
    int rd = ::open(wal_file_.c_str(), O_RDONLY);
    if (rd < 0) return;
    std::vector<char> chunk(64 * 1024);
    while (true) {
        ssize_t n = ::read(rd, chunk.data(), chunk.size());
        if (n <= 0) break;
        out->insert(out->end(), chunk.data(), chunk.data() + n);
    }
    ::close(rd);
#endif
}

std::vector<LogRecord> LogManager::ReadAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<char> raw;
    ScanFile(&raw);

    std::vector<LogRecord> records;
    size_t pos = 0;
    while (pos + sizeof(uint32_t) <= raw.size()) {
        uint32_t len = ReadU32(raw.data() + pos);
        pos += sizeof(uint32_t);
        if (pos + len > raw.size()) break;  // 截断的尾部记录：忽略。
        LogRecord rec;
        if (!DeserializeLogRecord(raw.data() + pos, len, &rec)) break;
        records.push_back(std::move(rec));
        pos += len;
    }
    return records;
}

#if defined(_WIN32)
#pragma warning(pop)
#endif

}  // namespace sqlcompiler