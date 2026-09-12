#include "semantic/SemanticAnalyzer.h"

#include "catalog/SystemCatalog.h"

#include <functional>
#include <utility>

namespace sqlcompiler {

SemanticAnalyzer::SemanticAnalyzer(SystemCatalog* catalog, SymbolTable& symbol_table)
    : catalog_(catalog), symbol_table_(symbol_table) {
}

// 内部版本：不调用 ClearErrors，便于在递归（如 SET_OP_STMT）时累积子树的错误。
// 这里采用「尾段统一检查」的做法：递归前先记下当前错误数量，递归后再合并新增。
bool SemanticAnalyzer::AnalyzeInternal(const StatementPtr& statement, bool& ok) {
    if (!statement) {
        AddError("null statement");
        ok = false;
        return false;
    }
    switch (statement->GetType()) {
        case NodeType::SELECT_STMT:
            ok &= AnalyzeSelect(*std::static_pointer_cast<SelectStatement>(statement));
            break;
        case NodeType::INSERT_STMT:
            ok &= AnalyzeInsert(*std::static_pointer_cast<InsertStatement>(statement));
            break;
        case NodeType::UPDATE_STMT:
            ok &= AnalyzeUpdate(*std::static_pointer_cast<UpdateStatement>(statement));
            break;
        case NodeType::DELETE_STMT:
            ok &= AnalyzeDelete(*std::static_pointer_cast<DeleteStatement>(statement));
            break;
        case NodeType::CREATE_TABLE_STMT:
            ok &= AnalyzeCreateTable(*std::static_pointer_cast<CreateTableStatement>(statement));
            break;
        case NodeType::DROP_TABLE_STMT:
            ok &= AnalyzeDropTable(*std::static_pointer_cast<DropTableStatement>(statement));
            break;
        case NodeType::CREATE_INDEX_STMT:
            ok &= AnalyzeCreateIndex(*std::static_pointer_cast<CreateIndexStatement>(statement));
            break;
        case NodeType::DROP_INDEX_STMT:
            ok &= AnalyzeDropIndex(*std::static_pointer_cast<DropIndexStatement>(statement));
            break;
        case NodeType::TRUNCATE_TABLE_STMT:
            ok &= AnalyzeTruncateTable(*std::static_pointer_cast<TruncateTableStatement>(statement));
            break;
        case NodeType::ALTER_TABLE_STMT:
            ok &= AnalyzeAlterTable(*std::static_pointer_cast<AlterStatement>(statement));
            break;
        // ---- 40_txn_view_udf：事务 / 视图 / 触发器 / UDF ----
        // 这些语句当前都按 no-op 通过语义检查：
        //   - 事务 / 触发器：纯控制流，无表达式求值。
        //   - 视图：catalog 已经记录 SELECT 子句；调用方会在执行期翻译。
        //   - UDF：catalog 记录函数体；调用方的表达式树在执行期才解析参数。
        // 因此本阶段只把它们放进 ok，不再展开进一步检查。
        case NodeType::BEGIN_STMT:
        case NodeType::COMMIT_STMT:
        case NodeType::ROLLBACK_STMT:
        case NodeType::ROLLBACK_TO_STMT:
        case NodeType::SAVEPOINT_STMT:
        case NodeType::RELEASE_SAVEPOINT_STMT:
        case NodeType::SET_ISOLATION_STMT:
        case NodeType::CREATE_VIEW_STMT:
        case NodeType::DROP_VIEW_STMT:
        case NodeType::CREATE_TRIGGER_STMT:
        case NodeType::DROP_TRIGGER_STMT:
        case NodeType::CREATE_FUNCTION_STMT:
        case NodeType::DROP_FUNCTION_STMT:
            break;
        // ---- 46_meta: EXPLAIN / SHOW ----
        // EXPLAIN：递归分析 inner，错误一并累积到 ok。
        // SHOW：仅在 TABLES 时无需 catalog 校验；COLUMNS/INDEX/CREATE_TABLE
        //       时校验 target_table 是否存在。
        case NodeType::EXPLAIN_STMT: {
            auto ex = std::static_pointer_cast<ExplainStatement>(statement);
            if (ex->inner) ok &= AnalyzeInternal(ex->inner, ok);
            break;
        }
        case NodeType::SHOW_STMT: {
            auto sh = std::static_pointer_cast<ShowStatement>(statement);
            if (sh->kind == ShowStatement::Kind::COLUMNS ||
                sh->kind == ShowStatement::Kind::INDEX ||
                sh->kind == ShowStatement::Kind::CREATE_TABLE) {
                if (!CheckTableExists(sh->target_table)) {
                    AddError("table not found: " + sh->target_table);
                    ok = false;
                }
            }
            break;
        }
        case NodeType::SET_OP_STMT: {
            auto so = std::static_pointer_cast<SetOperationStatement>(statement);
            // 列数与类型兼容性：左右两侧 SELECT 列表需有相同数量的列。
            std::function<int(const StatementPtr&)> count_cols =
                [&](const StatementPtr& s) -> int {
                if (!s) return -1;
                if (s->GetType() == NodeType::SELECT_STMT) {
                    auto ss = std::static_pointer_cast<SelectStatement>(s);
                    return static_cast<int>(ss->select_list.size());
                }
                if (s->GetType() == NodeType::SET_OP_STMT) {
                    auto so2 = std::static_pointer_cast<SetOperationStatement>(s);
                    if (so2->left) return count_cols(so2->left);
                }
                return -1;
            };
            int left_cols = count_cols(so->left);
            int right_cols = count_cols(so->right);
            if (left_cols >= 0 && right_cols >= 0 && left_cols != right_cols) {
                AddError("set operation column count mismatch: left has " +
                    std::to_string(left_cols) + " columns, right has " +
                    std::to_string(right_cols) + " columns");
                ok = false;
            }
            // 顶层 ORDER BY 列引用：把当前左右两侧的列别名都视为可见，避免
            // ORDER BY 引用左/右 SELECT 的别名报「column not found」。
            std::vector<std::string> order_aliases;
            std::function<void(const StatementPtr&)> collect_aliases =
                [&](const StatementPtr& s) {
                if (!s) return;
                if (s->GetType() == NodeType::SELECT_STMT) {
                    auto ss = std::static_pointer_cast<SelectStatement>(s);
                    for (const auto& a : ss->select_aliases) {
                        if (!a.empty()) order_aliases.push_back(a);
                    }
                    for (const auto& e : ss->select_list) {
                        if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
                            auto cr = std::static_pointer_cast<ColumnRefExpr>(e);
                            if (!cr->table_name.empty()) continue;
                            order_aliases.push_back(cr->column_name);
                        }
                    }
                } else if (s->GetType() == NodeType::SET_OP_STMT) {
                    auto so2 = std::static_pointer_cast<SetOperationStatement>(s);
                    collect_aliases(so2->left);
                    collect_aliases(so2->right);
                }
            };
            collect_aliases(so->left);
            collect_aliases(so->right);
            if (so->left) ok &= AnalyzeInternal(so->left, ok);
            if (so->right) ok &= AnalyzeInternal(so->right, ok);
            for (const auto& ob : so->order_by) {
                if (!ob.expr) continue;
                CheckExpressionMultiWithAliases(ob.expr, std::vector<std::string>{},
                                                 order_aliases);
            }
            break;
        }
        case NodeType::WITH_STMT: {
            auto wc = std::static_pointer_cast<WithClauseStatement>(statement);
            // 先分析每个 CTE 的查询，然后根据其 select_list 把 CTE 名称注册成
            // 符号表中的虚拟表，使主查询可以按表名引用 CTE。
            // 递归 CTE：先分析 anchor（cte_query）→ 注册 CTE 表 → 再分析
            // recursive_part，使递归 SELECT 能引用 CTE 自身。
            for (auto& cte : wc->ctes) {
                if (cte.cte_query) ok &= AnalyzeInternal(cte.cte_query, ok);
                TableInfo ti;
                ti.table_name = cte.cte_name;
                if (cte.cte_query) {
                    const auto& sl = cte.cte_query->select_list;
                    const auto& sa = cte.cte_query->select_aliases;
                    // 如果是 SELECT * FROM <real_table>，复制该表的列作为 CTE 列
                    // SELECT * 在 parser 中被表达为 FunctionCallExpr("*"), 也兼容 COLUMN_REF_EXPR
                    if (sl.size() == 1 && sl[0] &&
                        ((sl[0]->GetType() == NodeType::COLUMN_REF_EXPR &&
                          std::static_pointer_cast<ColumnRefExpr>(sl[0])->column_name == "*") ||
                         (sl[0]->GetType() == NodeType::FUNCTION_CALL_EXPR &&
                          (std::static_pointer_cast<FunctionCallExpr>(sl[0])->function_name == "*" ||
                           std::static_pointer_cast<FunctionCallExpr>(sl[0])->function_name == "STAR")))) {
                        const auto& ft = cte.cte_query->from_table;
                        if (!ft.empty()) {
                            const TableInfo* src = symbol_table_.GetTable(ft);
                            if (src) {
                                for (const auto& c : src->columns) {
                                    ColumnInfo ci;
                                    ci.name = c.name;
                                    ci.data_type = c.data_type;
                                    ti.columns.push_back(std::move(ci));
                                }
                            }
                        }
                    }
                    if (ti.columns.empty()) {
                        for (size_t i = 0; i < sl.size(); ++i) {
                            ColumnInfo ci;
                            if (i < cte.cte_column_aliases.size() && !cte.cte_column_aliases[i].empty()) {
                                ci.name = cte.cte_column_aliases[i];
                            } else if (i < sa.size() && !sa[i].empty()) {
                                ci.name = sa[i];
                            } else if (sl[i] && sl[i]->GetType() == NodeType::COLUMN_REF_EXPR) {
                                ci.name = std::static_pointer_cast<ColumnRefExpr>(sl[i])->column_name;
                            } else {
                                ci.name = "col" + std::to_string(i);
                            }
                            ci.data_type = "VARCHAR";
                            ti.columns.push_back(std::move(ci));
                        }
                    }
                } else if (!cte.cte_column_aliases.empty()) {
                    // 递归 CTE 但 anchor 不可达（理论上 parser 已保证 cte_query
                    // 非空，置此分支仅为防御）。用 cte_column_aliases 兜底占位。
                    for (const auto& alias : cte.cte_column_aliases) {
                        ColumnInfo ci;
                        ci.name = alias;
                        ci.data_type = "VARCHAR";
                        ti.columns.push_back(std::move(ci));
                    }
                }
                // 注册 CTE 表（递归 CTE 也无条件注册，使 recursive_part 中对
                // CTE 名的引用通过符号表检查）。
                symbol_table_.AddTable(ti);
                // 递归 CTE：在 anchor 已分析 + CTE 表已注册后，再分析 recursive_part，
                // 使递归 SELECT 中对 CTE 名（如 hierarchy）的列引用能解析。
                if (cte.recursive_part) ok &= AnalyzeInternal(cte.recursive_part, ok);
            }
            if (wc->body) ok &= AnalyzeInternal(wc->body, ok);
            break;
        }
        default:
            AddError("unsupported statement type");
            ok = false;
    }
    return ok;
}

