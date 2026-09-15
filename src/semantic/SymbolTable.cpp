#include "semantic/SymbolTable.h"

#include <cctype>
#include <unordered_set>

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
    // [perf] catalog-indexes: O(1) hash 查，先小写化键。原先 O(C·L·allocs)
    // 的 EqualsIgnoreCase 线性扫描改为一次 ToLower + map find。
    return column_index_.find(ToLower(column_name)) != column_index_.end();
}

const ColumnInfo* TableInfo::GetColumn(const std::string& column_name) const {
    auto it = column_index_.find(ToLower(column_name));
    if (it == column_index_.end()) return nullptr;
    return &columns[it->second];
}

void TableInfo::RebuildColumnIndex() {
    column_index_.clear();
    column_index_.reserve(columns.size());
    // emplace 保证「首次出现优先」——与原 HasColumn 线性扫描返回首个匹配的语义一致。
    for (size_t i = 0; i < columns.size(); ++i) {
        column_index_.emplace(ToLower(columns[i].name), i);
    }
}

SymbolTable::SymbolTable() {
}

bool SymbolTable::AddTable(const TableInfo& table_info) {
    std::string key = ToLower(table_info.table_name);
    if (tables_.find(key) != tables_.end()) return false;
    TableInfo ti = table_info;
    ti.table_name = table_info.table_name;
    // [perf] catalog-indexes: 进入 tables_ 前一次性建立列名旁路。
    // 后续 HasColumn/GetColumn 是 O(1) hash 查，不再每次分配 ToUpper 字符串。
    ti.RebuildColumnIndex();
    tables_[key] = std::move(ti);
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
        // [perf] catalog-indexes: 先把所有 PK 列名收集进 set，再单遍扫
        // columns 把命中列标记 is_primary_key。原先是 k·C 嵌套循环，
        // 改成 O(k + C) —— k 是所有 PK 列展开后的总数（含复合组重复列）。
        std::unordered_set<std::string> pk_cols;
        pk_cols.reserve(stmt.columns.size());
        for (const auto& pk : stmt.primary_keys) {
            if (pk.empty()) continue;
            info.primary_keys.push_back(pk);
            for (const auto& col_name : pk) pk_cols.insert(col_name);
        }
        for (auto& ci : info.columns) {
            if (pk_cols.count(ci.name)) ci.is_primary_key = true;
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