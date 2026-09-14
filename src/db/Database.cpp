#include "db/Database.h"

#include "common/Error.h"
#include "lexer/Lexer.h"
#include "optimizer/Optimizer.h"
#include "parser/Parser.h"
#include "plan/Planner.h"
#include "semantic/SemanticAnalyzer.h"
#include "semantic/SemanticErrorStage.h"
#include "txn/LogManager.h"
#include "txn/RecoveryManager.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

Database::Database(const std::string& db_file, size_t buffer_pool_size)
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
    // Spec 2.3 "接口设计与数据库集成"：在 BPM + DM 之上提供统一的存储访问
    // 门面 StorageAccess，供 catalog / 执行引擎 / 算子使用。BPM 与 DM 的所有权
    // 仍在 Database 内部，StorageAccess 仅持有非所有权裸指针。
    storage_ = std::make_unique<StorageAccess>(buffer_pool_manager_.get(),
                                               disk_manager_.get());

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

    txn_manager_ = std::make_unique<TransactionManager>();
    txn_manager_->SetBufferPoolManager(buffer_pool_manager_.get());
    txn_manager_->SetLogManager(log_manager_.get());
    txn_manager_->SetDiskManager(disk_manager_.get());

    catalog_ = std::make_unique<SystemCatalog>(storage_.get());
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
    // 把 StorageAccess 注入 ExecutionEngine，确保新构造的 ExecutionContext 看到
    // 统一的存储门面（ctx->storage.GetPage(...) 即可访问 BPM）。
    execution_engine_->SetStorageAccess(storage_.get());
    // 71_proc_out_params：把 Database 持有的会话变量表注入到 ExecutionEngine，
    // Execute() 在每次调用时把它挂到 ExecutionContext 上，让 SET @var / CALL 的
    // OUT / INOUT / Trigger AFTER @col 等所有写路径直接落到 Database 同一张表。
    execution_engine_->SetSessionVars(&session_vars_);

    // Phase B：每次 Database 启动都让 LogManager 的 durable_lsn 至少推进到当前
    // 文件末尾。AppendRecord 一条占位记录后 Flush，让后续 FlushPage 的 LSN 检查
    // 不会因为「durable_lsn 永远小于新写入」而无谓刷盘。
    if (log_manager_ != nullptr && created_wal) {
        // 新建空 WAL 时写一条 CHECKPOINT 作为起点标记，便于以后分析；
        // 不写也没有正确性问题（恢复时 ReadAll 会得到空）。
        recovery_->Checkpoint();
    }
}

Database::~Database() {
    Shutdown();
}

void Database::ResetLastArtifacts() {
    last_tokens_.clear();
    last_ast_.reset();
    last_plan_.reset();
}

ExecutionResult Database::ExecuteSQL(const std::string& sql) {
    // Phase C 调试命令：\crash_after_undo_steps N —— 让 Rollback 在撤销 N 步后
    // 立即 _Exit(1)。必须在通用 \crash 之前识别（否则通用路径会先 _Exit）。
    if (IsCrashDebugCommand(sql)) {
        int n = 0;
        if (HandleCrashAfterUndoSteps(sql, &n)) {
            if (txn_manager_ != nullptr) {
                txn_manager_->SetCrashAfterUndoSteps(n);
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

    // Phase 1.5：调试输出模式缓存。每次新语句入口都先清空；后续各阶段成功后
    // 再写入新值；任一阶段失败时，对应缓存条目保持为空。
    ResetLastArtifacts();

    ExecutionResult result;
    try {
        Lexer lexer(sql);
        auto tokens = lexer.Tokenize();
        // 词法成功：缓存 tokens（移动前快照，供 \.tokens 显示）。
        last_tokens_ = tokens;
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
            !is_read_only && txn_manager_ != nullptr &&
            txn_manager_->GetCurrentDepth() == 0;
        Transaction* autocommit_txn = nullptr;
        if (need_autocommit) {
            autocommit_txn = txn_manager_->Begin();
            // Phase B：写 BEGIN 记录，让 redo 阶段看到一致的事务边界。
            txn_manager_->LogBegin(autocommit_txn);
        }
        try {
            // 语法成功：缓存 AST。用 shared_ptr aliasing 构造（共享原 statement
            // 的控制块，单独指向同一对象），避免 unique_ptr 路径下的双重释放。
            last_ast_ = std::shared_ptr<Statement>(statement, statement.get());
            SemanticAnalyzer analyzer(catalog_.get(), catalog_->GetSymbolTable());
            if (!analyzer.Analyze(statement)) {
                // Spec 1.3: 每个语义错误都应携带 [Kind] 标签 + line=col 位置。
                // 格式: `semantic error [<Kind>] line=N[, col=M]: <message>`
                // 多个错误之间用 "; " 分隔。
                auto format_one = [](const SemanticError& e) {
                    std::string out = "semantic error [";
                    out += SemanticErrorKindToString(e.kind);
                    out += "]";
                    if (e.line >= 0) {
                        out += " line=" + std::to_string(e.line);
                        if (e.column >= 0) {
                            out += ", col=" + std::to_string(e.column);
                        }
                        out += ":";
                    }
                    out += " ";
                    out += e.message;
                    return out;
                };
                std::string msg;
                for (const auto& e : analyzer.GetErrors()) {
                    if (!msg.empty()) msg += "; ";
                    msg += format_one(e);
                }
                result.success = false;
                result.message = msg;
                if (need_autocommit && txn_manager_ != nullptr) {
                    txn_manager_->Rollback();
                }
                return result;
            }
            Planner planner(catalog_.get(), catalog_->GetSymbolTable());
            auto plan = planner.CreatePlan(statement);
            Optimizer optimizer(catalog_.get());
            plan = optimizer.Optimize(plan);
            // 计划成功：缓存计划树（共享所有权）。
            last_plan_ = std::shared_ptr<PlanNode>(plan, plan.get());
            auto exec_result = execution_engine_->Execute(plan);
            if (exec_result.success && buffer_pool_manager_) {
                buffer_pool_manager_->FlushAllPages();
            }
            if (need_autocommit && txn_manager_ != nullptr) {
                if (exec_result.success) {
                    txn_manager_->Commit();
                    MaybeCrashAfterSuccess();
                } else {
                    txn_manager_->Rollback();
                }
            }
            return exec_result;
        } catch (...) {
            if (need_autocommit && txn_manager_ != nullptr) {
                txn_manager_->Rollback();
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
    if (buffer_pool_manager_) buffer_pool_manager_->FlushAllPages();
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

}  // namespace sqlcompiler