#include "storage/LRUReplacer.h"

namespace sqlcompiler {

LRUReplacer::LRUReplacer(size_t num_frames) : num_frames_(num_frames) {
}

LRUReplacer::~LRUReplacer() {
}

void LRUReplacer::Pin(int frame_id) {
    // 单次哈希定位链表节点；不在候选集则直接返回（重复 Pin / 未 Unpin 场景的幂等处理）。
    auto it = position_map_.find(frame_id);
    if (it == position_map_.end()) return;
    // 链表节点与哈希表项必须成对删除，维持「键集与链表元素一一对应」的不变量。
    lru_list_.erase(it->second);
    position_map_.erase(it);
}

// Stage2（2026-09-12）：Unpin 由「find+operator[] 两次哈希」改为「emplace 单次
// 哈希直接取回迭代器」，削减缓存替换热路径的哈希访问。
void LRUReplacer::Unpin(int frame_id) {
    auto it = position_map_.find(frame_id);
    if (it != position_map_.end()) return;  // already in the list
    lru_list_.push_back(frame_id);
    position_map_.emplace(frame_id, --lru_list_.end());
}

bool LRUReplacer::Victim(int* frame_id) {
    // 候选集空则无可淘汰帧，直接失败（此时不写 *frame_id，保留调用方原值）。
    if (lru_list_.empty()) return false;
    // 队首即最久未被 Unpin 的候选帧，取出即完成淘汰，无需再做哈希定位。
    int victim = lru_list_.front();
    lru_list_.pop_front();
    position_map_.erase(victim);
    // frame_id 可能为 nullptr：淘汰照常完成，仅跳过写回帧号。
    if (frame_id) *frame_id = victim;
    return true;
}

size_t LRUReplacer::Size() const {
    return lru_list_.size();
}

}  // namespace sqlcompiler