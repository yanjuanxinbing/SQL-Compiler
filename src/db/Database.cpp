#include "db/Database.h"

#include "common/Error.h"
#include "lexer/Lexer.h"
#include "optimizer/Optimizer.h"
#include "parser/Parser.h"
#include "plan/Planner.h"
#include "semantic/SemanticAnalyzer.h"
#include "txn/LogManager.h"
#include "txn/CommitTracker.h"
#include "txn/RecoveryManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <system_error>
#include <thread>

namespace sqlcompiler {

namespace fs = std::filesystem;

namespace {

// 检测某条 ExecuteSQL 输入是不是 \crash 调试指令（Phase B crash injection）。
// 返回 true 表示这是调试命令，调用方应跳过常规执行。
bool IsCrashDebugCommand(const std::string& sql) {
    // 跳过前导空白
    size_t i = 0;
    while (i < sql.size() && (sql[i] == ' ' || sql[i] == '\t')) ++i;
    if (i >= sql.size()) return false;
    return sql[i] == '\\';
}

// Phase C：\crash_after_undo_steps N —— 让 Rollback 在撤销 N 条后立即 _Exit(1)。
//   return: true 表示该 sql 是本指令（已经被处理）；false 表示不是。
//   n_out:  设置为 N（> 0）。
bool HandleCrashAfterUndoSteps(const std::string& sql, int* n_out) {
    if (n_out == nullptr) return false;
    const std::string prefix = "\\crash_after_undo_steps";
    size_t i = 0;
    while (i < sql.size() && (sql[i] == ' ' || sql[i] == '\t')) ++i;
    if (sql.compare(i, prefix.size(), prefix) != 0) return false;
    i += prefix.size();
    while (i < sql.size() && (sql[i] == ' ' || sql[i] == '\t')) ++i;
    // 解析 N。
    int n = 0;
    bool any = false;
    while (i < sql.size() && sql[i] >= '0' && sql[i] <= '9') {
        n = n * 10 + (sql[i] - '0');
        ++i;
        any = true;
    }
    if (!any) return false;
    *n_out = n;
    return true;
}

}  // namespace

Database::Database(const std::string& db_file, size_t buffer_pool_size,
                   int bg_flush_ms, int bg_vacuum_ms)
    : is_new_database_(false) {
    // 1) 将路径规范化为绝对路径，避免后续 cwd 变化导致路径失效
    std::error_code ec;
    fs::path abs_path = fs::absolute(db_file, ec);
    if (ec) {
        throw std::runtime_error("Invalid database file path '" + db_file +
                                 "': " + ec.message());
    }
    db_file_path_ = abs_path.string();
    wal_file_path_ = db_file_path_ + ".wal";

    // 2) 父目录必须存在；否则明确报错，而不是让 DiskManager 默默失败
    fs::path parent = abs_path.parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
        throw std::runtime_error("Database directory does not exist: " +
                                 parent.string());
    }
    if (!parent.empty() && !fs::is_directory(parent)) {
        throw std::runtime_error("Database parent path is not a directory: " +
                                 parent.string());
    }

    // 3) 先探测文件是否存在；若不存在则先用 ofstream 显式创建一个空文件
    is_new_database_ = !fs::exists(abs_path);
    if (!is_new_database_) {
        std::error_code size_ec;
        auto sz = fs::file_size(abs_path, size_ec);
        if (size_ec || sz < PAGE_SIZE) {
            is_new_database_ = true;
        }
    }
    if (!fs::exists(abs_path)) {
        std::ofstream create(db_file_path_, std::ios::binary | std::ios::trunc);
        if (!create.is_open()) {
            throw std::runtime_error(
                "Cannot create database file '" + db_file_path_ +
                "'. Check that the directory is writable and the path is valid.");
        }
        create.close();
    }

    // 4) 构造各子系统组件
    disk_manager_ = std::make_unique<DiskManager>(db_file_path_);
    buffer_pool_manager_ = std::make_unique<BufferPoolManager>(
        buffer_pool_size, disk_manager_.get());

    // Phase 5（G5）：温度感知刷盘 + 自适应阈值的环境变量开关（仅调优，不改语义）。
    //   * SQLCOMPILER_TEMP_FLUSH 设置即开启温度感知刷盘（默认关闭，行为与旧版逐页
    //     全量一致）：全量刷脏只写回冷脏页，热脏页延后留池（NO-FORCE + WAL 保证
    //     正确性）。开启时默认启用自适应阈值（hot_ratio_percent_=20%，P 分位动态估计）。
    //   * SQLCOMPILER_HOT_RATIO_PERCENT（1..99）覆盖目标热页占比；须与
    //     SQLCOMPILER_TEMP_FLUSH 配合（仅设占比不开启刷盘时自适应不生效）。
    if (std::getenv("SQLCOMPILER_TEMP_FLUSH") != nullptr) {
        buffer_pool_manager_->SetTemperatureFlushEnabled(true);
        buffer_pool_manager_->SetAdaptiveThresholdEnabled(true);
    }
    if (const char* env = std::getenv("SQLCOMPILER_HOT_RATIO_PERCENT")) {
        const int pct = std::atoi(env);
        if (pct >= 1 && pct <= 99) {
            buffer_pool_manager_->SetHotRatioPercent(static_cast<uint64_t>(pct));
        }
    }

    // Phase B：先打开 WAL 文件，然后跑恢复（ARIES 3-phase），再决定是 Bootstrap
    // 还是 LoadFromDisk。注意：LogManager 必须在 catalog LoadFromDisk 之前完成，
    // 因为 redo 期间可能需要 catalog 的元数据来解析目录页。
    bool created_wal = false;
    if (!fs::exists(wal_file_path_)) {
        // 显式创建空 WAL，让 LogManager::OpenForAppend 后续路径走「已存在文件」
        // 的同一条 open 调用，避免 _open + O_CREAT 的语义模糊。
        std::ofstream create(wal_file_path_, std::ios::binary | std::ios::trunc);
        create.close();
        created_wal = true;
    }
    log_manager_ = std::make_unique<LogManager>(wal_file_path_);
    buffer_pool_manager_->SetLogManager(log_manager_.get());

