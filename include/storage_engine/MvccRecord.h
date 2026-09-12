#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sqlcompiler {

// MVCC 记录头（快照隔离的版本元数据）。
//
// 每条序列化记录前附加固定 48 字节头。最新可见版本保留在稳定 RID 槽位
// （head）；被替换/删除的旧版本作为独立 slot，用 prev_page_id/prev_slot_num
// 从 head 向前（越来越旧）链接成链。
//
// 兼容判定：每条记录内容以 uint32 魔数开头。旧数据库（无头）记录的前 4 字节
// 不等于 kMvccMagic，故 ReadMvccHeader 返回 false → 按单版 legacy 处理。
constexpr uint32_t kMvccMagic = 0x4D564343u;  // "MVCC"

#pragma pack(push, 1)
struct MvccRecordHeader {
    uint32_t   magic;         // kMvccMagic
    int64_t    begin_xid;     // 写出本版的写者 txn_id
    int64_t    end_xid;       // 0=当前仍为最新可见；否则为替换/删除本版的写者
    int64_t    begin_csn;     // begin_xid 提交时回填的 CSN
    int64_t    end_csn;       // 使本版失效的 CSN（0=尚未赋值）
    int32_t    prev_page_id;  // 更旧版本记录页号（INVALID_PAGE_ID=链尾）
    int32_t    prev_slot_num;
    int32_t    pad;           // 补齐到 48 字节、8 字节对齐
};
#pragma pack(pop)
static_assert(sizeof(MvccRecordHeader) == 48, "MvccRecordHeader must be 48 bytes");

// 把头部序列化到 dst（dst 需 >= 48 字节）。
inline void WriteMvccHeader(char* dst, const MvccRecordHeader& h) {
    std::memcpy(dst, &h, sizeof(MvccRecordHeader));
}

// 从 slot 内容读取头部；magic 不匹配（legacy 记录）返回 false，*out 不被写入。
inline bool ReadMvccHeader(const char* src, MvccRecordHeader* out) {
    uint32_t magic;
    std::memcpy(&magic, src, sizeof(uint32_t));
    if (magic != kMvccMagic) return false;
    std::memcpy(out, src, sizeof(MvccRecordHeader));
    return true;
}

}  // namespace sqlcompiler