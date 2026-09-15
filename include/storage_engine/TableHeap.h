#pragma once

#include <vector>

#include "storage/StorageAccess.h"
#include "storage_engine/Tuple.h"

namespace sqlcompiler {

// Phase A 前向声明。
class Transaction;
class LogManager;

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
    TableHeap(StorageAccess* storage, page_id_t first_page_id);

    // 创建一张全新的表堆（分配首页并初始化页头），返回新建的TableHeap
    static TableHeap* Create(StorageAccess* storage);

    // 打开一张已存在的表堆（如数据库重启后，由SystemCatalog持有的first_page_id）
    static TableHeap* Open(StorageAccess* storage, page_id_t first_page_id);

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
    //
    // out_new_rid：delete+insert 路径下原 slot 被墓碑化，行被搬到新 slot。
    // 调用方在 InsertIntoIndexes 等需要正确 RID 的场合必须使用这里输出的
    // 新 RID；传 nullptr 时函数等价于旧 API（不返回新 RID）。
    // column_types 同 InsertTuple。
    bool UpdateTuple(const RID& rid, const Tuple& new_tuple,
                     const std::vector<ValueType>& column_types,
                     RID* out_new_rid = nullptr);

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

    // 顺序扫描迭代器，供SeqScanExecutor使用
    class Iterator {
    public:
        Iterator(TableHeap* table_heap, RID start_rid);
        ~Iterator();

        // 注意：HasNext 会按需触发一次"定位首条有效记录"的扫描（O(M) on first
        // call where M 是首页上的 slot 数），之后保持 O(1) 摊销（page 在
        // current_page_guard_ 上跨 Next 调用复用 pin）。
        bool HasNext();
        Tuple Next(const std::vector<ValueType>& column_types);

        // 禁止拷贝：迭代器持有 pinned page，拷贝会双倍 unpin。但允许 move
        // （unique_ptr<Iterator> 在 make_unique / std::move 路径上需要 move
        // ctor），move 时把源对象的 pinned state 转移过来并把源置为"已耗尽"。
        Iterator(const Iterator&) = delete;
        Iterator& operator=(const Iterator&) = delete;
        Iterator(Iterator&& other) noexcept;
        Iterator& operator=(Iterator&& other) noexcept;

    private:
        TableHeap* table_heap_;
        RID current_rid_;
        // 跨 Next 调用复用的 pinned page（"page guard"）。INVALID_PAGE_ID
        // 表示当前没有 pin 任何页。Next 在跨页时 unpin 旧 + pin 新；Iterator
        // 析构时若仍 pin 着，必须 unpin 否则 frame 永不退出可淘汰候选集合。
        page_id_t current_page_guard_ = INVALID_PAGE_ID;
        // 与 current_page_guard_ 配对的裸 Page*。在跨 Next 调用之间复用，
        // 避免每次 Next 都重新 GetPage（会再 pin 一次、导致 pin count 漂移）。
        // 帧被 pin 的整个期间 Page* 都有效（frames_ 数组下标固定）。
        Page* current_page_ptr_ = nullptr;
        // "已耗尽"标记：扫完整张表后置 true，使 HasNext 不会再重新从首页起步
        // 扫描（否则会无限循环——不断把已经读过的 tuple 再次返回）。
        bool exhausted_ = false;

        // 把 current_page_guard_ 调整到 (page_id, slot) 所在的页；若不匹配，
        // unpin 旧的再 pin 新的。pin 失败则把 current_rid_ 置为 invalid 并
        // 返回 false。
        bool EnsurePagePinned(page_id_t page_id);
        // 在当前 pinned 页内从 start_slot 开始扫描下一个非墓碑 slot；如果到
        // 页尾没有，跳到 next_page 并继续扫；最终把结果写入 *next_rid 并把
        // current_page_guard_ 调整到包含它的页（保持 pin）。
        // 找不到（堆已耗尽）返回 false，current_rid_/current_page_guard_ 都被
        // 置为"已耗尽"状态。
        bool AdvanceToNextValidSlot(int start_slot, RID* next_rid);
    };

    // 返回指向第一条有效记录的迭代器
    Iterator Begin();

private:
    StorageAccess* storage_;
    page_id_t first_page_id_;
    // 写路径游标：指向"已知还有空间的第一页"。InsertTuple 从这里起步找页；
    // 命中一页且该页变满时前进；DeleteTuple 让一页重新有空间时（罕见）回退。
    // 重新打开堆时为 INVALID_PAGE_ID，第一次 InsertTuple 会重新扫描定位。
    page_id_t first_page_with_space_ = INVALID_PAGE_ID;
    Transaction* active_txn_ = nullptr;
    LogManager* log_manager_ = nullptr;  // Phase B：可选 WAL 写出器

    // 尝试在给定页内插入记录（写入槽位目录+记录内容），页空间不足返回false。
    // out_next_pid（可空）若非空，成功时填入页头里的 next_page_id；这样
    // InsertTuple 在失败时不必再次 pin 同一页来读 next_pid——一次 page fetch
    // 完成"尝试插入 + 取链表下一节点"。成功路径里 next_pid 仍指向当前页
    // 之后那一页，调用方按需用之（失败路径才真正需要它）。
    bool InsertIntoPage(page_id_t page_id, const Tuple& tuple, RID* rid,
                         const std::vector<ValueType>& column_types,
                         page_id_t* out_next_pid = nullptr);

    // 定位current之后下一个存在有效（未删除）记录的RID，写入next，
    // 供Iterator::HasNext()/Next()使用；到达堆文件末尾返回false
    bool FindNextRid(RID current, RID* next);
};

}  // namespace sqlcompiler
