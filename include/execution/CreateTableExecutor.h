#pragma once

#include <string>
#include <vector>

#include "ast/AST.h"
#include "execution/Executor.h"

namespace sqlcompiler {

// 建表算子：在SystemCatalog中注册新表的元数据，并为其分配初始数据页，
// 对应逻辑计划中的 CreateTableNode
class CreateTableExecutor : public Executor {
public:
    CreateTableExecutor(ExecutionContext* context, std::string table_name,
                         std::vector<ColumnDefinition> columns,
                         std::vector<std::vector<std::string>> primary_keys = {},
                         std::vector<std::vector<std::string>> unique_constraints = {},
                         std::vector<ForeignKeyDef> foreign_keys = {},
                         std::vector<TableCheckDef> table_checks = {},
                         bool if_not_exists = false);

    void Init() override;
    bool Next(Tuple* tuple) override;  // DDL语句不产出Tuple，恒定返回false

private:
    std::string table_name_;
    std::vector<ColumnDefinition> columns_;
    std::vector<std::vector<std::string>> primary_keys_;
    // 52_data_types: 表级 UNIQUE(col, ...) 约束。列级 UNIQUE 已附在
    // ColumnDefinition.is_unique 上。
    std::vector<std::vector<std::string>> unique_constraints_;
    // 53_ddl: 表级 FOREIGN KEY 约束。列级 REFERENCES 已在 ColumnDefinition 内。
    std::vector<ForeignKeyDef> foreign_keys_;
    // 58_constraints: 表级 CHECK(expr) / CONSTRAINT name CHECK(expr)。
    // 表级 CHECK 写入路径上对每条约束逐行求值（NULL 操作数按 SQL 标准
    // 三值逻辑视为通过，仅 FALSE 拒绝）。
    std::vector<TableCheckDef> table_checks_;
    bool if_not_exists_;
    bool executed_;
};

}  // namespace sqlcompiler
