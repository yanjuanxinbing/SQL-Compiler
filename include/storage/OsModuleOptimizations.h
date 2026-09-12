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
// =====================================================================

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>

namespace sqlcompiler::osopt {

// ---- Stage 1（2026-09-12）：CRC32 ----
uint32_t Crc32(const char* data, size_t len);          // 优化版：constexpr 表
uint32_t LegacyCrc32(const char* data, size_t len);    // 基线版：惰性建表

// ---- Stage 2（2026-09-12）：LRU 容器 ----
// 自包含的 LRU 候选集（list + position_map），供基准与 LRUReplacer 共用。
struct LruSet {
    std::list<int> list;
    std::unordered_map<int, std::list<int>::iterator> pos;

    void Pin(int frame_id);    // 优化版：单次哈希
    void Unpin(int frame_id);  // 优化版：单次哈希
    bool Victim(int* frame_id);
    size_t Size() const { return list.size(); }
};
void LruSetLegacyPin(LruSet& s, int frame_id);     // 基线版：两次哈希
void LruSetLegacyUnpin(LruSet& s, int frame_id);   // 基线版：两次哈希

// ---- Stage 3（2026-09-12）：页内分配器快路径探针 ----
// 仅用于对 first-fit 遍历成本做量化对比：返回一次 Allocate 需访问的块头次数。
size_t Stage3ProbeLegacyAccesses(char* buffer, int32_t payload_bytes, int heads);
size_t Stage3ProbeFastAccesses(char* buffer, int32_t payload_bytes, int heads);

// ---- 统一基准：输出各阶段前后指标 ----
// 输出格式稳定，供生成《性能对比报告》采集。
void RunBenchmarks();

}  // namespace sqlcompiler::osopt