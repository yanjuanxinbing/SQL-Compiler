#pragma once

#include <cstddef>
#include <vector>

#include "storage/Replacer.h"

namespace sqlcompiler {

// FIFO replacement policy with second-chance (clock-style) enhancement.
//
// Plain FIFO evicts the frame that was unpinned earliest, even if it has been
// re-accessed many times in between. This causes pathological behavior on
// workloads with a hot working set that is much smaller than the buffer pool:
// the hot frames get evicted just because they happened to enter the queue
// first.
//
// This class augments plain FIFO with a per-frame reference bit, exactly as
// in the classical second-chance / clock algorithm:
//
//   * Each Unpin(frame) either sets ref_bit=true (frame already in the ring)
//     or appends the frame at the clock hand's current position with
//     ref_bit=false (frame is brand-new to the ring).
//   * Victim walks the ring from the clock hand like a clock hand. If the
//     frame under the hand has ref_bit=true, we clear the bit and advance;
//     otherwise we evict that frame.
//
// 内部用稀疏数组实现：ring_[slot] 存该槽位的 frame_id（-1 表示空槽），
// position_[frame_id] 反查 frame_id 所在的 slot 以支持 O(1) Pin/erase。
// clock_hand_ 是单向推进的环形游标，Unpin 与 Victim 共用它定位下一位置。
//
// Net effect: a frame whose Unpin is called repeatedly (a hot page) accumulates
// ref_bits=true and survives cold churn, while truly cold frames eventually
// reach the hand with ref_bit=false and are evicted.
class FIFOReplacer : public Replacer {
public:
    explicit FIFOReplacer(size_t num_frames);
    ~FIFOReplacer() override;

    void Pin(int frame_id) override;
    void Unpin(int frame_id) override;
    bool Victim(int* frame_id) override;
    size_t Size() const override;

private:
    size_t num_frames_;
    // ring_[slot] = frame_id at that slot, -1 if empty.
    // slot 索引 ∈ [0, num_frames_)，与 frame_id 独立（slot 由 clock 顺序决定）。
    std::vector<int> ring_;
    // ref_[slot] = reference bit for the frame currently at that slot.
    std::vector<char> ref_;
    // position_[frame_id] = 该 frame_id 所在的 slot，-1 表示不在 ring 中。
    // 提供 O(1) 的 Pin / Unpin-再标记 路径，避免扫整个 ring。
    std::vector<int> position_;
    // 时钟指针：同时承担「新帧入环插入点」与「Victim 扫描起点」两个角色。
    size_t clock_hand_ = 0;
    // 当前 ring 中真实持有的 frame 数量（即 ref_ 中有效位之外的活跃元素数）。
    size_t size_ = 0;
};

}  // namespace sqlcompiler
