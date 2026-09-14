#include "execution/DropTableExecutor.h"

#include "catalog/SystemCatalog.h"
#include "common/Error.h"

namespace sqlcompiler {

DropTableExecutor::DropTableExecutor(ExecutionContext* context, std::string table_name,
                                     bool if_exists)
    : Executor(context), table_name_(std::move(table_name)),
      if_exists_(if_exists), executed_(false) {
}

void DropTableExecutor::Init() {
    if (executed_) return;
    SystemCatalog* cat = context_->GetCatalog();
    // Phase B：让 catalog 内部 sys_tables 写入带上当前事务。
    cat->SetActiveTransaction(context_->GetTransaction());
    bool ok = cat->DropTable(table_name_);
    cat->SetActiveTransaction(nullptr);
    if (!ok && !if_exists_) {
        throw CompilerException(ErrorStage::SEMANTIC,
            "table not found: " + table_name_);
    }
    executed_ = true;
}

bool DropTableExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

}  // namespace sqlcompiler