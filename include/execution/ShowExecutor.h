#pragma once

#include <memory>
#include <string>
#include <vector>

#include "execution/Executor.h"
#include "plan/Plan.h"
#include "storage_engine/Tuple.h"
#include "storage_engine/Value.h"

namespace sqlcompiler {

// SHOW 算子：把 catalog 元数据以单列 / 多列结果集的形式返回。
//
// 4 种 kind 分别对应不同的输出列：
//   - TABLES：1 列 name，每行一个表名（按字母排序）。
//   - COLUMNS：6 列 name / type / nullable / default / primary_key / check_expr，
//              每行列出该表的一列。
//   - INDEX：4 列 name / table / column / unique，每行一个索引。
//   - CREATE_TABLE：1 列 sql，单行；文本由 catalog.BuildCreateTableSQL 重建。
class ShowExecutor : public Executor {
public:
    ShowExecutor(ExecutionContext* context, ShowNode* node);

    void Init() override;
    bool Next(Tuple* tuple) override;

    // 当前节点的列名，供 ExecutionEngine 填进 result.column_names。
    const std::vector<std::string>& column_names() const { return column_names_; }

private:
    void BuildTables(SystemCatalog* catalog);
    void BuildColumns(SystemCatalog* catalog);
    void BuildIndexes(SystemCatalog* catalog);
    void BuildCreateTable(SystemCatalog* catalog);

    ShowNode* node_;
    std::vector<std::vector<Value>> rows_;
    std::vector<std::string> column_names_;
    size_t cursor_;
};

}  // namespace sqlcompiler