bool SemanticAnalyzer::Analyze(const StatementPtr& statement) {
    ClearErrors();
    if (!statement) {
        AddError("null statement");
        return false;
    }
    bool ok = true;
    AnalyzeInternal(statement, ok);
    return ok && errors_.empty();
}

const std::vector<SemanticError>& SemanticAnalyzer::GetErrors() const {
    return errors_;
}

void SemanticAnalyzer::ClearErrors() {
    errors_.clear();
}

bool SemanticAnalyzer::AnalyzeSelect(const SelectStatement& stmt) {
    bool ok = true;
    // 视图：若 from_table 在 catalog 中已注册为视图，跳过表存在性检查；
    // 否则按正常表检查。视图对应的列下标在执行期由 planner 翻译为子查询。
    bool from_is_view = false;
    if (!stmt.from_table.empty() && catalog_ != nullptr) {
        from_is_view = catalog_->HasView(stmt.from_table);
    }
    if (!stmt.from_table.empty() && !from_is_view) {
        ok &= CheckTableExists(stmt.from_table);
    }

    // 收集所有真实表名（不含别名），用于 CheckColumnExists 跨表查找
    std::vector<std::string> real_tables;
    // 别名映射：(alias_or_table_name -> real_table_name)。对 FROM t AS a 或
    // JOIN jt AS b 而言，限定列引用 `a.col`/`b.col` 必须落到 a/b 各自指向的
    // 真实表上做列存在性校验；否则就会出现「跨表偶然找到列就放过」的语义漏检。
    TableAliasMap table_aliases;
    if (!stmt.from_table.empty()) {
        real_tables.push_back(stmt.from_table);
        table_aliases.emplace_back(stmt.from_table, stmt.from_table);
        if (!stmt.from_table_alias.empty()) {
            table_aliases.emplace_back(stmt.from_table_alias, stmt.from_table);
        }
    }
    for (auto& j : stmt.joins) {
        bool join_is_view = (catalog_ != nullptr) && catalog_->HasView(j.table_name);
        if (!join_is_view) {
            ok &= CheckTableExists(j.table_name);
        }
        real_tables.push_back(j.table_name);
        table_aliases.emplace_back(j.table_name, j.table_name);
        if (!j.table_alias.empty()) {
            table_aliases.emplace_back(j.table_alias, j.table_name);
        }
    }
    // 没有 FROM 的查询（如 SELECT 1）：跳过 table/column 检查
    if (stmt.from_table.empty()) return ok;

    // SELECT 列表项按从左到右处理，靠后的项可引用靠前定义的别名。
    std::vector<std::string> visible_aliases;
    for (size_t i = 0; i < stmt.select_list.size(); ++i) {
        ok &= CheckExpressionMultiWithAliases(stmt.select_list[i], real_tables,
                                              visible_aliases, table_aliases);
        if (i < stmt.select_aliases.size() && !stmt.select_aliases[i].empty()) {
            visible_aliases.push_back(stmt.select_aliases[i]);
        }
    }
    if (stmt.where_clause) ok &= CheckExpressionMulti(stmt.where_clause, real_tables, table_aliases);
    for (auto& e : stmt.group_by) ok &= CheckExpressionMulti(e, real_tables, table_aliases);
    // HAVING / ORDER BY 中允许引用 SELECT 列表中的别名。
    std::vector<std::string> all_aliases;
    for (const auto& a : stmt.select_aliases) {
        if (!a.empty()) all_aliases.push_back(a);
    }
    if (stmt.having_clause) ok &= CheckExpressionMultiWithAliases(stmt.having_clause, real_tables,
                                                                  all_aliases, table_aliases);
    for (auto& it : stmt.order_by) ok &= CheckExpressionMultiWithAliases(it.expr, real_tables,
                                                                         all_aliases, table_aliases);
    for (auto& j : stmt.joins) {
        if (j.on_condition) ok &= CheckExpressionMulti(j.on_condition, real_tables, table_aliases);
        // USING (col1, col2, ...) — 校验每个 USING 列同时存在于左右两表。
        for (const auto& cn : j.using_columns) {
            bool in_left = false, in_right = false;
            const TableInfo* left_info = symbol_table_.GetTable(stmt.from_table);
            const TableInfo* right_info = symbol_table_.GetTable(j.table_name);
            if (left_info && left_info->HasColumn(cn)) in_left = true;
            if (right_info && right_info->HasColumn(cn)) in_right = true;
            if (!in_left || !in_right) {
                AddError("USING column '" + cn + "' not found in both tables of JOIN");
                ok = false;
            }
        }
        // NATURAL JOIN — 左右表至少存在一个公共列；否则按 SQL 标准退化为 CROSS JOIN。
        if (j.is_natural) {
            const TableInfo* left_info = symbol_table_.GetTable(stmt.from_table);
            const TableInfo* right_info = symbol_table_.GetTable(j.table_name);
            if (!left_info || !right_info) {
                AddError("NATURAL JOIN: table not found");
                ok = false;
            }
        }
    }
    return ok;
}