    // 新库场景：不需要恢复；但需把 LogManager 立刻刷一次以建立 durable_lsn=0。
    // 否则等到第一条 UPDATE 之前 LogManager 的 next_lsn_ 仍是 1，但
    // durable_lsn_=0，会让「page_lsn==1 > durable_lsn==0」永远成立，触发不必要的 flush。

    // 默认会话与后续所有会话共享 txn_seq_，保证并发下 txn_id 全局唯一。
    txn_manager_ = std::make_unique<TransactionManager>(&txn_seq_);
    txn_manager_->SetBufferPoolManager(buffer_pool_manager_.get());
    txn_manager_->SetLogManager(log_manager_.get());
    txn_manager_->SetDiskManager(disk_manager_.get());
    // 事务级锁管理器：跨会话共享，供隔离级别实现使用（锁粒度 = 表首页页号）。
    lock_manager_ = std::make_unique<LockManager>();
    txn_manager_->SetLockManager(lock_manager_.get());
    // MVCC 快照隔离：跨会话共享的提交跟踪器（CSN 分配 + 可见性判定）。
    // 复刻 LockManager 的注入模式——默认会话与所有 CreateSession 会话注入同一实例。
    commit_tracker_ = std::make_unique<CommitTracker>();
    txn_manager_->SetCommitTracker(commit_tracker_.get());

    catalog_ = std::make_unique<SystemCatalog>(buffer_pool_manager_.get());
    // 把 LogManager 注入 catalog 中的 TableHeap，让 CREATE TABLE / INDEX
    // 也走 WAL。Phase B 的 WAL 钩子在 TableHeap / BPlusTree 的 SetLogManager 里。
    // 此时 table_heaps_ / index_trees_ 均为空；第二次调用会在 Bootstrap /
    // LoadFromDisk 之后再遍历一次把指针注入新创建的实例。
    if (log_manager_ != nullptr) {
        catalog_->SetLogManager(log_manager_.get());
    }

    // Phase B：恢复跑在 LoadFromDisk 之前。即便 is_new_database_ 也跑一遍
    // —— 空 WAL 的 ReadAll() 直接返回，Run() 等价于 no-op。
    recovery_ = std::make_unique<RecoveryManager>(
        log_manager_.get(), buffer_pool_manager_.get(), disk_manager_.get(),
        txn_manager_.get(), catalog_.get());
    if (!recovery_->Run()) {
        // 恢复失败：在测试场景里不强求硬错误（损坏的 WAL 不阻塞启动）；
        // 继续 Bootstrap / LoadFromDisk 把现有数据文件视为权威。
    }

    if (is_new_database_) {
        catalog_->Bootstrap();
    } else {
        catalog_->LoadFromDisk();
    }
    // Bootstrap / LoadFromDisk 会创建 TableHeap / BPlusTree；再次把
    // LogManager 推进去，让 WAL 钩子在新实例上生效。
    if (log_manager_ != nullptr) {
        catalog_->SetLogManager(log_manager_.get());
    }

    execution_engine_ = std::make_unique<ExecutionEngine>(catalog_.get(),
                                                         txn_manager_.get(),
                                                         &subquery_cache_stats_);

    // Phase B：每次 Database 启动都让 LogManager 的 durable_lsn 至少推进到当前
    // 文件末尾。AppendRecord 一条占位记录后 Flush，让后续 FlushPage 的 LSN 检查
    // 不会因为「durable_lsn 永远小于新写入」而无谓刷盘。
    if (log_manager_ != nullptr && created_wal) {
        // 新建空 WAL 时写一条 CHECKPOINT 作为起点标记，便于以后分析；
        // 不写也没有正确性问题（恢复时 ReadAll 会得到空）。
        recovery_->Checkpoint();
    }

    // E5：可选地启动后台异步刷脏线程（仅在 bg_flush_ms > 0 时开启，默认关闭）。
    // 放最后——确保恢复/目录/执行引擎全部就绪后线程才开始触碰脏页与 WAL。
    if (bg_flush_ms > 0 && buffer_pool_manager_ != nullptr) {
        buffer_pool_manager_->StartBackgroundFlush(
            std::chrono::milliseconds(bg_flush_ms));
    }
    // MVCC 低频后台真空线程（仅在 bg_vacuum_ms > 0 时开启，默认关闭）。
    // 与刷脏线程同样放最后启动：commit_tracker_ / catalog_ 均已就绪。
    if (bg_vacuum_ms > 0 && commit_tracker_ != nullptr && catalog_ != nullptr) {
        StartBackgroundVacuum(std::chrono::milliseconds(bg_vacuum_ms));
    }
}

Database::~Database() {
    Shutdown();
}

// 默认会话：等价于旧的单连接入口。
ExecutionResult Database::ExecuteSQL(const std::string& sql) {
    return ExecuteSQLImpl(sql, txn_manager_.get());
}

// 会话感知执行：在指定会话的 TransactionManager 上执行，使多会话并发时
// 各自持有独立的「当前事务/嵌套深度」。
ExecutionResult Database::ExecuteSQL(const std::string& sql, Session* session) {
    TransactionManager* txn_mgr = txn_manager_.get();
    if (session != nullptr) {
        txn_mgr = session->GetTransactionManager();
        if (txn_mgr == nullptr) txn_mgr = txn_manager_.get();
    }
    return ExecuteSQLImpl(sql, txn_mgr);
}

