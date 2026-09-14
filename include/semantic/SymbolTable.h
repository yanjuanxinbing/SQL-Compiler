#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/AST.h"

namespace sqlcompiler {

// 列元信息
struct ColumnInfo {
    std::string name;
    std::string data_type;
    // VARCHAR(N) / CHAR(N) 中的 N；未声明为 -1（不限长）
    int32_t char_length = -1;
    bool is_primary_key = false;
    bool is_not_null = false;
    // 52_data_types: 列级 UNIQUE 约束。区别于 CREATE UNIQUE INDEX 的索引形式：
    // 本字段标记的列在写入路径上同样要求唯一；执行层把它转译为等价的隐式唯一索引。
    bool is_unique = false;
    // 52_data_types: AUTO_INCREMENT / SERIAL / IDENTITY 列标记。
    // 用户 INSERT 时若显式 NULL 或 0（或未列在 INSERT 列名列表里），执行层
    // 自动填入下一个递增 id；非空且非 0 时按用户值落库。
    bool is_auto_increment = false;
    // 列级 CHECK (expr)：执行期在 INSERT/UPDATE 路径上强制校验；
    // NULL 求值结果不视为违反约束（SQL 标准三值逻辑）。
    ExprPtr check_expr;
    // 列级 DEFAULT expr：当 INSERT 未为该列提供值（或显式 NULL）时，
    // 由执行层自动填入。允许的字面类型：int / float / string / NULL；
    // 函数调用、子查询等"非字面表达式"在执行期会抛 "default expression not supported"。
    ExprPtr default_expr;
    // 58_constraints: 列级 CHECK 的可选命名。仅当用户用 `CONSTRAINT name
    // CHECK (...)` 显式命名时填写。错误消息优先显示名称，便于定位。
    std::string constraint_name;
};

// 表元信息
struct TableInfo {
    std::string table_name;
    std::vector<ColumnInfo> columns;
    // 主键组。单列主键为一个只含一列的组；PRIMARY KEY(a, b) 为一个含两列的组。
    // 保留分组信息是必要的：复合主键要求「组合」唯一，而不是每列各自唯一。
    std::vector<std::vector<std::string>> primary_keys;
    // 52_data_types: 表级 UNIQUE 约束。CreateTableExecutor 把它与列级 UNIQUE
    // 合并为「统一唯一索引列表」自动创建。
    std::vector<std::vector<std::string>> unique_constraints;
    // 58_constraints: 表级 CHECK 约束列表（每条带可选 constraint_name）。
    // 写入路径上对每条 CHECK 用当前行求值；NULL 视为通过，仅 FALSE 拒绝。
    // 与列级 check_expr 互不替代——列级 CHECK 仅能引用该列，表级 CHECK 可跨列。
    struct TableCheck {
        std::string constraint_name;
        ExprPtr expr;
    };
    std::vector<TableCheck> table_checks;

    bool HasColumn(const std::string& column_name) const;
    const ColumnInfo* GetColumn(const std::string& column_name) const;

    // 返回用于唯一性校验的主键组。若 primary_keys 为空（例如从旧格式元数据
    // 读出），退化为「所有被标记 is_primary_key 的列构成一个复合组」。
    std::vector<std::vector<std::string>> GetPrimaryKeyGroups() const;
};

// 符号表（数据库元数据目录，Catalog）
// 维护当前已知的所有表结构，供语义分析、优化、计划生成阶段查询
class SymbolTable {
public:
    SymbolTable();

    // ---- 表管理 ----
    bool AddTable(const TableInfo& table_info);
    bool RemoveTable(const std::string& table_name);
    bool HasTable(const std::string& table_name) const;
    const TableInfo* GetTable(const std::string& table_name) const;

    // 根据 CREATE TABLE 语句注册一张新表
    bool AddTableFromCreateStatement(const CreateTableStatement& stmt);

    // 获取当前所有表名（用于调试/展示）
    std::vector<std::string> GetAllTableNames() const;

private:
    std::unordered_map<std::string, TableInfo> tables_;
};

}  // namespace sqlcompiler
