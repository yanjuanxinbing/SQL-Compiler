#include "storage/ClockReplacer.h"

namespace sqlcompiler {

ClockReplacer::ClockReplacer(size_t num_frames)
    : num_frames_(num_frames),
      ref_bit_(num_frames, 0),
      in_replacer_(num_frames, 0) {
}

ClockReplacer::~ClockReplacer() {
}

void ClockReplacer::Pin(int frame_id) {
    if (frame_id < 0 || static_cast<size_t>(frame_id) >= num_frames_) return;
    size_t i = static_cast<size_t>(frame_id);
    if (in_replacer_[i] != 0) {
        in_replacer_[i] = 0;
        if (count_ > 0) --count_;
    }
    // 被 pin 的帧视为正在使用，移出候选集；参考位保留供下次 Unpin 直接置 1。
}

void ClockReplacer::Unpin(int frame_id) {
    if (frame_id < 0 || static_cast<size_t>(frame_id) >= num_frames_) return;
    size_t i = static_cast<size_t>(frame_id);
    if (in_replacer_[i] == 0) {
        in_replacer_[i] = 1;
        ++count_;
    }
    // 刚被使用/解 pin：赋予一次二次机会。
    ref_bit_[i] = 1;
}

bool ClockReplacer::Victim(int* frame_id) {
    if (count_ == 0) return false;
    // 最坏需两圈：首圈把仍为 1 的参考位清零，次圈即可命中一个 0 位并淘汰。
    size_t max_steps = num_frames_ * 2;
    for (size_t step = 0; step < max_steps; ++step) {
        size_t i = hand_;
        hand_ = (hand_ + 1) % num_frames_;
        if (in_replacer_[i] == 0) continue;  // 已 pin，不在候选，直接跳过
        if (ref_bit_[i] != 0) {
            ref_bit_[i] = 0;  // 二次机会：清零并继续，指针已前移
            continue;
        }
        // 参考位为 0 → 确定为淘汰对象。
        in_replacer_[i] = 0;
        --count_;
        if (frame_id) *frame_id = static_cast<int>(i);
        return true;
    }
    return false;  // count_>0 时两圈内必命中，此分支仅防逻辑漂移
}

size_t ClockReplacer::Size() const {
    return count_;
}

}  // namespace sqlcompiler