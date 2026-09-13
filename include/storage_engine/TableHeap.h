#pragma once

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "index/IndexKey.h"
#include "storage/BufferPoolManager.h"
#include "storage_engine/Tuple.h"

namespace sqlcompiler {

// Phase A 前向声明。
class Transaction;
class LogManager;
class CommitTracker;

// TableHeap：将一张表组织为一组数据页的集合（堆文件结构，页与页之间通过
// 页头中的next_page_id串联成链表），页内建议采用"槽位目录（slot directory）"
// 的经典布局来存放变长记录：
//
//   [页头: next_page_id | slot_count | free_space_offset]
//   [slot_0: offset,length] [slot_1: offset,length] ...   <- 从页头向后增长
//   ...空闲区域...
//   [record_1] [record_0]                                  <- 从页尾向前增长
//
// 该结构是存储引擎中"记录(Row) <-> 页(Page)映射关系"的核心实现，
// 供执行引擎的SeqScan/Insert/Delete/Update等算子调用
class TableHeap {
public:
    TableHeap(BufferPoolManager* buffer_pool_manager, page_id_t first_page_id);

    // 创建一张全新的表堆（分配首页并初始化页头），返回新建的TableHeap
    static TableHeap* Create(BufferPoolManager* buffer_pool_manager);

    // 打开一张已存在的表堆（如数据库重启后，由SystemCatalog持有的first_page_id）
    static TableHeap* Open(BufferPoolManager* buffer_pool_manager, page_id_t first_page_id);

    // 插入一条记录：从first_page_id开始寻找有足够空闲空间的页，
    // 若都写满则通过buffer_pool_manager_->NewPage()追加新页。
    // 成功后通过rid返回该记录的位置。
    // column_types 描述每个字段的声明类型，用于序列化时为 NULL 选择匹配的字节宽度。
    bool InsertTuple(const Tuple& tuple, RID* rid,
                     const std::vector<ValueType>& column_types);

    // 根据rid读取一条记录，column_types用于反序列化
    bool GetTuple(const RID& rid, Tuple* tuple, const std::vector<ValueType>& column_types);

    // 根据rid删除一条记录（建议先实现墓碑标记/tombstone，即将slot标记为已删除，
    // 而非立刻压缩页面空间，简化实现）
    bool DeleteTuple(const RID& rid);

    // 根据rid更新一条记录（若新记录变长后仍能放入原slot则原地更新，
    // 否则可先DeleteTuple旧记录再InsertTuple新记录）。
    // column_types 同 InsertTuple。
    bool UpdateTuple(const RID& rid, const Tuple& new_tuple,
                     const std::vector<ValueType>& column_types);

    // 清空表中的所有记录（保留表结构与首页），供 TRUNCATE TABLE 使用
    // 释放除首页外的全部溢出页，并把首页重置为空槽位目录
    void ClearAll();

    page_id_t GetFirstPageId() const;

    // ---- Phase A：把当前事务挂到堆上（写路径把 undo log 写进 txn）----
    // DML 算子在写堆前调用一次。nullptr 表示隐式 auto-commit，无 undo。
    // 不是线程安全的：当前实现假定单线程写。
    void SetActiveTransaction(Transaction* txn) { active_txn_ = txn; }
    Transaction* GetActiveTransaction() const { return active_txn_; }

    // ---- Phase B：注入 LogManager，写路径同时落 WAL ----
    // nullptr 表示 Phase A 兼容模式：仍然抓 in-memory undo，但不再写 WAL。
    // 仅 Database 构造时设置一次，后续 DML 算子不直接调用。
    void SetLogManager(LogManager* lm) { log_manager_ = lm; }
    LogManager* GetLogManager() const { return log_manager_; }

    // ---- MVCC 快照隔离：读物化快照边界 ----
    // 快照扫描在 Init 时调用：把快照水位 S（>=0）与共享 CommitTracker 挂到本堆上，
    // 之后的 GetTuple/Iterator 按该快照过滤版本。csn < 0 或无 tracker = 非快照模式，
    // 采用现有锁基读（只读 head / legacy）。注意：本堆跨会话共享，并发下不同会话
    // 切换快照是「约」的，由上层串行化。
    void SetSnapshot(int64_t csn, CommitTracker* tracker) {
        snapshot_csn_ = csn;
        commit_tracker_ = tracker;
        // Phase 2：语句级内联真空预算重置（每个执行器语句经 SetSnapshot 进入）。
        // 写路径（Update/Delete）随后每次写新版本时顺带回收本页已确认 < 低水位的
        // 旧版本槽位，单语句至多回收 kInlineVacuumBudget 个，摊薄真空成本。
        inline_vacuum_remaining_ = kInlineVacuumBudget;
    }
    int64_t GetSnapshotCsn() const { return snapshot_csn_; }

