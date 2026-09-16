#pragma once

// =============================================================================
// RecoveryManager — ARIES 三阶段恢复（analysis → redo → undo）
//
// 职责：数据库启动时重放 WAL，把崩溃前「已提交但未落盘」的改动补齐（redo），
//       把「未提交」的改动抹除（undo），使数据文件与 WAL 恢复一致。
//
// 三阶段协作（由 Run() 一次串起）：
//   Analysis：全量扫描 WAL，重建 txn_table_（活动事务）与 dirty_page_table_（脏页）；
//   Redo    ：自脏页表中最小 rec_lsn 起顺序重放 UPDATE/CLR 的页镜像，幂等推进；
//   Undo    ：对残留活动事务沿 prev_lsn 链反向应用 before-image，逐条补写 CLR，
//             最后补写 ABORT 记录。
//
// 相对经典 ARIES 的简化：
//   * 不持久化 CHECKPOINT 内的活跃事务表 / 脏页表，Analysis 每次从 WAL 首条扫起
//     （无日志截断优化）；
//   * CHECKPOINT 记录仅作时间点标记，不携带额外表项。
// =============================================================================

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

// ARIES 三阶段恢复管理器。生命周期为「数据库启动期一次性使用」：
// 构造后调用 Run()，Run() 返回即表示恢复完成，随后本对象可被丢弃。
class RecoveryManager {
public:
    // 构造：仅保存各依赖的引用，不触发任何 I/O 或状态初始化——恢复动作全部在 Run() 内发生。
    // @param log_manager         WAL 日志管理器，恢复的唯一数据来源；为 nullptr 时 Run() 直接失败。
    // @param buffer_pool_manager 页缓存，redo/undo 经其取页、写入页镜像并置脏。
    // @param disk_manager        物理磁盘管理器，仅在 Run() 收尾时调用 Sync() 强制落盘。
    // @param txn_manager         事务管理器（预留引用：当前恢复完全由 WAL 驱动，不回调事务管理器）。
    // @param catalog             系统目录（预留引用：当前实现不访问）。
    RecoveryManager(LogManager* log_manager, BufferPoolManager* buffer_pool_manager,
                    DiskManager* disk_manager, TransactionManager* txn_manager,
                    SystemCatalog* catalog);

    // 启动期恢复入口：依次执行 Analysis / Redo / Undo，最后刷脏页并 Sync 落盘。
    // @return true  —— 恢复流程正常完成（含「WAL 为空」的全新库情形，此时无需恢复）；
    //         false —— 未注入 LogManager，无 WAL 可重放，调用方应视为数据不可信。
    // @note 本实现不会因「日志损坏」返回 false：日志读取阶段的异常会直接上抛给调用方，
    //       由调用方决定是否重建数据库。
    bool Run();

    // 写一条 txn_id=0 的 CHECKPOINT 记录并 Flush WAL。
    // @note 仅记录「检查点时间标记」，不含活动事务表 / 脏页表（V1 简化）；Flush 失败会被
    //       静默吞掉（测试场景可能未打开 WAL 文件），不向外抛异常。
    void Checkpoint();

private:
    // Analysis pass：全量扫描 WAL，重建恢复所需的两张内存表。
    // @param records WAL 全量记录（LogManager::ReadAll() 按 LSN 升序返回），本函数只读不改。
    // @note 产出 txn_table_ 与 dirty_page_table_，二者语义见成员变量处的说明。
    void AnalysisPass(const std::vector<LogRecord>& records);

    // Redo pass：重放「已写日志但可能未落盘」的页修改，把数据库推进到崩溃前状态。
    // @param records WAL 全量记录；遍历起点取 dirty_page_table_ 中最小 rec_lsn
    //                （无脏页时退化为 1，即从头全量扫描）。
    // @note 幂等性依据 `page.GetPageLsn() < record.lsn_` 判定：仅当目标页尚未记录到该条
    //       日志之后才覆写镜像，因此重复执行 redo 结果一致。UPDATE 取 after_image，
    //       CLR 取 before_image（其语义为「undo 后的状态」）。
    void RedoPass(const std::vector<LogRecord>& records);

    // Undo pass：回滚所有仍处于活动状态的事务，抹除未提交改动。
    // @param records WAL 全量记录；内部建立 lsn → LogRecord* 索引以支持按 prev_lsn 回溯。
    // @note 逐条撤销 UPDATE 的 before-image 并补写 CLR（CLR.undo_next_lsn 指向 undo 链下一条），
    //       遇 CLR 则直接跳转到其 undo_next_lsn，跳过已撤销的区间；收尾为每个被撤销事务写
    //       ABORT 记录并 Flush，随后清空两张内存表。
    void UndoPass(const std::vector<LogRecord>& records);

    // 依赖注入（均为非拥有裸指针，生命周期由调用方保证）。
    LogManager* log_manager_ = nullptr;                 // WAL 日志管理器；为 nullptr 时 Run() 返回 false
    BufferPoolManager* buffer_pool_manager_ = nullptr;  // 页缓存；redo/undo 的取页与置脏入口
    DiskManager* disk_manager_ = nullptr;               // 磁盘管理器；仅 Run() 收尾调用 Sync()
    TransactionManager* txn_manager_ = nullptr;         // 预留：当前恢复不回调事务管理器
    SystemCatalog* catalog_ = nullptr;                  // 预留：当前实现不访问系统目录

    // ARIES 内部分析状态（由 AnalysisPass 填充，UndoPass 消费并在结束时清空）：
    //   txn_table_:        txn_id → 该事务最新 LSN；仍存在的键即「崩溃时尚未结束」的事务。
    //   dirty_page_table_: page_id → 首次脏化该页的 LSN（取最早值），决定 Redo 的扫描起点。
    std::unordered_map<txn_id_t, lsn_t> txn_table_;
    std::unordered_map<page_id_t, lsn_t> dirty_page_table_;

    // 恢复期间把整页 before-image 写回指定页，是 undo 的基本动作。
    // @param pid   目标页号，合法值须 ≥ 0。
    // @param image 长度为 PAGE_SIZE 的页镜像，不得为空。
    // @return true  —— 成功取到页并完成写入；
    //         false —— pid 非法 / image 为空 / 缓冲池未注入 / 取页失败。
    // @note 写入后置脏；本函数不刷盘，统一由 Run() 收尾批量 Flush + Sync。
    bool RestorePageImage(page_id_t pid, const char* image);
};

}  // namespace sqlcompiler