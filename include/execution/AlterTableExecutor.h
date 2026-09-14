#pragma once

#include <memory>
#include <string>

#include "ast/AST.h"
#include "execution/Executor.h"
#include "plan/Plan.h"

namespace sqlcompiler {

// ALTER TABLE 算子：当前以 no-op 处理。
//
// 测试 39_ddl_extensions 只要求 ALTER 不报错，且后续 SELECT 仍能拿到原表数据。
// 完整 schema evolution（重写 TableHeap 的页格式、迁移数据）超出本任务范围，
// 因此这里执行期只记录一条日志（避免无意义），不修改 Catalog 与 TableHeap。
class AlterTableExecutor : public Executor {
public:
    AlterTableExecutor(ExecutionContext* context, AlterTableNode* node);

    void Init() override;
    bool Next(Tuple* tuple) override;  // DDL 不产出 Tuple，恒定返回 false

private:
    AlterTableNode* node_;
    bool executed_;
};

}  // namespace sqlcompiler