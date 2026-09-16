#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ast/AST.h"
#include "catalog/SystemCatalog.h"
#include "execution/ExecutionEngine.h"
#include "lexer/Token.h"
#include "plan/Plan.h"
#include "session/Session.h"
#include "storage/BufferPoolManager.h"
#include "storage/DiskManager.h"
#include "storage/LockManager.h"
#include "storage/StorageAccess.h"
#include "storage_engine/Value.h"  // 71_proc_out_params: SessionVars uses Value
#include "txn/TransactionManager.h"

namespace sqlcompiler {

// 前向声明：避免 Database.h 引入 LogManager 的全头（与 TransactionManager 形成环）。
class LogManager;
class RecoveryManager;
class CommitTracker;

// 数据库总入口（门面/Facade）：
// 串联 编译器模块（Lexer -> Parser -> SemanticAnalyzer -> Planner -> Optimizer）
// 与 执行系统模块（SystemCatalog -> BufferPoolManager -> DiskManager -> ExecutionEngine），
// 对外提供"输入一条SQL文本，返回执行结果"的统一接口，供CLI/main.cpp调用
class Database {
public:
    // db_file: 数据文件路径（不存在则创建新库）；buffer_pool_size: 缓冲池可容纳的页数
    // bg_flush_ms: 后台异步刷脏线程的唤醒间隔（毫秒，0 = 关闭，默认）。
    // bg_vacuum_ms: MVCC 后台多版本真空线程的唤醒间隔（毫秒，0 = 关闭，默认）。
    explicit Database(const std::string& db_file, size_t buffer_pool_size = 64,
                      int bg_flush_ms = 0, int bg_vacuum_ms = 0);
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

    // ---- 统一的存储访问门面 ----
    // 把 BPM + DM 封装为单个 StorageAccess 暴露给执行引擎 / 算子 / catalog，
    // 避免它们直接依赖 BufferPoolManager / DiskManager 内部细节。
    StorageAccess& Storage() { return *storage_; }
    const StorageAccess& Storage() const { return *storage_; }
    BufferPoolManager* GetBufferPoolManager() const { return buffer_pool_manager_.get(); }
    DiskManager* GetDiskManager() const { return disk_manager_.get(); }

    // ---- Phase B：崩溃注入（仅测试用） ----
    // 在接下来的 N 条 ExecuteSQL 成功完成后，强制 std::_Exit(1)。
    // 0 = 关闭（默认）。
    void SetCrashInjectionPoint(int n_statements);

    // 注入一次「立即崩溃」。由 \crash 调试命令触发；让 49_acid_recovery
    // 之类的测试在不重启进程的前提下也保留 main.cpp 的退出路径不变。
    void TriggerCrashNow();

    // ---- 71_proc_out_params：会话变量 (@var) 入口 ----
    //
    // Database 是会话变量的权威持有者；每次 ExecuteSQL 在构造 ctx 时把
    // 内部 session_vars_ 指针挂到 ExecutionContext 上，procedure / trigger
    // / 顶层 SET @x = expr 等路径都直接写到这张表。下次 SELECT @x 时由
    // ExpressionEvaluator 通过 ctx.GetSessionVar 回查。Database 自身不持有
    // 任何 ExecutionContext，写入后会立即对所有后续 SQL 可见。
    std::unordered_map<std::string, Value>& SessionVars() { return session_vars_; }
    const std::unordered_map<std::string, Value>& SessionVars() const {
        return session_vars_;
    }
    void SetSessionVar(const std::string& name, Value v) {
        session_vars_[name] = std::move(v);
    }
    Value GetSessionVar(const std::string& name) const {
        auto it = session_vars_.find(name);
        if (it == session_vars_.end()) return Value::MakeNull();
        return it->second;
    }
    void ClearSessionVars() { session_vars_.clear(); }

