#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "index/BPlusTreePage.h"
#include "index/IndexKey.h"
#include "index/PageGuard.h"
#include "storage/BufferPoolManager.h"
#include "storage_engine/Tuple.h"
#include "storage_engine/Value.h"

namespace sqlcompiler {

// Phase A 前向声明。
class Transaction;
class LogManager;

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
// 删除语义：Delete 沿下降路径记录「path stack」，到叶子后做一次 erase，再自底向上
// 走 path stack 修复 underflow：先尝试 redistribute（兄弟有富余），失败则 merge
// （把右兄弟并入当前节点，必要时递归向上）。空页通过 DeletePage 归还缓冲池；根
// 若是只剩一个孩子的内部节点，则把那个孩子搬进根页（root_page_id 不变，目录
// 元数据免维护）。所有写操作同时走 Phase A 的 undo 与 Phase B 的 WAL 钩子。
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

    // ---- Phase A：把当前事务挂到树上（写路径把 undo log 写进 txn）----
    // IndexMaintenance / DML 算子在写索引前调用一次。nullptr 表示隐式
    // auto-commit，无 undo。PageGuard 与 Transaction 二选一冲突时以本字段为准。
    void SetActiveTransaction(Transaction* txn) { active_txn_ = txn; }
    Transaction* GetActiveTransaction() const { return active_txn_; }

    // ---- Phase B：注入 LogManager，写索引时同时落 WAL ----
    void SetLogManager(LogManager* lm) { log_manager_ = lm; }
    LogManager* GetLogManager() const { return log_manager_; }

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

    // 删除再平衡（path-stack descent）所需的一组私有辅助。

    // 处理一层 underflow：parent_pid 的第 separator_index 个孩子已「失血」，
    // 若其左右兄弟有富余则 redistribute，否则 merge；合并后若 parent 也
    // underflow 则递归向上。root_page_id 之上不再回溯。失败（缓冲池耗尽等）
    // 返回 false，调用方负责判断是否要让整次 Delete 失败。
    bool RedistributeOrMerge(page_id_t parent_pid, size_t separator_index);

    // 物理合并：把 right_pid 的全部条目并入 left_pid，更新 left_pid 的页头
    // （key_count / first_child / 内部或叶子的指针）。不修改 parent 的分隔符
    // 也不释放 right_pid —— 由调用者（RedistributeOrMerge）负责后续：
    // 对叶子是修 next/prev 链，对内部节点是顺手做，对所有情况都是 DeletePage。
    // 对于内部节点，调用方必须把 parent 中分隔 left 与 right 的 (key, rid,
    // key_bytes) 传进来——它会出现在合并结果中（child = right.first_child），
    // 否则路径上的全序不变式被破坏。key_bytes 与 key_len 对应；key_len=0 表示
    // 无 key_bytes（罕见，例如空复合键）；仅 type == kInternal 时使用。
    // 返回 false 表示缓冲池耗尽或页损坏。
    bool MergeNodes(page_id_t left_pid, page_id_t right_pid, bptree::PageType type,
                    const IndexKey* parent_sep_key, const RID* parent_sep_rid,
                    const std::vector<char>* parent_sep_key_bytes);

    // 把 sibling 的一条 (key,rid) 转移到 deficient_leaf。sibling_is_left 为
    // true 表示 sibling 在 deficient 左侧：取 sibling 的最后一条移到 deficient
    // 头部，并把 parent 的对应分隔键改为 sibling 此刻的第一条（因为 right 的
    // 第一条已迁走）；为 false 则取 sibling 的第一条移到 deficient 尾部，并把
    // parent 的对应分隔键改为 deficient 此刻的最后一条（因为新条目落到
    // deficient，最左键变了）。parent_pid 与 separator_index 用来定位 parent
    // 中分隔 deficient 与 sibling 的那条 entry；leaf 链上的 next/prev 不变。
    bool RedistributeLeaf(page_id_t deficient_leaf, page_id_t sibling,
                          bool sibling_is_left, page_id_t parent_pid,
                          size_t separator_index);

    // 内部节点的 redistribute：把 sibling 的最末/最首 entry 转到 deficient，
    // 并把 parent 的 separator 通过 deficient/sibling 旋转。parent_pid 与
    // separator_index 用来定位 parent 中的分隔键以更新。
    bool RedistributeInternal(page_id_t deficient_internal, page_id_t sibling,
                              bool sibling_is_left, page_id_t parent_pid,
                              size_t separator_index);

    // 若 root 此刻是「只剩一个孩子的内部节点」，把那个孩子搬进 root 页本身，
    // 释放被搬走的子页。root_page_id 永不改变。
    bool CollapseRoot();

    BufferPoolManager* bpm_;
    std::vector<ValueType> key_schema_;
    bool is_unique_;
    page_id_t root_page_id_;
    Transaction* active_txn_ = nullptr;
    LogManager* log_manager_ = nullptr;  // Phase B：可选 WAL 写出器
};

}  // namespace sqlcompiler