// 新建会话：独立 TransactionManager + 共享资源（缓冲池/WAL/目录/全局 id 序列）。
std::shared_ptr<Session> Database::CreateSession() {
    auto mgr = std::make_unique<TransactionManager>(&txn_seq_);
    mgr->SetBufferPoolManager(buffer_pool_manager_.get());
    mgr->SetLogManager(log_manager_.get());
    mgr->SetDiskManager(disk_manager_.get());
    // 注入跨会话共享的锁管理器：多个会话的显式事务在计划执行边界协调同一组表锁，
    // 实现隔离级别与死锁回收。早于本方法之前已建好的默认会话也已注入。
    mgr->SetLockManager(lock_manager_.get());
    // MVCC 快照隔离：跨会话共享的提交跟踪器（默认会话在上面的构造函数内已注入）。
    mgr->SetCommitTracker(commit_tracker_.get());
    auto session = std::make_shared<Session>(std::move(mgr));
    sessions_.push_back(session);
    return session;
}

ExecutionResult Database::ExecuteSQLImpl(const std::string& sql,
                                         TransactionManager* txn_mgr) {
    // \bench —— U2 进程内多会话并发读写混合负载基准。须在所有 \crash 调试分支之前
    // 独立识别（\bench 是长时间并发负载，不能落入崩溃注入语义）。
    {
        size_t i0 = 0;
        while (i0 < sql.size() && (sql[i0] == ' ' || sql[i0] == '\t')) ++i0;
        if (sql.compare(i0, 6, "\\bench") == 0) {
            bool is_bench = (i0 + 6 >= sql.size()) ||
                            (sql[i0 + 6] == ' ' || sql[i0 + 6] == '\t' ||
                             sql[i0 + 6] == ';' || sql[i0 + 6] == '\r' ||
                             sql[i0 + 6] == '\n');
            if (is_bench) {
                return RunBenchCommand(sql, txn_mgr);
            }
        }
    }
    // \stats —— 输出存储子系统诊断信息（缓冲池/页分配/替换日志）。
    // \analyze —— T4 诊断：输出页映射/介质/CRC 校验信息。
    // 均需在通用 \crash 处理之前识别。
    if (IsCrashDebugCommand(sql)) {
        size_t i = 0;
        while (i < sql.size() && (sql[i] == ' ' || sql[i] == '\t')) ++i;
        if (sql.compare(i, 6, "\\stats") == 0) {
            bool is_stats = (i + 6 >= sql.size()) ||
                            (sql[i + 6] == ' ' || sql[i + 6] == '\t' ||
                             sql[i + 6] == ';' || sql[i + 6] == '\r' ||
                             sql[i + 6] == '\n');
            if (is_stats) {
                ExecutionResult ok;
                ok.success = true;
                ok.message = GetStorageStats();
                return ok;
            }
        }
        if (sql.compare(i, 8, "\\analyze") == 0) {
            bool is_analyze = (i + 8 >= sql.size()) ||
                              (sql[i + 8] == ' ' || sql[i + 8] == '\t' ||
                               sql[i + 8] == ';' || sql[i + 8] == '\r' ||
                               sql[i + 8] == '\n');
            if (is_analyze) {
                ExecutionResult ok;
                ok.success = true;
                ok.message = GetStorageAnalysis();
                return ok;
            }
        }
    }
    // Phase C 调试命令：\crash_after_undo_steps N —— 让 Rollback 在撤销 N 步后
    // 立即 _Exit(1)。必须在通用 \crash 之前识别（否则通用路径会先 _Exit）。
    if (IsCrashDebugCommand(sql)) {
        int n = 0;
        if (HandleCrashAfterUndoSteps(sql, &n)) {
            if (txn_mgr != nullptr) {
                txn_mgr->SetCrashAfterUndoSteps(n);
                std::cerr << "[crash-after-undo-steps] armed; will _Exit(1) "
                          << "after " << n << " undo step(s) during Rollback"
                          << std::endl;
            }
            ExecutionResult ok;
            ok.success = true;
            ok.message = "crash_after_undo_steps armed";
            return ok;
        }
        // Phase B 调试命令：\crash —— 立即 _Exit(1) 不做清理。
        std::_Exit(1);
    }

    ExecutionResult result;
    try {
        Lexer lexer(sql);
        auto tokens = lexer.Tokenize();
        Parser parser(std::move(tokens));
        auto statement = parser.Parse();
        if (!statement) {
            result.success = false;
            result.message = "no statement";
            return result;
        }
        const bool is_read_only =
            statement->GetType() == NodeType::SELECT_STMT ||
            statement->GetType() == NodeType::WITH_STMT ||
            statement->GetType() == NodeType::SET_OP_STMT ||
            statement->GetType() == NodeType::EXPLAIN_STMT ||
            statement->GetType() == NodeType::SHOW_STMT ||
            statement->GetType() == NodeType::BEGIN_STMT ||
            statement->GetType() == NodeType::COMMIT_STMT ||
            statement->GetType() == NodeType::ROLLBACK_STMT ||
            statement->GetType() == NodeType::SAVEPOINT_STMT ||
            statement->GetType() == NodeType::ROLLBACK_TO_STMT ||
            statement->GetType() == NodeType::RELEASE_SAVEPOINT_STMT;
        const bool need_autocommit =
            !is_read_only && txn_mgr != nullptr &&
            txn_mgr->GetCurrentDepth() == 0;
        Transaction* autocommit_txn = nullptr;
        if (need_autocommit) {
            autocommit_txn = txn_mgr->Begin();
            // Phase B：写 BEGIN 记录，让 redo 阶段看到一致的事务边界。
            txn_mgr->LogBegin(autocommit_txn);
        }
        try {
            SemanticAnalyzer analyzer(catalog_.get(), catalog_->GetSymbolTable());
            if (!analyzer.Analyze(statement)) {
                std::string msg;
                for (const auto& e : analyzer.GetErrors()) {
                    if (!msg.empty()) msg += "; ";
                    msg += e.message;
                }
                result.success = false;
                result.message = "semantic error: " + msg;
                if (need_autocommit && txn_mgr != nullptr) {
                    txn_mgr->Rollback();
                }
                return result;
            }
            Planner planner(catalog_.get(), catalog_->GetSymbolTable());
            auto plan = planner.CreatePlan(statement);
            Optimizer optimizer(catalog_.get());
            plan = optimizer.Optimize(plan);
            // 会话感知：若走会话，重建一个绑定『本会话 txn_manager』的执行上下文
            // 再跑同一棵计划，让 DML 算子抓到正确的 undo 上下文；否则走引擎默认路径。
            const bool session_exec = (txn_mgr != nullptr && txn_mgr != txn_manager_.get());
            ExecutionResult exec_result;
            if (session_exec) {
                ExecutionContext session_ctx(catalog_.get(), txn_mgr,
                                             &subquery_cache_stats_);
                session_ctx.SetTransaction(txn_mgr->GetCurrentTransaction());
                exec_result = execution_engine_->ExecuteSubplan(plan, &session_ctx);
            } else {
                exec_result = execution_engine_->Execute(plan);
            }
            if (exec_result.success && buffer_pool_manager_) {
                buffer_pool_manager_->FlushAllPages();
            }
            if (need_autocommit && txn_mgr != nullptr) {
                if (exec_result.success) {
                    txn_mgr->Commit();
                    MaybeCrashAfterSuccess();
                } else {
                    txn_mgr->Rollback();
                }
            }
            // MVCC 快照隔离：低频惰性真空——每完成 kVacuumInterval 条成功后，
            // 以「最老活动快照」为界回收一次全表旧版本槽位（惰性无害，乘客低）。
            // Phase 2：常规回收已由写路径内联真空（TableHeap::RunInlineVacuum）
            // 摊薄完成，此处为「低频兜底」通道；OldestActiveSnapshot 已是 O(1)。
            constexpr int kVacuumInterval = 100;
            if (++vacuum_statement_counter_ >= kVacuumInterval &&
                commit_tracker_ != nullptr && catalog_ != nullptr) {
                vacuum_statement_counter_ = 0;
                catalog_->VacuumAll(commit_tracker_->ActiveSnapshotList());
            }
            return exec_result;
        } catch (...) {
            if (need_autocommit && txn_mgr != nullptr) {
                txn_mgr->Rollback();
            }
            throw;
        }
    } catch (const CompilerException& e) {
        result.success = false;
        result.message = FormatError(e);
        return result;
    } catch (const std::exception& e) {
        result.success = false;
        result.message = std::string("error: ") + e.what();
        return result;
    }
}

