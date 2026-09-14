#pragma once

#include <string>
#include <vector>

#include "semantic/SymbolTable.h"
#include "storage/Page.h"
#include "storage_engine/Value.h"

namespace sqlcompiler {

// 一个索引的元数据。
//
// key_types 不持久化：它完全由「表定义 + 索引列名」推导得出，落盘只会带来
// 「改列类型后索引元数据陈旧」这类不一致风险。加载时由 TableInfo 现推。
struct IndexInfo {
    std::string index_name;              // 全局唯一
    std::string table_name;
    std::vector<std::string> key_columns;
    std::vector<ValueType> key_types;    // 运行时推导，不落盘
    bool is_unique = false;
    page_id_t root_page_id = INVALID_PAGE_ID;

    // 主键索引由 CREATE TABLE 自动创建，名字带 __pk_ 前缀，用户不可见也不可删
    bool IsPrimaryKeyIndex() const;
};

// 主键索引的命名规则：__pk_<表名>_<主键组序号>
std::string MakePrimaryKeyIndexName(const std::string& table_name,
                                    size_t group_index);

// 由表定义投影出索引列的运行时类型。存在未知列时返回 false。
bool BuildIndexKeyTypes(const TableInfo& table_info,
                        const std::vector<std::string>& key_columns,
                        std::vector<ValueType>* out);

// 索引列的可建性校验。不满足时返回 false 并填入原因。
//
// 两条限制及其理由：
//   - 变长列必须有明确长度上限（VARCHAR(n)，且 n 不超过 kMaxIndexedCharLength）：
//     否则单个键可能撑爆一个页面，导致分裂无法收敛。
//   - 列必须非空（主键列或 NOT NULL）：NULL 在 SQL 里的比较语义是三值逻辑，
//     放进 B+Tree 的全序里会引入大量特例。第一版直接禁止，留待后续放开。
constexpr int kMaxIndexedCharLength = 512;
bool ValidateIndexableColumns(const TableInfo& table_info,
                              const std::vector<std::string>& key_columns,
                              std::string* error);

}  // namespace sqlcompiler
