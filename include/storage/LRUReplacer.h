#pragma once

// =============================================================================
// LRUReplacer —— 最近最少使用（Least Recently Used）页替换策略
//
// 核心思想：用「帧最近一次变为可淘汰（Unpin）的时刻」近似访问新近度，
//           淘汰时总选最久未被使用者。
//
// 数据结构与复杂度：std::list<int> lru_list_ 按新近度排序（队尾＝最近使用，
//   队首＝最久未用）；std::unordered_map<int, list::iterator> position_map_
//   记录每帧在链表中的位置，把「按帧号删除链表节点」从 O(n) 降为 O(1)。
//   Pin / Unpin / Victim / Size 均摊 O(1)，额外空间 O(候选帧数)。
//
// 相对其它策略的取舍：
//   * 相比 FIFO：多一张哈希表的内存与常数开销，换来对重复访问热页的识别能力；
//   * 相比 Clock：精确的新近度需要频繁改写链表与哈希，常数更大；
//   * 相比 LRU-K：不保留 K 次访问历史，抗「顺序扫描冲击」能力弱，胜在简单直观。
//
// 必须维持的不变量：
//   * 一个帧在 lru_list_ 中至多出现一次：position_map_ 的键集与 lru_list_ 的元素
//     严格一一对应、同增同删。一旦漂移，Unpin 会把同一帧重复插入，Pin/Victim
//     也会删错节点。
//   * 被 Pin 的帧必须从候选集移除：候选集内只放「可淘汰帧」，因此 Size() 是
//     可淘汰帧数而非帧总数。
//
// 本实现不校验 frame_id 范围（任意 int 都会被当作候选帧记录）；接口契约见
// storage/Replacer.h。
// =============================================================================

#include <list>
#include <unordered_map>

#include "storage/Replacer.h"

namespace sqlcompiler {

// 最近最少使用（LRU）替换策略实现。
//
// 语义速览：Unpin(f) 把 f 追加到链表尾部（视为最近使用）；Victim() 取链表头部
// （最久未使用）的帧淘汰；Pin(f) 把 f 从链表摘除，使其不再被选中。
class LRUReplacer : public Replacer {
public:
    // 构造。
    // @param num_frames 缓冲池帧总数（合法帧号为 [0, num_frames)）。
    // @note 该值仅被保存：候选集规模完全由 lru_list_ 决定，本实现也不做范围校验，
    //       故 num_frames_ 不参与任何运算。
    explicit LRUReplacer(size_t num_frames);

    // 析构：链表与哈希表随成员自动释放，无额外资源需要回收。
    ~LRUReplacer() override;

    // 把帧移出可淘汰候选集（该帧正被使用）。
    // @param frame_id 缓冲池帧号（帧数组下标，非 page_id）。
    // @note 幂等：帧不在候选集中时（从未 Unpin、已被 Victim 取走、或重复 Pin）
    //       直接返回，不做任何修改。
    void Pin(int frame_id) override;

    // 把帧加入可淘汰候选集，并置于链表尾部（视为最近使用）。
    // @param frame_id 缓冲池帧号。
    // @note 幂等：帧已在候选集中时（连续两次 Unpin 且中间无 Pin）直接返回，
    //       既不重复插入，也不刷新其新近度。
    void Unpin(int frame_id) override;

    // 选出并移除一个可淘汰帧：链表头部即最久未被使用者。
    // @param frame_id 输出参数，成功时写入被淘汰的帧号。
    // @return true  —— 已从候选集移除，*frame_id 有效；
    //         false —— 候选集为空，*frame_id 不会被写入。
    // @note 允许传入 nullptr：此时仍完成淘汰并返回 true，仅跳过写回帧号。
    bool Victim(int* frame_id) override;

    // 当前可淘汰的帧数量。
    // @return 候选帧个数；被 Pin 的帧不计入。
    size_t Size() const override;

private:
    // 缓冲池帧总数；由构造保存，当前实现不参与运算（候选集规模由 lru_list_ 决定）。
    size_t num_frames_;
    // 候选帧链表：队尾＝最近 Unpin，队首＝最久未 Unpin，Victim 淘汰队首。
    std::list<int> lru_list_;
    // frame_id → 该帧在 lru_list_ 中的迭代器；键集与链表元素一一对应，用于 O(1) 定位删除。
    std::unordered_map<int, std::list<int>::iterator> position_map_;
};

}  // namespace sqlcompiler
