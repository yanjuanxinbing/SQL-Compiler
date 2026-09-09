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
                         std::vector<ColumnDefinition> columns);

    void Init() override;
    bool Next(Tuple* tuple) override;  // DDL语句不产出Tuple，恒定返回false

private:
    std::string table_name_;
    std::vector<ColumnDefinition> columns_;
    bool executed_;
};

}  // namespace sqlcompiler
