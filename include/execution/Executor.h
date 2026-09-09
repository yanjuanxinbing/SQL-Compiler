#pragma once

#include <memory>

#include "catalog/SystemCatalog.h"
#include "storage_engine/Tuple.h"

namespace sqlcompiler {

// 执行上下文：贯穿整个查询执行过程，向各算子提供目录与存储访问入口
class ExecutionContext {
public:
    explicit ExecutionContext(SystemCatalog* catalog);

    SystemCatalog* GetCatalog() const;

private:
    SystemCatalog* catalog_;
};

// 执行算子基类，采用火山模型（Volcano / Iterator Model）：
//   Init() 完成准备工作（如打开表迭代器）；
//   Next() 每次产出一条Tuple，返回false表示没有更多数据（DDL/DML语句
//   可以不产出Tuple，Next()恒定返回false，副作用在Init()中完成）
class Executor {
public:
    explicit Executor(ExecutionContext* context);
    virtual ~Executor() = default;

    virtual void Init() = 0;
    virtual bool Next(Tuple* tuple) = 0;

protected:
    ExecutionContext* context_;
};
using ExecutorPtr = std::unique_ptr<Executor>;

}  // namespace sqlcompiler
