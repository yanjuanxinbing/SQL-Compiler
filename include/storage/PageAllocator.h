#pragma once

#include <cstdint>

namespace sqlcompiler {

// 页内内存分配器（PageAllocator）：
// 在给定的一块字节区域（通常是某个 Page 的数据区）内部实现一个 free-list 堆，
// 管理其中"记录/对象"空间的分配、释放与相邻块合并，并可通过 GetStats() 观测
// 空闲空间与碎片状况。设计目标：
//   - 页内（in-page）：直接作用于调用方提供的 char* 缓冲区，无额外全局状态；
//   - free-list 堆：分配用 first-fit，释放与相邻空闲块合并（左右双向）；
//   - 分配可回放：相同缓冲区大小 + 相同操作序列 ⇒ 布局确定（地址稳定）；
//   - 碎片率可观测：GetStats() 报告已用/空闲、最大连续块、外部碎片、头部开销。
//
// 区域内存布局（所有偏移为 int32，8 字节对齐）：
//   [0] u32 magic
//   [4] i32 capacity              （区域总字节数）
//   [8] i32 allocated_payload     （已分配的用户字节数，仅统计用）
//   [12]i32 free_head             （空闲链表头块偏移，0 = 空）
//   [16] 块区起点
// 每个块（空闲或已分配）以 8 字节头开始：
//   [off]   i32 size_flag         高 29 位=块字节数(含头, 8 对齐)；bit0=free 标志
//   [off+4] i32 next_free         仅空闲块有效：链表下一块偏移（0=尾）
//   已分配块的用户载荷位于 off+8 起，共 size-8 字节。
//
// 说明：本分配器作为"页内内存堆"原语，提供确定性分配与碎片观测；不替代
// TableHeap 的磁盘 slotted-page 格式（该格式是持久化布局，保持不变）。
class PageAllocator {
public:
    static constexpr int32_t kRegionHeader = 16;  // 区域头字节（magic/capacity/al_payload/free_head）
    static constexpr int32_t kBlockHeader = 8;    // 每块头字节（size_flag / next_free）
    static constexpr int32_t kAlign = 8;          // 对齐粒度
    static constexpr int32_t kMinBlock = kBlockHeader + kAlign;  // 最小可保留的空闲块=16
    static constexpr uint32_t kMagic = 0x50414C31u;              // "PAL1"

    // 页内空间使用统计（用于碎片率等观测）
    struct Stats {
        int32_t capacity = 0;              // 区域总字节数
        int32_t allocated_bytes = 0;       // 已分配块占用字节（含各块 8B 头）
        int32_t free_capacity = 0;         // 空闲块占用字节（含各块 8B 头）
        int32_t usable_free = 0;           // 可实际使用的空闲载荷 =
                                          //   free_capacity - 8*num_free_blocks
        int32_t num_free_blocks = 0;
        int32_t largest_free_block = 0;    // 最大连续空闲块（含头），单次最大可分配≈该值-8
        int32_t external_fragmentation = 0;// 无法合并成一次大块分配的空闲载荷 =
                                          //   usable_free - (largest_free_block - 8)
        int32_t header_overhead = 0;       // 16(区域头) + 8*num_free_blocks
    };

    // 在 buffer 的前 size 字节内初始化一个新堆。size 至少 kRegionHeader+kBlockHeader。
    // 成功后整个区域块区被格式化为一个完整空闲块。返回是否成功。
    static bool Init(char* buffer, int32_t size);

    // 在堆内分配 size 字节，成功写出载荷偏移到 out_offset 并返回 true；空间不足返回 false。
    // size 必须 > 0。
    static bool Allocate(char* buffer, int32_t size, int32_t* out_offset);

    // 释放一个此前由 Allocate 返回的载荷偏移，并与左右相邻空闲块合并。成功返回 true。
    static bool Free(char* buffer, int32_t payload_offset);

    // 返回当前堆的空间使用与碎片统计。
    static Stats GetStats(const char* buffer);

private:
    static bool Formatted(const char* buffer);
    static int32_t ReadI32(const char* buffer, int32_t off);
    static void WriteI32(char* buffer, int32_t off, int32_t v);
    static bool IsFree(const char* buffer, int32_t block_off);
    static int32_t BlockSize(const char* buffer, int32_t block_off);
    static void SetSizeFree(char* buffer, int32_t block_off, int32_t size);
    static int32_t NextOf(const char* buffer, int32_t block_off);
    static void SetNext(char* buffer, int32_t block_off, int32_t next);
    static bool UnlinkFree(char* buffer, int32_t block_off);  // 从空闲链表中摘除
};

}  // namespace sqlcompiler