#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "txn/LogRecord.h"

namespace sqlcompiler {

// LogManager — Write-Ahead Log 文件管理器。
//
// 文件格式（沿用 LogRecord.cpp 的编码）：
//   [u32 length][LogRecord bytes][u32 length][LogRecord bytes]...
//
// 公共 API：
//   lsn_t AppendRecord(LogRecord&&)  —— 追加一条日志，返回其 LSN。
//   void Flush()                     —— fdatasync/FlushFileBuffers。
//   lsn_t durable_lsn() const        —— 已被 Flush 到磁盘的最大 LSN。
//   std::vector<LogRecord> ReadAll() —— 全量扫描（启动期 RecoveryManager 用）。
//
// 设计要点（ARIES STEAL+NO-FORCE）：
//   * AppendRecord 仅写入文件并返回 LSN；不强制刷盘。
//   * Flush 把缓冲区里所有 LSN 刷到磁盘；durable_lsn() 反映「最远可恢复的 LSN」。
//   * LogManager 不是线程安全的：当前模型单线程写，所有调用都在同一线程。
//     mutex_ 仅为 Serialize 内部 future-proof。
class LogManager {
public:
    // wal_file: 形如 "<db_file>.wal"；若文件不存在则创建。
    explicit LogManager(const std::string& wal_file);
    ~LogManager();

    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    // 追加一条记录；返回该记录的 LSN。LSN 单调递增，从 1 开始。
    lsn_t AppendRecord(LogRecord&& rec);

    // Phase C：写入一条 CLR（Compensation Log Record）。
    //   page_bytes     —— undo 后整页状态（即刚刚把 page 设成的形态）。
    //   undo_next_lsn  —— undo 链上下一个待撤销的 LSN；写入 CLR 的 undo_next_lsn_
    //                    字段。崩溃恢复期 undo pass 遇到本 CLR 时直接跳到该值。
    // 内部调 AppendRecord(type=CLR)，CLR 的 prev_lsn 由 AppendRecord 通过
    // last_lsn_per_txn_ 自动串到 txn 上一条记录；txn 的 prev_lsn 链因此继续
    // 走标准 prev_lsn，不被 CLR 截断。
    lsn_t AppendCLR(txn_id_t txn_id, page_id_t page_id, const char* page_bytes,
                    lsn_t undo_next_lsn);

    // Phase D：写入 SAVEPOINT_ROLLBACK / SAVEPOINT_RELEASE 记录。
    // 两个方法都是「信息性」标记，不携带 page-image；恢复期 AnalysisPass /
    // UndoPass 直接跳过它们，但 WAL 的 prev_lsn 链因此推进一格，让后续记录
    // （例如 COMMIT / ABORT）的 prev_lsn 正确指向最近的 CLR 或 UPDATE。
    lsn_t AppendSavepointRollback(txn_id_t txn_id, const std::string& name);
    lsn_t AppendSavepointRelease(txn_id_t txn_id, const std::string& name);

    // 强制把缓冲区刷到磁盘（fdatasync / FlushFileBuffers）。
    // 同时把 fdatasync 后的 LSN 记录到 durable_lsn_，之后 durable_lsn()
    // 会返回这个值，供 BufferPoolManager 决定能否 FlushPage。
    void Flush();

    // 当前「已被持久化」的最大 LSN。
    lsn_t durable_lsn() const { return durable_lsn_; }

    // 扫描整个 WAL 文件，逐条反序列化返回。仅 RecoveryManager 在启动期使用，
    // 不与 AppendRecord 并发。返回的 LogRecord 已经把 lsn_/prev_lsn_ 字段填好。
    std::vector<LogRecord> ReadAll();

private:
    // 平台相关句柄。Windows 用 HANDLE（_open 也行，但 HANDLE 与 FlushFileBuffers
    // 配合更直接）；POSIX 用 int fd。
    void* file_handle_ = nullptr;  // HANDLE on Windows, int* fd on POSIX (heap)
    int posix_fd_ = -1;            // POSIX: -1 表示未打开

    std::string wal_file_;
    std::mutex mutex_;
    lsn_t next_lsn_ = 1;       // 下一个待分配的 LSN
    lsn_t durable_lsn_ = 0;     // 已持久化的最大 LSN
    // 每个事务最近一次 AppendRecord 的 LSN；AppendRecord 时用此值填 prev_lsn_。
    // 重启后由 RecoveryManager::AnalysisPass 从持久化日志重建。
    std::unordered_map<txn_id_t, lsn_t> last_lsn_per_txn_;

    // 平台抽象：open / close / write / sync
    bool OpenForAppend();
    void CloseFile();
    // 写入 length 字节；返回是否成功。强制走 OS 直写，不经过 C++ stream 缓冲。
    bool WriteBytes(const char* data, size_t length);
    // fdatasync / FlushFileBuffers
    bool SyncOs();

    // 重放：扫描整个文件到 raw bytes，由 ReadAll 反序列化为 LogRecord。
    void ScanFile(std::vector<char>* out);
};

}  // namespace sqlcompiler