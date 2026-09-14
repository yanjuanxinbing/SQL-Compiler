#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

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
// 并发模型（Phase 1：乐观页级并发，取代树级粗粒度锁）：
//   - 读路径（FindFirst / LowerBound / Begin / 游标 Next）逐页取共享闩，记录路径
//     上每页版本号（页头 offset 12 保留字段），遍历结束后校验版本未变，否则重启。
//     读不阻塞写、无锁等待——冲突只导致本次遍历作废重来。
//   - 写路径（Insert / Delete）乐观下降 + 校验后，只对目标叶子取独占写闩；叶子
//     放不下时走慢路径：带写闩下降、沿途预分裂（分裂用「先 pin 后加闩」保证父+子
//     同时持闩而不违反 BPM 锁序），结构变更在写闩内完成并自增版本号发布。
//   - 每次页内容修改都必须自增版本号（版本号单调，杜绝 ABA）；版本号只用于进程内
//     并发控制，不改页格式，落盘/恢复语义不变。
//   - 游标跨叶前进前先校验当前叶版本：并发分裂会改写叶子的 next 指针，必须据此
//     刷新扫描位置，避免跳过或重复条目。
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

    // Phase 3（t4）：低频索引真空——遍历整棵树，用 is_dead 谓词逐条判定索引项
    // (key, rid) 是否可回收（回表判定的具体实现由调用方 SystemCatalog 经
    // TableHeap::DecideIndexEntry 提供，保证「移除时任何活动快照都读不到该行」），
    // 可回收则物理删除该条目。
    // best-effort：删除只打墓碑、不合并节点（与 Delete 同取舍）；期间条目被并发
    // 增删不破坏正确性——判据单调（已判死的条目保持死）。返回本趟移除条目数。
    uint64_t Vacuum(const std::function<bool(const RID&, const IndexKey&)>& is_dead);

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
    //
    // 乐观语义：游标持有的是「叶子页内容的物化快照 + 装载时的版本号」，跨叶边界
    // 前进前先校验版本。若叶子被并发分裂/改写，重新装载并越过已发出的条目继续，
    // 保证不跳过新分裂出的右半页、不重复发出同一条目。
    class Cursor {
    public:
        // lower_key == nullptr 时从叶子开头开始（Begin）；否则定位到第一个
        // >= lower_key 的条目（LowerBound）。构造时完成首次装载。
        Cursor(const BPlusTree* tree, page_id_t leaf_pid, const IndexKey* lower_key);
        // 取出当前项并前进；到末尾返回 false。
        bool Next(IndexKey* key, RID* rid);
        // 装载叶子时的版本号。LowerBound/Begin 用它复核下降路径末端的叶版本，
        // 不一致说明下降结果已过期，需整体重启。
        uint32_t GetLeafVersion() const { return leaf_version_; }

    private:
        bool LoadLeaf(page_id_t pid);
        bool CurrentLeafUnchanged() const;
        // U1b 物理删页安全恢复：当前叶子被并发合并/物理释放（或 next 页消失）
        // 而无法快速续扫时，按最近发出的条目（或扫描起点锚点）重新下降定位并续扫。
        // 返回 false 表示重试耗尽后仍无法定位（调用方终止迭代）。
        bool RelocateRestart();

        const BPlusTree* tree_ = nullptr;
        page_id_t leaf_pid_ = INVALID_PAGE_ID;
        size_t index_ = 0;
        // 当前叶子页的物化内容。一次读入、逐条产出，避免每次 Next 都重解页面。
        std::vector<std::pair<IndexKey, RID>> entries_;
        page_id_t next_leaf_ = INVALID_PAGE_ID;
        // 装载当前叶子时的版本号：跨叶前进前校验，并发分裂会改写 next 指针。
        uint32_t leaf_version_ = 0;
        // 最近一次发出的 (key, rid)：叶子内容变化后据此重新定位，避免重发/漏发。
        bool has_last_ = false;
        IndexKey last_key_;
        RID last_rid_;
        // 扫描起点锚点：尚未发出任何条目时，RelocateRestart 据此回到原起点续扫
        // （begin=true 回到最左叶；否则定位到第一个 >= anchor_key_ 的条目）。
        bool anchor_begin_ = true;
        IndexKey anchor_key_;
    };

    // 定位到第一个 >= key 的位置
    std::unique_ptr<Cursor> LowerBound(const IndexKey& key) const;
    // 定位到最左端
    std::unique_ptr<Cursor> Begin() const;

    // ---- U1 可观测性（只读）----
    // 返回树高（单叶树=1）。沿最左路径读闩下降计数。
    int GetHeight() const;
    // 统计全树叶/内节点的利用率：min_ratio、avg_ratio（已用字节/页），
    // 并输出叶、内节点的页数。用于验证「删除后利用率不持续下降」与 `\stats`。
    void ComputeUtilization(double* min_ratio, double* avg_ratio,
                            uint64_t* leaf_pages, uint64_t* internal_pages) const;

    // ---- U1d 可观测性（只读）：乐观重启校验统计 ----
    // fast=结构计数短路命中的校验次数（O(1)）；full=回退全路径逐页校验次数；
    // restarts=校验失败导致的重启次数；mod=当前结构变更计数。用于测试与观测
    // 「校验成本随树高线性增长」的缓解效果。
    void GetOptimisticStats(uint64_t* fast, uint64_t* full, uint64_t* restarts,
                            uint32_t* mod) const {
        if (fast) *fast = stat_fast_validate_.load();
        if (full) *full = stat_full_validate_.load();
        if (restarts) *restarts = stat_restarts_.load();
        if (mod) *mod = structure_mod_.load();
    }