    // ---- Phase 1.5: 调试输出模式 ----
    // 最近一次成功完成对应阶段的 SQL 编译产物，用于 REPL 的 \.tokens / \.ast
    // / \.plan / \.optimized 元命令展示。每条 ExecuteSQL 调用开始时都会重置这
    // 三个缓存，并在各阶段成功后写入新值；阶段失败时该条目保持为空指针 /
    // 空向量，调用方应判空再决定如何打印。
    //
    // 计划缓存拆成两份：
    //   LastPlan()         —— Optimizer::Optimize 完成后的最终计划（执行器真正跑的）。
    //   LastPlanBeforeOptText() —— Planner::CreatePlan 刚返回、Optimizer 尚未
    //                            改写前的原始计划树的 ToString() 序列化文本。
    // 用「文本快照」而非「PlanNode 指针」的原因：Optimizer::PushDownPredicates
    // 会就地改写 SeqScanNode::predicate（line 224），并可能改写 Filter 与子树
    // 的拓扑；若仅靠 aliasing shared_ptr 抓原根，会观察到优化后的状态。文本
    // 快照在调用 Optimize 前一次性渲染，规避了就地修改问题，且不需要为每个
    // PlanNode / Expr 派生类实现 Clone()。
    const std::vector<Token>& LastTokens() const { return last_tokens_; }
    const Statement* LastAst() const { return last_ast_.get(); }
    const PlanNode* LastPlan() const { return last_plan_.get(); }
    const std::string& LastPlanBeforeOptText() const {
        return last_plan_before_opt_text_;
    }

    // 生成存储子系统诊断信息字符串（由 \stats 命令触发）。
    // 输出缓冲池命中/缺失/替换统计、命中率、磁盘页数与近期页替换日志，
    // 满足指导书「页级读写、缓存命中统计、页替换日志输出」的要求。
    std::string GetStorageStats() const;

    // ---- T4 诊断：\analyze 命令 ----
    // 生成页映射 / 介质 / CRC 校验的诊断信息字符串（由 \analyze 命令触发）：
    //   * 页映射：缓冲池中每个逻辑页 → 帧号、是否脏、访问温度；
    //   * 介质：当前块设备名（memory / sparse / 网络回环 / 文件）；
    //   * CRC 累计校验失败计数与磁盘 IO 计数（介质损坏观测）。
    std::string GetStorageAnalysis() const;

    // 诊断：磁盘物理读/写页累计计数（转发 DiskManager）。供量化测试（如
    // 复合索引多列前缀收敛带来的扫描页数下降）与 \stats 扩展使用；只读无副作用。
    long long GetDiskIOReadCount() const;
    long long GetDiskIOWriteCount() const;

    // ---- MVCC 低频后台真空线程（可观测性/控制）----
    // 线程生命周期由构造参数 bg_vacuum_ms 驱动；以下访问器供诊断与测试使用。
    bool IsBackgroundVacuumEnabled() const;
    long GetBackgroundVacuumTicks() const;  // 被唤醒并执行真空的次数（可观测）

    // ---- U3-3：非相关子查询物化缓存观测（跨语句累计）----
    // 每条语句的 ExecutionContext 通过 ExecutionEngine 把物化/命中计数汇入此 sink，
    // 供 \stats 展示与白盒单元测试断言（多会话并发时原子累计）。
    const SubqueryCacheStats& GetSubqueryCacheStats() const {
        return subquery_cache_stats_;
    }

private:
    // storage_ 必须声明在 disk_manager_ / buffer_pool_manager_ 之前：成员按
    // 声明逆序析构，因此 storage_ 最后销毁，其持有的非所有权裸指针在 BPM / DM
    // 析构前已经失效但不被访问，安全。
    std::unique_ptr<StorageAccess> storage_;
    std::unique_ptr<DiskManager> disk_manager_;
    // Phase B：LogManager 必须声明在 BufferPoolManager 之前——成员逆序析构时
    // BufferPoolManager 的析构仍会 FlushAllPages → FlushPageUnlocked 访问
    // log_manager_->durable_lsn()/Flush()，若 LogManager 先被销毁则构成
    // use-after-free（Phase 4 起 durable_lsn 持锁读取后必现崩溃）。
    std::unique_ptr<LogManager> log_manager_;        // Phase B：WAL 写出器
    std::unique_ptr<BufferPoolManager> buffer_pool_manager_;
    std::unique_ptr<SystemCatalog> catalog_;
    std::unique_ptr<ExecutionEngine> execution_engine_;
    std::unique_ptr<TransactionManager> txn_manager_;
    // U3-3：非相关子查询物化缓存观测 sink（Engine 构造时注入，见 Database.cpp）。
    SubqueryCacheStats subquery_cache_stats_;
    std::unique_ptr<RecoveryManager> recovery_;      // Phase B：启动期 ARIES 恢复
    // T2：跨会话共享的事务级锁管理器（隔离级别 + 死锁回收）。
    std::unique_ptr<LockManager> lock_manager_;
    // MVCC 快照隔离：跨会话共享的提交跟踪器（CSN 注入 + 版本可见性判定）。
    std::unique_ptr<CommitTracker> commit_tracker_;

