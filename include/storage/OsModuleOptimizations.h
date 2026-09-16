#pragma once

// =====================================================================
// 操作系统（页面管理）模块 — 分阶段系统优化整合层
// =====================================================================
// 本文件是「所有 OS 模块优化代码」的统一落地载体：
//   * 每个阶段都提供 优化版（Optimized）+ 基线版（Legacy，优化前实现）孪生，
//     由 RunBenchmarks() 在同一进程内实测前后指标，保证对比数据真实可比；
//   * 每个阶段用 banner 注释标记优化内容与实施时间点；
//   * 优化版被生产模块（DiskManager / LRUReplacer / PageAllocator）调用，
//     正确性由既有 storage_ut 单测锁定；基准测试仅输出指标、不做性能断言，
//     避免在 CI/单测里产生不稳定判定。
//
// 阶段总览（时间点为实施日 2026-09-12）：
//   Stage 1（内存/校验热路径）：CRC32 惰性表 → constexpr 常量表，消除
//     首次调用的运行时建表延迟与多线程首次并发的数据竞争。
//   Stage 2（缓存替换热路径）：LRU 的 Unpin/Pin 由「find+operator[] 两次
//     哈希」改为「find+emplace/直接复用迭代器」单次哈希。
//   Stage 3（页内分配热路径）：first-fit 分配/释放的侵入式记账与头部读取
//     局部化，减少每次遍历对 buffer 的重复访问。
//
// ---- 必须维持的不变量与约束 ----
//   * 优化版与基线版必须语义等价：Crc32 / LegacyCrc32 对同一输入必须逐位相同；
//     LruSet 与 LruSetLegacy* 对同一操作序列必须保持相同的候选集内容与顺序；
//   * 优化版即生产实现，被 DiskManager / LRUReplacer / PageAllocator 调用，正确性由
//     storage_ut 单测锁定；RunBenchmarks() 只打印指标、不做性能断言（避免 CI 抖动）；
//   * 各函数为无共享可变状态的纯内存路径，不涉及 I/O，也不改变任何持久化布局；
//   * 基准指标在同一进程内采集，故优化版与基线版的调用顺序、预热状态会影响数值，
//     改动基准编排会破坏可比性。
// =====================================================================

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>

namespace sqlcompiler::osopt {

// ---- Stage 1（2026-09-12）：CRC32 ----
// 计算 CRC32（反射多项式 0xEDB88320，IEEE 802.3 标准；初值与终值均为 0xFFFFFFFF 异或）。
// @param data 输入缓冲区首地址；len 为 0 时允许为 nullptr（不访问），否则须可读 len 字节。
// @param len  输入字节数（单位：字节），可为 0。
// @return 32 位 CRC 值；len 为 0 时返回 0x00000000（初值 0xFFFFFFFF 与终值异或抵消）。
// @note 查找表为编译期 constexpr 常量（见 .cpp 的 kCrcTable），无惰性建表、无静态可变
//       状态，因此首次调用无额外延迟，且多线程首次并发调用无数据竞争。
uint32_t Crc32(const char* data, size_t len);          // 优化版：constexpr 表
// 基线版参照实现：首次调用时用静态标志惰性构建同一张表（保留原实现的数据竞争）。
// @param data / @param len / @return 语义与 Crc32 完全相同（同一多项式、同一表内容）。
// @note 仅供 RunBenchmarks() 做吞吐对比，生产路径不要使用；其静态表构建无同步保护。
uint32_t LegacyCrc32(const char* data, size_t len);    // 基线版：惰性建表

// ---- Stage 2（2026-09-12）：LRU 容器 ----
// 自包含的 LRU 候选集（list + position_map），供基准与 LRUReplacer 共用。
// 约定：list 自队首（front）到队尾（back）为「最近使用 → 最不常使用」，淘汰取 front；
//       不变量：pos 的键集与 list 的元素集合始终一一对应（键不存在 ⇔ 不在候选集中）。
struct LruSet {
    std::list<int> list;  // 候选帧序列；front = 最近使用，back = 最不常使用（淘汰端）
    std::unordered_map<int, std::list<int>::iterator> pos;  // frame_id → list 中对应结点

