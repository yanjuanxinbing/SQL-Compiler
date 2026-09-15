#include "semantic/SymbolTable.h"

#include <cctype>
#include <string_view>

namespace sqlcompiler {

namespace {

// Lowercase an ASCII string in place. SQL identifiers are ASCII; non-ASCII
// bytes (e.g. Chinese) pass through unchanged.
inline void LowercaseInPlace(std::string& s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
}

std::string ToLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

}  // namespace

void TableInfo::BuildColumnIndex() {
    column_index_.clear();
    column_index_.reserve(columns.size());
    for (size_t i = 0; i < columns.size(); ++i) {
        // 直接把列名原样放入 hash 表；hash/equal 都是大小写不敏感版本。
        column_index_.emplace(columns[i].name, i);
    }
}

void TableInfo::EnsureColumnIndexBuilt() const {
    if (column_index_.empty() && !columns.empty()) {
        const_cast<TableInfo*>(this)->BuildColumnIndex();
    }
}

bool TableInfo::HasColumn(const std::string& column_name) const {
    return HasColumnFast(column_name);
}

bool TableInfo::HasColumnFast(std::string_view column_name) const {
    // C++17 std::unordered_map 没有异构查找，统一构造 std::string 走 find。
    // 字符串本身在调用栈上是热路径上的 AST std::string（zero-cost）。
    EnsureColumnIndexBuilt();
    return column_index_.find(std::string(column_name)) != column_index_.end();
}

const ColumnInfo* TableInfo::GetColumn(const std::string& column_name) const {
    return GetColumnFast(column_name);
}

const ColumnInfo* TableInfo::GetColumnFast(std::string_view column_name) const {
    EnsureColumnIndexBuilt();
    auto it = column_index_.find(std::string(column_name));
    if (it == column_index_.end()) return nullptr;
    return &columns[it->second];
}

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

SymbolTable::SymbolTable() {
}

bool SymbolTable::AddTable(const TableInfo& table_info) {
    TableInfo ti = table_info;
    return AddTable(std::move(ti));
}

bool SymbolTable::AddTable(TableInfo&& table_info) {
    // 大小写不敏感的 hash/equal 让"小写 key"不再是必需；直接用原名。
    // 仍然构造一个 lowercased 副本作为 map key 是浪费 —— hash 本身已是
    // 大小写不敏感的，equal 也容忍差异。
    // 然而为了保持旧行为（避免 map key 与用户输入之间出现大小写分裂），
    // 这里使用原始名作为 key（透明 hash 在 find 时仍然大小写不敏感）。
    std::string key = table_info.table_name;
    LowercaseInPlace(key);
    if (tables_.find(key) != tables_.end()) return false;
    table_info.BuildColumnIndex();
    tables_.emplace(std::move(key), std::move(table_info));
    return true;
}

bool SymbolTable::RemoveTable(const std::string& table_name) {
    std::string key = ToLower(table_name);
    return tables_.erase(key) > 0;
}

bool SymbolTable::HasTable(const std::string& table_name) const {
    std::string key = ToLower(table_name);
    return tables_.find(key) != tables_.end();
}

const TableInfo* SymbolTable::GetTable(const std::string& table_name) const {
    std::string key = ToLower(table_name);
    auto it = tables_.find(key);
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
        // 58_constraints: 列级 CONSTRAINT name CHECK 命名同步。空字符串代表
        // 匿名 CHECK，错误消息沿用 <table>.<col> 形式。
        ci.constraint_name = cd.constraint_name;
        info.columns.push_back(std::move(ci));
    }
    // 58_constraints: 表级 CHECK 约束。语法层已经按出现顺序填入 stmt.table_checks，
    // 这里原样落到 TableInfo 上供执行期逐行求值；命名约束随 expr 一起保留。
    for (const auto& tc : stmt.table_checks) {
        TableInfo::TableCheck catalog_tc;
        catalog_tc.constraint_name = tc.constraint_name;
        catalog_tc.expr = tc.expr;
        info.table_checks.push_back(std::move(catalog_tc));
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
    return AddTable(std::move(info));
}

std::vector<std::string> SymbolTable::GetAllTableNames() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const auto& kv : tables_) names.push_back(kv.second.table_name);
    return names;
}

}  // namespace sqlcompiler