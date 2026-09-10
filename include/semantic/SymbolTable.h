#pragma once

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
};

// 表元信息
struct TableInfo {
    std::string table_name;
    std::vector<ColumnInfo> columns;
    // 主键组。单列主键为一个只含一列的组；PRIMARY KEY(a, b) 为一个含两列的组。
    // 保留分组信息是必要的：复合主键要求「组合」唯一，而不是每列各自唯一。
    std::vector<std::vector<std::string>> primary_keys;

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
