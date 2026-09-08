#include "semantic/SymbolTable.h"

#include <algorithm>
#include <cctype>

namespace sqlcompiler {

namespace {

// 把字符串统一转成大写，便于做大小写不敏感的标识符匹配
std::string ToUpper(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    return ToUpper(a) == ToUpper(b);
}

}  // namespace

// ============ TableInfo ============

bool TableInfo::HasColumn(const std::string& column_name) const {
    for (const auto& col : columns) {
        if (EqualsIgnoreCase(col.name, column_name)) {
            return true;
        }
    }
    return false;
}

const ColumnInfo* TableInfo::GetColumn(const std::string& column_name) const {
    for (const auto& col : columns) {
        if (EqualsIgnoreCase(col.name, column_name)) {
            return &col;
        }
    }
    return nullptr;
}

// ============ SymbolTable ============

SymbolTable::SymbolTable() = default;

bool SymbolTable::AddTable(const TableInfo& table_info) {
    if (table_info.table_name.empty()) {
        return false;
    }
    if (HasTable(table_info.table_name)) {
        return false;
    }
    tables_[table_info.table_name] = table_info;
    return true;
}

bool SymbolTable::RemoveTable(const std::string& table_name) {
    return tables_.erase(table_name) > 0;
}

bool SymbolTable::HasTable(const std::string& table_name) const {
    if (table_name.empty()) {
        return false;
    }
    // 找到等价表名（大小写不敏感）
    for (const auto& kv : tables_) {
        if (EqualsIgnoreCase(kv.first, table_name)) {
            return true;
        }
    }
    return false;
}

const TableInfo* SymbolTable::GetTable(const std::string& table_name) const {
    if (table_name.empty()) {
        return nullptr;
    }
    // 先尝试精确匹配（O(1)），未命中再退回到大小写不敏感扫描
    auto it = tables_.find(table_name);
    if (it != tables_.end()) {
        return &it->second;
    }
    for (const auto& kv : tables_) {
        if (EqualsIgnoreCase(kv.first, table_name)) {
            return &kv.second;
        }
    }
    return nullptr;
}

bool SymbolTable::AddTableFromCreateStatement(const CreateTableStatement& stmt) {
    TableInfo info;
    info.table_name = stmt.table_name;

    info.columns.reserve(stmt.columns.size());
    for (const auto& cd : stmt.columns) {
        ColumnInfo col;
        col.name = cd.column_name;
        col.data_type = cd.data_type;
        col.is_primary_key = cd.is_primary_key;
        col.is_not_null = cd.is_not_null;
        info.columns.push_back(std::move(col));
    }
    return AddTable(info);
}

std::vector<std::string> SymbolTable::GetAllTableNames() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const auto& kv : tables_) {
        names.push_back(kv.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace sqlcompiler