bool SemanticAnalyzer::AnalyzeInsert(const InsertStatement& stmt) {
    bool ok = CheckTableExists(stmt.table_name);
    const TableInfo* info = symbol_table_.GetTable(stmt.table_name);
    if (!info) return false;
    int expected = static_cast<int>(info->columns.size());
    for (auto& cn : stmt.columns) {
        ok &= CheckColumnExists(stmt.table_name, cn);
    }
    int actual_cols = static_cast<int>(stmt.columns.size());
    if (actual_cols == 0) actual_cols = expected;
    for (auto& row : stmt.values_list) {
        if (static_cast<int>(row.size()) != actual_cols) {
            AddError("INSERT column count mismatch: got " +
                std::to_string(row.size()) + " expected " +
                std::to_string(actual_cols));
            ok = false;
        }
    }
    // 43_upsert: ON DUPLICATE KEY UPDATE 仅在 VALUES 路径下合法；
    // INSERT ... SELECT 不支持（候选行不可枚举）。
    if (stmt.has_on_duplicate) {
        if (stmt.query) {
            AddError("ON DUPLICATE KEY UPDATE is not supported with INSERT ... SELECT");
            ok = false;
        }
        if (stmt.upsert_assignments.empty()) {
            AddError("ON DUPLICATE KEY UPDATE requires at least one assignment");
            ok = false;
        }
        // 校验每个被赋值的列存在于目标表中；表达式 CheckExpressionMulti 允许
        // 引用目标表的列 + 解析 VALUES(col)。
        std::vector<std::pair<std::string, ExprPtr>> valid_assigns;
        for (const auto& kv : stmt.upsert_assignments) {
            ok &= CheckColumnExists(stmt.table_name, kv.first);
            // 表达式里允许 ColumnRefExpr（目标表现有列）+ UpsertValuesRefExpr
            // （VALUES(col)）+ 其它表达式；统一交给 CheckExpression 校验。
            ok &= CheckExpression(kv.second, stmt.table_name);
        }
    }
    return ok;
}

