#include "db/Database.h"

#include "common/Error.h"
#include "lexer/Lexer.h"
#include "optimizer/Optimizer.h"
#include "parser/Parser.h"
#include "plan/Planner.h"
#include "semantic/SemanticAnalyzer.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace sqlcompiler {

namespace fs = std::filesystem;

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
    //    这样做的好处：
    //      - 避免 DiskManager 构造时 open(in|out) 失败后的恢复路径不可控
    //      - 在创建失败时（如权限不足）能给出清晰的错误信息
    //    注意：必须同时把「存在但为 0 字节 / 不足一页」的文件视为新库。
    //    否则 LoadFromDisk() 会假定 page 0 是 sys_tables 首页，但该页从未被
    //    分配；随后的 CREATE TABLE 会把 page 0 分配给用户表，导致用户表堆与
    //    系统目录堆共用同一页，SELECT 时按错误 schema 反序列化目录元组而崩溃。
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
    catalog_ = std::make_unique<SystemCatalog>(buffer_pool_manager_.get());
    if (is_new_database_) {
        catalog_->Bootstrap();
    } else {
        catalog_->LoadFromDisk();
    }
    execution_engine_ = std::make_unique<ExecutionEngine>(catalog_.get());
}

Database::~Database() {
    Shutdown();
}

ExecutionResult Database::ExecuteSQL(const std::string& sql) {
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
        SemanticAnalyzer analyzer(catalog_.get(), catalog_->GetSymbolTable());
        if (!analyzer.Analyze(statement)) {
            std::string msg;
            for (const auto& e : analyzer.GetErrors()) {
                if (!msg.empty()) msg += "; ";
                msg += e.message;
            }
            result.success = false;
            result.message = "semantic error: " + msg;
            return result;
        }
        Planner planner(catalog_.get(), catalog_->GetSymbolTable());
        auto plan = planner.CreatePlan(statement);
        Optimizer optimizer(catalog_.get());
        plan = optimizer.Optimize(plan);
        auto exec_result = execution_engine_->Execute(plan);
        // 无 WAL/检查点机制时，脏页只在 Shutdown 刷盘；一旦用户 Ctrl+C 或进程
        // 异常退出，已回报 OK 的 DDL/DML 会丢失，更糟的是磁盘上会留下「已分配
        // 但内容全零」的页，下次打开时被当成合法页解析。这里在每条语句成功后
        // 落盘，保证 REPL 看到的 OK 与磁盘状态一致。
        if (exec_result.success && buffer_pool_manager_) {
            buffer_pool_manager_->FlushAllPages();
        }
        return exec_result;
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
            // Trim
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
    // Trailing statement without semicolon
    size_t a = buf.find_first_not_of(" \t\r\n");
    size_t b = buf.find_last_not_of(" \t\r\n");
    if (a != std::string::npos) {
        std::string stmt = buf.substr(a, b - a + 1);
        if (!stmt.empty()) results.push_back(ExecuteSQL(stmt));
    }
    return results;
}

void Database::Shutdown() {
    if (buffer_pool_manager_) buffer_pool_manager_->FlushAllPages();
}

}  // namespace sqlcompiler