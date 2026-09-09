#include "semantic/SymbolTable.h"

#include <algorithm>
#include <cctype>

namespace sqlcompiler {

namespace {

std::string ToUpper(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return out;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    return ToUpper(a) == ToUpper(b);
}

}  // namespace

bool TableInfo::HasColumn(const std::string& column_name) const {
    for (const auto& c : columns) {
        if (EqualsIgnoreCase(c.name, column_name)) return true;
    }
    return false;
}

const ColumnInfo* TableInfo::GetColumn(const std::string& column_name) const {
    for (const auto& c : columns) {
        if (EqualsIgnoreCase(c.name, column_name)) return &c;
    }
    return nullptr;
}

SymbolTable::SymbolTable() {
}

bool SymbolTable::AddTable(const TableInfo& table_info) {
    auto it = tables_.find(table_info.table_name);
    if (it != tables_.end()) return false;
    tables_[table_info.table_name] = table_info;
    return true;
}

bool SymbolTable::RemoveTable(const std::string& table_name) {
    return tables_.erase(table_name) > 0;
}

bool SymbolTable::HasTable(const std::string& table_name) const {
    return tables_.find(table_name) != tables_.end();
}

const TableInfo* SymbolTable::GetTable(const std::string& table_name) const {
    auto it = tables_.find(table_name);
    if (it == tables_.end()) return nullptr;
    return &it->second;
}

bool SymbolTable::AddTableFromCreateStatement(const CreateTableStatement& stmt) {
    TableInfo info;
    info.table_name = stmt.table_name;
    info.columns.reserve(stmt.columns.size());
    for (const auto& cd : stmt.columns) {
        ColumnInfo ci;
        ci.name = cd.column_name;
        ci.data_type = cd.data_type;
        ci.is_primary_key = cd.is_primary_key;
        ci.is_not_null = cd.is_not_null;
        info.columns.push_back(std::move(ci));
    }
    return AddTable(info);
}

std::vector<std::string> SymbolTable::GetAllTableNames() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const auto& kv : tables_) names.push_back(kv.first);
    return names;
}

}  // namespace sqlcompiler