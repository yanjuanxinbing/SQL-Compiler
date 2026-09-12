#include "storage/PageAllocator.h"

#include <cstring>

namespace sqlcompiler {

// size_flag 编码：
//   位0 = free 标志；位1..（对齐）保留；块字节数 = value & ~7。
namespace {
constexpr int32_t kFreeFlag = 1;
constexpr int32_t kSizeMask = static_cast<int32_t>(~7);
}  // namespace

int32_t PageAllocator::ReadI32(const char* buffer, int32_t off) {
    int32_t v;
    std::memcpy(&v, buffer + off, sizeof(int32_t));
    return v;
}

void PageAllocator::WriteI32(char* buffer, int32_t off, int32_t v) {
    std::memcpy(buffer + off, &v, sizeof(int32_t));
}

bool PageAllocator::Formatted(const char* buffer) {
    return ReadI32(buffer, 0) == static_cast<int32_t>(kMagic);
}

bool PageAllocator::IsFree(const char* buffer, int32_t block_off) {
    return (ReadI32(buffer, block_off) & kFreeFlag) != 0;
}

int32_t PageAllocator::BlockSize(const char* buffer, int32_t block_off) {
    return ReadI32(buffer, block_off) & kSizeMask;
}

void PageAllocator::SetSizeFree(char* buffer, int32_t block_off, int32_t size) {
    WriteI32(buffer, block_off, size | kFreeFlag);
}

int32_t PageAllocator::NextOf(const char* buffer, int32_t block_off) {
    return ReadI32(buffer, block_off + 4);
}

void PageAllocator::SetNext(char* buffer, int32_t block_off, int32_t next) {
    WriteI32(buffer, block_off + 4, next);
}

bool PageAllocator::UnlinkFree(char* buffer, int32_t block_off) {
    int32_t head = ReadI32(buffer, 12);
    int32_t prev = 0;
    int32_t cur = head;
    while (cur != 0) {
        if (cur == block_off) {
            int32_t nxt = NextOf(buffer, cur);
            if (prev == 0) {
                WriteI32(buffer, 12, nxt);
            } else {
                SetNext(buffer, prev, nxt);
            }
            return true;
        }
        prev = cur;
        cur = NextOf(buffer, cur);
    }
    return false;
}

bool PageAllocator::Init(char* buffer, int32_t size) {
    if (buffer == nullptr || size < kRegionHeader + kBlockHeader) return false;
    WriteI32(buffer, 0, static_cast<int32_t>(kMagic));
    WriteI32(buffer, 4, size);
    WriteI32(buffer, 8, 0);                       // allocated_payload = 0
    WriteI32(buffer, 12, kRegionHeader);          // free_head = 首块
    // 第一块：覆盖 [kRegionHeader, size) 的空闲块
    SetSizeFree(buffer, kRegionHeader, size - kRegionHeader);
    SetNext(buffer, kRegionHeader, 0);
    return true;
}

bool PageAllocator::Allocate(char* buffer, int32_t size, int32_t* out_offset) {
    if (buffer == nullptr || !Formatted(buffer) || size <= 0) return false;
    int32_t al = (size + kAlign - 1) & ~(kAlign - 1);  // 对齐后载荷字节
    int32_t need = kBlockHeader + al;
    if (need < 0) return false;                          // 溢出保护

    int32_t head = ReadI32(buffer, 12);
    int32_t prev = 0;
    int32_t cur = head;
    while (cur != 0) {
        int32_t blk = BlockSize(buffer, cur);
        if (blk >= need) {
            int32_t nxt = NextOf(buffer, cur);
            // 从空闲链表中摘出该块
            if (prev == 0) {
                WriteI32(buffer, 12, nxt);
            } else {
                SetNext(buffer, prev, nxt);
            }
            // 若剩余空间够保留一个最小空闲块则切分，并把它压回链表头
            if (blk - need >= kMinBlock) {
                int32_t rem = cur + need;
                SetSizeFree(buffer, rem, blk - need);
                SetNext(buffer, rem, ReadI32(buffer, 12));
                WriteI32(buffer, 12, rem);
                WriteI32(buffer, cur, need);  // 当前块尺寸收缩为 need（去掉 free 位）
            } else {
                WriteI32(buffer, cur, blk);   // 整块使用（清 free 位）
            }
            WriteI32(buffer, 8, ReadI32(buffer, 8) + al);  // 累计已分配载荷
            if (out_offset) *out_offset = static_cast<int32_t>(cur + kBlockHeader);
            return true;
        }
        prev = cur;
        cur = NextOf(buffer, cur);
    }
    return false;
}

bool PageAllocator::Free(char* buffer, int32_t payload_offset) {
    if (buffer == nullptr || !Formatted(buffer)) return false;
    int32_t capacity = ReadI32(buffer, 4);
    int32_t b = payload_offset - kBlockHeader;
    if (b < kRegionHeader || b >= capacity || payload_offset >= capacity) return false;
    if (IsFree(buffer, b)) return false;  // 重复释放

    int32_t size = BlockSize(buffer, b);
    if (size < kBlockHeader) return false;
    int32_t payload_freed = size - kBlockHeader;  // 本次释放的用户载荷（记账用）
    SetSizeFree(buffer, b, size);

    // 左扫描：找"结束于 b"的空闲前驱 p，合并后基址为 p
    int32_t merge_base = b;
    int32_t merged = size;
    {
        int32_t p = kRegionHeader;
        while (p < b) {
            if (p + BlockSize(buffer, p) == b && IsFree(buffer, p)) {
                merged += BlockSize(buffer, p);
                UnlinkFree(buffer, p);
                merge_base = p;
                break;
            }
            p += BlockSize(buffer, p);
        }
    }
    // 向右合并：下一块空闲则并入
    {
        int32_t nb = merge_base + merged;
        if (nb + kBlockHeader <= capacity && IsFree(buffer, nb)) {
            merged += BlockSize(buffer, nb);
            UnlinkFree(buffer, nb);
        }
    }

    // 把合并后的块压回空闲链表头
    SetSizeFree(buffer, merge_base, merged);
    SetNext(buffer, merge_base, ReadI32(buffer, 12));
    WriteI32(buffer, 12, merge_base);
    // 记账：释放的载荷减少已分配字节
    WriteI32(buffer, 8, ReadI32(buffer, 8) - payload_freed);
    return true;
}

PageAllocator::Stats PageAllocator::GetStats(const char* buffer) {
    Stats st;
    if (buffer == nullptr || !Formatted(buffer)) return st;
    int32_t capacity = ReadI32(buffer, 4);
    st.capacity = capacity;
    int32_t payload = capacity - kRegionHeader;  // 块区总字节

    int32_t free_bytes = 0;       // 空闲块占用（含8B头）
    int32_t num_free = 0;
    int32_t largest = 0;
    int32_t cur = ReadI32(buffer, 12);
    while (cur != 0) {
        int32_t blk = BlockSize(buffer, cur);
        if (blk >= kBlockHeader) {
            free_bytes += blk;
            if (blk > largest) largest = blk;
            ++num_free;
        }
        cur = NextOf(buffer, cur);
    }
    st.num_free_blocks = num_free;
    st.free_capacity = free_bytes;
    st.allocated_bytes = payload - free_bytes;        // 已分配块字节（含8B头）
    st.usable_free = free_bytes - kBlockHeader * num_free;
    st.largest_free_block = largest;
    st.external_fragmentation = st.usable_free -
                                (largest >= kBlockHeader ? (largest - kBlockHeader) : 0);
    st.header_overhead = kRegionHeader + kBlockHeader * num_free;
    return st;
}

}  // namespace sqlcompiler