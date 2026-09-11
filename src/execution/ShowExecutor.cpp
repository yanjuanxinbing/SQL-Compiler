// ============ SHOW 算子：内省 catalog ============
//
// 把 SHOW TABLES / SHOW COLUMNS / SHOW INDEX / SHOW CREATE TABLE 的结果
// 在 Init 阶段一次性算好，Next 逐行发射。
//
// 设计要点：
//   - 不与 children 联动：所有数据来自 catalog，不走子执行器。
//   - 输出按"普通查询结果集"形状：列名 + 行 + 行数，由 ExecutionEngine::Execute
//     把 column_names 写到 ExecutionResult。

#include "execution/ShowExecutor.h"

#include "catalog/SystemCatalog.h"
#include "catalog/IndexInfo.h"

#include <sstream>
#include <utility>

namespace sqlcompiler {

namespace {

std::string BoolText(bool b) { return b ? "YES" : "NO"; }

std::string ColumnTypeText(const ColumnInfo& c) {
    std::string s = c.data_type;
    if ((c.data_type == "VARCHAR" || c.data_type == "CHAR") && c.char_length > 0) {
        s += "(" + std::to_string(c.char_length) + ")";
    }
    return s;
}

}  // namespace

ShowExecutor::ShowExecutor(ExecutionContext* context, ShowNode* node)
    : Executor(context), node_(node), cursor_(0) {
}

void ShowExecutor::Init() {
    SystemCatalog* catalog = context_ ? context_->GetCatalog() : nullptr;
    if (!catalog) return;
    switch (node_->kind) {
        case ShowNode::Kind::TABLES:       BuildTables(catalog); break;
        case ShowNode::Kind::COLUMNS:      BuildColumns(catalog); break;
        case ShowNode::Kind::INDEX:        BuildIndexes(catalog); break;
        case ShowNode::Kind::CREATE_TABLE: BuildCreateTable(catalog); break;
    }
}

void ShowExecutor::BuildTables(SystemCatalog* catalog) {
    column_names_ = {"name"};
    auto names = catalog->ListAllTables();
    rows_.reserve(names.size());
    for (auto& n : names) {
        rows_.push_back({Value::MakeVarchar(std::move(n))});
    }
}

void ShowExecutor::BuildColumns(SystemCatalog* catalog) {
    column_names_ = {"name", "type", "nullable", "default", "primary_key", "check_expr"};
    auto cols = catalog->GetColumnInfos(node_->target_table);
    rows_.reserve(cols.size());
    for (const auto& c : cols) {
        rows_.push_back({
            Value::MakeVarchar(c.name),
            Value::MakeVarchar(ColumnTypeText(c)),
            Value::MakeVarchar(BoolText(!c.is_not_null && !c.is_primary_key)),
            Value::MakeVarchar(c.default_expr ? c.default_expr->ToString() : ""),
            Value::MakeVarchar(BoolText(c.is_primary_key)),
            Value::MakeVarchar(c.check_expr ? c.check_expr->ToString() : ""),
        });
    }
}

void ShowExecutor::BuildIndexes(SystemCatalog* catalog) {
    column_names_ = {"name", "table", "column", "unique"};
    auto indexes = catalog->GetIndexesForTable(node_->target_table);
    rows_.reserve(indexes.size());
    for (const auto* info : indexes) {
        if (!info) continue;
        // 单列索引时 column 填列名；多列时把多个列名用逗号拼接。
        std::string cols;
        for (size_t i = 0; i < info->key_columns.size(); ++i) {
            if (i) cols += ", ";
            cols += info->key_columns[i];
        }
        rows_.push_back({
            Value::MakeVarchar(info->index_name),
            Value::MakeVarchar(info->table_name),
            Value::MakeVarchar(cols),
            Value::MakeVarchar(BoolText(info->is_unique)),
        });
    }
}

void ShowExecutor::BuildCreateTable(SystemCatalog* catalog) {
    column_names_ = {"sql"};
    std::string sql = catalog->BuildCreateTableSQL(node_->target_table);
    if (sql.empty()) {
        rows_.push_back({Value::MakeVarchar("")});
        return;
    }
    rows_.push_back({Value::MakeVarchar(std::move(sql))});
}

bool ShowExecutor::Next(Tuple* tuple) {
    if (cursor_ >= rows_.size()) return false;
    if (tuple) {
        *tuple = Tuple(rows_[cursor_]);
    }
    ++cursor_;
    return true;
}

}  // namespace sqlcompiler
