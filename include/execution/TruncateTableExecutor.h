#pragma once

#include <string>

#include "execution/Executor.h"

namespace sqlcompiler {

// 清空表算子：移除表中所有记录但保留表结构与元数据。
// 对应逻辑计划中的 TruncateTableNode。
class TruncateTableExecutor : public Executor {
public:
    TruncateTableExecutor(ExecutionContext* context, std::string table_name);

    void Init() override;
    bool Next(Tuple* tuple) override;  // DDL语句不产出Tuple，恒定返回false

private:
    std::string table_name_;
    bool executed_;
};

}  // namespace sqlcompiler