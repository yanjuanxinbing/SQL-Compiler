#include "storage/LRUKReplacer.h"

namespace sqlcompiler {

LRUKReplacer::LRUKReplacer(size_t num_frames, size_t k)
    : num_frames_(num_frames), k_(k == 0 ? 1 : k) {
}

LRUKReplacer::~LRUKReplacer() {
}

void LRUKReplacer::Pin(int frame_id) {
    // 越界帧号直接忽略：后续按帧号索引 access_count_/history_ 等表，必须先做范围检查。
    if (frame_id < 0 || static_cast<size_t>(frame_id) >= num_frames_) return;
    // 若帧当前可淘汰（在某个集合中），先用「入列时的第 K 次访问时间」把它移除。
    // 注意此处尚未记录本次访问，history_ 队首仍是入列时用的那个时间戳，
    // 因此可以直接用它还原出集合节点的 k_distance。
    auto iti = in_historic_.find(frame_id);
    if (iti != in_historic_.end()) {
        size_t old_dist = history_[frame_id].front();  // 入列时的 k_distance
        if (iti->second) {
            historic_store_.erase(Node{old_dist, frame_id});
        } else {
            recent_store_.erase(Node{old_dist, frame_id});
        }
        in_historic_.erase(iti);
        --curr_size_;  // 与 in_historic_ 同步递减，维持 curr_size_ == in_historic_.size()
    }
    // 记录本次访问引用：追加时间戳，仅保留最近 K 次。
    // timestamp_ 全局单调递增，故 history_ 中的时间戳天然按新旧有序，
    // 队首即「第 K 近」的那次访问（未满 K 次时为首次访问）。
    ++timestamp_;
    ++access_count_[frame_id];
    auto& hist = history_[frame_id];
    hist.push_back(timestamp_);
    if (hist.size() > k_) hist.pop_front();
}

void LRUKReplacer::Unpin(int frame_id) {
    // 越界帧号直接忽略（同 Pin）。
    if (frame_id < 0 || static_cast<size_t>(frame_id) >= num_frames_) return;
    if (in_historic_.find(frame_id) != in_historic_.end()) return;  // 已可淘汰：幂等
    // 防御：理论上 Unpin 前必有一次 Pin（访问计数 > 0）。若为 0，补记一次引用。
    if (access_count_[frame_id] == 0) {
        ++timestamp_;
        access_count_[frame_id] = 1;
        history_[frame_id].push_back(timestamp_);
    }
    size_t cnt = access_count_[frame_id];
    // 排序键：history_ 队首＝访问次数满 K 时的「第 K 近一次访问」时间戳，
    // 未满 K 时即首次访问时间戳——两者都越旧越先被淘汰。
    size_t dist = history_[frame_id].front();  // 第 K 次（或首次，未满 K）访问时间
    if (cnt >= k_) {
        in_historic_[frame_id] = true;
        historic_store_.insert(Node{dist, frame_id});
    } else {
        in_historic_[frame_id] = false;
        recent_store_.insert(Node{dist, frame_id});
    }
    ++curr_size_;
}

bool LRUKReplacer::Victim(int* frame_id) {
    if (curr_size_ == 0) return false;
    // 先淘汰「非相关」的近期帧（未满 K 次，多为一次性访问），以保护相关热页。
    std::set<Node>* sel = &recent_store_;
    if (recent_store_.empty()) {
        sel = &historic_store_;
    }
    auto it = sel->begin();
    Node v = *it;
    sel->erase(it);
    int fid = v.frame_id;
    // 淘汰即遗忘：连同访问计数与历史一并清除，日后该帧若被重新取用，
    // 会从 0 次访问重新计数（先落入 recent_store_，需重新积累 K 次才受保护）。
    in_historic_.erase(fid);
    history_.erase(fid);
    access_count_.erase(fid);
    --curr_size_;  // 与 in_historic_ 同步递减
    if (frame_id) *frame_id = fid;
    return true;
}

size_t LRUKReplacer::Size() const {
    return curr_size_;
}

}  // namespace sqlcompiler