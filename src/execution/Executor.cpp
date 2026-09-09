#include "execution/Executor.h"

namespace sqlcompiler {

ExecutionContext::ExecutionContext(SystemCatalog* catalog) : catalog_(catalog) {
}

SystemCatalog* ExecutionContext::GetCatalog() const {
    return catalog_;
}

Executor::Executor(ExecutionContext* context) : context_(context) {
}

}  // namespace sqlcompiler