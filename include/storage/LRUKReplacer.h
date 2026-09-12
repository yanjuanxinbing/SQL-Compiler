#pragma once

#include <cstddef>
#include <deque>
#include <set>
#include <unordered_map>

#include "storage/Replacer.h"

namespace sqlcompiler {

// LRU-K 替换策略：记录每个帧最近 K 次访问的时间戳，淘汰时选择「第 K 次访问最久远」
// 的帧（即过去 K 次访问间隔里最「冷」的）。
//
// 与普通 LRU 相比，LRU-K 更稳健地抵御「顺序扫描冲击」：只被访问 1 次(K 未满)的页面
// 属「非相关(uncorrelated)」，优先被替换；而累计访问达到 K 次的「相关(correlated)」
// 热页会受到保护，不会因一次连续扫描就被逐出缓存。
//
// 数据结构（两套集合）：
//   * recent_store_ ：访问次数 < K 的可淘汰帧，按「首次访问时间」FIFO 排序。
//   * historic_store_：访问次数 >= K 的可淘汰帧，按「第 K 次访问时间戳」升序，
//       最小的（第 K 次访问最久远）最先被淘汰。
// 淘汰顺序：先淘汰 recent_store_（未满 K 次的引用更可能是一次性访问），
// 为空时才淘汰 historic_store_（取第 K 次访问最旧的帧）。K=1 时退化为 LRU。
class LRUKReplacer : public Replacer {
public:
    // num_frames：有效帧范围 [0, num_frames)。
    // k：历史窗口（K 值）；0 或 1 时按 1 处理（即退化为 LRU）。
    LRUKReplacer(size_t num_frames, size_t k = 2);
    ~LRUKReplacer() override;

    // 记录一次访问引用（每个 Pin 即一次使用），并将帧移出可淘汰集合。
    void Pin(int frame_id) override;

    // 使帧成为可淘汰候选（未在使用中），按当前访问计数与第 K 次访问时间入列。
    void Unpin(int frame_id) override;

    // 按 LRU-K 选出一个可淘汰的 frame；集合为空返回 false。
    bool Victim(int* frame_id) override;

    // 当前可淘汰的帧数量。
    size_t Size() const override;

private:
    // 排序节点：k_distance 为该帧入列时的「第 K 次访问时间戳」。
    struct Node {
        size_t k_distance;
        int frame_id;
        bool operator<(const Node& o) const {
            if (k_distance != o.k_distance) return k_distance < o.k_distance;
            return frame_id < o.frame_id;
        }
    };

    size_t num_frames_;
    size_t k_;                   // K 值（实际生效 >= 1）
    size_t curr_size_ = 0;       // 可淘汰帧数
    size_t timestamp_ = 0;       // 单调递增全局时间戳（模拟「访问时刻」）

    std::set<Node> recent_store_;    // 访问次数 < K 的可淘汰帧
    std::set<Node> historic_store_;  // 访问次数 >= K 的可淘汰帧

    std::unordered_map<int, size_t> access_count_;                // 累计访问次数
    std::unordered_map<int, std::deque<size_t>> history_;         // 最近 K 次访问时间戳
    std::unordered_map<int, bool> in_historic_;  // 帧当前位于哪个集合（true=historic）
};

}  // namespace sqlcompiler