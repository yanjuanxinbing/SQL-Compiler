#pragma once

// =============================================================================
// Replacer — 帧替换策略抽象接口
//
// 职责：决定缓冲池「缓存已满时淘汰哪一个帧」。BufferPoolManager 在无空闲帧时调用
//       Victim() 索取候选帧，并在每次 pin 计数变化时通过 Pin()/Unpin() 同步候选集。
//
// 使用契约（由 BufferPoolManager 保证，实现者据此设计数据结构）：
//   * frame_id 是缓冲池内部帧数组 pages_ 的下标，取值 [0, pool_size_)——不是 page_id；
//   * Pin(frame_id) 表示该帧被占用（pin 计数 > 0），必须立即从候选集中移除；重复 Pin
//     同一帧必须幂等，不得报错、不得重复插入；
//   * Unpin(frame_id) 只在该帧 pin 计数归零时由 BPM 调用，表示它成为可淘汰候选；重复
//     Unpin 同一帧也必须幂等（不得产生重复候选）；
//   * Victim() 返回的帧必须从候选集中移除，且保证其未被 pin——实现者不得返回已 Pin
//     的帧，否则换出会写坏正在使用的页；
//   * 「键集与链表/队列元素一一对应」是各实现内部必须维持的不变量。
//
// 线程模型：接口不含任何同步手段，也不要求实现加锁；全部 Pin/Unpin/Victim 调用与
//   BufferPoolManager 的帧表操作同源（同一调用线程），并发安全由调用方保证。
//
// 实现者：LRUReplacer（最近最少使用）、FIFOReplacer（先进先出 + 二次机会）、
//   LRUKReplacer、ClockReplacer；由 BufferPoolManager 以 unique_ptr<Replacer> 持有，
//   故基类析构必须为虚函数（通过基类指针释放派生对象）。
// =============================================================================

#include <cstddef>

namespace sqlcompiler {

// 页替换策略抽象接口，供BufferPoolManager在缓存已满时选择被淘汰的帧（frame）
// frame_id 指缓冲池内部帧数组的下标，而非page_id
class Replacer {
public:
    // 虚析构：BufferPoolManager 通过 std::unique_ptr<Replacer> 持有派生实例。
    virtual ~Replacer() = default;

    // 记录某个frame正被使用（pin），使其不可作为淘汰候选
    // @param frame_id 帧号，合法值 [0, pool_size_)。
    // @note 幂等：帧不在候选集中时直接返回（重复 Pin / 尚未 Unpin 的场景都不报错）。
    virtual void Pin(int frame_id) = 0;

    // 将某个frame标记为可被淘汰候选（unpin，即引用计数归零后调用）
    // @param frame_id 帧号，合法值 [0, pool_size_)。
    // @note 幂等：帧已在候选集中时不重复插入；调用方保证只在 pin 计数归零时调用。
    virtual void Unpin(int frame_id) = 0;

    // 按替换策略选出一个可淘汰的frame_id，写入frame_id并返回true；
    // 若当前没有可淘汰的frame，返回false
    // @param frame_id 输出参数：成功时写入选中的帧号；可为 nullptr（此时只做淘汰动作，
    //                 不返回帧号）。失败时不写入，保留调用方原值。
    // @return true  —— 已淘汰并移出一个候选帧（其帧号已写入 *frame_id）；
    //         false —— 候选集为空（池内帧全被 pin 住），无帧可淘汰。
    // @note 取出的帧会从候选集中移除；BPM 随后调用 Pin() 标记其被占用（对已不在候选
    //       集中的帧是 no-op），该帧要重新成为候选须等下一次 pin 计数归零时的 Unpin()。
    virtual bool Victim(int* frame_id) = 0;

    // 当前可被淘汰的frame数量
    // @return 候选集中的帧数（已 Pin 的帧不计入）；无候选时为 0。
    virtual size_t Size() const = 0;
};

}  // namespace sqlcompiler
