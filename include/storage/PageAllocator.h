#pragma once

#include <cstdint>

namespace sqlcompiler {

// =============================================================================
// PageAllocator — 页内 free-list 堆内存分配器
//
// ---- 组件职责 ----
// 页内内存分配器（PageAllocator）：
// 在给定的一块字节区域（通常是某个 Page 的数据区）内部实现一个 free-list 堆，
// 管理其中"记录/对象"空间的分配、释放与相邻块合并，并可通过 GetStats() 观测
// 空闲空间与碎片状况。设计目标：
//   - 页内（in-page）：直接作用于调用方提供的 char* 缓冲区，无额外全局状态；
//   - free-list 堆：分配用 first-fit，释放与相邻空闲块合并（左右双向）；
//   - 分配可回放：相同缓冲区大小 + 相同操作序列 ⇒ 布局确定（地址稳定）；
//   - 碎片率可观测：GetStats() 报告已用/空闲、最大连续块、外部碎片、头部开销。
//
// ---- 内存布局 ----
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
// ---- 不变量与约束 ----
//   * 缓冲区可能未按 4 字节对齐：字段读写一律走 memcpy（见 .cpp 的 ReadI32/WriteI32），
//     不得对缓冲区指针做类型化解引用或对齐假设；
//   * 块区自 kRegionHeader(16) 起，块尺寸恒为 kAlign(8) 的整数倍，块头 8B 计入 size；
//   * size_flag 的 bit0 是 free 标志，块字节数 = size_flag & ~7；同一时刻一个块要么带
//     free 标志并挂在空闲链表中，要么是已分配块（无 free 标志且不在链表中）；
//   * 空闲链表头存于区域头偏移 12 处，0 表示空链；链表只含空闲块，不含已分配块；
//   * 分配确定性：相同容量 + 相同操作序列 ⇒ 块布局与返回偏移完全可复现；
//   * 未初始化缓冲区 / 越界偏移 / 重复释放一律拒绝并返回失败，不得写坏缓冲区。
//
// ---- 与持久化格式的关系 ----
// 说明：本分配器作为"页内内存堆"原语，提供确定性分配与碎片观测；不替代
// TableHeap 的磁盘 slotted-page 格式（该格式是持久化布局，保持不变）。
// =============================================================================
class PageAllocator {
public:
    // 区域头字节数：magic / capacity / allocated_payload / free_head 四个 i32。
    // 不变量：Init() 的 size 参数必须 ≥ kRegionHeader + kBlockHeader，否则 Init 失败。
    static constexpr int32_t kRegionHeader = 16;  // 区域头字节（magic/capacity/al_payload/free_head）
    // 每个块（空闲或已分配）的固定块头字节数：size_flag(4) + next_free(4)；
    // 块尺寸 size 已含该 8B 头，故载荷可用字节数 = size - kBlockHeader。
    static constexpr int32_t kBlockHeader = 8;    // 每块头字节（size_flag / next_free）
    // 块尺寸与载荷偏移的对齐粒度（字节）；size_flag 低 3 位恒为 0（bit0 复用为 free 标志）。
    static constexpr int32_t kAlign = 8;          // 对齐粒度
    // 切分后允许保留的最小空闲块字节数（含块头）；剩余不足此值就不再切分，整块给出。
    static constexpr int32_t kMinBlock = kBlockHeader + kAlign;  // 最小可保留的空闲块=16
    // 区域魔数 "PAL1"，供 Formatted() 判定缓冲区是否已被 Init；未初始化的缓冲区不可使用。
    static constexpr uint32_t kMagic = 0x50414C31u;              // "PAL1"

    // 页内空间使用统计（用于碎片率等观测）
    struct Stats {
        int32_t capacity = 0;              // 区域总字节数
        int32_t allocated_bytes = 0;       // 已分配块占用字节（含各块 8B 头）
        int32_t free_capacity = 0;         // 空闲块占用字节（含各块 8B 头）
        int32_t usable_free = 0;           // 可实际使用的空闲载荷 =
                                          //   free_capacity - 8*num_free_blocks
        int32_t num_free_blocks = 0;       // 空闲链表中的块数量（单位：块）
        int32_t largest_free_block = 0;    // 最大连续空闲块（含头），单次最大可分配≈该值-8
        int32_t external_fragmentation = 0;// 无法合并成一次大块分配的空闲载荷 =
                                          //   usable_free - (largest_free_block - 8)
        int32_t header_overhead = 0;       // 16(区域头) + 8*num_free_blocks
    };
    // Stats 各字段单位均为字节（num_free_blocks 除外，单位为块），按块区布局计算：
    // 不变量：capacity = kRegionHeader + allocated_bytes + free_capacity
    //         usable_free = free_capacity - kBlockHeader * num_free_blocks
    //         header_overhead = kRegionHeader + kBlockHeader * num_free_blocks
    // 未初始化的缓冲区（Formatted() 为假）全部字段保持 0。

