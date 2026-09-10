#pragma once

#include <cstdint>

#include "storage/Page.h"

namespace sqlcompiler {

// ============================================================================
// B+Tree 节点页面布局
//
// 设计取舍：
//   - 节点即 4KB 页面，与 TableHeap 的槽位页同构（slot 目录从前往后长，
//     变长键从页尾向前长），这样变长复合键与二分查找可以共存：slot 目录定长
//     可随机寻址，键内容变长存在记录区。
//   - page_type 显式落盘。全零页的 page_type == 0，与任何合法节点都不同，
//     因此不会像 TableHeap 早期版本那样把零页误认成「next 指向 page 0」的
//     合法节点而形成自指环。
// ============================================================================

namespace bptree {

enum class PageType : uint8_t {
    kUninitialized = 0,
    kInternal = 1,
    kLeaf = 2,
};

// ---- 公共页头（16 字节，与 TableHeap 的 kHeaderBytes 对齐）----
// offset 0  : uint8  page_type
// offset 1  : uint8  is_root
// offset 2  : uint16 reserved
// offset 4  : uint16 key_count
// offset 6  : uint16 free_space_offset   (记录区起点，初始 PAGE_SIZE)
// offset 8  : int32  parent_page_id
// offset 12 : int32  reserved
constexpr size_t kHeaderBytes = 16;

// ---- 叶子页 ----
// offset 16 : int32 next_leaf_page_id
// offset 20 : int32 prev_leaf_page_id
// offset 24 : slot 目录，每项 16 字节：
//               uint32 key_off | uint16 key_len | uint16 flags
//               int32  rid_page_id | int32 rid_slot_num
constexpr size_t kLeafHeaderBytes = 24;
constexpr size_t kLeafSlotBytes = 16;
constexpr int kMaxLeafSlots =
    static_cast<int>((PAGE_SIZE - kLeafHeaderBytes) / kLeafSlotBytes);  // 254

// ---- 内部页 ----
// offset 16 : int32 first_child_page_id
// offset 20 : slot 目录，每项 20 字节：
//               uint32 key_off | uint16 key_len | uint16 flags | int32 child_page_id
//               int32  rid_page_id | int32 rid_slot_num
//             语义：分隔键是右子树第一条记录的完整 (key, rid)，
//                   即 child[i] 中所有项 < sep[i] <= child[i+1] 中所有项。
//
//             分隔键必须带上 RID：非唯一索引里同一个 key 的多条记录可能被分裂
//             到相邻两页，若分隔键只有 key，下降时无法区分该去左页还是右页，
//             重复键就会插到错误的叶子上、破坏全序。
constexpr size_t kInternalHeaderBytes = 20;
constexpr size_t kInternalSlotBytes = 20;
constexpr int kMaxInternalSlots =
    static_cast<int>((PAGE_SIZE - kInternalHeaderBytes) / kInternalSlotBytes);  // 203

// slot 的 flags 位
constexpr uint16_t kSlotDeleted = 0x1;  // 墓碑：删除不做合并，仅打标

// 单个索引键序列化后的字节上限。取页容量的 1/4，保证一页至少能放下 4 个键，
// 否则分裂会无法收敛（分裂后仍放不下就会无限分裂）。
constexpr size_t kMaxKeyBytes = PAGE_SIZE / 4;

}  // namespace bptree
}  // namespace sqlcompiler
