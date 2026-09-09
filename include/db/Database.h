#pragma once

#include <memory>
#include <string>
#include <vector>

#include "catalog/SystemCatalog.h"
#include "execution/ExecutionEngine.h"
#include "storage/BufferPoolManager.h"
#include "storage/DiskManager.h"

namespace sqlcompiler {

// 数据库总入口（门面/Facade）：
// 串联 编译器模块（Lexer -> Parser -> SemanticAnalyzer -> Planner -> Optimizer -> CodeGenerator）
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

private:
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> buffer_pool_manager_;
    std::unique_ptr<SystemCatalog> catalog_;
    std::unique_ptr<ExecutionEngine> execution_engine_;

    std::string db_file_path_;       // 规范化后的绝对路径
    bool is_new_database_;           // 用于判断启动时是Bootstrap()还是LoadFromDisk()
};

}  // namespace sqlcompiler
