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
    // TODO: 建议维护一个双向链表(lru_list_)记录unpinned frame的访问顺序，
    // 链表尾部为最近使用，头部为最久未使用（或反之，保持一致即可），
    // 再配合position_map_做到O(1)定位，从而Pin/Unpin都能O(1)从链表中移除节点
    size_t num_frames_;
    std::list<int> lru_list_;
    std::unordered_map<int, std::list<int>::iterator> position_map_;
};

}  // namespace sqlcompiler