    // ---- U2 基准：\bench 命令 ----
    // 进程内多会话并发读写混合负载吞吐/延迟基准（由 \bench <threads> <ops> 触发）。
    ExecutionResult RunBenchCommand(const std::string& sql, TransactionManager* txn_mgr);

    // T2 并发会话：全局 txn_id 序列器 + 会话所有权容器 + 会话执行器私有辅助。
    TxnIdSequencer txn_seq_;
    std::vector<std::shared_ptr<Session>> sessions_;
    ExecutionResult ExecuteSQLImpl(const std::string& sql, TransactionManager* txn_mgr);

    std::string db_file_path_;       // 规范化后的绝对路径
    std::string wal_file_path_;      // <db_file>.wal
    bool is_new_database_;           // 用于判断启动时是Bootstrap()还是LoadFromDisk()
    // MVCC 快照隔离：低频惰性真空触发器（每执行 N 条成功后租一次全表 Vacuum）。
    int vacuum_statement_counter_ = 0;

    // ---- Phase 1.5: 调试输出模式缓存 ----
    // ExecuteSQL 在词法/语法/计划成功后写入；阶段失败时不写入，保留上一次成功的值。
    // 每次 ExecuteSQL 入口处都会先 ResetLastArtifacts() 全部清空。
    // AST / Plan 用 std::shared_ptr 是为了与 Parser / Planner 返回的
    // StatementPtr / PlanNodePtr 共享同一控制块（aliasing constructor），
    // 既保证本字段的指针有效，又不会重复释放底层对象。
    std::vector<Token> last_tokens_;
    std::shared_ptr<Statement> last_ast_;
    // 优化后的计划（执行器真正跑的）。PlanNode 树通过 aliasing shared_ptr 与
    // Optimizer::Optimize 返回值共享所有权，避免双重释放。
    std::shared_ptr<PlanNode> last_plan_;
    // 优化前的计划：Planner::CreatePlan 刚返回时一次性渲染成 ToString() 文本
    // 缓存。文本而不是 PlanNode 指针的原因见 LastPlanBeforeOptText() 注释。
    std::string last_plan_before_opt_text_;
    void ResetLastArtifacts();

    // ---- Phase B：崩溃注入状态 ----
    std::atomic<int> crash_after_n_statements_{0};  // > 0 时每成功执行一条 N--

    // 71_proc_out_params：会话变量存储。Database 是会话变量（@var）的唯一权威
    // 持有者；每次 ExecuteSQL 在构造 ExecutionContext 时把同一指针注入 ctx，
    // UdfExecutor（SET @var）、TriggerExecutor（AFTER @col）和 CALL/SELECT
    // 的 @var 读写都直接打到这张表。ExecuteSQL 结束不需要拷贝 —— 上下文只是
    // 引用了 Database 的成员。线程模型：项目当前为单线程 REPL / 脚本驱动。
    std::unordered_map<std::string, Value> session_vars_;

    // ---- MVCC 低频后台真空线程 ----
    // 与 E5 后台刷脏线程同一模式：仅当构造参数 bg_vacuum_ms > 0 时启动，默认关闭
    // （行为与旧版完全一致）。线程每隔 interval 以「最老活动快照」为界对全部表做
    // 一次惰性多版本真空（TableHeap::Vacuum），回收已提交且不再被任何活动快照
    // 可见的旧版本槽位。生命周期：构造末尾启动，Shutdown 时先停再刷盘/同步。
    void StartBackgroundVacuum(std::chrono::milliseconds interval);
    void StopBackgroundVacuum();  // 幂等：未运行时 no-op
    void BackgroundVacuumLoop();

    std::thread bg_vacuum_thread_;          // 后台真空线程；未运行时为空
    mutable std::mutex bg_vacuum_mutex_;    // 保护 bg_vacuum_running_ / stop_ / interval_
    std::condition_variable bg_vacuum_cv_;
    bool bg_vacuum_running_ = false;
    bool bg_vacuum_stop_ = false;
    std::chrono::milliseconds bg_vacuum_interval_{0};
    std::atomic<long> bg_vacuum_ticks_{0};

    // 在 ExecuteSQL 内部、autocommit commit 之后调一次。若还有崩溃名额则扣减，
    // 归零后立即 _Exit(1)。
    void MaybeCrashAfterSuccess();
};

}  // namespace sqlcompiler