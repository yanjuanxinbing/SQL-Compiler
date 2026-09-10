#include "execution/CreateTableExecutor.h"

#include "catalog/IndexInfo.h"
#include "common/Error.h"

namespace sqlcompiler {

CreateTableExecutor::CreateTableExecutor(ExecutionContext* context, std::string table_name,
                                          std::vector<ColumnDefinition> columns,
                                          std::vector<std::vector<std::string>> primary_keys,
                                          bool if_not_exists)
    : Executor(context), table_name_(std::move(table_name)),
      columns_(std::move(columns)), primary_keys_(std::move(primary_keys)),
      if_not_exists_(if_not_exists), executed_(false) {
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
        ci.char_length = cd.char_length;
        ci.is_primary_key = cd.is_primary_key;
        ci.is_not_null = cd.is_not_null;
        info.columns.push_back(std::move(ci));
    }
    // 主键分组：表级 PRIMARY KEY(a, b) 必须按「组合唯一」校验，因此分组信息要
    // 原样落库；没有表级子句时，退化为「所有内联 PK 列构成一个组」。
    info.primary_keys = primary_keys_;
    if (info.primary_keys.empty()) {
        std::vector<std::string> inline_pk;
        for (const auto& ci : info.columns) {
            if (ci.is_primary_key) inline_pk.push_back(ci.name);
        }
        if (!inline_pk.empty()) info.primary_keys.push_back(std::move(inline_pk));
    }
    if (!context_->GetCatalog()->CreateTable(info)) {
        // CREATE TABLE IF NOT EXISTS：表已存在则静默成功，不抛错。
        if (!if_not_exists_) {
            throw CompilerException(ErrorStage::SEMANTIC,
                "table already exists: " + table_name_);
        }
        executed_ = true;
        return;
    }

    // 为每个主键组自动建一棵唯一索引树，PRIMARY KEY 约束由它来兑现
    // （否则唯一性校验只能退化成全表扫描）。表刚建好是空的，无需回填。
    //
    // 建索引失败不让 CREATE TABLE 整体失败：约束校验有全表扫描兜底路径，
    // 表本身仍然可用。典型失败原因是主键列声明了不限长的 VARCHAR。
    for (size_t g = 0; g < info.primary_keys.size(); ++g) {
        IndexInfo idx;
        idx.index_name = MakePrimaryKeyIndexName(table_name_, g);
        idx.table_name = table_name_;
        idx.key_columns = info.primary_keys[g];
        idx.is_unique = true;
        std::string ignored_error;
        context_->GetCatalog()->CreateIndex(idx, &ignored_error);
    }
    executed_ = true;
}

bool CreateTableExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

}  // namespace sqlcompiler