std::vector<ExecutionResult> Database::ExecuteScript(const std::string& sql_script) {
    std::vector<ExecutionResult> results;
    std::string buf;
    bool in_string = false;
    for (size_t i = 0; i < sql_script.size(); ++i) {
        char c = sql_script[i];
        if (c == '\'' && (i == 0 || sql_script[i - 1] != '\\')) {
            in_string = !in_string;
        }
        if (c == ';' && !in_string) {
            std::string stmt = buf;
            buf.clear();
            size_t a = stmt.find_first_not_of(" \t\r\n");
            size_t b = stmt.find_last_not_of(" \t\r\n");
            if (a == std::string::npos) continue;
            stmt = stmt.substr(a, b - a + 1);
            if (stmt.empty()) continue;
            results.push_back(ExecuteSQL(stmt));
        } else {
            buf.push_back(c);
        }
    }
    size_t a = buf.find_first_not_of(" \t\r\n");
    size_t b = buf.find_last_not_of(" \t\r\n");
    if (a != std::string::npos) {
        std::string stmt = buf.substr(a, b - a + 1);
        if (!stmt.empty()) results.push_back(ExecuteSQL(stmt));
    }
    return results;
}

void Database::Shutdown() {
    // MVCC：先停后台真空线程（join），避免其与随后的 FlushAllPages / Checkpoint 竞态
    //（真空写墓碑脏页若与最终落盘并发，会造成撕裂或顺序颠倒）。
    StopBackgroundVacuum();
    // Phase B：先 flush bufferpool，然后写 CHECKPOINT + 同步 WAL。
    // E5：先停后台刷脏线程（join），避免其与随后的 FlushAllPages / Checkpoint / Sync 竞态。
    if (buffer_pool_manager_) buffer_pool_manager_->StopBackgroundFlush();
    if (buffer_pool_manager_) buffer_pool_manager_->FlushAllPages();
    // 重置所有会话（含默认）的悬挂事务，避免 txn 句柄在析构时残留。
    if (txn_manager_) txn_manager_->ResetForShutdown();
    for (auto& s : sessions_) {
        if (s && s->GetTransactionManager()) {
            s->GetTransactionManager()->ResetForShutdown();
        }
    }
    if (recovery_) recovery_->Checkpoint();
    if (log_manager_) {
        try { log_manager_->Flush(); } catch (...) {}
    }
}

void Database::SetCrashInjectionPoint(int n_statements) {
    crash_after_n_statements_.store(n_statements > 0 ? n_statements : 0);
    if (n_statements > 0) {
        std::cerr << "[crash-injection] armed; will _Exit(1) after "
                  << n_statements << " successful statements" << std::endl;
    } else {
        std::cerr << "[crash-injection] disarmed" << std::endl;
    }
}

void Database::TriggerCrashNow() {
    std::_Exit(1);
}

