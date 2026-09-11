#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage/BufferPoolManager.h"
#include "storage/DiskManager.h"
#include "txn/LogManager.h"
#include "txn/LogRecord.h"

namespace sqlcompiler {

// 前向声明
class SystemCatalog;
class TransactionManager;

// RecoveryManager — ARIES 3-phase 恢复（analysis → redo → undo）。
//
// V1 简化：
//   * 不持久化 CHECKPOINT 内的活跃事务表 / 脏页表；Analysis pass 直接从 WAL
//     首条记录扫起；
//   * 不实现 CLR（Compensation Log Record）；Phase B 的 undo 直接用 prev_lsn
//     链反向应用 before-image，不写 CLR。Phase C 会扩展 CLR 链。
//
// 流程：
//   Run() 一次完成三阶段，最后写一条 CHECKPOINT 记录并 Flush WAL。
class RecoveryManager {
public:
    RecoveryManager(LogManager* log_manager, BufferPoolManager* buffer_pool_manager,
                    DiskManager* disk_manager, TransactionManager* txn_manager,
                    SystemCatalog* catalog);

    // 启动期恢复入口。返回是否成功恢复（失败仅表示磁盘上日志彻底损坏，
    // 此时上层可以选择重建数据库）。
    bool Run();

    // 写 CHECKPOINT 记录 + Flush WAL。仅记录「checkpoint 标记」，不带活动事务
    // 表 / 脏页表（V1 简化）。
    void Checkpoint();

private:
    // Analysis pass：扫描 log_records（已 ReadAll 得到的）填充 txn_table_
    // 与 dirty_page_table_。
    void AnalysisPass(const std::vector<LogRecord>& records);

    // Redo pass：从 dirty_page_table_ 中最小 rec_lsn 开始，对每条 UPDATE
    // 记录，若 page.page_lsn_ < record.lsn，则把 after_image memcpy 到 page。
    void RedoPass(const std::vector<LogRecord>& records);

    // Undo pass：对每个还在 txn_table_ 里的活动事务，按 prev_lsn 链反向应用
    // before-image；为每个被撤销的 page 写一条 ABORT 记录。
    void UndoPass(const std::vector<LogRecord>& records);

    LogManager* log_manager_ = nullptr;
    BufferPoolManager* buffer_pool_manager_ = nullptr;
    DiskManager* disk_manager_ = nullptr;
    TransactionManager* txn_manager_ = nullptr;
    SystemCatalog* catalog_ = nullptr;

    // ARIES 内部分析状态：
    //   txn_table_:         txn_id → 该事务最新 LSN
    //   dirty_page_table_:  page_id → 首次弄脏该 page 的 LSN
    std::unordered_map<txn_id_t, lsn_t> txn_table_;
    std::unordered_map<page_id_t, lsn_t> dirty_page_table_;

    // 恢复期间从 before-image 取出整页并写到指定 page。
    bool RestorePageImage(page_id_t pid, const char* image);
};

}  // namespace sqlcompiler