#include "execution/NoOpExecutor.h"

namespace sqlcompiler {

NoOpExecutor::NoOpExecutor(ExecutionContext* context) : Executor(context) {
}

void NoOpExecutor::Init() {
    // 40_txn_view_udf：no-op。
}

bool NoOpExecutor::Next(Tuple* /*tuple*/) {
    return false;
}

}  // namespace sqlcompiler