bool SemanticAnalyzer::AnalyzeUpdate(const UpdateStatement& stmt) {
    bool ok = CheckTableExists(stmt.table_name);
    for (auto& kv : stmt.assignments) {
        ok &= CheckColumnExists(stmt.table_name, kv.first);
        ok &= CheckExpression(kv.second, stmt.table_name);
    }
    if (stmt.where_clause) ok &= CheckExpression(stmt.where_clause, stmt.table_name);
    return ok;
}

bool SemanticAnalyzer::AnalyzeDelete(const DeleteStatement& stmt) {
    bool ok = CheckTableExists(stmt.table_name);
    if (stmt.where_clause) ok &= CheckExpression(stmt.where_clause, stmt.table_name);
    return ok;
}

bool SemanticAnalyzer::AnalyzeCreateTable(const CreateTableStatement& stmt) {
    bool ok = true;
    if (symbol_table_.HasTable(stmt.table_name) && !stmt.if_not_exists) {
        AddError("table already exists: " + stmt.table_name);
        ok = false;
    }
    // Check duplicate column names
    for (size_t i = 0; i < stmt.columns.size(); ++i) {
        for (size_t j = i + 1; j < stmt.columns.size(); ++j) {
            const auto& a = stmt.columns[i].column_name;
            const auto& b = stmt.columns[j].column_name;
            std::string ua, ub;
            for (char c : a) ua.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            for (char c : b) ub.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            if (ua == ub) {
                AddError("duplicate column name: " + a);
                ok = false;
            }
        }
        // Validate type (支持 BIGINT/INTEGER/DOUBLE/DECIMAL/CHAR/TEXT/STRING 等全部归一化类型)
        std::string up;
        for (char c : stmt.columns[i].data_type)
            up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        if (up != "INT" && up != "INTEGER" && up != "BIGINT" &&
            up != "FLOAT" && up != "DOUBLE" && up != "DECIMAL" &&
            up != "VARCHAR" && up != "CHAR" && up != "TEXT" && up != "STRING" &&
            up != "DATE" && up != "TIMESTAMP") {
            AddError("unsupported column type: " + stmt.columns[i].data_type);
            ok = false;
        }
    }
    // 表级 PRIMARY KEY(a, b, ...) 引用的列必须存在于列定义中。
    for (const auto& pk : stmt.primary_keys) {
        for (const auto& pk_col : pk) {
            bool found = false;
            for (const auto& cd : stmt.columns) {
                if (cd.column_name == pk_col) { found = true; break; }
            }
            if (!found) {
                AddError("PRIMARY KEY references unknown column: " + pk_col);
                ok = false;
            }
        }
    }
    return ok;
}