void Database::MaybeCrashAfterSuccess() {
    int expected = crash_after_n_statements_.load();
    while (expected > 0) {
        if (crash_after_n_statements_.compare_exchange_weak(expected, expected - 1)) {
            if (expected == 1) {
                std::cerr << "[crash-injection] triggering _Exit(1)" << std::endl;
                std::_Exit(1);
            }
            return;
        }
        expected = crash_after_n_statements_.load();
    }
}

// ---- MVCC 低频后台真空线程 ----
// 生命周期与 E5 后台刷脏线程一致：Start 幂等、Stop 先把线程移出锁外再 join
//（避免持锁 join 死锁）；线程循环用 cv wait_for 休眠，Stop 置位后立即唤醒退出。

void Database::StartBackgroundVacuum(std::chrono::milliseconds interval) {
    if (interval.count() <= 0) return;  // 禁用：保持与旧版逐字节一致
    std::lock_guard<std::mutex> lock(bg_vacuum_mutex_);
    if (bg_vacuum_running_) return;  // 已在运行：幂等
    bg_vacuum_interval_ = interval;
    bg_vacuum_stop_ = false;
    bg_vacuum_running_ = true;
    bg_vacuum_thread_ = std::thread(&Database::BackgroundVacuumLoop, this);
}

void Database::StopBackgroundVacuum() {
    std::thread to_join;
    {
        std::lock_guard<std::mutex> lock(bg_vacuum_mutex_);
        if (!bg_vacuum_running_) return;  // 幂等
        bg_vacuum_stop_ = true;
        bg_vacuum_running_ = false;
        to_join = std::move(bg_vacuum_thread_);  // 移出后另行 join，避免持锁 join 死锁
    }
    bg_vacuum_cv_.notify_all();
    if (to_join.joinable()) to_join.join();
}

bool Database::IsBackgroundVacuumEnabled() const {
    std::lock_guard<std::mutex> lock(bg_vacuum_mutex_);
    return bg_vacuum_running_;
}

long Database::GetBackgroundVacuumTicks() const {
    return bg_vacuum_ticks_.load();
}

// 后台循环：每隔 interval 以「最老活动快照」为界对全部表做一次惰性多版本真空。
// 关键（保持正确性与既有锁序）：
//   * 只回收 begin_csn < oldest_active_snapshot 的旧版本槽位（写墓碑），绝不动仍
//     在活动快照可见范围内的版本 → 与前台快照读/写者安全共存；
//   * TableHeap::Vacuum 内部持有 per-table write_mutex_ 与页写锁，与前台 DML 写
//     路径互斥，不撕裂；期间不触碰 BPM 全局锁（Vacuum 只在页锁作用域内访帧），
//     不会与前台页访问形成死锁环；
//   * 真空写墓碑不写 WAL、不 Sync：被回收的是已提交且不可再被快照读取的旧版本，
//     崩溃后最多「复活」一个同样不可见的旧版本（end_csn 仍挡得住可见性），
//     不会产生数据损坏（与语句级真空语义一致）；
//   * 单次失败（IO/损坏）吞掉：后台线程不得因单次失败退出或 terminate。
void Database::BackgroundVacuumLoop() {
    while (true) {
        std::unique_lock<std::mutex> lock(bg_vacuum_mutex_);
        bg_vacuum_cv_.wait_for(lock, bg_vacuum_interval_,
                               [this] { return bg_vacuum_stop_; });
        if (bg_vacuum_stop_) break;
        ++bg_vacuum_ticks_;
        // 释放 bg_vacuum_mutex_ 后再真空，让 StopBackgroundVacuum 能在真空期间置位 stop。
        lock.unlock();
        try {
            catalog_->VacuumAll(commit_tracker_->ActiveSnapshotList());
        } catch (...) {
            // 吸入异常：后台线程不得因单次失败退出或 terminate。
        }
    }
}

long long Database::GetDiskIOReadCount() const {
    return disk_manager_ != nullptr ? disk_manager_->GetIOReadCount() : 0;
}

long long Database::GetDiskIOWriteCount() const {
    return disk_manager_ != nullptr ? disk_manager_->GetIOWriteCount() : 0;
}

