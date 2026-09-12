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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>

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
                   int bg_flush_ms)
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
                                                         txn_manager_.get());

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
    // \stats —— 输出存储子系统诊断信息（缓冲池/页分配/替换日志）。
    // 需在通用 \crash 处理之前识别。
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
                ExecutionContext session_ctx(catalog_.get(), txn_mgr);
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
            constexpr int kVacuumInterval = 100;
            if (++vacuum_statement_counter_ >= kVacuumInterval &&
                commit_tracker_ != nullptr && catalog_ != nullptr) {
                vacuum_statement_counter_ = 0;
                catalog_->VacuumAll(commit_tracker_->OldestActiveSnapshot());
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
    oss << "dirty writebacks     : " << st.writeback_count << "\n";
    oss << "hit ratio            : " << std::fixed << std::setprecision(2)
        << hit_ratio << "%\n";
    oss << "disk pages / free    : "
        << disk_manager_->GetNumPages() << " / "
        << disk_manager_->GetNumFreePages() << "\n";
    oss << "disk reads / writes  : "
        << disk_manager_->GetIOReadCount() << " / "
        << disk_manager_->GetIOWriteCount() << "\n";
    oss << "background flush     : "
        << (buffer_pool_manager_->IsBackgroundFlushEnabled()
                ? "every " +
                      std::to_string(
                          buffer_pool_manager_->GetBackgroundFlushInterval().count()) +
                      "ms, ticks=" +
                      std::to_string(buffer_pool_manager_->GetBackgroundFlushTicks())
                : "disabled")
        << "\n";
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
    return oss.str();
}

}  // namespace sqlcompiler