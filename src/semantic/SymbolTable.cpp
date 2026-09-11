#include "semantic/SymbolTable.h"

#include <cctype>

namespace sqlcompiler {

namespace {

std::string ToUpper(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return out;
}

std::string ToLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    return ToUpper(a) == ToUpper(b);
}

}  // namespace

std::vector<std::vector<std::string>> TableInfo::GetPrimaryKeyGroups() const {
    if (!primary_keys.empty()) return primary_keys;
    // 兼容路径：元数据里没有分组信息时，按「所有 PK 列构成一个复合组」处理。
    std::vector<std::string> group;
    for (const auto& c : columns) {
        if (c.is_primary_key) group.push_back(c.name);
    }
    std::vector<std::vector<std::string>> groups;
    if (!group.empty()) groups.push_back(std::move(group));
    return groups;
}

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
    std::string key = ToLower(table_info.table_name);
    if (tables_.find(key) != tables_.end()) return false;
    TableInfo ti = table_info;
    ti.table_name = table_info.table_name;
    tables_[key] = ti;
    return true;
}

bool SymbolTable::RemoveTable(const std::string& table_name) {
    return tables_.erase(ToLower(table_name)) > 0;
}

bool SymbolTable::HasTable(const std::string& table_name) const {
    return tables_.find(ToLower(table_name)) != tables_.end();
}

const TableInfo* SymbolTable::GetTable(const std::string& table_name) const {
    auto it = tables_.find(ToLower(table_name));
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
        ci.char_length = cd.char_length;
        ci.is_primary_key = cd.is_primary_key;
        ci.is_not_null = cd.is_not_null;
        // 把 AST 上的 CHECK / DEFAULT 表达式接到目录侧，让执行层在写入时
        // 校验 / 替换默认。两者均为可空，未声明时此指针为空。
        ci.check_expr = cd.check_expr;
        ci.default_expr = cd.default_expr;
        info.columns.push_back(std::move(ci));
    }
    // 表级 PRIMARY KEY(a, b, ...) 原样保留为一个主键组（复合主键要求组合唯一），
    // 同时投影到每列的 is_primary_key，便于既有执行路径（如 InsertExecutor 的
    // 自增逻辑）保持按单列判定的一致性。
    if (!stmt.primary_keys.empty()) {
        for (const auto& pk : stmt.primary_keys) {
            if (pk.empty()) continue;
            info.primary_keys.push_back(pk);
            for (const auto& col_name : pk) {
                for (auto& ci : info.columns) {
                    if (ci.name == col_name) {
                        ci.is_primary_key = true;
                        break;
                    }
                }
            }
        }
    } else {
        // 列内联的 PRIMARY KEY：合并为一个组。若有多列内联标注，按复合主键处理，
        // 这样既覆盖常见的单列主键，也不会把「多列各自唯一」这种更强的约束强加
        // 给用户。
        std::vector<std::string> inline_pk;
        for (const auto& ci : info.columns) {
            if (ci.is_primary_key) inline_pk.push_back(ci.name);
        }
        if (!inline_pk.empty()) info.primary_keys.push_back(std::move(inline_pk));
    }
    return AddTable(info);
}

std::vector<std::string> SymbolTable::GetAllTableNames() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const auto& kv : tables_) names.push_back(kv.second.table_name);
    return names;
}

}  // namespace sqlcompiler