    // 在 buffer 的前 size 字节内初始化一个新堆，成功后整个块区被格式化为一个完整空闲块。
    // @param buffer 堆缓冲区首地址，调用方保证其可写；为 nullptr 时直接失败。
    // @param size   堆可用总字节数（含 16B 区域头），须 ≥ kRegionHeader + kBlockHeader(=24)；
    //                该值原样写入区域头的 capacity 字段，之后不再改变。
    // @return true  —— 初始化成功，堆可用（free_head 指向覆盖 [16, size) 的首个空闲块）；
    //         false —— buffer 为 nullptr 或 size 过小，此时缓冲区内容不作任何保证。
    // @note 本函数覆写区域头 16 字节与首块块头（共 24 字节），不清零其余字节（空闲块
    //       载荷内容无意义）；重复 Init 会按同一步骤重新覆盖，结果幂等。
    static bool Init(char* buffer, int32_t size);

    // 在堆内以 first-fit 分配一块不小于 size 字节的载荷。
    // @param buffer     由 Init 初始化过的缓冲区；magic 不匹配（未初始化）时直接失败。
    // @param size       请求的载荷字节数，须 > 0；内部按 kAlign 向上取整为 al = align(size)。
    // @param out_offset 输出参数，成功时写入载荷起始偏移（= 块偏移 + kBlockHeader）；
    //                   可为 nullptr，此时仅完成分配与记账、不返回偏移。
    // @return true  —— 找到块字节数 ≥ kBlockHeader + al 的空闲块并完成分配；
    //         false —— buffer 为空 / 未格式化 / size ≤ 0 / 对齐后长度溢出 / 无满足块。
    // @note 切分策略：仅当剩余 blk - need ≥ kMinBlock 时切出尾部空闲块并压回链表头，
    //       否则整块交给调用方，避免留下无法再利用的小碎片。
    // @note 确定性：始终从链表头开始 first-fit 扫描，新切出的空闲块压回头部；相同容量
    //       加相同操作序列，块布局与返回偏移完全可复现（可重放）。
    static bool Allocate(char* buffer, int32_t size, int32_t* out_offset);

    // 释放一个此前由 Allocate 返回的载荷偏移，并与左右相邻空闲块双向合并后挂回链表头。
    // @param buffer         由 Init 初始化过的缓冲区；magic 不匹配时直接失败。
    // @param payload_offset Allocate 返回的载荷偏移（载荷首地址，非块偏移），须落在
    //                       [kRegionHeader + kBlockHeader, capacity) 区间内并指向某块载荷。
    // @return true  —— 释放成功（已与相邻空闲块合并并挂到空闲链表头）；
    //         false —— buffer 为空 / 未格式化 / 偏移越界（含 ≥ capacity）/ 目标块已带
    //                  free 标志（重复释放）/ 块尺寸小于 kBlockHeader（头部损坏）。
    // @note 重复释放为防御性分支：命中已空闲块时只返回 false，不改动缓冲区任何字段。
    // @note 记账：allocated_payload 减去本次释放块的有效载荷（size - kBlockHeader）。
    static bool Free(char* buffer, int32_t payload_offset);

    // 统计当前堆的空间使用与碎片状况，供观测/调优使用；不修改缓冲区。
    // @param buffer 由 Init 初始化过的缓冲区；为 nullptr 或 magic 不匹配时返回全零统计。
    // @return Stats 结构，字段单位与含义见上；无效缓冲区时返回默认构造的全 0 结果。
    // @note 统计只采信空闲链表上的块（逐块累加 size 与计数）；已分配块字节数由
    //       「块区总字节 - 空闲块字节」反推，因此链表若被人为破坏，统计值也会随之失真。
    static Stats GetStats(const char* buffer);

private:
    // 校验区域头魔数，判断缓冲区是否已被 Init（唯一的「有效堆」判定入口）。
    static bool Formatted(const char* buffer);
    // 按主机字节序从 buffer + off 读取 i32；用 memcpy 规避未对齐访问，off 由调用方保证在界内。
    static int32_t ReadI32(const char* buffer, int32_t off);
    // 按主机字节序把 v 写入 buffer + off；同样走 memcpy，不做对齐假设。
    static void WriteI32(char* buffer, int32_t off, int32_t v);
    // 读取 block_off 处块头的 bit0，判断该块当前是否空闲。
    static bool IsFree(const char* buffer, int32_t block_off);
    // 读取块字节数（含 8B 块头）：size_flag & ~7，已屏蔽 free 标志位。
    static int32_t BlockSize(const char* buffer, int32_t block_off);
    // 写入「尺寸 | free 标志」，即把一个块标记为占用 size 字节的空闲块。
    static void SetSizeFree(char* buffer, int32_t block_off, int32_t size);
    // 读取空闲块块头中的 next_free 链接（仅空闲块有效，0 表示链尾）。
    static int32_t NextOf(const char* buffer, int32_t block_off);
    // 写入空闲块块头中的 next_free 链接。
    static void SetNext(char* buffer, int32_t block_off, int32_t next);
    // 从空闲链表中摘除：单链 O(空闲块数) 扫描，改头指针或前驱的 next_free；
    // 未命中返回 false（不修改链表）。
    static bool UnlinkFree(char* buffer, int32_t block_off);  // 从空闲链表中摘除
};

}  // namespace sqlcompiler