#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "index/IndexKey.h"
#include "index/PageGuard.h"
#include "storage/BufferPoolManager.h"
#include "storage_engine/Tuple.h"
#include "storage_engine/Value.h"

namespace sqlcompiler {

// 磁盘 B+Tree：节点即 4KB 页面，全部经 BufferPoolManager 访问。
//
// 结构约定：
//   - 叶子节点存 (key, RID)，按 (key, rid) 严格全序；叶子间由 next/prev 双向链
//     串起来，供范围扫描顺序推进。
//   - 内部节点的分隔键是右子树第一条记录的完整 (key, rid)：
//     child[i] 内所有项 < sep[i] <= child[i+1] 内所有项。
//   - 空树的根是一个空叶子页（而非 INVALID），插入路径无需处理「树为空」特例。
//   - 根页 id 恒定不变（见 SplitRoot），因此目录里存的 root_page_id 永不失效。
//
// 明确不做（TODO）：删除只打墓碑，不做节点合并与再平衡，大量删除后会留下半空
// 节点。这个取舍换来的是删除路径无需同时 pin 多个兄弟节点，也就不存在缓冲池
// 耗尽与 pin 泄漏的风险。
class BPlusTree {
public:
    BPlusTree(BufferPoolManager* bpm, std::vector<ValueType> key_schema,
              bool is_unique, page_id_t root_page_id);

    // 新建一棵空树（分配根叶子页）。失败返回 nullptr。
    static std::unique_ptr<BPlusTree> Create(BufferPoolManager* bpm,
                                             std::vector<ValueType> key_schema,
                                             bool is_unique);
    // 打开已有的树
    static std::unique_ptr<BPlusTree> Open(BufferPoolManager* bpm,
                                           std::vector<ValueType> key_schema,
                                           bool is_unique,
                                           page_id_t root_page_id);
    // 回收整棵树占用的所有页面
    static void Destroy(BufferPoolManager* bpm, page_id_t root_page_id);

    // 插入。唯一索引上若键已存在则返回 false（不插入）。
    bool Insert(const IndexKey& key, const RID& rid);

    // 只读点查：返回该键的第一条 RID；不存在时返回无效 RID。
    // 唯一性校验依赖它「绝不写入」的性质。
    RID FindFirst(const IndexKey& key) const;

    // 删除指定 (key, rid)。找不到返回 false。
    bool Delete(const IndexKey& key, const RID& rid);

    page_id_t GetRootPageId() const { return root_page_id_; }
    bool IsUnique() const { return is_unique_; }
    const std::vector<ValueType>& GetKeySchema() const { return key_schema_; }

    // 范围扫描游标：从 >= 某键的位置开始，沿叶子链顺序推进。
    class Cursor {
    public:
        Cursor(const BPlusTree* tree, page_id_t leaf_pid, size_t index);
        // 取出当前项并前进；到末尾返回 false。
        bool Next(IndexKey* key, RID* rid);

    private:
        bool LoadLeaf(page_id_t pid);

        const BPlusTree* tree_ = nullptr;
        page_id_t leaf_pid_ = INVALID_PAGE_ID;
        size_t index_ = 0;
        // 当前叶子页的物化内容。一次读入、逐条产出，避免每次 Next 都重解页面。
        std::vector<std::pair<IndexKey, RID>> entries_;
        page_id_t next_leaf_ = INVALID_PAGE_ID;
    };

    // 定位到第一个 >= key 的位置
    std::unique_ptr<Cursor> LowerBound(const IndexKey& key) const;
    // 定位到最左端
    std::unique_ptr<Cursor> Begin() const;

private:
    // 只读下降：沿内部节点走到 (key, rid) 所属的叶子。查找与删除用。
    //
    // 不在页头维护 parent_page_id：父指针一旦落盘，就存在「分裂后忘记更新子
    // 节点父指针」的一致性风险；而插入采用下降途中预分裂，压根不需要回溯父节点。
    page_id_t FindLeafPage(const IndexKey& key, const RID& rid) const;
    page_id_t LeftmostLeafPage() const;

    // 分裂 parent 的孩子 child，并把分隔键插入 parent。
    // 前置条件：parent 已由调用方保证有足够空间容纳一个分隔键（预分裂不变式）。
    bool SplitChild(page_id_t parent_pid, page_id_t child_pid, size_t reserve);

    // 分裂根页。根页 id 恒定不变：把根的内容搬到一个新页，再把根改写成只有
    // 两个孩子的内部节点。这样目录里的 root_page_id 一经写入就永不失效，
    // 省掉「根分裂后必须回写目录元数据」这条极易遗漏的一致性路径。
    bool SplitRoot();

    BufferPoolManager* bpm_;
    std::vector<ValueType> key_schema_;
    bool is_unique_;
    page_id_t root_page_id_;
};

}  // namespace sqlcompiler
