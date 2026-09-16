#include "storage/PageAllocator.h"

#include <cstring>

namespace sqlcompiler {

// size_flag 编码：
//   位0 = free 标志；位1..（对齐）保留；块字节数 = value & ~7。
// 说明：kSizeMask(=~7) 同时清掉 bit0(free) 与保留的 bit1..2，故读尺寸无需再单独掩码。
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
    // 空闲链表是「偏移单链」，摘除必须知道前驱，故从链表头起线性扫描并记录 prev；
    // prev == 0 表示命中头结点，此时改区域头里的 free_head 而非某个块的 next。
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
    // 区域头 24 字节（16B 头 + 首块头）即最小的可格式化区域，低于此值无意义，直接拒绝；
    // 这里不写 magic，因此失败时缓冲区会被 Formatted() 判定为无效堆。
    if (buffer == nullptr || size < kRegionHeader + kBlockHeader) return false;
    WriteI32(buffer, 0, static_cast<int32_t>(kMagic));
    WriteI32(buffer, 4, size);
    WriteI32(buffer, 8, 0);                       // allocated_payload = 0
    WriteI32(buffer, 12, kRegionHeader);          // free_head = 首块
    // 第一块：覆盖 [kRegionHeader, size) 的空闲块；整块即空闲清单的全部内容（next=0）。
    SetSizeFree(buffer, kRegionHeader, size - kRegionHeader);
    SetNext(buffer, kRegionHeader, 0);
    return true;
}

bool PageAllocator::Allocate(char* buffer, int32_t size, int32_t* out_offset) {
    if (buffer == nullptr || !Formatted(buffer) || size <= 0) return false;
    // 载荷按 8 向上取整，块长 = 块头 + 对齐后载荷；size 接近 INT32_MAX 时加法会溢出，
    // 负值直接拒绝（否则后续 blk >= need 的比较会误判为「到处都能放」）。
    int32_t al = (size + kAlign - 1) & ~(kAlign - 1);  // 对齐后载荷字节
    int32_t need = kBlockHeader + al;
    if (need < 0) return false;                          // 溢出保护

    // first-fit：沿空闲链表找第一个块长 ≥ need 的块；同样需要 prev 以便 O(1) 摘链。
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
            // 若剩余空间够保留一个最小空闲块则切分，并把它压回链表头；
            // 不足 kMinBlock 时整块给出，宁可内部浪费也不留下无法再分配的小碎片。
            if (blk - need >= kMinBlock) {
                int32_t rem = cur + need;
                SetSizeFree(buffer, rem, blk - need);
                SetNext(buffer, rem, ReadI32(buffer, 12));
                WriteI32(buffer, 12, rem);
                WriteI32(buffer, cur, need);  // 当前块尺寸收缩为 need（去掉 free 位）
            } else {
                WriteI32(buffer, cur, blk);   // 整块使用（清 free 位）
            }
            // 只累计「对齐后的载荷」，不含块头：与 Free 中按 size-8 扣减的记账口径一致。
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
    // 载荷偏移 → 块偏移；三处边界都要查：块头不得早于块区起点、块偏移与载荷偏移都不得
    // 越过 capacity（后者防 payload_offset ∈ (capacity-8, capacity) 这类贴边入参）。
    int32_t capacity = ReadI32(buffer, 4);
    int32_t b = payload_offset - kBlockHeader;
    if (b < kRegionHeader || b >= capacity || payload_offset >= capacity) return false;
    if (IsFree(buffer, b)) return false;  // 重复释放

    int32_t size = BlockSize(buffer, b);
    if (size < kBlockHeader) return false;
    int32_t payload_freed = size - kBlockHeader;  // 本次释放的用户载荷（记账用）
    SetSizeFree(buffer, b, size);

    // 左扫描：找"结束于 b"的空闲前驱 p，合并后基址为 p
    // 空闲链表按分配先后而非地址排序，无法直接取「物理前一块」，只能从块区起点按块长
    // 逐块推进（p += BlockSize）模拟物理顺序；物理上最多只有一个块与 b 相邻，找到即停。
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
    // 右邻块唯一，直接算 merge_base + merged 即可；nb + kBlockHeader <= capacity 保证块头
    // 完整落在区域内才去读它（末块之后可能已抵达 capacity，不能越界读）。
    {
        int32_t nb = merge_base + merged;
        if (nb + kBlockHeader <= capacity && IsFree(buffer, nb)) {
            merged += BlockSize(buffer, nb);
            UnlinkFree(buffer, nb);
        }
    }

    // 把合并后的块压回空闲链表头（头插，保持与切分一致的确定性顺序）
    SetSizeFree(buffer, merge_base, merged);
    SetNext(buffer, merge_base, ReadI32(buffer, 12));
    WriteI32(buffer, 12, merge_base);
    // 记账：释放的载荷减少已分配字节（块头不计入 allocated_payload）
    WriteI32(buffer, 8, ReadI32(buffer, 8) - payload_freed);
    return true;
}

PageAllocator::Stats PageAllocator::GetStats(const char* buffer) {
    Stats st;
    if (buffer == nullptr || !Formatted(buffer)) return st;
    int32_t capacity = ReadI32(buffer, 4);
    st.capacity = capacity;
    int32_t payload = capacity - kRegionHeader;  // 块区总字节

    // 只遍历空闲链表即可：已分配块字节数由「块区总字节 - 空闲块字节」反推，无需再扫块区。
    // blk >= kBlockHeader 是防御分支：块头被外部写坏（尺寸 < 8）时不计入统计，避免出现负数。
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
    // 外部碎片 = 可用空闲载荷 - 单次最大可分配载荷；无空闲块时最大可分配按 0 计（三元保护，
    // 避免 largest 小于块头时得到负数）。
    st.external_fragmentation = st.usable_free -
                                (largest >= kBlockHeader ? (largest - kBlockHeader) : 0);
    st.header_overhead = kRegionHeader + kBlockHeader * num_free;
    return st;
}

}  // namespace sqlcompiler