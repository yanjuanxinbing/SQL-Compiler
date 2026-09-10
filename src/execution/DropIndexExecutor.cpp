#include "execution/DropIndexExecutor.h"

#include "common/Error.h"

namespace sqlcompiler {

DropIndexExecutor::DropIndexExecutor(ExecutionContext* context,
                                     std::string index_name, bool if_exists)
    : Executor(context), index_name_(std::move(index_name)),
      if_exists_(if_exists), executed_(false) {
}

void DropIndexExecutor::Init() {
    if (executed_) return;
    SystemCatalog* catalog = context_->GetCatalog();
    const IndexInfo* info = catalog->GetIndex(index_name_);
    if (info != nullptr && info->IsPrimaryKeyIndex()) {
        // 主键索引是 PRIMARY KEY 约束的实现载体，删掉它等于悄悄取消约束
        throw CompilerException(
            ErrorStage::SEMANTIC,
            "cannot drop index '" + index_name_ +
                "': it backs a PRIMARY KEY constraint");
    }
    if (!catalog->DropIndex(index_name_) && !if_exists_) {
        throw CompilerException(ErrorStage::SEMANTIC,
                                "index not found: " + index_name_);
    }
    executed_ = true;
}

bool DropIndexExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

}  // namespace sqlcompiler
