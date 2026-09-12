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
    StorageAccess* storage_;
    page_id_t first_page_id_;
    Transaction* active_txn_ = nullptr;
    LogManager* log_manager_ = nullptr;  // Phase B：可选 WAL 写出器

    // 尝试在给定页内插入记录（写入槽位目录+记录内容），页空间不足返回false
    bool InsertIntoPage(page_id_t page_id, const Tuple& tuple, RID* rid,
                         const std::vector<ValueType>& column_types);

    // 定位current之后下一个存在有效（未删除）记录的RID，写入next，
    // 供Iterator::HasNext()/Next()使用；到达堆文件末尾返回false
    bool FindNextRid(RID current, RID* next);
};

}  // namespace sqlcompiler
