#pragma once

#include <list>
#include <unordered_map>

#include "storage/Replacer.h"

namespace sqlcompiler {

// 最近最少使用（LRU）替换策略实现
class LRUReplacer : public Replacer {
public:
    explicit LRUReplacer(size_t num_frames);
    ~LRUReplacer() override;

    void Pin(int frame_id) override;
    void Unpin(int frame_id) override;
    bool Victim(int* frame_id) override;
    size_t Size() const override;

private:
    size_t num_frames_;
    std::list<int> lru_list_;
    std::unordered_map<int, std::list<int>::iterator> position_map_;
};

}  // namespace sqlcompiler