bool SemanticAnalyzer::AnalyzeDropTable(const DropTableStatement& stmt) {
    // IF EXISTS 时不存在也不算错误，交由执行阶段静默跳过
    if (stmt.if_exists) return true;
    return CheckTableExists(stmt.table_name);
}

bool SemanticAnalyzer::AnalyzeCreateIndex(const CreateIndexStatement& stmt) {
    if (!CheckTableExists(stmt.table_name)) return false;
    const TableInfo* table = symbol_table_.GetTable(stmt.table_name);
    if (table == nullptr) return false;
    if (stmt.key_columns.empty()) {
        AddError("index must have at least one column");
        return false;
    }
    bool ok = true;
    for (const auto& col : stmt.key_columns) {
        if (table->GetColumn(col) == nullptr) {
            AddError("column not found: " + stmt.table_name + "." + col);
            ok = false;
        }
    }
    // 列的可索引性（长度上限、非空）留给执行阶段的 SystemCatalog::CreateIndex 统一
    // 判定，避免同一套规则在两处各写一遍而漂移。
    return ok;
}

bool SemanticAnalyzer::AnalyzeDropIndex(const DropIndexStatement& stmt) {
    // 索引是否存在只有 SystemCatalog 知道（SymbolTable 不持有索引元数据），
    // 因此存在性检查放在执行阶段。
    (void)stmt;
    return true;
}

