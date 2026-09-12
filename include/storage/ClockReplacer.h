#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "storage/Replacer.h"

namespace sqlcompiler {

// 时钟（Clock，二次机会 Second-Chance）替换策略实现。
// 相对 LRU 无需哈希定位，只用一个环形指针 + 每帧一"参考位(access bit)"，
// 是接近 LRU 效果而成本更贴近操作系统中缓存置换实现的经典算法。
//
// 语义约定（与 LRU/FIFO 的 Replacer 接口保持一致）：
//   - Pin(frame_id)   ：该帧正被使用，移出候选集（不可被淘汰）。参考位保留。
//   - Unpin(frame_id) ：该帧可被淘汰，加入候选集，并把参考位置 1（刚被使用，
//                       享有一次"二次机会"）。
//   - Victim()        ：沿环形指针扫描，参考位 1 的清零放行（给机会并前移指针）；
//                       参考位 0 的即为淘汰对象，取出后移出候选集。
// 最坏情形下 Victim 需全扫两圈（首圈把所有参考位清零），平均摊还开销低于
// LRU 的链表定位，代价是替换顺序并非严格时间序（近似 LRU）。
class ClockReplacer : public Replacer {
public:
    explicit ClockReplacer(size_t num_frames);
    ~ClockReplacer() override;

    void Pin(int frame_id) override;
    void Unpin(int frame_id) override;
    bool Victim(int* frame_id) override;
    size_t Size() const override;

private:
    size_t num_frames_;
    std::vector<uint8_t> ref_bit_;      // 参考位：1 = 二次机会
    std::vector<uint8_t> in_replacer_;  // 是否处于候选集（可淘汰）
    size_t count_ = 0;                  // 候选集帧数（in_replacer_ 之和）
    size_t hand_ = 0;                   // 时钟指针，指向下一个被检查的帧
};

}  // namespace sqlcompiler