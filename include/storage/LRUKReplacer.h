#pragma once

// =============================================================================
// LRUKReplacer —— LRU-K 页替换策略
//
// 核心思想：记录每个帧最近 K 次访问的时间戳，淘汰时选择「第 K 次访问最久远」的帧
//   （即过去 K 次访问间隔里最「冷」的），而不是像 LRU 那样只看最近一次引用。
//   相对于普通 LRU，它更稳健地抵御「顺序扫描冲击」：只被访问过、次数未满 K 的页面
//   属「非相关（uncorrelated）」引用，优先被替换；累计访问达到 K 次的「相关
//   （correlated）」热页受到保护，不会因一次连续扫描就被逐出缓存。
//
// 数据结构（两套有序集合 + 三张按帧号索引的表）：
//   * recent_store_   ：访问次数 < K 的可淘汰帧，按「历史中最早那次保留的访问时间」
//                       排序（近似 FIFO），淘汰时优先从此集合取；
//   * historic_store_ ：访问次数 >= K 的可淘汰帧，按「第 K 次访问时间」升序，最久远
//                       者最先被淘汰；
//   * access_count_（累计访问次数）、history_（最近 K 次访问时间戳的双端队列）、
//     in_historic_（帧当前位于哪个集合，兼作「是否在候选集中」的标志）。
//   淘汰顺序：recent_store_ 非空则取其中最早者，为空才退到 historic_store_ 取首元素。
//
// 复杂度：Pin / Unpin 各含一次 set 插入或删除（O(log n)）加少量哈希访问；Victim 为
//   O(log n)（取集合首元素并删除）；额外空间 O(候选帧数 × K)。
//
// 必须维持的不变量：
//   * in_historic_ 的键集恰为「当前可淘汰帧」，其规模与 curr_size_ 一致，且一个帧至多
//     出现在一个集合中；集合内节点的 k_distance 必须等于该帧入列时 history_ 的队首——
//     Pin 正是靠 history_[frame_id].front() 还原该键来删除节点，一旦不一致就会删不掉，
//     在集合里留下无法被 Pin 摘除的陈旧候选。
//   * 被 Pin 的帧必须从所属集合与 in_historic_ 中移除。
//   * K 的退化：k 为 0 或 1 时按 1 处理；此时任何一次访问后计数都已 >= K，所有候选帧
//     都进入 historic_store_（recent_store_ 不再被用到），退化为按最近一次访问时间
//     排序的 LRU。
// =============================================================================

#include <cstddef>
#include <deque>
#include <set>
#include <unordered_map>

#include "storage/Replacer.h"

namespace sqlcompiler {

// LRU-K 替换策略实现。
//
// 语义速览（核心思想、数据结构与不变量见文件顶部 banner）：
//   Pin(f)   ：记一次访问引用（时间戳 +1 压入 history_，仅保留最近 K 次），并把 f
//              移出当前可淘汰集合；
//   Unpin(f) ：把 f 放回候选集，按访问计数是否达到 K 决定入 recent_store_
//              （计数 < K）还是 historic_store_（计数 >= K）；
//   Victim() ：优先取 recent_store_ 中最早入列者，为空时取 historic_store_ 中第 K 次
//              访问最久远者。
class LRUKReplacer : public Replacer {
public:
    // 构造。
    // @param num_frames 有效帧范围 [0, num_frames)；越界帧号在 Pin/Unpin 中被忽略。
    // @param k 历史窗口（K 值）；0 或 1 时按 1 处理（退化为 LRU，见 banner 的 K 退化说明）。
    // @note 构造仅保存初值，不预分配任何按帧号的容器（各表按需懒创建）。
    LRUKReplacer(size_t num_frames, size_t k = 2);

    // 析构：各容器随成员自动释放，无额外资源需要回收。
    ~LRUKReplacer() override;

    // 记录一次访问引用（每个 Pin 即一次使用），并把该帧移出可淘汰集合。
    // @param frame_id 缓冲池帧号（帧数组下标，非 page_id）。
    // @note 移除节点时用 history_ 队首还原入列时的 k_distance——入列后该帧未再被访问，
    //       故队首与入列值一致；越界帧号被直接忽略，不做任何记账。
    void Pin(int frame_id) override;

    // 把帧放回可淘汰集合（未在使用中），按访问计数与第 K 次访问时间入列。
    // @param frame_id 缓冲池帧号。
    // @note 幂等：帧已在候选集中时直接返回，既不重复入列也不刷新其排序键；若该帧此前
    //       从未被 Pin（访问计数为 0），会补记一次引用作为防御（见 .cpp 说明）。
    void Unpin(int frame_id) override;

    // 按 LRU-K 规则选出一个可淘汰帧：优先 recent_store_（非相关帧），其次
    // historic_store_（相关帧中第 K 次访问最久远者）。
    // @param frame_id 输出参数，成功时写入被淘汰的帧号。
    // @return true  —— 已从候选集移除，*frame_id 有效；
    //         false —— 候选集为空（curr_size_ == 0），*frame_id 不会被写入。
    // @note 允许传入 nullptr：仍完成淘汰并返回 true，仅跳过写回帧号；被淘汰帧的访问
    //       计数与历史会一并清除，日后重新取用时按 0 次访问重新计数。
    bool Victim(int* frame_id) override;

    // 当前可淘汰的帧数量。
    // @return 候选帧个数（即 curr_size_）；被 Pin 的帧不计入。
    size_t Size() const override;

private:
    // 有序集合的排序节点：k_distance 为该帧入列时的「第 K 次访问时间戳」（未满 K 次
    // 时即其首次访问时间戳），frame_id 仅用于同 k_distance 时打破平局，保证 std::set
    // 的比较满足严格弱序、淘汰结果确定。
    struct Node {
        size_t k_distance;
        int frame_id;
        bool operator<(const Node& o) const {
            if (k_distance != o.k_distance) return k_distance < o.k_distance;
            return frame_id < o.frame_id;
        }
    };

    size_t num_frames_;          // 有效帧范围 [0, num_frames_)；仅用于 Pin/Unpin 的范围检查
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