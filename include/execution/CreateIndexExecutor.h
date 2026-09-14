#pragma once

#include <string>
#include <vector>

#include "execution/Executor.h"

namespace sqlcompiler {

// 建索引算子：创建 B+Tree、持久化索引元数据，并把表中已有数据回填进索引。
// 对应逻辑计划中的 CreateIndexNode。
class CreateIndexExecutor : public Executor {
public:
    CreateIndexExecutor(ExecutionContext* context, std::string index_name,
                        std::string table_name,
                        std::vector<std::string> key_columns, bool is_unique);

    void Init() override;
    bool Next(Tuple* tuple) override;  // DDL 不产出 Tuple

private:
    std::string index_name_;
    std::string table_name_;
    std::vector<std::string> key_columns_;
    bool is_unique_;
    bool executed_;
};

}  // namespace sqlcompiler
