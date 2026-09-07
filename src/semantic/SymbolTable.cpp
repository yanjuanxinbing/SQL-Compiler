#include "semantic/SymbolTable.h"

namespace sqlcompiler {

bool TableInfo::HasColumn(const std::string& column_name) const {
    // TODO: 遍历columns，判断是否存在同名列（建议忽略大小写）
    return false;
}

const ColumnInfo* TableInfo::GetColumn(const std::string& column_name) const {
    // TODO: 遍历columns，返回匹配列的指针，找不到返回nullptr
    return nullptr;
}

SymbolTable::SymbolTable() {
    // TODO: 如有需要可预置一些内置表/系统表
}

bool SymbolTable::AddTable(const TableInfo& table_info) {
    // TODO: 若表名已存在则返回false，否则插入tables_并返回true
    return false;
}

bool SymbolTable::RemoveTable(const std::string& table_name) {
    // TODO: 若表存在则从tables_中移除并返回true，否则返回false
    return false;
}

bool SymbolTable::HasTable(const std::string& table_name) const {
    // TODO: 判断tables_中是否存在该表名
    return false;
}

const TableInfo* SymbolTable::GetTable(const std::string& table_name) const {
    // TODO: 返回对应TableInfo的指针，不存在返回nullptr
    return nullptr;
}

bool SymbolTable::AddTableFromCreateStatement(const CreateTableStatement& stmt) {
    // TODO: 根据CreateTableStatement构造TableInfo，并调用AddTable()注册
    return false;
}

std::vector<std::string> SymbolTable::GetAllTableNames() const {
    // TODO: 遍历tables_，收集所有表名并返回
    return {};
}

}  // namespace sqlcompiler