bool SemanticAnalyzer::AnalyzeTruncateTable(const TruncateTableStatement& stmt) {
    return CheckTableExists(stmt.table_name);
}

bool SemanticAnalyzer::AnalyzeAlterTable(const AlterStatement& stmt) {
    // ALTER TABLE 现在由 AlterTableExecutor 真正改写 schema，因此语义层
    // 必须做完整校验，避免「错误参数也能 ALTER 成功」导致目录与数据脱节。
    if (!CheckTableExists(stmt.table_name)) return false;
    const TableInfo* info = symbol_table_.GetTable(stmt.table_name);
    if (!info) return false;  // CheckTableExists 已报错。
    switch (stmt.action) {
        case AlterAction::ADD_COLUMN: {
            const auto& cd = stmt.column_def;
            if (!cd || cd->column_name.empty()) {
                AddError("ALTER TABLE ADD COLUMN requires a column name");
                return false;
            }
            if (info->HasColumn(cd->column_name)) {
                AddError("column already exists: " + cd->column_name);
                return false;
            }
            return true;
        }
        case AlterAction::DROP_COLUMN: {
            if (!info->HasColumn(stmt.drop_column_name)) {
                AddError("column not found: " + stmt.drop_column_name);
                return false;
            }
            return true;
        }
        case AlterAction::RENAME_TO: {
            if (stmt.new_table_name.empty()) {
                AddError("ALTER TABLE RENAME TO requires a new table name");
                return false;
            }
            if (stmt.new_table_name == stmt.table_name) return true;
            if (symbol_table_.HasTable(stmt.new_table_name)) {
                AddError("table already exists: " + stmt.new_table_name);
                return false;
            }
            return true;
        }
        case AlterAction::MODIFY_COLUMN: {
            const auto& cd = stmt.column_def;
            if (!cd || cd->column_name.empty()) {
                AddError("ALTER TABLE MODIFY COLUMN requires a column name");
                return false;
            }
            if (!info->HasColumn(cd->column_name)) {
                AddError("column not found: " + cd->column_name);
                return false;
            }
            return true;
        }
    }
    return true;
}

