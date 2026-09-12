#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "catalog/SystemCatalog.h"
#include "execution/ExecutionEngine.h"
#include "session/Session.h"
#include "storage/BufferPoolManager.h"
#include "storage/DiskManager.h"
#include "storage/LockManager.h"
#include "txn/TransactionManager.h"

namespace sqlcompiler {

// 前向声明：避免 Database.h 引入 LogManager 的全头（与 TransactionManager 形成环）。
class LogManager;
class RecoveryManager;
class CommitTracker;

// 数据库总入口（门面/Facade）：
// 串联 编译器模块（Lexer -> Parser -> SemanticAnalyzer -> Planner -> Optimizer -> CodeGenerator）
// 与 执行系统模块（SystemCatalog -> BufferPoolManager -> DiskManager -> ExecutionEngine），
// 对外提供"输入一条SQL文本，返回执行结果"的统一接口，供CLI/main.cpp调用
class Database {
public:
    // db_file: 数据文件路径（不存在则创建新库）；buffer_pool_size: 缓冲池可容纳的页数
    explicit Database(const std::string& db_file, size_t buffer_pool_size = 64,
                      int bg_flush_ms = 0);
    ~Database();

    // 执行一条SQL语句，内部完成 词法->语法->语义->计划->优化->执行 全流程，
    // 任一阶段出错都会被捕获并体现在ExecutionResult::success/message中
    ExecutionResult ExecuteSQL(const std::string& sql);

    // 执行以分号分隔的多条SQL语句（如从.sql文件读入的脚本），
    // 返回每条语句各自的执行结果，便于逐条展示
    std::vector<ExecutionResult> ExecuteScript(const std::string& sql_script);

    // 将缓冲池中所有脏页写回磁盘，通常在CLI退出前调用
    void Shutdown();

    // ---- T2 多连接并发事务：会话感知执行 ----
    // 在指定会话（拥有独立事务状态）上执行一条 SQL。多线程对同一 Database 各持
    // 一个 CreateSession() 返回的会话并发调用即可实现「各自独立事务/自动提交」。
    // 传 nullptr 等价于默认会话（单连接旧行为）。
    ExecutionResult ExecuteSQL(const std::string& sql, Session* session);

    // 新建一个会话：拥有独立的 TransactionManager（独立当前事务与嵌套深度），
    // 但共享缓冲池/WAL/系统目录，并共享全局 txn_id 序列器（保证 id 唯一）。
    // 返回的 Session 交由 Database 持有所有权；调用方可持 shared_ptr 副本使用。
    std::shared_ptr<Session> CreateSession();

    // 获取默认会话的事务管理器（兼容旧 API）。
    TransactionManager* GetTransactionManager() const { return txn_manager_.get(); }

    // ---- Phase B：崩溃注入（仅测试用） ----
    // 在接下来的 N 条 ExecuteSQL 成功完成后，强制 std::_Exit(1)。
    // 0 = 关闭（默认）。
    void SetCrashInjectionPoint(int n_statements);

    // 注入一次「立即崩溃」。由 \crash 调试命令触发；让 49_acid_recovery
    // 之类的测试在不重启进程的前提下也保留 main.cpp 的退出路径不变。
    void TriggerCrashNow();

    // 生成存储子系统诊断信息字符串（由 \stats 命令触发）。
    // 输出缓冲池命中/缺失/替换统计、命中率、磁盘页数与近期页替换日志，
    // 满足指导书「页级读写、缓存命中统计、页替换日志输出」的要求。
    std::string GetStorageStats() const;

private:
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> buffer_pool_manager_;
    std::unique_ptr<SystemCatalog> catalog_;
    std::unique_ptr<ExecutionEngine> execution_engine_;
    std::unique_ptr<TransactionManager> txn_manager_;
    std::unique_ptr<LogManager> log_manager_;        // Phase B：WAL 写出器
    std::unique_ptr<RecoveryManager> recovery_;      // Phase B：启动期 ARIES 恢复
    // T2：跨会话共享的事务级锁管理器（隔离级别 + 死锁回收）。
    std::unique_ptr<LockManager> lock_manager_;
    // MVCC 快照隔离：跨会话共享的提交跟踪器（CSN 注入 + 版本可见性判定）。
    std::unique_ptr<CommitTracker> commit_tracker_;

    // T2 并发会话：全局 txn_id 序列器 + 会话所有权容器 + 会话执行器私有辅助。
    TxnIdSequencer txn_seq_;
    std::vector<std::shared_ptr<Session>> sessions_;
    ExecutionResult ExecuteSQLImpl(const std::string& sql, TransactionManager* txn_mgr);

    std::string db_file_path_;       // 规范化后的绝对路径
    std::string wal_file_path_;      // <db_file>.wal
    bool is_new_database_;           // 用于判断启动时是Bootstrap()还是LoadFromDisk()
    // MVCC 快照隔离：低频惰性真空触发器（每执行 N 条成功后租一次全表 Vacuum）。
    int vacuum_statement_counter_ = 0;

    // ---- Phase B：崩溃注入状态 ----
    std::atomic<int> crash_after_n_statements_{0};  // > 0 时每成功执行一条 N--

    // 在 ExecuteSQL 内部、autocommit commit 之后调一次。若还有崩溃名额则扣减，
    // 归零后立即 _Exit(1)。
    void MaybeCrashAfterSuccess();
};

}  // namespace sqlcompiler