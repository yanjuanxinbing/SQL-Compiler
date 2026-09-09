#include "execution/CreateTableExecutor.h"

#include "common/Error.h"

namespace sqlcompiler {

CreateTableExecutor::CreateTableExecutor(ExecutionContext* context, std::string table_name,
                                          std::vector<ColumnDefinition> columns)
    : Executor(context), table_name_(std::move(table_name)),
      columns_(std::move(columns)), executed_(false) {
}

void CreateTableExecutor::Init() {
    if (executed_) return;
    TableInfo info;
    info.table_name = table_name_;
    info.columns.reserve(columns_.size());
    for (const auto& cd : columns_) {
        ColumnInfo ci;
        ci.name = cd.column_name;
        ci.data_type = cd.data_type;
        ci.is_primary_key = cd.is_primary_key;
        ci.is_not_null = cd.is_not_null;
        info.columns.push_back(std::move(ci));
    }
    if (!context_->GetCatalog()->CreateTable(info)) {
        throw CompilerException(ErrorStage::SEMANTIC,
            "table already exists: " + table_name_);
    }
    executed_ = true;
}

bool CreateTableExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

}  // namespace sqlcompiler