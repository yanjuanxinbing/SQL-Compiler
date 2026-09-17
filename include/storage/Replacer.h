#pragma once

#include <cstddef>

namespace sqlcompiler {

// 页替换策略抽象接口，供BufferPoolManager在缓存已满时选择被淘汰的帧（frame）
// frame_id 指缓冲池内部帧数组的下标，而非page_id
class Replacer {
public:
    virtual ~Replacer() = default;

    // 记录某个frame正被使用（pin），使其不可作为淘汰候选
    virtual void Pin(int frame_id) = 0;

    // 将某个frame标记为可被淘汰候选（unpin，即引用计数归零后调用）
    virtual void Unpin(int frame_id) = 0;

    // 按替换策略选出一个可淘汰的frame_id，写入frame_id并返回true；
    // 若当前没有可淘汰的frame，返回false
    virtual bool Victim(int* frame_id) = 0;

    // 当前可被淘汰的frame数量
    virtual size_t Size() const = 0;
};

}  // namespace sqlcompiler
