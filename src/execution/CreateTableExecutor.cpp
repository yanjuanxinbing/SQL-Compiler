#include "execution/CreateTableExecutor.h"

#include "catalog/IndexInfo.h"
#include "catalog/SystemCatalog.h"
#include "common/Error.h"

namespace sqlcompiler {

CreateTableExecutor::CreateTableExecutor(ExecutionContext* context, std::string table_name,
                                          std::vector<ColumnDefinition> columns,
                                          std::vector<std::vector<std::string>> primary_keys,
                                          std::vector<std::vector<std::string>> unique_constraints,
                                          std::vector<ForeignKeyDef> foreign_keys,
                                          std::vector<TableCheckDef> table_checks,
                                          bool if_not_exists)
    : Executor(context), table_name_(std::move(table_name)),
      columns_(std::move(columns)), primary_keys_(std::move(primary_keys)),
      unique_constraints_(std::move(unique_constraints)),
      foreign_keys_(std::move(foreign_keys)),
      table_checks_(std::move(table_checks)),
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
        // 52_data_types: 列级 UNIQUE 与 AUTO_INCREMENT 标记搬到 ColumnInfo。
        ci.is_unique = cd.is_unique;
        ci.is_auto_increment = cd.is_auto_increment;
        // 把 AST 上的 CHECK / DEFAULT 表达式搬到 ColumnInfo；执行期在
        // INSERT / UPDATE 路径上兑现这两类约束。两者可空：未声明时为 nullptr。
        ci.check_expr = cd.check_expr;
        ci.default_expr = cd.default_expr;
        // 58_constraints: 列级 CONSTRAINT name CHECK 命名同步。
        ci.constraint_name = cd.constraint_name;
        info.columns.push_back(std::move(ci));
    }
    // 58_constraints: 表级 CHECK(expr) / CONSTRAINT name CHECK(expr)。
    for (const auto& tc : table_checks_) {
        TableInfo::TableCheck catalog_tc;
        catalog_tc.constraint_name = tc.constraint_name;
        catalog_tc.expr = tc.expr;
        info.table_checks.push_back(std::move(catalog_tc));
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
    // 52_data_types: 表级 UNIQUE(col, ...) 约束分组（来自 CREATE TABLE 语句）。
    info.unique_constraints = unique_constraints_;
    SystemCatalog* cat = context_->GetCatalog();
    // 53_ddl: 校验 schema 限定。
    {
        auto qn = SystemCatalog::SplitQualifiedName(table_name_);
        if (!qn.first.empty() && !cat->HasSchema(qn.first)) {
            throw CompilerException(ErrorStage::SEMANTIC,
                "schema does not exist: " + qn.first);
        }
    }
    // Phase B：让 catalog 的内部写路径（sys_tables / sys_indexes）也带上当前
    // 事务，让 WAL 记录里 txn_id 与 DML 一致。
    cat->SetActiveTransaction(context_->GetTransaction());
    if (!cat->CreateTable(info)) {
        // CREATE TABLE IF NOT EXISTS：表已存在则静默成功，不抛错。
        cat->SetActiveTransaction(nullptr);
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
        cat->CreateIndex(idx, &ignored_error);
    }
    // 52_data_types: 为列级 UNIQUE / 表级 UNIQUE 自动建隐式唯一索引。
    // 索引命名约定：__uq_<table>_<n>，与主键索引 __pk_ 前缀区分。
    // 同样允许失败：唯一性校验有全表扫描兜底路径。
    auto make_uniq_index_name = [&](size_t n) {
        return std::string("__uq_") + table_name_ + "_" + std::to_string(n);
    };
    // 合并列级 / 表级 UNIQUE 列表，避免同一列被建两次索引。
    std::vector<std::vector<std::string>> uniq_groups = info.unique_constraints;
    for (const auto& c : info.columns) {
        if (!c.is_unique) continue;
        bool dup = false;
        for (const auto& g : uniq_groups) {
            if (g.size() == 1 && g[0] == c.name) { dup = true; break; }
        }
        if (!dup) uniq_groups.push_back({c.name});
    }
    for (size_t g = 0; g < uniq_groups.size(); ++g) {
        IndexInfo idx;
        idx.index_name = make_uniq_index_name(g);
        idx.table_name = table_name_;
        idx.key_columns = uniq_groups[g];
        idx.is_unique = true;
        std::string ignored_error;
        cat->CreateIndex(idx, &ignored_error);
    }

    // 53_ddl: 注册 FOREIGN KEY 约束到 catalog。
    //   - 合并列级 (column.inline_foreign_keys) 与表级 (foreign_keys_) 列表。
    //   - 校验 parent_table 存在、子列与父列长度一致、子列在当前表上存在；
    //     校验失败回滚 CREATE TABLE 失败但表已落盘 —— 当前实现容忍部分落盘
    //     状态（下一条语句会因 FK 不完整而失败，但不阻塞当前 CREATE TABLE）。
    //   - 校验 parent_cols 实际存在于 parent_table 中；不一致抛错。
    auto validate_col_in_table = [](const TableInfo* t, const std::string& col) {
        if (!t) return false;
        return t->GetColumn(col) != nullptr;
    };
    for (const auto& fk : foreign_keys_) {
        for (const auto& cc : fk.child_cols) {
            if (!validate_col_in_table(&info, cc)) {
                cat->SetActiveTransaction(nullptr);
                throw CompilerException(ErrorStage::SEMANTIC,
                    "FOREIGN KEY: child column not found: " + cc);
            }
        }
        // parent_table 在当前点可能尚未建（顺序未定的 DDL）；若存在则校验列。
        // 否则保留 FK 不做列校验，等真正 INSERT/UPDATE 时再校验父表存在性。
        const TableInfo* pt = cat->GetTable(fk.parent_table);
        if (pt != nullptr) {
            for (const auto& pc : fk.parent_cols) {
                if (!validate_col_in_table(pt, pc)) {
                    cat->SetActiveTransaction(nullptr);
                    throw CompilerException(ErrorStage::SEMANTIC,
                        "FOREIGN KEY: parent column not found: " +
                        fk.parent_table + "." + pc);
                }
            }
        }
        cat->AddForeignKey(table_name_, fk.child_cols, fk.parent_table,
                           fk.parent_cols, fk.on_delete_action,
                           fk.on_update_action);
    }
    // 列级 REFERENCES parent(col) —— 单列 FK 默认 RESTRICT。
    for (const auto& cd : columns_) {
        for (const auto& ifk : cd.inline_foreign_keys) {
            std::vector<std::string> child_cols = {cd.column_name};
            std::vector<std::string> parent_cols = {ifk.parent_col};
            const TableInfo* pt = cat->GetTable(ifk.parent_table);
            if (pt != nullptr && !validate_col_in_table(pt, ifk.parent_col)) {
                cat->SetActiveTransaction(nullptr);
                throw CompilerException(ErrorStage::SEMANTIC,
                    "FOREIGN KEY: parent column not found: " +
                    ifk.parent_table + "." + ifk.parent_col);
            }
            cat->AddForeignKey(table_name_, child_cols, ifk.parent_table,
                               parent_cols, /*on_delete=*/0, /*on_update=*/0);
        }
    }
    cat->SetActiveTransaction(nullptr);
    executed_ = true;
}

bool CreateTableExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

}  // namespace sqlcompiler