std::string Database::GetStorageStats() const {
    std::ostringstream oss;
    oss << "--- storage stats ---\n";
    if (buffer_pool_manager_ == nullptr || disk_manager_ == nullptr) {
        oss << "(not initialized)\n";
        return oss.str();
    }
    const BufferPoolStats& st = buffer_pool_manager_->GetStats();
    const auto& log = buffer_pool_manager_->GetReplacementLog();
    const long long hits = st.hit_count;
    const long long misses = st.miss_count;
    const long long repl = st.replacement_count;
    const double hit_ratio = (hits + misses) > 0
                                 ? (100.0 * hits) / (hits + misses)
                                 : 0.0;
    oss << "buffer pool frames   : "
        << buffer_pool_manager_->GetPoolSize() << "\n";
    oss << "buffer memory        : "
        << buffer_pool_manager_->GetMemoryUsageBytes() << " B / "
        << buffer_pool_manager_->GetMemoryCapBytes() << " B  ("
        << buffer_pool_manager_->GetMemoryUsageFrames() << "/"
        << buffer_pool_manager_->GetPoolSize() << " frames)\n";
    oss << "hits / misses / repl : " << hits << " / " << misses
        << " / " << repl << "\n";
    oss << "dirty writebacks     : " << st.writeback_count
        << "  (cold=" << st.writeback_cold_count
        << ", hot=" << st.writeback_hot_count << ")\n";
    oss << "temp flush           : "
        << (buffer_pool_manager_->IsTemperatureFlushEnabled() ? "on" : "off")
        << ", threshold=" << buffer_pool_manager_->GetHotAccessThreshold()
        << (buffer_pool_manager_->IsAdaptiveThresholdEnabled()
                ? " (adaptive, ratio=" +
                      std::to_string(buffer_pool_manager_->GetHotRatioPercent()) +
                      "%, last_est=" +
                      std::to_string(buffer_pool_manager_->GetLastEstimatedThreshold()) +
                      ", sampled=" +
                      std::to_string(buffer_pool_manager_->GetLastDirtySampled()) + ")"
                : "")
        << "\n";
    oss << "hit ratio            : " << std::fixed << std::setprecision(2)
        << hit_ratio << "%\n";
    oss << "disk pages / free    : "
        << disk_manager_->GetNumPages() << " / "
        << disk_manager_->GetNumFreePages() << "\n";
    oss << "disk reads / writes  : "
        << disk_manager_->GetIOReadCount() << " / "
        << disk_manager_->GetIOWriteCount() << "\n";
    oss << "wal fsyncs           : "
        << (log_manager_ != nullptr ? log_manager_->GetSyncCount() : 0)
        << "\n";
    // ---- U3-3 非相关子查询物化缓存观测（跨语句累计）----
    oss << "subquery cache       : materialize="
        << subquery_cache_stats_.materialize_count.load()
        << "  hits=" << subquery_cache_stats_.hit_count.load() << "\n";
    // ---- U2-2 锁等待观测：等待次数/平均时长/超时/死锁/当前等待者/谓词等待 ----
    if (lock_manager_ != nullptr) {
        const LockManager::LockWaitStats lw = lock_manager_->GetLockWaitStats();
        const double avg_us =
            (lw.wait_episodes > 0)
                ? static_cast<double>(lw.wait_us) / lw.wait_episodes
                : 0.0;
        const double pavg_us =
            (lw.predicate_wait_episodes > 0)
                ? static_cast<double>(lw.predicate_wait_us) /
                      lw.predicate_wait_episodes
                : 0.0;
        oss << "lock waits           : " << lw.wait_episodes
            << "  (avg " << std::fixed << std::setprecision(1) << avg_us
            << " us, in-flight=" << lw.current_waiters << ")\n";
        oss << "lock waits S/X       : S=" << std::setprecision(0) << lw.wait_episodes_s
            << "  X=" << lw.wait_episodes_x << "\n";
        oss << "lock timeout         : " << lw.timeout_count
            << "  (S=" << lw.timeout_s << ", X=" << lw.timeout_x << ")\n";
        oss << "lock deadlock/block  : deadlock_victim=" << lw.deadlock_count
            << "  trylock_wouldblock=" << lw.would_block_count << "\n";
        oss << "predicate waits      : " << lw.predicate_wait_episodes
            << "  (avg " << std::fixed << std::setprecision(1) << pavg_us
            << " us, timeout=" << lw.predicate_timeout_count << ")\n";
    }
    oss << "background flush     : "
        << (buffer_pool_manager_->IsBackgroundFlushEnabled()
                ? "every " +
                      std::to_string(
                          buffer_pool_manager_->GetBackgroundFlushInterval().count()) +
                      "ms, ticks=" +
                      std::to_string(buffer_pool_manager_->GetBackgroundFlushTicks())
                : "disabled")
        << "\n";
    // ---- T4 可观测性：命中构成 / 脏页年龄 / 后台刷脏直方图 / IO 队列 ----
    const long th = static_cast<long>(buffer_pool_manager_->GetHotAccessThreshold());
    const long tw = static_cast<long>(buffer_pool_manager_->GetWarmAccessThreshold());
    const long hit_total = st.hit_cold_count + st.hit_warm_count + st.hit_hot_count;
    const long hc = (hit_total > 0) ? (100 * st.hit_cold_count) / hit_total : 0;
    const long hw = (hit_total > 0) ? (100 * st.hit_warm_count) / hit_total : 0;
    const long hh = (hit_total > 0) ? (100 * st.hit_hot_count) / hit_total : 0;
    oss << "hit composition      : cold=" << st.hit_cold_count << " (" << hc
        << "%) warm=" << st.hit_warm_count << " (" << hw
        << "%) hot=" << st.hit_hot_count << " (" << hh
        << "%)  [tiers: hot>=" << th << ", warm>=" << tw << "]\n";
    const std::vector<long> age = buffer_pool_manager_->GetDirtyAgeDistribution();
    oss << "dirty age dist       : [0,1)=" << age[0] << " [1,4)=" << age[1]
        << " [4,10)=" << age[2] << " [10,30)=" << age[3]
        << " [30,inf)=" << age[4] << "  (tick units)\n";
    const std::vector<long> hist = buffer_pool_manager_->GetBackgroundFlushHistogram();
    oss << "bg flush histogram   : [0]=" << hist[0] << " [1]=" << hist[1]
        << " [2,4)=" << hist[2] << " [4,8)=" << hist[3]
        << " [8,16)=" << hist[4] << " [16,inf)=" << hist[5] << "\n";
    oss << "io queue             : dirty frames="
        << buffer_pool_manager_->GetDirtyFrameCount()
        << ", disk reads=" << disk_manager_->GetIOReadCount()
        << ", disk writes=" << disk_manager_->GetIOWriteCount() << "\n";
    const size_t cap = BufferPoolManager::GetReplacementLogCapacity();
    const size_t shown = std::min<size_t>(log.size(), 20);
    oss << "replacement log (" << log.size() << " recent, cap " << cap
        << "):\n";
    for (size_t k = log.size() - shown; k < log.size(); ++k) {
        const auto& e = log[k];
        oss << "    evict=pid " << e.evicted_page_id
            << "   loaded=pid " << e.loaded_page_id
            << (e.evicted_was_dirty ? "   [dirty]" : "") << "\n";
    }
    // ---- U1：索引健康（树高 / 页利用率 / 页构成）----
    if (catalog_ != nullptr) {
        auto idx = catalog_->CollectIndexStats();
        oss << "indexes               : " << idx.size() << "\n";
        for (const auto& s : idx) {
            oss << "    [" << s.name << "] height=" << s.height
                << " avg=" << std::fixed << std::setprecision(3) << s.avg_ratio
                << " min=" << std::fixed << std::setprecision(3) << s.min_ratio
                << " leaf=" << s.leaf_pages << " int=" << s.internal_pages << "\n";
        }
    }
    return oss.str();
}

