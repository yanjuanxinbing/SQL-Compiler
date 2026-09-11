#pragma once

#include "execution/Executor.h"

namespace sqlcompiler {

// 无副作用的执行器：用于 40_txn_view_udf 中的事务 / 视图 / 触发器 / UDF 创建与
// 删除语句。Init() 完成（什么也不做），Next() 立即返回 false，表示没有更多行。
class NoOpExecutor : public Executor {
public:
    explicit NoOpExecutor(ExecutionContext* context);

    void Init() override;
    bool Next(Tuple* tuple) override;
};

}  // namespace sqlcompiler
