#include "execution/TruncateTableExecutor.h"

#include "common/Error.h"

namespace sqlcompiler {

TruncateTableExecutor::TruncateTableExecutor(ExecutionContext* context, std::string table_name)
    : Executor(context), table_name_(std::move(table_name)), executed_(false) {
}

void TruncateTableExecutor::Init() {
    if (executed_) return;
    if (!context_->GetCatalog()->TruncateTable(table_name_)) {
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