    // Phase 3：版本索引缓存观测（命中/重建次数；测试与技术文档用）。
    uint64_t GetVersionIndexHitCount() const { return version_index_hits_.load(); }
    uint64_t GetVersionIndexBuildCount() const { return version_index_builds_.load(); }
    // Phase 3（t3）：墓碑槽复用观测（InsertIntoPage/UpdateTuple 迁槽复用墓碑目录项
    // 的次数；测试与技术文档用）。
    uint64_t GetTombstoneReuseCount() const { return tombstone_reuse_count_.load(); }

    // 惰性真空回收：回收所有 begin_csn < oldest_active_csn 的「已被替代/删除」
    // 非 head 旧版本槽位（写墓碑）。以最老活动快照为界，避免误删仍可能被读取的版本。
    void Vacuum(int64_t oldest_active_csn);

    // Phase 3（t4）索引真空的条目级判定结果。
    enum class IndexVacuumDecision { kKeep, kRemove };

    // Phase 3（t4）索引墓碑回收判定：索引项 (entry_key, rid) 是否可物理移除。
    // 仅页读闩 + 槽头检查 + 版本键比较，不依赖本堆 SetSnapshot 状态（后台真空
    // 不得扰动共享堆的会话快照）。kRemove 的两种情形均保证「所有活动快照都看不到
    // 该条目指向的行版本」：
    //   (i)  槽不可见：墓碑 / 越界 / 损坏，或 MVCC 头 end_xid!=0 && end_csn!=0 &&
    //        end_csn <= active_snapshots.front()（与堆 Vacuum 同款判据，覆盖快照逻辑删除）；
    //   (ii) 头稳定（end_xid==0）且已提交（begin_csn>0）、头键 != 条目键（旧键条目）：
    //        - 头 begin_csn <= 最老活动快照 → 任何活动快照的可见版本都是该头，旧键
    //          版本无人可见（覆盖「改写已对最老快照可见」的旧条目）；
    //        - 头 begin_csn > 最老活动快照 → **精确化（Phase 3 收尾）**：不再保守
    //          kKeep，而是沿版本链走到「最老快照的可见版本」，逐版本检查其可见区间
    //          [v.begin_csn, succ.begin_csn) 是否含任一活动快照且该版本键 == 条目键；
    //          有则 kKeep（仍有老快照需要该条目），无则 kRemove。
    //    其余一律 kKeep（legacy 无头 / 未提交写者 / 头键==条目键的活跃条目 /
    //    链损坏或超长：保守保留，最坏只是漏回收，不会误删仍可能被读到的条目）。
    // active_snapshots：活动快照 CSN 升序列表（可含重复），由调用方从 CommitTracker
    // 一次性拍取；空列表或最老水位 <= 0 时整体 kKeep（无快照可判定）。
    // key_col_indices / column_types：版本反序列化后按索引列提取键与条目键比较。
    IndexVacuumDecision DecideIndexEntry(
        const RID& rid, const IndexKey& entry_key,
        const std::vector<int64_t>& active_snapshots,
        const std::vector<int32_t>& key_col_indices,
        const std::vector<ValueType>& column_types) const;

    // 顺序扫描迭代器，供SeqScanExecutor使用
    class Iterator {
    public:
        Iterator(TableHeap* table_heap, RID start_rid);

        bool HasNext() const;
        Tuple Next(const std::vector<ValueType>& column_types);

    private:
        TableHeap* table_heap_;
        RID current_rid_;
    };

    // 返回指向第一条有效记录的迭代器
    Iterator Begin();

private:
    BufferPoolManager* buffer_pool_manager_;
    page_id_t first_page_id_;
    Transaction* active_txn_ = nullptr;
    LogManager* log_manager_ = nullptr;  // Phase B：可选 WAL 写出器
    // MVCC 快照隔离：csn < 0 或无 tracker = 非快照（现有锁基读）。
    int64_t snapshot_csn_ = -1;
    CommitTracker* commit_tracker_ = nullptr;
    // T2 并发写串行化互斥量：InsertTuple 的「找页->判满->新建页->链接->插入」
    // 是一段跨多次缓冲池访问的 check-then-act 序列，单独靠锁页/缓冲池全局锁只能
    // 串行化单次帧访问，无法阻止两个会话同时扩展同一尾部页导致链表分叉、丢行。
    // 用 per-table 互斥量把这整段序列串行化，使并发写与顺序执行结果等价（正确性
    // 优先于吞吐）。递归锁因为 UpdateTuple 增长路径会再嵌套调用 DeleteTuple/
    // InsertTuple。是「约」的：本实现把写粒度假定为「每表一次只一个写者」。
    mutable std::recursive_mutex write_mutex_;

    // 尝试在给定页内插入记录（写入槽位目录+记录内容），页空间不足返回false
    bool InsertIntoPage(page_id_t page_id, const Tuple& tuple, RID* rid,
                         const std::vector<ValueType>& column_types);

