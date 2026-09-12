#include "storage/FIFOReplacer.h"

namespace sqlcompiler {

FIFOReplacer::FIFOReplacer(size_t num_frames) : num_frames_(num_frames) {
}

FIFOReplacer::~FIFOReplacer() {
}

void FIFOReplacer::Pin(int frame_id) {
    // 关键：Pin 必须把 frame 从 fifo_queue_ 里同步 erase，否则
    // Unpin 后再 Pin 会在队列里留下"幽灵节点"——Victim 跳过了它，
    // 但它一直占着 list 节点，长时间运行的会话里队列会无限增长。
    // 借 position_map_ 做到 O(1) 删除（参考 LRUReplacer 的同款做法）。
    //
    // Second-chance：ref_bits_ 必须同步清理——一旦帧被 Pin，它就不再
    // 处于可淘汰候选集合，连同它的 reference bit 一并丢弃。
    auto it = position_map_.find(frame_id);
    if (it == position_map_.end()) return;
    fifo_queue_.erase(it->second);
    position_map_.erase(it);
    ref_bits_.erase(frame_id);
}

void FIFOReplacer::Unpin(int frame_id) {
    auto it = position_map_.find(frame_id);
    if (it != position_map_.end()) {
        // 帧已经在候选队列里（一次 Unpin 之后又 Unpin，没穿插过 Pin）。
        // 这正是 second-chance 算法的核心触发：标记 ref_bit=true，让
        // Victim() 走到队首时给该帧一次"免死金牌"。**位置保持不变**：
        // 不把帧挪到队尾——clock 算法的旋转由 Victim() 自然完成，
        // 这里强行挪动反而会破坏队列的 FIFO 顺序假设。
        ref_bits_[frame_id] = true;
        return;
    }
    // 第一次进入候选队列的新帧：ref_bit 初始化为 false，等下次 Unpin
    // 或 Victim 来更新。
    fifo_queue_.push_back(frame_id);
    auto inserted = fifo_queue_.end();
    --inserted;
    position_map_[frame_id] = inserted;
    ref_bits_[frame_id] = false;
}

bool FIFOReplacer::Victim(int* frame_id) {
    // Second-chance / clock algorithm:
    //   - 取队首 f；
    //   - 若 ref_bit=true，清零并把 f 移到队尾（"免死一次"），继续；
    //   - 若 ref_bit=false，淘汰 f。
    // 上界 num_frames_ + 1：最坏情况下队列里所有帧都被打了 ref_bit=true，
    // 一个轮次内每个都会被清零并推回队尾；下一轮队首 ref_bit 一定是
    // false，必然命中淘汰路径。
    if (fifo_queue_.empty()) return false;

    const size_t max_iters = num_frames_ + 1;
    for (size_t iter = 0; iter < max_iters && !fifo_queue_.empty(); ++iter) {
        int f = fifo_queue_.front();
        fifo_queue_.pop_front();

        // pop_front 让 position_map_ 里指向 f 旧位置的 iterator 失效了；
        // 我们要么驱逐 f（erase 旧 entry），要么把 f 重新挂到队尾（覆盖为
        // 新位置的 iterator）。两种情况下旧 entry 都不再被引用。
        auto pos_it = position_map_.find(f);
        auto rb_it = ref_bits_.find(f);
        bool ref = (rb_it != ref_bits_.end()) && rb_it->second;

        if (ref) {
            // 免死一次：清零 ref_bit，挂回队尾，继续轮转。
            ref_bits_[f] = false;
            fifo_queue_.push_back(f);
            auto inserted = fifo_queue_.end();
            --inserted;
            // 覆盖旧 iterator，避免悬空引用（旧 iterator 已被 pop_front
            // 失效；如果保留下来，下次 erase / push_back 时就会用错位置）。
            position_map_[f] = inserted;
            continue;
        }

        // 真·淘汰：从三个容器里一并摘掉。
        if (pos_it != position_map_.end()) position_map_.erase(pos_it);
        ref_bits_.erase(f);
        if (frame_id) *frame_id = f;
        return true;
    }

    // 兜底：理论上走不到这里（max_iters 内必然有一个 ref_bit=false 的帧
    // 浮到队首）。若真走到这一步，说明 num_frames_ 与队列实际规模对不上
    // 或三个容器漂移，直接取队首驱逐以避免无限循环。
    if (!fifo_queue_.empty()) {
        int f = fifo_queue_.front();
        fifo_queue_.pop_front();
        position_map_.erase(f);
        ref_bits_.erase(f);
        if (frame_id) *frame_id = f;
        return true;
    }
    return false;
}

size_t FIFOReplacer::Size() const {
    return fifo_queue_.size();
}

}  // namespace sqlcompiler