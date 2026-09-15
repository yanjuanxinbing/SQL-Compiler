#include "storage/FIFOReplacer.h"

namespace sqlcompiler {

FIFOReplacer::FIFOReplacer(size_t num_frames)
    : num_frames_(num_frames),
      ring_(num_frames, -1),
      ref_(num_frames, 0),
      position_(num_frames, -1) {
}

FIFOReplacer::~FIFOReplacer() {
}

void FIFOReplacer::Pin(int frame_id) {
    if (frame_id < 0 || static_cast<size_t>(frame_id) >= num_frames_) return;
    int slot = position_[frame_id];
    if (slot == -1) return;
    // 把该槽清空，ref_ 也清零（即便 Pin 后不再 Unpin，ref 也不应残留）。
    ring_[slot] = -1;
    ref_[slot] = 0;
    position_[frame_id] = -1;
    --size_;
}

void FIFOReplacer::Unpin(int frame_id) {
    if (frame_id < 0 || static_cast<size_t>(frame_id) >= num_frames_) return;
    int slot = position_[frame_id];
    if (slot != -1) {
        // 帧已经在候选队列里（一次 Unpin 之后又 Unpin，没穿插过 Pin）。
        // 这正是 second-chance 算法的核心触发：标记 ref_bit=true，让
        // Victim() 走到该槽位时给该帧一次"免死金牌"。**位置保持不变**：
        // 不把帧挪到新位置——clock 算法的旋转由 Victim() 自然完成，
        // 这里强行挪动反而会破坏队列的 FIFO 顺序假设。
        ref_[slot] = 1;
        return;
    }
    // 第一次进入候选队列的新帧：插到 clock_hand_ 当前指向的槽位，ref_bit=0。
    // 注意：clock_hand_ 推进后，Victim 从新位置开始扫，因此这个新帧会排到
    // 「当时已在 ring 中、且比它更早插入」的所有帧之后才被考虑——等价于
    // 原 std::list::push_back 的「插到队尾」语义。
    int f = frame_id;
    ring_[clock_hand_] = f;
    ref_[clock_hand_] = 0;
    position_[f] = static_cast<int>(clock_hand_);
    clock_hand_ = (clock_hand_ + 1) % num_frames_;
    ++size_;
}

bool FIFOReplacer::Victim(int* frame_id) {
    if (size_ == 0) return false;

    // 时钟扫描：每个 slot 最多访问两次（一次清 ref、二次驱逐）。
    // num_frames_ * 2 + 1 的上界足够覆盖「扫一圈清 ref + 再扫一圈命中」的最坏路径。
    const size_t max_iters = num_frames_ * 2 + 1;
    for (size_t iter = 0; iter < max_iters; ++iter) {
        int f = ring_[clock_hand_];
        if (f == -1) {
            // 空槽：直接前进；不消耗 ref 配额。
            clock_hand_ = (clock_hand_ + 1) % num_frames_;
            continue;
        }
        if (ref_[clock_hand_]) {
            // 免死一次：清零 ref_bit，前进，继续轮转。
            ref_[clock_hand_] = 0;
            clock_hand_ = (clock_hand_ + 1) % num_frames_;
            continue;
        }
        // 真·淘汰：把该 slot 与 frame_id 的反向索引一起摘掉。
        position_[f] = -1;
        ring_[clock_hand_] = -1;
        if (frame_id) *frame_id = f;
        clock_hand_ = (clock_hand_ + 1) % num_frames_;
        --size_;
        return true;
    }
    // 兜底：理论上走不到这里（max_iters 内必然有一个 ref_bit=false 的帧
    // 被命中）。若真走到这一步，说明 num_frames_ 与 ring 实际规模对不上或
    // 容器漂移，直接取当前 hand 处驱逐以避免无限循环。
    if (!ring_.empty() && ring_[clock_hand_] != -1) {
        int f = ring_[clock_hand_];
        position_[f] = -1;
        ring_[clock_hand_] = -1;
        if (frame_id) *frame_id = f;
        clock_hand_ = (clock_hand_ + 1) % num_frames_;
        --size_;
        return true;
    }
    return false;
}

size_t FIFOReplacer::Size() const {
    return size_;
}

}  // namespace sqlcompiler
