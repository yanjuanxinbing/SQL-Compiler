// ============================================================================
// 53_ddl: CREATE/DROP SCHEMA / CREATE/DROP SEQUENCE 执行器
// ----------------------------------------------------------------------------
// 这些执行器都直接改写 catalog 的内存态。schema 与 sequence 在当前实现下仅
// 内存态：不需要持久化（重启数据库会丢失 schema / sequence 注册），与
// tasks/53_ddl 描述一致（"in-memory counter in catalog"）。
//
//   - CREATE SCHEMA name：把 schema 名加入 catalog.schemas_。
//   - DROP SCHEMA name：若 schema 内仍存在表则报错；否则移除。
//   - CREATE SEQUENCE name [START n] [INCREMENT n]：注册 sequence 初始值。
//   - DROP SEQUENCE name：移除 sequence。
//
// 这些动作的副作用与 NoOp 一致——返回空 Tuple 集——由 Executor::Init 在
// 第一次执行时完成副作用，Next 始终返回 false。
// ============================================================================

#include "execution/SchemaSequenceExecutor.h"

#include "catalog/SystemCatalog.h"
#include "common/Error.h"

namespace sqlcompiler {

namespace {

// 在 schema 内查找任一仍然存在的表。schema 限定为 "<schema>." 前缀；
// 没有该前缀的表（默认 schema）不算属于目标 schema。
bool SchemaHasTables(SystemCatalog* catalog, const std::string& schema_name) {
    const std::string prefix = schema_name + ".";
    for (const auto& kv : catalog->GetSymbolTable().GetAllTableNames()) {
        if (kv.size() > prefix.size() &&
            kv.compare(0, prefix.size(), prefix) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ============== CreateSchemaExecutor ==============

CreateSchemaExecutor::CreateSchemaExecutor(ExecutionContext* context, CreateSchemaNode* node)
    : Executor(context), node_(node), executed_(false) {}

void CreateSchemaExecutor::Init() {
    if (executed_ || !node_) return;
    SystemCatalog* catalog = context_->GetCatalog();
    if (catalog == nullptr) return;
    if (!catalog->CreateSchema(node_->schema_name, node_->if_not_exists)) {
        if (!node_->if_not_exists) {
            throw CompilerException(ErrorStage::SEMANTIC,
                "schema already exists: " + node_->schema_name);
        }
    }
    executed_ = true;
}

bool CreateSchemaExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

// ============== DropSchemaExecutor ==============

DropSchemaExecutor::DropSchemaExecutor(ExecutionContext* context, DropSchemaNode* node)
    : Executor(context), node_(node), executed_(false) {}

void DropSchemaExecutor::Init() {
    if (executed_ || !node_) return;
    SystemCatalog* catalog = context_->GetCatalog();
    if (catalog == nullptr) return;
    if (!catalog->HasSchema(node_->schema_name)) {
        if (!node_->if_exists) {
            throw CompilerException(ErrorStage::SEMANTIC,
                "schema does not exist: " + node_->schema_name);
        }
        executed_ = true;
        return;
    }
    if (SchemaHasTables(catalog, node_->schema_name)) {
        throw CompilerException(ErrorStage::SEMANTIC,
            "schema not empty: " + node_->schema_name);
    }
    catalog->DropSchema(node_->schema_name, /*if_exists=*/true);
    executed_ = true;
}

bool DropSchemaExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

// ============== CreateSequenceExecutor ==============

CreateSequenceExecutor::CreateSequenceExecutor(ExecutionContext* context, CreateSequenceNode* node)
    : Executor(context), node_(node), executed_(false) {}

void CreateSequenceExecutor::Init() {
    if (executed_ || !node_) return;
    SystemCatalog* catalog = context_->GetCatalog();
    if (catalog == nullptr) return;
    if (!catalog->CreateSequence(node_->sequence_name,
                                node_->start_value,
                                node_->increment,
                                node_->if_not_exists)) {
        if (!node_->if_not_exists) {
            throw CompilerException(ErrorStage::SEMANTIC,
                "sequence already exists: " + node_->sequence_name);
        }
    }
    executed_ = true;
}

bool CreateSequenceExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

// ============== DropSequenceExecutor ==============

DropSequenceExecutor::DropSequenceExecutor(ExecutionContext* context, DropSequenceNode* node)
    : Executor(context), node_(node), executed_(false) {}

void DropSequenceExecutor::Init() {
    if (executed_ || !node_) return;
    SystemCatalog* catalog = context_->GetCatalog();
    if (catalog == nullptr) return;
    if (!catalog->HasSequence(node_->sequence_name)) {
        if (!node_->if_exists) {
            throw CompilerException(ErrorStage::SEMANTIC,
                "sequence does not exist: " + node_->sequence_name);
        }
        executed_ = true;
        return;
    }
    catalog->DropSequence(node_->sequence_name, /*if_exists=*/true);
    executed_ = true;
}

bool DropSequenceExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

}  // namespace sqlcompiler
