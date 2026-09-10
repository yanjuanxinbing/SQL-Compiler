#include "execution/DropTableExecutor.h"

#include "common/Error.h"

namespace sqlcompiler {

DropTableExecutor::DropTableExecutor(ExecutionContext* context, std::string table_name,
                                     bool if_exists)
    : Executor(context), table_name_(std::move(table_name)),
      if_exists_(if_exists), executed_(false) {
}

void DropTableExecutor::Init() {
    if (executed_) return;
    if (!context_->GetCatalog()->DropTable(table_name_) && !if_exists_) {
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