#include "execution/TruncateTableExecutor.h"

#include "catalog/SystemCatalog.h"
#include "common/Error.h"

namespace sqlcompiler {

TruncateTableExecutor::TruncateTableExecutor(ExecutionContext* context, std::string table_name)
    : Executor(context), table_name_(std::move(table_name)), executed_(false) {
}

void TruncateTableExecutor::Init() {
    if (executed_) return;
    SystemCatalog* cat = context_->GetCatalog();
    // Phase B：让 catalog / 内部 sys_tables 写入带上当前事务。
    cat->SetActiveTransaction(context_->GetTransaction());
    bool ok = cat->TruncateTable(table_name_);
    if (ok) {
        // 表数据被清空，索引里的 RID 全部失效，必须一并重建为空树，
        // 否则后续查询会沿着悬空 RID 读出垃圾。
        cat->ResetIndexesOfTable(table_name_);
    }
    cat->SetActiveTransaction(nullptr);
    if (!ok) {
        throw CompilerException(ErrorStage::SEMANTIC,
            "table not found: " + table_name_);
    }
    executed_ = true;
}

bool TruncateTableExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

}  // namespace sqlcompiler