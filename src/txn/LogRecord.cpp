// =============================================================================
// LogRecord — ARIES-style 物理 page-image 日志记录。
//
// 序列化格式（little-endian，写入磁盘 / 从磁盘读回共用）：
//   u32 length                    <- 4 字节长度前缀（不含自身），便于扫描恢复
//   u64 lsn
//   u64 prev_lsn
//   i64 txn_id
//   u8  type
//   i32 page_id
//   u32 before_len                (== PAGE_SIZE 当 type==UPDATE 或 CLR，否则 0)
//   u32 after_len                 (== PAGE_SIZE 当 type==UPDATE，否则 0)
//   u64 undo_next_lsn             (Phase C：CLR 用，存 undo 链上下一个 LSN)
//   u32 savepoint_name_len        (Phase D：SAVEPOINT_ROLLBACK / SAVEPOINT_RELEASE 用)
//   [before_image bytes]
//   [after_image bytes]
//   [savepoint_name bytes]
//
// 读路径：先读 4 字节长度，再读 length 字节整条记录，校验头部长度。
// 写路径：序列化头部 + page-image + savepoint name，前置 4 字节长度。
// =============================================================================

#include "txn/LogRecord.h"

#include <cstring>

namespace sqlcompiler {

namespace {

inline void WriteU64(char* dst, uint64_t v) {
    std::memcpy(dst, &v, sizeof(uint64_t));
}
inline void WriteI64(char* dst, int64_t v) {
    std::memcpy(dst, &v, sizeof(int64_t));
}
inline void WriteI32(char* dst, int32_t v) {
    std::memcpy(dst, &v, sizeof(int32_t));
}
inline void WriteU32(char* dst, uint32_t v) {
    std::memcpy(dst, &v, sizeof(uint32_t));
}
inline void WriteU8(char* dst, uint8_t v) { dst[0] = static_cast<char>(v); }

inline uint64_t ReadU64(const char* src) {
    uint64_t v;
    std::memcpy(&v, src, sizeof(uint64_t));
    return v;
}
inline int64_t ReadI64(const char* src) {
    int64_t v;
    std::memcpy(&v, src, sizeof(int64_t));
    return v;
}
inline int32_t ReadI32(const char* src) {
    int32_t v;
    std::memcpy(&v, src, sizeof(int32_t));
    return v;
}
inline uint32_t ReadU32(const char* src) {
    uint32_t v;
    std::memcpy(&v, src, sizeof(uint32_t));
    return v;
}
inline uint8_t ReadU8(const char* src) {
    return static_cast<uint8_t>(src[0]);
}

}  // namespace

// 返回把记录写入 buf 后的字节数；buf 不足时返回 0（由调用方分配够大空间）。
// header 之后紧跟 before_image / after_image / savepoint_name。
size_t SerializeLogRecord(const LogRecord& rec, char* buf, size_t buf_size) {
    if (buf_size < kLogRecordHeaderSize) return 0;
    char* p = buf;
    WriteU64(p, rec.lsn_);                        p += sizeof(uint64_t);
    WriteU64(p, rec.prev_lsn_);                   p += sizeof(uint64_t);
    WriteI64(p, rec.txn_id_);                     p += sizeof(int64_t);
    WriteU8(p, static_cast<uint8_t>(rec.type_));  p += sizeof(uint8_t);
    WriteI32(p, rec.page_id_);                    p += sizeof(int32_t);
    uint32_t before_len = static_cast<uint32_t>(rec.before_image_.size());
    uint32_t after_len  = static_cast<uint32_t>(rec.after_image_.size());
    uint32_t name_len   = static_cast<uint32_t>(rec.savepoint_name_.size());
    WriteU32(p, before_len);                      p += sizeof(uint32_t);
    WriteU32(p, after_len);                       p += sizeof(uint32_t);
    // Phase C：CLR 专用 undo_next_lsn。无条件序列化保证向前兼容；
    // 非 CLR 记录写 0，恢复端读到后会忽略。
    WriteU64(p, rec.undo_next_lsn_);              p += sizeof(uint64_t);
    // Phase D：SAVEPOINT_* 记录携带 savepoint 名称；其它类型写 0。
    WriteU32(p, name_len);                        p += sizeof(uint32_t);
    if (before_len > 0) {
        if (buf_size < kLogRecordHeaderSize + before_len) return 0;
        std::memcpy(p, rec.before_image_.data(), before_len);
        p += before_len;
    }
    if (after_len > 0) {
        if (buf_size < kLogRecordHeaderSize + before_len + after_len) return 0;
        std::memcpy(p, rec.after_image_.data(), after_len);
        p += after_len;
    }
    if (name_len > 0) {
        if (buf_size < kLogRecordHeaderSize + before_len + after_len + name_len)
            return 0;
        std::memcpy(p, rec.savepoint_name_.data(), name_len);
        p += name_len;
    }
    return static_cast<size_t>(p - buf);
}

// 从 src 反序列化出一条 LogRecord。src_len 至少要 >= kLogRecordHeaderSize。
// 返回 true 表示反序列化成功；否则 src 损坏或长度不足。
bool DeserializeLogRecord(const char* src, size_t src_len, LogRecord* out) {
    if (out == nullptr) return false;
    if (src_len < kLogRecordHeaderSize) return false;
    out->lsn_         = ReadU64(src + 0);
    out->prev_lsn_    = ReadU64(src + 8);
    out->txn_id_      = ReadI64(src + 16);
    uint8_t t         = ReadU8(src + 24);
    out->type_        = static_cast<LogRecordType>(t);
    out->page_id_     = ReadI32(src + 25);
    uint32_t before_len = ReadU32(src + 29);
    uint32_t after_len  = ReadU32(src + 33);
    out->undo_next_lsn_ = ReadU64(src + 37);
    uint32_t name_len  = ReadU32(src + 45);
    if (src_len < kLogRecordHeaderSize + before_len + after_len + name_len)
        return false;
    const char* images_start = src + kLogRecordHeaderSize;
    out->before_image_.assign(images_start, images_start + before_len);
    out->after_image_.assign(images_start + before_len,
                             images_start + before_len + after_len);
    if (name_len > 0) {
        out->savepoint_name_.assign(images_start + before_len + after_len,
                                    images_start + before_len + after_len + name_len);
    } else {
        out->savepoint_name_.clear();
    }
    return true;
}

// 单条日志记录的总字节数（含头部、page-image、savepoint_name）。
size_t LogRecordSize(const LogRecord& rec) {
    return kLogRecordHeaderSize + rec.before_image_.size() +
           rec.after_image_.size() + rec.savepoint_name_.size();
}

}  // namespace sqlcompiler