    // 把 frame_id 从候选集中移除（Pin 表示该帧已被占用，不再作为淘汰候选）。
    // @param frame_id 帧号（非负整数），允许尚未入候选集。
    // @return 无。
    // @note 幂等：不在候选集中时直接返回，不改变任何状态；实现在一次 find 内完成擦除。
    void Pin(int frame_id);    // 优化版：单次哈希
    // 把 frame_id 加入候选集（Unpin 表示该帧已释放，可被淘汰）；重复加入不生效。
    // @param frame_id 帧号（非负整数），允许重复调用。
    // @return 无。
    // @note 幂等：已存在时直接返回；新帧置于队尾（最不常使用端）。
    void Unpin(int frame_id);  // 优化版：单次哈希
    // 淘汰一个候选帧：取队首（最不常使用）并从候选集中移除。
    // @param frame_id 输出参数，成功时写入被淘汰帧号；可为 nullptr（仅执行淘汰）。
    // @return true  —— 候选集非空，已淘汰并列写出 frame_id（若指针非空）；
    //         false —— 候选集为空，无帧可淘汰，frame_id 不被写入。
    bool Victim(int* frame_id);
    // 当前候选帧数量（单位：个），即 list 的元素个数。
    // @return 候选集大小；无副作用。
    size_t Size() const { return list.size(); }
};
// 基线版 Pin：语义与 LruSet::Pin 相同，但实现多一次哈希（供 ops/s 对比）。
// @param s 目标候选集（引用，原地修改）；@param frame_id 帧号。
// @return 无。
void LruSetLegacyPin(LruSet& s, int frame_id);     // 基线版：两次哈希
// 基线版 Unpin：语义与 LruSet::Unpin 相同，但用 operator[] 再哈希一次插入迭代器。
// @param s 目标候选集（引用，原地修改）；@param frame_id 帧号。
// @return 无。
void LruSetLegacyUnpin(LruSet& s, int frame_id);   // 基线版：两次哈希

// ---- Stage 3（2026-09-12）：页内分配器快路径探针 ----
// 仅用于对 first-fit 遍历成本做量化对比：返回一次 Allocate 需访问的块头次数。
// 两个函数都会先按 payload_bytes / heads 重建同一份「交错空闲块」布局再统计，
// 因此可比较；它们不调用 PageAllocator，只在相同的 buffer 格式上模拟遍历。
// @param buffer        测试用缓冲区首地址（须可读写、长度 ≥ payload_bytes）；nullptr 返回 0。
// @param payload_bytes 请求的载荷字节数（> 0），探针取其大于单块容量以保证遍历到底。
// @param heads         布局中交错空闲块的个数（> 0），即遍历上界。
// @return 遍历过程中访问块头的次数（单位：次，越小越好）；buffer 为空时返回 0。
size_t Stage3ProbeLegacyAccesses(char* buffer, int32_t payload_bytes, int heads);
// 优化版探针：每轮循环用一次 8 字节读同时取出 size 与 next，故访问次数约为基线的一半。
// @param buffer / @param payload_bytes / @param heads / @return 同 Stage3ProbeLegacyAccesses。
// @note 该探针只量化「读块头的次数」这一相对指标，不代表真实耗时；生产侧同源改动见
//       PageAllocator.cpp 的 Allocate / Free。
size_t Stage3ProbeFastAccesses(char* buffer, int32_t payload_bytes, int heads);

// ---- 统一基准：输出各阶段前后指标 ----
// 输出格式稳定，供生成《性能对比报告》采集。
// @return 无返回值；所有结果以 printf 打到 stdout（Stage1 CRC 吞吐、Stage2 LRU 吞吐、
//         Stage3 分配遍历访问次数，以及一致性 PASS/FAIL 标记）。
// @note 只采集与打印指标，不做任何性能断言、不抛异常；需遵守上面「同进程对比」的
//       编排约束，否则数值不可比。
void RunBenchmarks();

}  // namespace sqlcompiler::osopt