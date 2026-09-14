#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/AST.h"
#include "catalog/SystemCatalog.h"
#include "execution/ExecutionEngine.h"
#include "lexer/Token.h"
#include "plan/Plan.h"
#include "storage/BufferPoolManager.h"
#include "storage/DiskManager.h"
#include "storage/StorageAccess.h"
#include "storage_engine/Value.h"  // 71_proc_out_params: SessionVars uses Value
#include "txn/TransactionManager.h"

namespace sqlcompiler {

// 前向声明：避免 Database.h 引入 LogManager 的全头（与 TransactionManager 形成环）。
class LogManager;
class RecoveryManager;

// 数据库总入口（门面/Facade）：
// 串联 编译器模块（Lexer -> Parser -> SemanticAnalyzer -> Planner -> Optimizer）
// 与 执行系统模块（SystemCatalog -> BufferPoolManager -> DiskManager -> ExecutionEngine），
// 对外提供"输入一条SQL文本，返回执行结果"的统一接口，供CLI/main.cpp调用
class Database {
public:
    // db_file: 数据文件路径（不存在则创建新库）；buffer_pool_size: 缓冲池可容纳的页数
    explicit Database(const std::string& db_file, size_t buffer_pool_size = 64);
    ~Database();

    // 执行一条SQL语句，内部完成 词法->语法->语义->计划->优化->执行 全流程，
    // 任一阶段出错都会被捕获并体现在ExecutionResult::success/message中
    ExecutionResult ExecuteSQL(const std::string& sql);

    // 执行以分号分隔的多条SQL语句（如从.sql文件读入的脚本），
    // 返回每条语句各自的执行结果，便于逐条展示
    std::vector<ExecutionResult> ExecuteScript(const std::string& sql_script);

    // 将缓冲池中所有脏页写回磁盘，通常在CLI退出前调用
    void Shutdown();

    // ---- Phase A：事务管理入口 ----
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
    // / \.plan 元命令展示。每条 ExecuteSQL 调用开始时都会重置这三个缓存，并在
    // 各阶段成功后写入新值；阶段失败时该条目保持为空指针 / 空向量，
    // 调用方应判空再决定如何打印。
    const std::vector<Token>& LastTokens() const { return last_tokens_; }
    const Statement* LastAst() const { return last_ast_.get(); }
    const PlanNode* LastPlan() const { return last_plan_.get(); }

private:
    // storage_ 必须声明在 disk_manager_ / buffer_pool_manager_ 之前：成员按
    // 声明逆序析构，因此 storage_ 最先销毁，其持有的非所有权裸指针在 BPM / DM
    // 析构前已经失效但不被访问，安全。
    std::unique_ptr<StorageAccess> storage_;
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> buffer_pool_manager_;
    std::unique_ptr<SystemCatalog> catalog_;
    std::unique_ptr<ExecutionEngine> execution_engine_;
    std::unique_ptr<TransactionManager> txn_manager_;
    std::unique_ptr<LogManager> log_manager_;        // Phase B：WAL 写出器
    std::unique_ptr<RecoveryManager> recovery_;      // Phase B：启动期 ARIES 恢复

    std::string db_file_path_;       // 规范化后的绝对路径
    std::string wal_file_path_;      // <db_file>.wal
    bool is_new_database_;           // 用于判断启动时是Bootstrap()还是LoadFromDisk()

    // ---- Phase 1.5: 调试输出模式缓存 ----
    // ExecuteSQL 在词法/语法/计划成功后写入；阶段失败时不写入，保留上一次成功的值。
    // 每次 ExecuteSQL 入口处都会先 ResetLastArtifacts() 全部清空。
    // AST / Plan 用 std::shared_ptr 是为了与 Parser / Planner 返回的
    // StatementPtr / PlanNodePtr 共享同一控制块（aliasing constructor），
    // 既保证本字段的指针有效，又不会重复释放底层对象。
    std::vector<Token> last_tokens_;
    std::shared_ptr<Statement> last_ast_;
    std::shared_ptr<PlanNode> last_plan_;
    void ResetLastArtifacts();

    // ---- Phase B：崩溃注入状态 ----
    std::atomic<int> crash_after_n_statements_{0};  // > 0 时每成功执行一条 N--

    // 71_proc_out_params：会话变量存储。Database 是会话变量（@var）的唯一权威
    // 持有者；每次 ExecuteSQL 在构造 ExecutionContext 时把同一指针注入 ctx，
    // UdfExecutor（SET @var）、TriggerExecutor（AFTER @col）和 CALL/SELECT
    // 的 @var 读写都直接打到这张表。ExecuteSQL 结束不需要拷贝 —— 上下文只是
    // 引用了 Database 的成员。线程模型：项目当前为单线程 REPL / 脚本驱动。
    std::unordered_map<std::string, Value> session_vars_;

    // 在 ExecuteSQL 内部、autocommit commit 之后调一次。若还有崩溃名额则扣减，
    // 归零后立即 _Exit(1)。
    void MaybeCrashAfterSuccess();
};

}  // namespace sqlcompiler