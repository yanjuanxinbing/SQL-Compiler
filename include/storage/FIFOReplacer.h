#pragma once

#include <list>
#include <unordered_set>

#include "storage/Replacer.h"

namespace sqlcompiler {

// 先进先出（FIFO）替换策略实现
class FIFOReplacer : public Replacer {
public:
    explicit FIFOReplacer(size_t num_frames);
    ~FIFOReplacer() override;

    void Pin(int frame_id) override;
    void Unpin(int frame_id) override;
    bool Victim(int* frame_id) override;
    size_t Size() const override;

private:
    // TODO: fifo_queue_按frame被Unpin的顺序排队，
    // Victim()从队首开始查找第一个仍在unpinned_set_中的frame_id；
    // Pin()将其从unpinned_set_中移除（不必立刻从队列中删除，Victim时跳过即可）
    size_t num_frames_;
    std::list<int> fifo_queue_;
    std::unordered_set<int> unpinned_set_;
};

}  // namespace sqlcompiler
