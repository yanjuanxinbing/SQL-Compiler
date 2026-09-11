#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "catalog/SystemCatalog.h"
#include "storage_engine/Tuple.h"
#include "storage_engine/Value.h"

namespace sqlcompiler {

// 单条物化的 CTE：执行 CTE_DEFINE 节点时把 children[0] 的子计划跑完，
// 把所有结果行收集在这里。后续同一次查询执行中任何 CteBindNode 都直接
// 从这里读取，不必重新执行子计划。
struct CteMaterialization {
    std::string cte_name;
    std::vector<Tuple> rows;
};

// 执行上下文：贯穿整个查询执行过程，向各算子提供目录与存储访问入口
class ExecutionContext {
public:
    explicit ExecutionContext(SystemCatalog* catalog);

    SystemCatalog* GetCatalog() const;

    // CTE 注册表：CTE_DEFINE 节点负责写入，CteBind 节点负责读取。
    // 同一次 Execute 调用内对相同 cte_name 只允许写一次（递归 CTE 多次追加）。
    void RegisterCte(const std::string& name, std::vector<Tuple> rows);
    void AppendCteRows(const std::string& name, const std::vector<Tuple>& rows);
    const std::vector<Tuple>* GetCteRows(const std::string& name) const;
    bool HasCte(const std::string& name) const;
    // 递归 CTE 的 CTE_BIND 在每次迭代时切换到「本轮新增的工作集」上。
    // 用 push/pop 把外层可见的旧结果保留下来，迭代结束后恢复。
    void PushCteOverride(const std::string& name, std::vector<Tuple> rows);
    void PopCteOverride(const std::string& name);

    // 相关子查询（correlated subquery）：子查询内部的 ColumnRefExpr 可能引用外层 SELECT
    // 的当前行。EvaluateSubquery 在运行子计划前把当前外层行的列名→值映射存入 outer_bind_，
    // 子计划里的 ExpressionEvaluator 通过 ExecutionContext 拿到这个 map 后回退解析外层列引用。
    void SetOuterBind(const std::unordered_map<std::string, Value>* bind) {
        outer_bind_ = bind;
    }
    const std::unordered_map<std::string, Value>* GetOuterBind() const {
        return outer_bind_;
    }

    // 当前子查询 / 子计划可见的「内层表名集合」（含真实表名和别名）。
    // EvaluateColumnRef 在限定列未命中 cmap 时据此判断是否要去 outer_bind 找外层列。
    // nullptr 表示没有子查询上下文，按旧行为回退到无限定列名查找。
    void SetInnerTables(const std::unordered_set<std::string>* tables) {
        inner_tables_ = tables;
    }
    const std::unordered_set<std::string>* GetInnerTables() const {
        return inner_tables_;
    }

private:
    SystemCatalog* catalog_;
    std::unordered_map<std::string, CteMaterialization> cte_results_;
    // 临时覆盖层：把 cte_results_[name] 暂存到 vector 首部，新行插入时使用。
    // pop 时还原。这样递归 CTE 既能保留累计结果，又能迭代时让子查询看到 delta。
    std::unordered_map<std::string, std::vector<std::vector<Tuple>>> cte_overrides_;
    // 相关子查询的外层行绑定：nullptr 表示当前不在子查询求值上下文中。
    const std::unordered_map<std::string, Value>* outer_bind_ = nullptr;
    // 当前子查询的内层表名集合（含别名）。
    const std::unordered_set<std::string>* inner_tables_ = nullptr;
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
