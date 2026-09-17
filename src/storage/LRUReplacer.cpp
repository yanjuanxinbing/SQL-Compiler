#include "storage/LRUReplacer.h"

namespace sqlcompiler {

LRUReplacer::LRUReplacer(size_t num_frames) : num_frames_(num_frames) {
}

LRUReplacer::~LRUReplacer() {
}

void LRUReplacer::Pin(int frame_id) {
    auto it = position_map_.find(frame_id);
    if (it == position_map_.end()) return;
    lru_list_.erase(it->second);
    position_map_.erase(it);
}

void LRUReplacer::Unpin(int frame_id) {
    auto it = position_map_.find(frame_id);
    if (it != position_map_.end()) return;  // already in the list
    lru_list_.push_back(frame_id);
    auto inserted = lru_list_.end();
    --inserted;
    position_map_[frame_id] = inserted;
}

bool LRUReplacer::Victim(int* frame_id) {
    if (lru_list_.empty()) return false;
    int victim = lru_list_.front();
    lru_list_.pop_front();
    position_map_.erase(victim);
    if (frame_id) *frame_id = victim;
    return true;
}

size_t LRUReplacer::Size() const {
    return lru_list_.size();
}

}  // namespace sqlcompiler