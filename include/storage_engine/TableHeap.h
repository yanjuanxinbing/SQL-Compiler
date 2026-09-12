#pragma once

#include <mutex>
#include <vector>

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
    }
    int64_t GetSnapshotCsn() const { return snapshot_csn_; }

    // 惰性真空回收：回收所有 begin_csn < oldest_active_csn 的「已被替代/删除」
    // 非 head 旧版本槽位（写墓碑）。以最老活动快照为界，避免误删仍可能被读取的版本。
    void Vacuum(int64_t oldest_active_csn);

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

    // 定位current之后下一个存在有效（未删除）记录的RID，写入next，
    // 供Iterator::HasNext()/Next()使用；到达堆文件末尾返回false
    bool FindNextRid(RID current, RID* next);

    // MVCC：判断 (pid, slot) 是否被任意一处的「非墓碑、有 MVCC 头」版本引为
    // prev（即它是某个更旧版本，而非稳定 head）。快照扫描据此跳过旧版本 slot。
    bool IsReferencedAsPrev(page_id_t pid, int32_t slot) const;
};

}  // namespace sqlcompiler