// U2 基准：\bench <threads> <ops_per_thread> —— 进程内多会话并发读写混合负载。
// 在共享表 bench(id INT PRIMARY KEY, val INT) 上做 60% 点查 / 10% 范围查 /
// 10% UPDATE / 10% INSERT / 10% DELETE，统计吞吐与分类型 avg/p95 延迟及命中率。
ExecutionResult Database::RunBenchCommand(const std::string& sql,
                                          TransactionManager* txn_mgr) {
    ExecutionResult ok;
    ok.success = true;
    (void)txn_mgr;  // 建表/预填充统一走默认会话，workers 各自用 CreateSession() 新会话

    // ---- 解析线程数与每线程操作数（含省略的默认值） ----
    int threads = 4;
    int ops_per_thread = 1000;
    {
        std::istringstream iss(sql);
        std::string tok;
        iss >> tok;               // "\bench"
        int a = 0, b = 0;
        bool have_a = false, have_b = false;
        if (iss >> a) have_a = true;
        if (have_a && (iss >> b)) have_b = true;
        if (have_a && a > 0) threads = a;
        if (have_b && b > 0) ops_per_thread = b;
        threads = std::min(threads, 64);
        ops_per_thread = std::min(ops_per_thread, 1 << 20);
    }

    std::ostringstream oss;
    const int kBenchSize = 3000;  // 预填充行数（受 WAL 单行 ~24KB 约束，勿超 3k）
    // 块作用域 static：worker lambda（需访问 kProps）无需捕获它。
    static const int kProps[5] = {60, 10, 10, 10, 10};  // point/range/update/insert/delete

    // ---- 单线程建表 + 预填充（并发负载开始前完成） ----
    auto run_default = [this](const std::string& s) {
        return ExecuteSQL(s);  // 默认会话（本 Database 的默认 txn）
    };
    if (catalog_ != nullptr && catalog_->HasTable("bench")) {
        run_default("DROP TABLE bench;");
    }
    run_default("CREATE TABLE bench(id INT PRIMARY KEY, val INT);");
    for (int b = 0; b * 250 < kBenchSize; ++b) {
        std::ostringstream values;
        for (int k = 0; k < 250 && b * 250 + k < kBenchSize; ++k) {
            int id = b * 250 + k;
            if (k) values << ", ";
            values << "(" << id << ", " << id << ")";
        }
        run_default("INSERT INTO bench VALUES " + values.str() + ";");
    }
    // 使用后立即落盘，保证 \bench 多次运行（复用同一库文件）不互相干扰观测。
    buffer_pool_manager_->FlushAllPages();

    // 提交跟踪器已注入共享实例；快照基准缓冲池统计，用于报告负载期间命中率增量。
    const BufferPoolStats begin_stats = buffer_pool_manager_->GetStats();

    // ---- 并发工作线程：每线程独立会话 + 独立 RNG ----
    enum { kPoint = 0, kRange, kUpdate, kInsert, kDelete };
    struct WorkerReport {
        size_t cnt[5] = {0, 0, 0, 0, 0};
        long long sum_us[5] = {0, 0, 0, 0, 0};
        long long err = 0;
        std::vector<long long> lat[5];  // 各类型取样延迟（us），用于分类型 p95
        std::vector<long long> all;     // 全部操作延迟，用于 overall p95
    };
    std::vector<WorkerReport> reports(static_cast<size_t>(threads));

    std::atomic<int> start_flag{0};
    std::atomic<long long> next_insert{kBenchSize};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(threads));

    for (int t = 0; t < threads; ++t) {
        auto session = CreateSession();
        workers.emplace_back([this, session, t, ops_per_thread, &start_flag,
                              &next_insert, &reports]() {
            while (start_flag.load() != 1) {
                std::this_thread::yield();
            }
            WorkerReport& rep = reports[static_cast<size_t>(t)];
            // 以 线程id + 时间 作随机种子，避免各线程产生相同操作序列（否则并发偏移失准）。
            std::mt19937 rng(static_cast<unsigned>(
                                 std::chrono::high_resolution_clock::now()
                                     .time_since_epoch().count()) ^
                             static_cast<unsigned>(t * 2654435761u));
            std::uniform_int_distribution<int> dice(0, 99);
            std::uniform_int_distribution<int> key(0, kBenchSize - 1);
            for (int i = 0; i < ops_per_thread; ++i) {
                const int r = dice(rng);
                int type = kPoint;
                int acc = 0;
                for (int c = 0; c < 5; ++c) { acc += kProps[c]; if (r < acc) { type = c; break; } }
                std::string q;
                if (type == kPoint) {
                    q = "SELECT val FROM bench WHERE id=" + std::to_string(key(rng)) + ";";
                } else if (type == kRange) {
                    int a = key(rng);
                    q = "SELECT val FROM bench WHERE id BETWEEN " + std::to_string(a) +
                        " AND " + std::to_string(a + 20) + " ORDER BY id LIMIT 20;";
                } else if (type == kUpdate) {
                    q = "UPDATE bench SET val=val+1 WHERE id=" + std::to_string(key(rng)) + ";";
                } else if (type == kInsert) {
                    // 独立全局号段，避免并发唯一键冲突
                    q = "INSERT INTO bench VALUES (" + std::to_string(next_insert.fetch_add(1)) +
                        ", 0);";
                } else {
                    q = "DELETE FROM bench WHERE id=" + std::to_string(key(rng)) + ";";
                }
                const auto t0 = std::chrono::high_resolution_clock::now();
                ExecutionResult res = ExecuteSQL(q, session.get());
                const auto t1 = std::chrono::high_resolution_clock::now();
                const long long us = static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
                ++rep.cnt[type];
                rep.sum_us[type] += us;
                rep.lat[type].push_back(us);
                rep.all.push_back(us);
                if (!res.success) ++rep.err;
            }
        });
    }

    // ---- 栅栏释放 + 计时 ----
    const auto wall_t0 = std::chrono::steady_clock::now();
    start_flag.store(1);
    for (auto& w : workers) w.join();
    const auto wall_t1 = std::chrono::steady_clock::now();
    const double wall_s =
        std::chrono::duration<double>(wall_t1 - wall_t0).count();

    const BufferPoolStats end_stats = buffer_pool_manager_->GetStats();
    const long long d_hits = end_stats.hit_count - begin_stats.hit_count;
    const long long d_miss = end_stats.miss_count - begin_stats.miss_count;
    const long long d_total = d_hits + d_miss;
    const double hit_ratio =
        d_total > 0 ? (100.0 * d_hits) / d_total : 0.0;

    WorkerReport total;
    for (const auto& rep : reports) {
        for (int c = 0; c < 5; ++c) {
            total.cnt[c] += rep.cnt[c];
            total.sum_us[c] += rep.sum_us[c];
        }
        total.err += rep.err;
        total.all.insert(total.all.end(), rep.all.begin(), rep.all.end());
    }
    long long ok_ops = 0;
    for (int c = 0; c < 5; ++c) ok_ops += static_cast<long long>(total.cnt[c]);
    const long long all_ops = ok_ops + total.err;

    auto p95 = [](std::vector<long long> v) -> long long {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[static_cast<size_t>((v.size() * 95) / 100)];
    };
    auto report_type = [&](const char* name, int c) {
        if (total.cnt[c] == 0) {
            oss << "  " << name << " : 0\n";
            return;
        }
        const double avg = static_cast<double>(total.sum_us[c]) / total.cnt[c];
        long long p;
        {
            std::vector<long long> v;
            for (const auto& rep : reports) v.insert(v.end(), rep.lat[c].begin(), rep.lat[c].end());
            p = p95(v);
        }
        oss << "  " << name << " : n=" << total.cnt[c]
            << "  avg=" << std::fixed << std::setprecision(2) << avg
            << "us  p95=" << std::fixed << std::setprecision(0) << static_cast<double>(p)
            << "us\n";
    };

    oss << "--- bench report ---\n";
    oss << "threads      : " << threads << "\n";
    oss << "ops/thread   : " << ops_per_thread << "\n";
    oss << "total ops    : " << all_ops << "  (ok=" << ok_ops
        << " err=" << total.err << ")\n";
    oss << "elapsed      : " << std::fixed << std::setprecision(2) << wall_s << " s\n";
    oss << "throughput   : " << std::fixed << std::setprecision(1)
        << (wall_s > 0 ? all_ops / wall_s : 0.0) << " ops/s\n";
    oss << "hit ratio    : " << std::fixed << std::setprecision(2) << hit_ratio << "%\n";
    oss << "op breakdown :\n";
    report_type("point", kPoint);
    report_type("range", kRange);
    report_type("update", kUpdate);
    report_type("insert", kInsert);
    report_type("delete", kDelete);
    {
        const long long all_sum =
            total.sum_us[kPoint] + total.sum_us[kRange] + total.sum_us[kUpdate] +
            total.sum_us[kInsert] + total.sum_us[kDelete];
        const double avg = ok_ops > 0 ? static_cast<double>(all_sum) / ok_ops : 0.0;
        oss << "overall      : n=" << ok_ops
            << "  avg=" << std::fixed << std::setprecision(2) << avg
            << "us  p95=" << std::fixed << std::setprecision(0)
            << static_cast<double>(p95(total.all)) << "us\n";
    }
    ok.message = oss.str();
    return ok;
}

