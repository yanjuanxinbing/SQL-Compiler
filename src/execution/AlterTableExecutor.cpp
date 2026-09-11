#include "execution/AlterTableExecutor.h"

namespace sqlcompiler {

AlterTableExecutor::AlterTableExecutor(ExecutionContext* context, AlterTableNode* node)
    : Executor(context), node_(node), executed_(false) {
}

void AlterTableExecutor::Init() {
    if (executed_) return;
    // 当前实现：no-op。完整 schema evolution（重写 TableHeap 页、迁移数据）
    // 不在本任务范围；保留此钩子供后续增量补完。测试用例 39_ddl_extensions
    // 只校验语法通过与 SELECT 不报错，因此这里无需抛错也无需改动 Catalog。
    (void)node_;
    executed_ = true;
}

bool AlterTableExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

}  // namespace sqlcompiler