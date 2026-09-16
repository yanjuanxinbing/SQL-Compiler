#pragma once

#include <list>
#include <unordered_map>

#include "storage/Replacer.h"

namespace sqlcompiler {

// =============================================================================
// FIFOReplacer —— 带「二次机会」增强的先进先出（FIFO）页替换策略
//
// 核心思想：纯 FIFO 淘汰「最早被 Unpin」的帧，即使该帧此间被反复访问过；当热点
//   工作集远小于缓冲池时会出现病态行为——热页仅仅因为先入队就被逐出。本类按经典
//   「二次机会 / 时钟」算法为每个候选帧附加一个参考位（reference bit）：
//     * Unpin(frame)：帧已在队列中（此前 Unpin 过且中间无 Pin）→ 置 ref_bit=true；
//       帧是全新的 → 追加到队尾且 ref_bit 记 false。
//     * Victim()：像时钟指针一样从队首扫描；队首 ref_bit=true 则清零并挪到队尾
//       继续找；队首 ref_bit=false 才淘汰该帧。
//   净效果：被反复 Unpin 的热页不断累积 ref_bit=true，从而抵御冷数据更替；真正冷
//   的帧最终以 ref_bit=false 抵达队首并被淘汰。
//
// 数据结构与复杂度：std::list<int> fifo_queue_ 为候选队列；position_map_ 保存帧在
//   队列中的迭代器，使 Pin/Victim 的定位删除为 O(1)；ref_bits_ 保存各候选帧的参考
//   位。Pin / Unpin 均摊 O(1)；Victim 最坏需轮转整圈（扫描步数见 .cpp 中的上界
//   推导），即 O(候选帧数)；Size 为 O(1)。
//
// 相对其它策略的取舍：
//   * 相比 LRU：不维护精确新近度，队列 + 位图的更新成本更低，但顺序只是近似；
//   * 相比 Clock：因按 frame_id 间接寻址而多维护一张哈希表，内存略高，好处是与
//     LRUReplacer 结构同构、便于对照教学；
//   * 相比 LRU-K：只有一位参考位，抗「顺序扫描冲击」能力弱。
//
// 必须维持的不变量：
//   * 一个帧在 fifo_queue_ 中至多出现一次；position_map_ 与 ref_bits_ 的键集和
//     fifo_queue_ 的元素一一对应，三者必须同增同删。若发生漂移，Victim 会漏掉该帧
//     或留下「幽灵节点」，长时间运行的会话里队列会无限增长。
//   * 被 Pin 的帧必须从候选队列中移除，并连同其参考位一起丢弃。
//
// 有意未实现（V1 不做）：冷热分区（2Q / CAR 家族）——保留独立的「热区」前缀、只从
// 「冷区」后缀淘汰；若未来负载倾斜度上升，值得补充。
// =============================================================================
// 带二次机会的 FIFO 替换策略实现。
//
// 语义速览：Unpin(f) 首次使 f 入队（追加队尾，参考位 0），再次 Unpin 则把参考位置 1
// 且不改变其队列位置（时钟旋转由 Victim 完成）；Pin(f) 把 f 连同参考位一起移出候选
// 集；Victim() 从队首轮转扫描，淘汰第一个参考位为 0 的候选帧。
class FIFOReplacer : public Replacer {
public:
    // 构造。
    // @param num_frames 缓冲池帧总数（合法帧号为 [0, num_frames)）。
    // @note 该值仅用于 Victim 估算扫描步数上界（见 .cpp），本实现不做范围校验。
    explicit FIFOReplacer(size_t num_frames);

    // 析构：队列、位置表与参考位表随成员自动释放，无额外资源需要回收。
    ~FIFOReplacer() override;

    // 把帧移出可淘汰候选集（该帧正被使用），并清除其参考位。
    // @param frame_id 缓冲池帧号（帧数组下标，非 page_id）。
    // @note 幂等：帧不在候选队列中时直接返回，不做任何修改。
    void Pin(int frame_id) override;

    // 把帧标记为可淘汰候选：首次入队追加到队尾（参考位记 0），已在队列中的重复
    // Unpin 仅把参考位置 1（二次机会），队列位置保持不变。
    // @param frame_id 缓冲池帧号。
    // @note 幂等（不会重复入队）；但重复 Unpin 会持续置位参考位，使该帧每次至少
    //       多撑一轮扫描才可能被淘汰。
    void Unpin(int frame_id) override;

    // 按二次机会规则选出并移除一个可淘汰帧。
    // @param frame_id 输出参数，成功时写入被淘汰的帧号。
    // @return true  —— 已从候选队列移除，*frame_id 有效；
    //         false —— 候选队列为空，*frame_id 不会被写入。
    // @note 允许传入 nullptr：此时仍完成淘汰并返回 true，仅跳过写回帧号。
    bool Victim(int* frame_id) override;

    // 当前可淘汰的帧数量。
    // @return 候选帧个数；被 Pin 的帧不计入。
    size_t Size() const override;

private:
    // 缓冲池帧总数；Victim 以 num_frames_ + 1 作为扫描步数上界（推导见 .cpp）。
    size_t num_frames_;
    // 候选帧队列：队首＝最早入队（优先淘汰），队尾＝最新入队（二次机会后回到此处）。
    std::list<int> fifo_queue_;
    // fifo_queue_ 中的帧号 → 其在队列中的迭代器，用于 Pin 的 O(1) 删除。
    // 有意保存迭代器而非下标，使 list 的 erase 保持 O(1)。
    std::unordered_map<int, std::list<int>::iterator> position_map_;
    // 每个候选帧的二次机会参考位。始终与 position_map_ 同步：一个帧出现在 ref_bits_
    // 当且仅当它也在 position_map_ 中；Pin/Victim 时两者一并清除。
    std::unordered_map<int, bool> ref_bits_;
};

}  // namespace sqlcompiler