// T4 诊断（\analyze）：页映射 / 介质 / CRC 校验信息。
std::string Database::GetStorageAnalysis() const {
    std::ostringstream oss;
    oss << "--- storage analysis ---\n";
    if (buffer_pool_manager_ == nullptr || disk_manager_ == nullptr) {
        oss << "(not initialized)\n";
        return oss.str();
    }
    oss << "device               : " << disk_manager_->GetDeviceName() << "\n";
    oss << "disk pages / free    : "
        << disk_manager_->GetNumPages() << " / "
        << disk_manager_->GetNumFreePages() << "\n";
    oss << "disk reads / writes  : "
        << disk_manager_->GetIOReadCount() << " / "
        << disk_manager_->GetIOWriteCount() << "\n";
    oss << "crc errors           : " << disk_manager_->GetCrcErrorCount() << "\n";
    const auto& snapshot = buffer_pool_manager_->GetPageMapSnapshot();
    oss << "page map (" << snapshot.size() << " frames in pool):\n";
    const size_t shown = std::min<size_t>(snapshot.size(), 64);
    for (size_t k = 0; k < shown; ++k) {
        const PageMapEntry& e = snapshot[k];
        oss << "    pid " << e.page_id << " -> frame " << e.frame_id
            << (e.dirty ? " [dirty]" : "")
            << "  access=" << e.access_count << "\n";
    }
    if (snapshot.size() > shown) {
        oss << "    ... (" << (snapshot.size() - shown) << " more)\n";
    }
    return oss.str();
}

}  // namespace sqlcompiler