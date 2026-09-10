#pragma once

#include <string>

#include "execution/Executor.h"

namespace sqlcompiler {

// 删索引算子：移除索引元数据并回收 B+Tree 占用的全部页面。
class DropIndexExecutor : public Executor {
public:
    DropIndexExecutor(ExecutionContext* context, std::string index_name,
                      bool if_exists);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    std::string index_name_;
    bool if_exists_;
    bool executed_;
};

}  // namespace sqlcompiler
