#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "storage/Page.h"

namespace sqlcompiler {

// LSN（Log Sequence Number）：单调递增的 64 位整数，从 1 开始。
// 0 保留为「无效 / 尚未分配」哨兵值。
using lsn_t = uint64_t;
using txn_id_t = int64_t;
constexpr lsn_t INVALID_LSN = 0;

// Phase B 的 5 种日志记录类型 + Phase C 加入 CLR（Compensation Log Record）；
// Phase D 加入 SAVEPOINT_ROLLBACK / SAVEPOINT_RELEASE。
// 当前实现已把字节布局留够宽度。
enum class LogRecordType : uint8_t {
    BEGIN = 0,
    COMMIT = 1,
    ABORT = 2,
    UPDATE = 3,
    CHECKPOINT = 4,
    // Phase C：补偿日志记录，由 Rollback / 恢复期 undo pass 在撤销一个 UPDATE
    // 后立刻写入，承载被撤销 page 的「undo 后状态」+ undo_next_lsn 让恢复期能
    // 跳过已 undo 的工作。
    CLR = 5,
    // Phase D：ROLLBACK TO savepoint 完成时由 TransactionManager 写入。
    // 不携带 page-image；savepoint_name_ 标识被回滚到的保存点。
    // 该记录为「信息性」标记，不修改任何 page，恢复期 AnalysisPass / UndoPass
    // 直接跳过；CLR 链上的 undo_next_lsn 仍然指向真正的下一个待撤销 LSN。
    SAVEPOINT_ROLLBACK = 6,
    // Phase D：RELEASE SAVEPOINT 时写入；同样为信息性标记。
    SAVEPOINT_RELEASE = 7,
};

// 一条物理日志记录（ARIES 风格 page-image 记录）。
//
// 字段语义：
//   lsn_         ——  本记录的 LSN（由 LogManager::AppendRecord 分配）。
//   prev_lsn_    ——  同一事务上一条记录的 LSN；用 prev_lsn 链反向回放 undo。
//   txn_id_      ——  所属事务 id。
//   type_        ——  记录类型（BEGIN/COMMIT/ABORT/UPDATE/CHECKPOINT/CLR）。
//   page_id_     ——  受影响 page；CHECKPOINT / BEGIN / COMMIT / ABORT
//                  下无意义，设为 INVALID_PAGE_ID。
//   before_image_/after_image_ —— UPDATE 记录下是 page 的整页快照；
//                                  CLR 记录下 before_image_ 存的是「undo 后
//                                  page 状态」（即我们刚刚把 page 设成的状态），
//                                  这样 redo pass 在崩溃后能完整重做到 undo 后
//                                  形态。其它类型不携带 image。
//   undo_next_lsn_ —— 仅 CLR 有意义：「undo 链上下一个待撤销的 LSN」。
//                    崩溃恢复期 undo pass 遇到 CLR 时直接跳到该值，不再走
//                    CLR 的 prev_lsn，从而跳过已经完成的工作。
//
// 设计取舍：每条 UPDATE/CLR 记录 1 个 PAGE_SIZE，单条日志最大 4KB 级别。
// 在教育规模足够；生产 ARIES 会用「差额物理日志」压缩到几百字节，但当前实现
// 优先简单与可调试性。
struct LogRecord {
    lsn_t lsn_ = INVALID_LSN;
    lsn_t prev_lsn_ = INVALID_LSN;
    txn_id_t txn_id_ = 0;
    LogRecordType type_ = LogRecordType::BEGIN;
    page_id_t page_id_ = INVALID_PAGE_ID;
    std::vector<char> before_image_;  // size == PAGE_SIZE 当 type==UPDATE 或 CLR
    std::vector<char> after_image_;   // size == PAGE_SIZE 当 type==UPDATE
    lsn_t undo_next_lsn_ = INVALID_LSN;  // 仅 CLR 有效
    // Phase D：仅 SAVEPOINT_ROLLBACK / SAVEPOINT_RELEASE 携带；其它类型为空。
    std::string savepoint_name_;

    // UPDATE / CLR 都携带 page-image；其它类型不携带。
    bool HasPageImage() const {
        return type_ == LogRecordType::UPDATE || type_ == LogRecordType::CLR;
    }

    // 仅 SAVEPOINT_ROLLBACK / SAVEPOINT_RELEASE 携带 savepoint 名称。
    bool HasSavepointName() const {
        return type_ == LogRecordType::SAVEPOINT_ROLLBACK ||
               type_ == LogRecordType::SAVEPOINT_RELEASE;
    }
};

// 串行化的固定头部格式（little-endian），跟在 4 字节长度前缀之后：
//   u64 lsn, u64 prev_lsn, i64 txn_id, u8 type, i32 page_id,
//   u32 before_len, u32 after_len, u64 undo_next_lsn  (Phase C：CLR 用)
//   u32 savepoint_name_len                          (Phase D：SAVEPOINT_* 用)
//   [before_image bytes]
//   [after_image bytes]
//   [savepoint_name bytes]
//
// 选择固定头部 + 长度字段（而不是定长 8KB page image）的理由：让 BEGIN/COMMIT
// 等非 update 记录只占几十字节，磁盘利用率高；并让 CHECKPOINT 之类未来可能
// 包含可变长度表项的扩展不必破坏向后兼容。
constexpr size_t kLogRecordHeaderSize =
    sizeof(uint64_t) +        // lsn
    sizeof(uint64_t) +        // prev_lsn
    sizeof(int64_t) +         // txn_id
    sizeof(uint8_t) +         // type
    sizeof(int32_t) +         // page_id
    sizeof(uint32_t) +        // before_len
    sizeof(uint32_t) +        // after_len
    sizeof(uint64_t) +        // undo_next_lsn (Phase C: CLR-only)
    sizeof(uint32_t);         // savepoint_name_len (Phase D: SAVEPOINT_*-only)

// 把 LogRecord 序列化到 buf；返回写入字节数。buf 不足返回 0。
size_t SerializeLogRecord(const LogRecord& rec, char* buf, size_t buf_size);

// 从 src 反序列化出一条 LogRecord。src_len 是 src 可读字节数。
bool DeserializeLogRecord(const char* src, size_t src_len, LogRecord* out);

// 单条日志记录的总字节数（含 header + before_image + after_image）。
size_t LogRecordSize(const LogRecord& rec);

}  // namespace sqlcompiler