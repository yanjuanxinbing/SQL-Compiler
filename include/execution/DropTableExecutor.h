#pragma once

#include <string>

#include "execution/Executor.h"

namespace sqlcompiler {

// 删表算子：从SystemCatalog中移除表的元数据，并回收其占用的数据页，
// 对应逻辑计划中的 DropTableNode
class DropTableExecutor : public Executor {
public:
    DropTableExecutor(ExecutionContext* context, std::string table_name,
                      bool if_exists = false);

    void Init() override;
    bool Next(Tuple* tuple) override;  // DDL语句不产出Tuple，恒定返回false

private:
    std::string table_name_;
    bool if_exists_;
    bool executed_;
};

}  // namespace sqlcompiler