private:
    // 乐观下降路径上记录的 (页号, 版本号) 快照，供遍历结束后校验。
    struct PathEntry {
        page_id_t pid;
        uint32_t version;
    };

    // U1d/N2：乐观分布的完整快照。除路径逐页版本外，还记录**下降开始时的树级结构
    // 变更计数**（mod_at_descent）和命中叶子的版本。若下降期间结构计数未变，则整个
    // 祖先链路必然未被改写，校验只需复核叶子版本（O(1)），无需重取每个祖先页——
    // 把「校验成本随树高线性增长」降为常见路径 O(1)。
    struct DescPath {
        std::vector<PathEntry> entries;
        uint32_t mod_at_descent = 0;
        bool IsEmpty() const { return entries.empty(); }
        page_id_t leaf_pid() const { return entries.empty() ? INVALID_PAGE_ID : entries.back().pid; }
        uint32_t leaf_version() const { return entries.empty() ? 0 : entries.back().version; }
    };

    // 乐观只读下降：逐页取共享闩记录版本号，返回 (key, rid) 所属的叶子页。
    // 失败（页不存在/环路/损坏）返回 INVALID_PAGE_ID。
    page_id_t OptimisticFindLeafPage(const IndexKey& key, const RID& rid,
                                     DescPath* path) const;
    // 乐观只读下降：一直取第一个孩子，返回最左叶子页（Begin 用）。
    page_id_t OptimisticLeftmostLeafPage(DescPath* path) const;
    // 校验下降路径：先走结构计数短路——若下降期间无任何结构改写，仅复核叶子版本
    //（O(1)）；否则回退全路径逐页版本复核（O(树高)）。任一失败返回 false（调用方
    // 重启遍历）。绝不比逐页复核更宽松：结构改写必自增计数，计数未变 ⇒ 祖先未变。
    bool ValidatePath(const DescPath& path) const;

    // ---- U1d：结构变更计数与校验统计（用于短路 + 可观测）----
    // 每次分裂/拆分/再平衡/收根等**结构改写**自增。读路径据此判定"下降期间有
    // 无结构改写"，从而跳过昂贵的全路径版本重取。
    void BumpStructureCounter() { structure_mod_ = structure_mod_.load() + 1; }
    mutable std::atomic<uint32_t> structure_mod_{0};
    mutable std::atomic<uint64_t> stat_fast_validate_{0};  // 结构计数短路命中（O(1) 校验）
    mutable std::atomic<uint64_t> stat_full_validate_{0};  // 回退全路径逐页校验
    mutable std::atomic<uint64_t> stat_restarts_{0};       // 校验失败导致的重启

    // 向已持写闩的叶子页插入一条记录（合并 + 整页重写 + 版本号自增 + undo/WAL）。
    // 调用方负责在插入前完成唯一性检查与 MarkDirty。
    bool InsertIntoLeaf(PageWriteGuard* leaf, const IndexKey& key, const RID& rid,
                        std::vector<char>&& key_bytes);

    // 慢路径：快路径发现目标叶子放不下时，带写闩下降、沿途预分裂，保证叶子有空间。
    bool InsertSlowPath(const IndexKey& key, const RID& rid,
                        const std::vector<char>& key_bytes, size_t reserve);

    // 分裂 parent 的孩子 child，并把分隔键插入 parent。
    // 前置条件：parent 已由调用方保证有足够空间容纳一个分隔键（预分裂不变式）。
    // 内部以「先 pin 后加闩」同时持 parent/child（及可选 next）写闩，锁序恒为
    // 父 → 子 → next。
    // 返回值：1=分裂成功；0=硬失败（页损坏/缓冲池耗尽，调用方放弃插入）；
    //        2=结构已被并发改写（parent 被并发分裂、child 已不在其下），
    //          调用方应整体重启慢路径下降。
    int SplitChild(page_id_t parent_pid, page_id_t child_pid, size_t reserve);

    // 分裂根页。根页 id 恒定不变：把根的内容搬到一个新页，再把根改写成只有
    // 两个孩子的内部节点。这样目录里的 root_page_id 一经写入就永不失效，
    // 省掉「根分裂后必须回写目录元数据」这条极易遗漏的一致性路径。
    // reserve 用于持闩重查根是否仍溢出：并发分裂可能已让根腾出空间，此时
    // 直接返回成功（并回收误分配的新页），由调用方继续下降。
    bool SplitRoot(size_t reserve);

    // ---- U1/U1b：删除后下溢再平衡 ----
    // Delete / Vacuum 在目标节点下溢（<40%）后调用，把该子树与同一父节点下的
    // 相邻兄弟合并/再分配，级联处理父节点下溢，根单子时收缩（收树高）。合并会
    // 物理回收被并页（BPM 删除页 → .fpl 空闲位图）；child_pid 为刚变小的节点
    // 页号，key 用于带写闩下降定位其父节点（按 key 路由必然经过该子树）。
    // 返回本趟发生的结构变更次数（用于测试/观测对账）；并发冲突或损坏时保守
    // 放弃（返回 0），绝不破坏一致性。
    int RebalanceAfterDelete(page_id_t child_pid, const IndexKey& key);
    // 若根是仅含一个孩子的内部节点，把根就地改写为该子树的拷贝并回收原孩子页
    //（树高降 1）。根页 id 恒定不变（复用 SplitRoot 约定）。返回是否发生收缩。
    // 自身会调用 BPM（回收孩子页），因此只能在**不持任何页闩**时调用。
    bool CollapseRootIfNeeded();

    // ---- U1c：物理删页可靠回收 ----
    // 合并/根收缩后回收被并页须等并发读者 Unpin 之后才能成功。这里用「挂起队列 +
    // 安全点排空」：DeletePage 失败（页仍被 pin）时先把页号暂存（此时页仍分配于 BPM、
    // 不会被 AllocatePage 复用，延迟回收安全），待后续写操作入口（不持任何页闩时）
    // 重试排空，保证被弃页最终回到 .fpl 空闲位图。
    //
    // TryFreePage：尝试立即物理回收；失败（并发 pin）则入挂起队列。
    // DrainPendingFrees：对挂起队列整体重试一次；仍失败者回队。必须在**不持任何页闩**
    // （不持任何 BPM pin）时调用——DeletePage 内部会取 BPM 全局锁。
    void TryFreePage(page_id_t pid);
    void DrainPendingFrees();

    BufferPoolManager* bpm_;
    std::vector<ValueType> key_schema_;
    bool is_unique_;
    page_id_t root_page_id_;
    Transaction* active_txn_ = nullptr;
    LogManager* log_manager_ = nullptr;  // Phase B：可选 WAL 写出器
    // U1c：等待重试的被弃页队列（去重），见 TryFreePage / DrainPendingFrees。
    std::mutex pending_free_mutex_;
    std::vector<page_id_t> pending_free_vec_;
    std::unordered_set<page_id_t> pending_free_set_;
    // 注：树级锁已移除（Phase 1 乐观页级并发）。并发安全由「页级读写闩 + 版本号
    // 校验 + 乐观重启」保证：读路径校验版本、写路径只锁目标叶子/分裂路径。
};

}  // namespace sqlcompiler
