#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "storage/Replacer.h"

namespace sqlcompiler {

// =============================================================================
// ClockReplacer —— 时钟（Clock / 二次机会 Second-Chance）页替换策略
//
// 核心思想：用一个环形指针（时钟指针 hand_）配合每帧一位「参考位」近似 LRU：
//   Unpin 时把帧纳入候选集并置参考位为 1；Victim 从指针处向前扫描，参考位为 1 的
//   帧清零后放行（给一次「二次机会」），参考位为 0 的帧即被淘汰。
//
// 数据结构与复杂度：两个按帧号直接寻址的数组——ref_bit_（参考位）与 in_replacer_
//   （是否处于候选集），外加候选计数 count_ 与指针 hand_。因帧号即数组下标，
//   Pin / Unpin / Size 均为 O(1)，无需哈希定位与链表；Victim 最坏扫两圈（首圈把
//   仍为 1 的参考位清零）为 O(num_frames_)，平均摊还开销低于 LRU 的链表定位。
//
// 相对其它策略的取舍：
//   * 相比 LRU：不需哈希表与链表，内存与常数更小，但替换顺序只是近似时间序（参考
//     位仅 1 位，无法区分多个「新近」层级），代价是命中率可能略低；
//   * 相比 FIFO：多了参考位与「放行一轮」的机制，能避免热页仅因先入队而被逐出。
//
// 必须维持的不变量：
//   * count_ 恒等于 in_replacer_ 中 1 的个数（Unpin 递增、Pin/Victim 递减）；
//     Victim 以 count_ == 0 作为「无候选帧」的快速判定，二者不得漂移；
//   * 一个帧至多占一个候选槽位（in_replacer_[i] 是 0/1 标志），重复 Unpin 不会重复
//     计数；被 Pin 的帧必须移出候选集，而参考位可以保留（供下次 Unpin 直接置 1）；
//   * hand_ 恒落在 [0, num_frames_) 内（每次推进取模）。num_frames_ 为 0 时所有
//     Pin/Unpin 均被范围检查拒绝、Victim 因 count_ == 0 提前返回，不会触发取模。
// =============================================================================
class ClockReplacer : public Replacer {
public:
    // 构造：按帧总数预分配两个标记数组，参考位与候选标记均初始化为 0（初始无候选帧）。
    // @param num_frames 缓冲池帧总数，合法帧号为 [0, num_frames)。
    explicit ClockReplacer(size_t num_frames);

    // 析构：两个数组随成员自动释放，无额外资源需要回收。
    ~ClockReplacer() override;

    // 把帧移出可淘汰候选集（该帧正被使用）。
    // @param frame_id 缓冲池帧号（帧数组下标，非 page_id）。
    // @note 幂等：帧不在候选集时仅保留其参考位、不做修改；越界帧号被直接忽略。
    void Pin(int frame_id) override;

    // 把帧纳入可淘汰候选集，并把参考位置 1（赋予一次二次机会）。
    // @param frame_id 缓冲池帧号。
    // @note 幂等：已在候选集时不会重复计数；越界帧号被直接忽略。
    void Unpin(int frame_id) override;

    // 沿时钟指针扫描，淘汰一个参考位为 0 的候选帧。
    // @param frame_id 输出参数，成功时写入被淘汰的帧号。
    // @return true  —— 已从候选集移除，*frame_id 有效；
    //         false —— 候选集为空（count_ == 0），*frame_id 不会被写入。
    // @note 允许传入 nullptr：仍完成淘汰并返回 true，仅跳过写回帧号；count_ > 0 时
    //       不会因「扫满两圈仍未命中」而返回 false（该兜底分支仅防内部状态漂移）。
    bool Victim(int* frame_id) override;

    // 当前可淘汰的帧数量。
    // @return 候选帧个数（即 count_）；被 Pin 的帧不计入。
    size_t Size() const override;

private:
    size_t num_frames_;                 // 缓冲池帧总数，两个标记数组的长度与取模基数
    std::vector<uint8_t> ref_bit_;      // 参考位：1 = 二次机会
    std::vector<uint8_t> in_replacer_;  // 是否处于候选集（可淘汰）
    size_t count_ = 0;                  // 候选集帧数（in_replacer_ 之和）
    size_t hand_ = 0;                   // 时钟指针，指向下一个被检查的帧
};

}  // namespace sqlcompiler