bool SemanticAnalyzer::CheckTableExists(const std::string& table_name) {
    if (symbol_table_.HasTable(table_name)) return true;
    AddError("table not found: " + table_name);
    return false;
}

bool SemanticAnalyzer::CheckColumnExists(const std::string& table_name,
                                          const std::string& column_name) {
    // table_name may be a comma-separated list of table names (for JOIN).
    auto check_one = [&](const std::string& t) -> bool {
        const TableInfo* info = symbol_table_.GetTable(t);
        if (info && info->HasColumn(column_name)) return true;
        return false;
    };
    if (table_name.find(',') != std::string::npos) {
        size_t start = 0;
        while (start < table_name.size()) {
            size_t end = table_name.find(',', start);
            std::string t = table_name.substr(start,
                end == std::string::npos ? std::string::npos : end - start);
            if (check_one(t)) return true;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        AddError("column not found: " + column_name);
        return false;
    }
    if (check_one(table_name)) return true;
    AddError("column not found: " + table_name + "." + column_name);
    return false;
}


// 跨表列检查（支持表别名）：限定列 `alias.col` 必须解析到 alias 对应的真实
// 表，且 col 必须存在于该表中；未限定列在所有真实表中查找。
bool SemanticAnalyzer::CheckExpressionMulti(const ExprPtr& expr,
                                          const std::vector<std::string>& tables,
                                          const TableAliasMap& table_aliases) {
    if (!expr) return true;
    switch (expr->GetType()) {
        case NodeType::LITERAL_EXPR:
            return true;
        case NodeType::COLUMN_REF_EXPR: {
            auto cr = std::static_pointer_cast<ColumnRefExpr>(expr);
            // 限定列（alias/table.col）：先把 cr->table_name 解析到真实表，
            // 再在该真实表上做列存在性校验。
            if (!cr->table_name.empty()) {
                std::string resolved;
                for (const auto& kv : table_aliases) {
                    if (kv.first == cr->table_name) { resolved = kv.second; break; }
                }
                if (resolved.empty()) {
                    // 别名/表名都不在当前 FROM 作用域中：报错。
                    AddError("column not found: " + cr->column_name);
                    return false;
                }
                const TableInfo* info = symbol_table_.GetTable(resolved);
                if (!info || !info->HasColumn(cr->column_name)) {
                    AddError("column not found: " + cr->column_name);
                    return false;
                }
                return true;
            }
            // 未限定列：在所有真实表中查找
            for (const auto& t : tables) {
                const TableInfo* info = symbol_table_.GetTable(t);
                if (info && info->HasColumn(cr->column_name)) return true;
            }
            AddError("column not found: " + cr->column_name);
            return false;
        }
        case NodeType::BINARY_EXPR: {
            auto be = std::static_pointer_cast<BinaryExpr>(expr);
            return CheckExpressionMulti(be->left, tables, table_aliases) &&
                   CheckExpressionMulti(be->right, tables, table_aliases);
        }
        case NodeType::UNARY_EXPR: {
            auto ue = std::static_pointer_cast<UnaryExpr>(expr);
            return CheckExpressionMulti(ue->operand, tables, table_aliases);
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            auto fc = std::static_pointer_cast<FunctionCallExpr>(expr);
            bool ok = true;
            for (auto& a : fc->arguments) ok &= CheckExpressionMulti(a, tables, table_aliases);
            return ok;
        }
        case NodeType::LIKE_EXPR: {
            // 44_pattern_match: LIKE / ILIKE / REGEXP / RLIKE 仅校验
            // 左/右子表达式中的列引用，模式串为字面量无需检查。
            auto le = std::static_pointer_cast<LikeExprNode>(expr);
            return CheckExpressionMulti(le->operand, tables, table_aliases) &&
                   CheckExpressionMulti(le->pattern, tables, table_aliases);
        }
        default:
            return true;
    }
}

// 同上，但额外允许出现在 aliases 中的名字解析为合法标识符（对应 SELECT 别名）。
// 用于 ORDER BY / HAVING / 同 SELECT 列表中靠后项的引用。
bool SemanticAnalyzer::CheckExpressionMultiWithAliases(const ExprPtr& expr,
                                                       const std::vector<std::string>& tables,
                                                       const std::vector<std::string>& aliases,
                                                       const TableAliasMap& table_aliases) {
    if (!expr) return true;
    switch (expr->GetType()) {
        case NodeType::LITERAL_EXPR:
            return true;
        case NodeType::COLUMN_REF_EXPR: {
            auto cr = std::static_pointer_cast<ColumnRefExpr>(expr);
            // 限定列（alias/table.col）：解析到真实表，再在该表上做列存在性校验
            if (!cr->table_name.empty()) {
                std::string resolved;
                for (const auto& kv : table_aliases) {
                    if (kv.first == cr->table_name) { resolved = kv.second; break; }
                }
                if (resolved.empty()) {
                    AddError("column not found: " + cr->column_name);
                    return false;
                }
                const TableInfo* info = symbol_table_.GetTable(resolved);
                if (!info || !info->HasColumn(cr->column_name)) {
                    AddError("column not found: " + cr->column_name);
                    return false;
                }
                return true;
            }
            // 未限定列：先看是不是 SELECT 别名
            for (const auto& a : aliases) {
                if (a == cr->column_name) return true;
            }
            // 再在真实表中查找
            for (const auto& t : tables) {
                const TableInfo* info = symbol_table_.GetTable(t);
                if (info && info->HasColumn(cr->column_name)) return true;
            }
            AddError("column not found: " + cr->column_name);
            return false;
        }
        case NodeType::BINARY_EXPR: {
            auto be = std::static_pointer_cast<BinaryExpr>(expr);
            return CheckExpressionMultiWithAliases(be->left, tables, aliases, table_aliases) &&
                   CheckExpressionMultiWithAliases(be->right, tables, aliases, table_aliases);
        }
        case NodeType::UNARY_EXPR: {
            auto ue = std::static_pointer_cast<UnaryExpr>(expr);
            return CheckExpressionMultiWithAliases(ue->operand, tables, aliases, table_aliases);
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            auto fc = std::static_pointer_cast<FunctionCallExpr>(expr);
            bool ok = true;
            for (auto& a : fc->arguments) ok &= CheckExpressionMultiWithAliases(a, tables, aliases, table_aliases);
            return ok;
        }
        case NodeType::LIKE_EXPR: {
            auto le = std::static_pointer_cast<LikeExprNode>(expr);
            return CheckExpressionMultiWithAliases(le->operand, tables, aliases, table_aliases) &&
                   CheckExpressionMultiWithAliases(le->pattern, tables, aliases, table_aliases);
        }
        default:
            return true;
    }
}

bool SemanticAnalyzer::CheckExpression(const ExprPtr& expr, const std::string& table_name) {
    if (!expr) return true;
    bool ok = true;
    switch (expr->GetType()) {
        case NodeType::LITERAL_EXPR:
            return true;
        case NodeType::COLUMN_REF_EXPR: {
            auto cr = std::static_pointer_cast<ColumnRefExpr>(expr);
            // If the column reference is qualified, verify the specific table.
            if (!cr->table_name.empty()) {
                ok &= CheckColumnExists(cr->table_name, cr->column_name);
            } else {
                ok &= CheckColumnExists(table_name, cr->column_name);
            }
            return ok;
        }
        case NodeType::BINARY_EXPR: {
            auto be = std::static_pointer_cast<BinaryExpr>(expr);
            ok &= CheckExpression(be->left, table_name);
            ok &= CheckExpression(be->right, table_name);
            return ok;
        }
        case NodeType::UNARY_EXPR: {
            auto ue = std::static_pointer_cast<UnaryExpr>(expr);
            return CheckExpression(ue->operand, table_name);
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            auto fc = std::static_pointer_cast<FunctionCallExpr>(expr);
            for (auto& a : fc->arguments) ok &= CheckExpression(a, table_name);
            return ok;
        }
        default:
            return true;
    }
}

void SemanticAnalyzer::AddError(const std::string& message, int line) {
    SemanticError err;
    err.message = message;
    err.line = line;
    errors_.push_back(std::move(err));
}

}  // namespace sqlcompiler