    // Phase 2：内联轻量真空（O(1) 快照水位 + 无后台真空的写路径摊薄通道）。
    // 写路径（Update/Delete）持页写锁时顺带回收本页「已确认 < 低水位」的旧版本
    // 槽位（判据同 Vacuum：end_xid!=0 && end_csn!=0 && end_csn<=低水位），单语句
    // 至多回收 kInlineVacuumBudget 个，把全表真空成本摊薄到日常写路径上；后台
    // 真空保留为「低频兜底」。回收判据已证明与版本链遍历/FCW 检测安全共存。
    // Phase 3（t3）增强：回收时做「链摘除」——把本页内引用被回收版本为 prev 的
    // 后继版本，其 prev 改接到被回收版本自身的 prev（跳过回收版本），保证墓碑槽
    // 不再被任何版本引用、可被 InsertIntoPage/UpdateTuple 安全复用。
    static constexpr int kInlineVacuumBudget = 8;
    // 在已持页写锁的页上回收旧版本槽位（page_id 供链摘除比对；
    // budget 为本次最多回收数），返回实际回收数。
    int InlineVacuumPage(page_id_t page_id, char* data, int32_t slot_count,
                         int64_t low_water, int budget);
    // 内联真空入口：取全局低水位并执行回收（无 tracker / 预算耗尽 / 无活动快照时
    // no-op），并扣减语句级预算。
    void RunInlineVacuum(page_id_t page_id, char* data, int32_t slot_count);
    // 语句级内联真空剩余预算（由 SetSnapshot 重置）。
    int32_t inline_vacuum_remaining_ = 0;

    // ---- Phase 3：版本链 O(1) 查询（内存版本索引缓存）----
    // 快照点查加速：缓存 rid → 按 begin_csn 升序的「稳定版本」数组。稳定 = 写者已
    // 提交（begin_csn>0）且端已回填（end_xid==0 或 end_csn>0）。GetTuple 用二分
    // 定位「最新 begin_csn<=S」的候选版本，端可见性 CSN 直判（免逐版本
    // LookupCommitted 锁与跨页读取）。
    // 自愈：head 槽头部作为缓存标记——任何写版本/删除/提交回填都改动 head 槽头，
    // 标记不匹配即沿链重走重建；真空墓碑只落在「先被替代（写路径已触发重建）」的
    // 版本上（head 被删时槽位本身变墓碑，GetTuple 在进入快照逻辑前即返回 false），
    // 候选槽读校验再兜底一层。
    struct VersionIndexEntry {
        int64_t begin_xid = 0;              // 版本写者（供 RecordSnapshotRead 基）
        int64_t begin_csn = 0;              // >0：写者已提交的 CSN
        int64_t end_csn = 0;                // 0 = 当前仍可见；>0 = 失效水位
        page_id_t page_id = INVALID_PAGE_ID;  // 版本所在页（可能跨页）
        int32_t slot_num = -1;
    };
    struct VersionIndex {
        std::vector<VersionIndexEntry> versions;  // 按 begin_csn 升序（旧→新）
        // 构建时的 head 槽头部快照（命中校验用）。
        int64_t head_begin_xid = 0;
        int64_t head_begin_csn = 0;
        int64_t head_end_xid = 0;
        int64_t head_end_csn = 0;
        int32_t head_prev_pid = INVALID_PAGE_ID;
        int32_t head_prev_slot = -1;
    };
    // rid -> 版本索引（key = page_id<<32 | slot_num 的 64 位编码，免自定义 hash）。
    // 独立于 write_mutex_：仅快照读线程触碰；命中/重建都是短临界区，且不在持该
    // 锁期间取页锁（锁序：先取页读锁读 head/候选，再进缓存锁；反之不可）。
    std::unordered_map<uint64_t, VersionIndex> version_index_;
    mutable std::mutex version_index_mutex_;
    // 观测：缓存命中/重建次数（测试与技术文档用；仅最佳努力计数）。
    mutable std::atomic<uint64_t> version_index_hits_{0};
    mutable std::atomic<uint64_t> version_index_builds_{0};
    // Phase 3（t3）：墓碑槽复用计数（InsertIntoPage/UpdateTuple 复用墓碑目录项）。
    mutable std::atomic<uint64_t> tombstone_reuse_count_{0};
    // rid 的 64 位缓存键编码。
    static uint64_t VersionIndexKey(const RID& r) {
        return ((uint64_t)(uint32_t)r.page_id << 32) | (uint32_t)r.slot_num;
    }

    // 定位current之后下一个存在有效（未删除）记录的RID，写入next，
    // 供Iterator::HasNext()/Next()使用；到达堆文件末尾返回false
    bool FindNextRid(RID current, RID* next);

    // MVCC：判断 (pid, slot) 是否被任意一处的「非墓碑、有 MVCC 头」版本引为
    // prev（即它是某个更旧版本，而非稳定 head）。快照扫描据此跳过旧版本 slot。
    bool IsReferencedAsPrev(page_id_t pid, int32_t slot) const;
};

}  // namespace sqlcompiler
