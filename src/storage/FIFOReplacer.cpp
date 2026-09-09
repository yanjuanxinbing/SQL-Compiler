#include "storage/FIFOReplacer.h"

namespace sqlcompiler {

FIFOReplacer::FIFOReplacer(size_t num_frames) : num_frames_(num_frames) {
}

FIFOReplacer::~FIFOReplacer() {
}

void FIFOReplacer::Pin(int frame_id) {
    unpinned_set_.erase(frame_id);
}

void FIFOReplacer::Unpin(int frame_id) {
    if (unpinned_set_.count(frame_id) > 0) return;
    unpinned_set_.insert(frame_id);
    fifo_queue_.push_back(frame_id);
}

bool FIFOReplacer::Victim(int* frame_id) {
    while (!fifo_queue_.empty()) {
        int candidate = fifo_queue_.front();
        fifo_queue_.pop_front();
        auto it = unpinned_set_.find(candidate);
        if (it == unpinned_set_.end()) {
            // Stale entry — skip
            continue;
        }
        unpinned_set_.erase(it);
        if (frame_id) *frame_id = candidate;
        return true;
    }
    return false;
}

size_t FIFOReplacer::Size() const {
    return unpinned_set_.size();
}

}  // namespace sqlcompiler