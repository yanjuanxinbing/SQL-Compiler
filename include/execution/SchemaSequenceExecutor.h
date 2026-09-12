#pragma once

#include "execution/Executor.h"
#include "plan/Plan.h"

namespace sqlcompiler {

// ============================================================================
// 53_ddl: SCHEMA / SEQUENCE 执行器声明
// ----------------------------------------------------------------------------
// 这些执行器仅修改 catalog 的内存态（schema / sequence）。Init 时执行副作用
// （注册/释放），Next 始终返回 false —— 与 NoOpExecutor 等价。
// ============================================================================

class CreateSchemaExecutor : public Executor {
public:
    CreateSchemaExecutor(ExecutionContext* context, CreateSchemaNode* node);
    void Init() override;
    bool Next(Tuple* tuple) override;
private:
    CreateSchemaNode* node_;
    bool executed_;
};

class DropSchemaExecutor : public Executor {
public:
    DropSchemaExecutor(ExecutionContext* context, DropSchemaNode* node);
    void Init() override;
    bool Next(Tuple* tuple) override;
private:
    DropSchemaNode* node_;
    bool executed_;
};

class CreateSequenceExecutor : public Executor {
public:
    CreateSequenceExecutor(ExecutionContext* context, CreateSequenceNode* node);
    void Init() override;
    bool Next(Tuple* tuple) override;
private:
    CreateSequenceNode* node_;
    bool executed_;
};

class DropSequenceExecutor : public Executor {
public:
    DropSequenceExecutor(ExecutionContext* context, DropSequenceNode* node);
    void Init() override;
    bool Next(Tuple* tuple) override;
private:
    DropSequenceNode* node_;
    bool executed_;
};

}  // namespace sqlcompiler
