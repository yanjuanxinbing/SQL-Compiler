#pragma once

#include <list>
#include <unordered_map>

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
//   * Each Unpin(frame) either sets ref_bit=true (frame already in the queue
//     from a prior Unpin) or appends the frame to the back with ref_bit=false
//     (frame is brand-new to the queue).
//   * Victim walks the queue from the front like a clock hand. If the front
//     frame has ref_bit=true, we clear the bit, push the frame to the back,
//     and continue. If ref_bit=false, we evict that frame.
//
// Net effect: a frame whose Unpin is called repeatedly (a hot page) accumulates
// ref_bits=true and survives cold churn, while truly cold frames eventually
// reach the front with ref_bit=false and are evicted.
//
// Possible future refinement (intentionally NOT implemented in V1): hot/cold
// partition (a.k.a. 2Q / CAR family) — keep a separate "hot" prefix and only
// evict from the "cold" suffix. Worth doing if workload skew grows.
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
    std::list<int> fifo_queue_;
    // Mirror of the frames currently sitting in fifo_queue_, kept for O(1)
    // erase on Pin. We intentionally track iterators (not raw indices) so
    // list splice/erase remains O(1).
    std::unordered_map<int, std::list<int>::iterator> position_map_;
    // Second-chance reference bit for each unpinned frame. Always in sync with
    // position_map_: a frame is present in ref_bits_ iff it is also present in
    // position_map_. We erase from both maps together on Pin/Victim.
    std::unordered_map<int, bool> ref_bits_;
};

}  // namespace sqlcompiler
