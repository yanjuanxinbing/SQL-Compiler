#include "semantic/SemanticAnalyzer.h"

#include "catalog/SystemCatalog.h"
#include "common/CaseInsensitive.h"
#include "common/EditDistance.h"

#include <algorithm>
#include <functional>
#include <unordered_set>
#include <utility>

namespace sqlcompiler {

SemanticAnalyzer::SemanticAnalyzer(SystemCatalog* catalog, SymbolTable& symbol_table)
    : catalog_(catalog), symbol_table_(symbol_table) {
}

// 内部版本：不调用 ClearErrors，便于在递归（如 SET_OP_STMT）时累积子树的错误。
// 这里采用「尾段统一检查」的做法：递归前先记下当前错误数量，递归后再合并新增。
bool SemanticAnalyzer::AnalyzeInternal(const StatementPtr& statement, bool& ok) {
    if (!statement) {
        AddError(SemanticErrorKind::Other, "null statement");
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
        case NodeType::MERGE_STMT:
            ok &= AnalyzeMerge(*std::static_pointer_cast<MergeStatement>(statement));
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
        case NodeType::CREATE_VIEW_STMT:
        case NodeType::DROP_VIEW_STMT:
        // 60_view_trigger: 物化视图 / 物化视图 ALTER 走 no-op 语义校验。
        case NodeType::CREATE_MATERIALIZED_VIEW_STMT:
        case NodeType::ALTER_MATERIALIZED_VIEW_STMT:
        case NodeType::CREATE_TRIGGER_STMT:
        case NodeType::DROP_TRIGGER_STMT:
        case NodeType::CREATE_FUNCTION_STMT:
        case NodeType::DROP_FUNCTION_STMT:
        // ---- 59_procs (Category 8)：PROCEDURE / CALL ----
        case NodeType::CREATE_PROCEDURE_STMT:
        case NodeType::DROP_PROCEDURE_STMT:
        case NodeType::CALL_STMT:
        // ---- 53_ddl: SCHEMA / SEQUENCE ----
        case NodeType::CREATE_SCHEMA_STMT:
        case NodeType::DROP_SCHEMA_STMT:
        case NodeType::CREATE_SEQUENCE_STMT:
        case NodeType::DROP_SEQUENCE_STMT:
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
                // 60_view_trigger: SHOW COLUMNS / DESCRIBE / SHOW INDEX /
                // SHOW CREATE TABLE 对物化视图同样有效；先把 MV 当作合法
                // 目标放过（catalog 的 GetColumnInfos 已优先按 MV columns 展开）。
                if (catalog_ == nullptr ||
                    !catalog_->HasMaterializedView(sh->target_table)) {
                    if (!CheckTableExists(sh->target_table, sh->line, sh->column)) {
                        AddError(SemanticErrorKind::TableNotFound,
                                 "table not found: " + sh->target_table,
                                 sh->line, sh->column);
                        ok = false;
                    }
                }
            }
            break;
        }
        case NodeType::SET_OP_STMT: {
            auto so = std::static_pointer_cast<SetOperationStatement>(statement);
            // item #16: 一次走完「列数 + 别名收集」，替代原 count_cols/collect_aliases
            // 两个 lambda + 2 次 AnalyzeInternal 的 4× walker。对深度 D 的 SET_OP 树
            // 总开销从 O(4D) 降到 O(D)。
            SetOpWalkResult left_walk = WalkSetOpTree(so->left);
            SetOpWalkResult right_walk = WalkSetOpTree(so->right);
            int left_cols = left_walk.col_count;
            int right_cols = right_walk.col_count;
            if (left_cols >= 0 && right_cols >= 0 && left_cols != right_cols) {
                AddError(SemanticErrorKind::ArityMismatch,
                    "set operation column count mismatch: left has " +
                    std::to_string(left_cols) + " columns, right has " +
                    std::to_string(right_cols) + " columns",
                    so->line, so->column);
                ok = false;
            }
            // 顶层 ORDER BY 列引用：把当前左右两侧的列别名都视为可见，避免
            // ORDER BY 引用左/右 SELECT 的别名报「column not found」。
            std::vector<std::string> order_aliases;
            order_aliases.reserve(left_walk.aliases.size() + right_walk.aliases.size());
            order_aliases.insert(order_aliases.end(),
                                 left_walk.aliases.begin(), left_walk.aliases.end());
            order_aliases.insert(order_aliases.end(),
                                 right_walk.aliases.begin(), right_walk.aliases.end());
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
                // bug3: 即使 cte_query 为空也要分析 cte_body，让 UNION/INTERSECT/
                // EXCEPT 这类 SetOp body 的列被注册到符号表。旧实现 cte_query
                // 仅在单 SELECT 或递归 UNION ALL 左侧时非空，导致普通 CTE 的
                // SetOp body 既不分析也没列定义。
                if (!cte.cte_query && cte.cte_body) {
                    ok &= AnalyzeInternal(cte.cte_body, ok);
                }
                TableInfo ti;
                ti.table_name = cte.cte_name;
                if (cte.cte_query) {
                    // item #17: 列派生收敛到 BuildCteColumns。
                    BuildCteColumns(ti, *cte.cte_query, cte.cte_column_aliases);
                } else {
                    // bug3: 非递归 CTE 且 body 是 SetOperationStatement 时
                    // cte_query 为空，从 cte_body 的左侧 SELECT 派生列名——
                    // SetOp 的列名约定与左支 SELECT 保持一致（ANSI/PG 语义）。
                    const SelectStatement* body_select = nullptr;
                    if (auto sop = dynamic_cast<const SetOperationStatement*>(cte.cte_body.get())) {
                        body_select = dynamic_cast<const SelectStatement*>(sop->left.get());
                    } else if (auto sel = dynamic_cast<const SelectStatement*>(cte.cte_body.get())) {
                        body_select = sel;
                    }
                    if (body_select) {
                        // item #17: 同样的列派生逻辑直接复用，避免重复代码漂移。
                        BuildCteColumns(ti, *body_select, cte.cte_column_aliases);
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
            AddError(SemanticErrorKind::Other, "unsupported statement type",
                     statement->line, statement->column);
            ok = false;
    }
    return ok;
}

bool SemanticAnalyzer::Analyze(const StatementPtr& statement) {
    ClearErrors();
    if (!statement) {
        AddError(SemanticErrorKind::Other, "null statement");
        return false;
    }
    bool ok = true;
    AnalyzeInternal(statement, ok);
    return ok && errors_.empty();
}

// item #16: 单次递归遍历 SET_OP 子树，把「叶子列数 + 别名收集」一次做完。
// 原实现是两个 lambda + 2 次 AnalyzeInternal 对同一棵子树各走一遍；新版合并
// 成一次 walk，递归总次数从 4×O(D) 降到 1×O(D)（D = SET_OP 嵌套深度）。
//
// 行为等价性：原 count_cols 在没有 SELECT_STMT 叶子时返回 -1；别名收集要求
// 「走到 SELECT_STMT 才收集，跳过 SET_OP_STMT 自身」。本函数保持相同语义。
SemanticAnalyzer::SetOpWalkResult SemanticAnalyzer::WalkSetOpTree(
    const StatementPtr& statement) const {
    SetOpWalkResult result;
    result.col_count = -1;
    if (!statement) return result;
    if (statement->GetType() == NodeType::SELECT_STMT) {
        auto ss = std::static_pointer_cast<SelectStatement>(statement);
        result.col_count = static_cast<int>(ss->select_list.size());
        result.aliases.reserve(ss->select_aliases.size() + ss->select_list.size());
        for (const auto& a : ss->select_aliases) {
            if (!a.empty()) result.aliases.push_back(a);
        }
        for (const auto& e : ss->select_list) {
            if (e && e->GetType() == NodeType::COLUMN_REF_EXPR) {
                auto cr = std::static_pointer_cast<ColumnRefExpr>(e);
                if (!cr->table_name.empty()) continue;
                result.aliases.push_back(cr->column_name);
            }
        }
        return result;
    }
    if (statement->GetType() == NodeType::SET_OP_STMT) {
        auto so = std::static_pointer_cast<SetOperationStatement>(statement);
        SetOpWalkResult left = WalkSetOpTree(so->left);
        SetOpWalkResult right = WalkSetOpTree(so->right);
        // 行为：原 count_cols 只看 left 分支（遇到 SET_OP_STMT 就递归左支），
        // 即忽略 right 分支的列数差异。这里保留同语义：col_count 取 left
        // 的列数（与原实现一致）。
        result.col_count = left.col_count;
        // 但别名要把 right 分支的也并入 —— 原 collect_aliases 对 left/right
        // 都递归，所以别名会双向累积。
        result.aliases.reserve(left.aliases.size() + right.aliases.size());
        result.aliases.insert(result.aliases.end(),
                              left.aliases.begin(), left.aliases.end());
        result.aliases.insert(result.aliases.end(),
                              right.aliases.begin(), right.aliases.end());
        return result;
    }
    return result;
}

// item #17: 把 WITH_STMT 中两个几乎完全相同的列派生块统一到一个函数。
// 原逻辑在 cte_query 路径和 cte_body 路径上各写了一遍「SELECT * 展开 / 否则
// 按列派生」 流程。本函数只关心 SELECT_STMT 本身，调用方负责传入正确的
// SELECT（anchor 或 body 的 left）。所有分支细节（* 展开 / cte_column_aliases /
// select_aliases / COLUMN_REF / col<i> 占位）都集中在这一处。
void SemanticAnalyzer::BuildCteColumns(TableInfo& ti,
                                       const SelectStatement& select,
                                       const std::vector<std::string>& cte_column_aliases) const {
    const auto& sl = select.select_list;
    const auto& sa = select.select_aliases;
    // SELECT * / SELECT table.* 展开：复制源表列到 ti.columns。
    if (sl.size() == 1 && sl[0] &&
        ((sl[0]->GetType() == NodeType::COLUMN_REF_EXPR &&
          std::static_pointer_cast<ColumnRefExpr>(sl[0])->column_name == "*") ||
         (sl[0]->GetType() == NodeType::FUNCTION_CALL_EXPR &&
          (std::static_pointer_cast<FunctionCallExpr>(sl[0])->function_name == "*" ||
           std::static_pointer_cast<FunctionCallExpr>(sl[0])->function_name == "STAR")))) {
        const auto& ft = select.from_table;
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
    if (!ti.columns.empty()) return;
    // 逐列派生：cte_column_aliases → select_aliases → COLUMN_REF → col<i>。
    for (size_t i = 0; i < sl.size(); ++i) {
        ColumnInfo ci;
        if (i < cte_column_aliases.size() && !cte_column_aliases[i].empty()) {
            ci.name = cte_column_aliases[i];
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
    bool from_is_mv = false;
    if (!stmt.from_table.empty() && catalog_ != nullptr) {
        from_is_view = catalog_->HasView(stmt.from_table);
        // 60_view_trigger: 物化视图也跳过表存在性检查（planner 会替换为 backing table）。
        from_is_mv = catalog_->HasMaterializedView(stmt.from_table);
    }
    if (!stmt.from_table.empty() && !from_is_view && !from_is_mv) {
        ok &= CheckTableExists(stmt.from_table, stmt.line, stmt.column);
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
        // BUG-4: 当 FROM 引用了视图时，把视图 SELECT 的输出列（含 AS 别名）
        // 注册为一张虚拟表放进 symbol_table_，让 ORDER BY / HAVING / WHERE
        // 中的别名引用能通过列存在性校验。语义层先于 Planner 运行，
        // 因此视图展开必须在语义层完成（与 LATERAL / VALUES 派生表同形）。
        // Planner::TryExpandView 后续仍会把视图替换为 derived_table，
        // 但那时 ORDER BY 早已过语义检查，所以这里必须做一遍。
        if (from_is_view && catalog_ != nullptr) {
            const SystemCatalog::ViewDefinition* v =
                catalog_->LookupView(stmt.from_table);
            if (v != nullptr && v->query != nullptr) {
                TableInfo ti;
                ti.table_name = stmt.from_table;
                const auto& sl = v->query->select_list;
                const auto& sa = v->query->select_aliases;
                for (size_t i = 0; i < sl.size(); ++i) {
                    ColumnInfo ci;
                    if (i < sa.size() && !sa[i].empty()) {
                        ci.name = sa[i];
                    } else if (sl[i] &&
                               sl[i]->GetType() == NodeType::COLUMN_REF_EXPR) {
                        ci.name = std::static_pointer_cast<ColumnRefExpr>(sl[i])
                                      ->column_name;
                    } else {
                        ci.name = "col" + std::to_string(i);
                    }
                    ci.data_type = "VARCHAR";
                    ti.columns.push_back(std::move(ci));
                }
                symbol_table_.AddTable(ti);
            }
        }
    }
    for (auto& j : stmt.joins) {
        // 派生表 JOIN 右操作数：JOIN (SELECT ...) [AS] alias —— 不查 catalog，
        // 改为注册其 alias 作为「虚拟表」。列来自内层 select_list 与 select_aliases。
        // （与 LATERAL 子查询走相同的「虚拟表」模式，但语义层不维护 outer_bind。）
        if (j.derived_subquery || j.derived_set_op) {
            std::string derived_alias = !j.table_alias.empty() ? j.table_alias : j.table_name;
            // 把内层 SELECT / 集合运算的 SELECT 节点收齐，统一抽取列名。
            // 集合运算（UNION/INTERSECT/EXCEPT）取 left 的 select list 作代表：
            // 两个分支同构、列数对齐，方言层足够覆盖常见用法。
            SelectStatement* inner_select = nullptr;
            if (j.derived_subquery) {
                inner_select = j.derived_subquery.get();
            } else if (j.derived_set_op && j.derived_set_op->left) {
                inner_select = dynamic_cast<SelectStatement*>(j.derived_set_op->left.get());
            }
            TableInfo ti;
            ti.table_name = derived_alias;
            if (inner_select) {
                const auto& sl = inner_select->select_list;
                const auto& sa = inner_select->select_aliases;
                for (size_t i = 0; i < sl.size(); ++i) {
                    ColumnInfo ci;
                    if (i < sa.size() && !sa[i].empty()) {
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
            // Catalog 的 SymbolTable 在跨语句间持久化；同名派生别名再次出现
            // （典型如：前一条 SELECT 已把 `c` 注册为虚拟表，下一条 SELECT 复用
            // 同名派生别名）时 RemoveTable 把旧条目清掉，让 AddTable 重新生效。
            // 否则旧的列集合会覆盖新 SELECT 的列，导致外层列引用解析失败。
            symbol_table_.RemoveTable(derived_alias);
            symbol_table_.AddTable(ti);
            real_tables.push_back(derived_alias);
            table_aliases.emplace_back(derived_alias, derived_alias);
            continue;
        }
        // 55_query: LATERAL 派生表 join —— 不在 catalog 中查表存在性；
        // 改为注册其 alias 作为「虚拟表」，并把内部子查询的 select list / 别名
        // 收集为该虚拟表的列，让外层 `sub.col` 与 `m` 等引用通过。
        if (j.is_lateral) {
            std::string derived_alias = !j.table_alias.empty() ? j.table_alias : j.table_name;
            TableInfo ti;
            ti.table_name = derived_alias;
            if (j.lateral_subquery) {
                const auto& sl = j.lateral_subquery->select_list;
                const auto& sa = j.lateral_subquery->select_aliases;
                for (size_t i = 0; i < sl.size(); ++i) {
                    ColumnInfo ci;
                    if (i < sa.size() && !sa[i].empty()) {
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
            symbol_table_.AddTable(ti);
            real_tables.push_back(derived_alias);
            table_aliases.emplace_back(derived_alias, derived_alias);
            continue;
        }
        bool join_is_view = (catalog_ != nullptr) && catalog_->HasView(j.table_name);
        if (!join_is_view) {
            ok &= CheckTableExists(j.table_name, stmt.line, stmt.column);
        }
        real_tables.push_back(j.table_name);
        table_aliases.emplace_back(j.table_name, j.table_name);
        if (!j.table_alias.empty()) {
            table_aliases.emplace_back(j.table_alias, j.table_name);
        }
    }
    // 没有 FROM 的查询（如 SELECT 1）：跳过 table/column 检查
    if (stmt.from_table.empty() && stmt.values_rows.empty()) return ok;

    // 55_query: VALUES 派生表注册为虚拟表，让 `t.id` / `id` 列引用通过。
    if (!stmt.values_rows.empty()) {
        TableInfo ti;
        ti.table_name = stmt.derived_alias;
        size_t ncols = stmt.values_column_aliases.empty()
                          ? (stmt.values_rows.empty() ? 0 : stmt.values_rows[0].size())
                          : stmt.values_column_aliases.size();
        for (size_t i = 0; i < ncols; ++i) {
            ColumnInfo ci;
            if (i < stmt.values_column_aliases.size() &&
                !stmt.values_column_aliases[i].empty()) {
                ci.name = stmt.values_column_aliases[i];
            } else {
                ci.name = "col" + std::to_string(i);
            }
            ci.data_type = "VARCHAR";
            ti.columns.push_back(std::move(ci));
        }
        symbol_table_.AddTable(ti);
        real_tables.push_back(stmt.derived_alias);
        table_aliases.emplace_back(stmt.derived_alias, stmt.derived_alias);
    }

    // SELECT 列表项按从左到右处理，靠后的项可引用靠前定义的别名。
    std::vector<std::string> visible_aliases;
    for (size_t i = 0; i < stmt.select_list.size(); ++i) {
        ok &= CheckExpressionMultiWithAliases(stmt.select_list[i], real_tables,
                                              visible_aliases, table_aliases);
        if (i < stmt.select_aliases.size() && !stmt.select_aliases[i].empty()) {
            visible_aliases.push_back(stmt.select_aliases[i]);
        }
    }
    // GROUP BY / HAVING / ORDER BY 中允许引用 SELECT 列表中的别名
    // （SQL 标准：别名在同 SELECT 块内可见，GROUP BY/HAVING/ORDER BY 均可引用；
    // 与 PostgreSQL / MySQL / SQL Server 等主流实现一致）。
    // 实际语义正确性由 Planner 在生成 AggregateNode 时把裸别名
    // ColumnRefExpr 改写为对应 SELECT-list 表达式的克隆 —— 否则 AggregateExecutor
    // 会在 column_index_map 中查不到别名，GROUP BY 整列退化为 NULL，所有行
    // 坍缩到同一组。
    std::vector<std::string> all_aliases;
    for (const auto& a : stmt.select_aliases) {
        if (!a.empty()) all_aliases.push_back(a);
    }
    if (stmt.where_clause) ok &= CheckExpressionMulti(stmt.where_clause, real_tables, table_aliases);
    for (auto& e : stmt.group_by) ok &= CheckExpressionMultiWithAliases(e, real_tables,
                                                                       all_aliases, table_aliases);
    if (stmt.having_clause) ok &= CheckExpressionMultiWithAliases(stmt.having_clause, real_tables,
                                                                  all_aliases, table_aliases);
    for (auto& it : stmt.order_by) ok &= CheckExpressionMultiWithAliases(it.expr, real_tables,
                                                                         all_aliases, table_aliases);
    for (auto& j : stmt.joins) {
        if (j.on_condition) ok &= CheckExpressionMulti(j.on_condition, real_tables, table_aliases);
        // USING (col1, col2, ...) — 校验每个 USING 列同时存在于左右两表。
        // 把 left_info / right_info 上提到循环外，避免对每个 USING 列重复查表。
        const TableInfo* left_info = symbol_table_.GetTable(stmt.from_table);
        const TableInfo* right_info = symbol_table_.GetTable(j.table_name);
        for (const auto& cn : j.using_columns) {
            bool in_left = (left_info != nullptr) && left_info->HasColumnFast(cn);
            bool in_right = (right_info != nullptr) && right_info->HasColumnFast(cn);
            if (!in_left || !in_right) {
                AddError(SemanticErrorKind::ColumnNotFound,
                         "USING column '" + cn + "' not found in both tables of JOIN",
                         stmt.line, stmt.column);
                ok = false;
            }
        }
        // NATURAL JOIN — 左右表至少存在一个公共列；否则按 SQL 标准退化为 CROSS JOIN。
        if (j.is_natural) {
            if (!left_info || !right_info) {
                AddError(SemanticErrorKind::TableNotFound,
                         "NATURAL JOIN: table not found",
                         stmt.line, stmt.column);
                ok = false;
            }
        }
    }
    return ok;
}

bool SemanticAnalyzer::AnalyzeInsert(const InsertStatement& stmt) {
    // 60_view_trigger: INSERT INTO view_name —— 视图被识别为可写视图（单表 SELECT
    // 且 from_table 命中 catalog 中已存在的表）。语义层允许这种改写，跳过对 view
    // 自身的"table not found"检查，由 Planner 在后续路径上把 target 替换为底层表。
    bool ok = true;
    bool target_is_view = false;
    std::string resolved_target = stmt.table_name;
    if (catalog_ != nullptr) {
        const SystemCatalog::ViewDefinition* v = catalog_->LookupView(stmt.table_name);
        if (v != nullptr && v->query != nullptr && v->query->joins.empty() &&
            !v->query->from_table.empty()) {
            target_is_view = true;
            resolved_target = v->query->from_table;
        }
    }
    if (!target_is_view) {
        ok = CheckTableExists(stmt.table_name, stmt.line, stmt.column);
    }
    const TableInfo* info = symbol_table_.GetTable(resolved_target);
    if (!info) {
        // 视图已注册但底层表未找到 —— 抛错保持原行为。
        if (target_is_view) {
            AddError(SemanticErrorKind::TableNotFound,
                     "view underlying table not found: " + resolved_target,
                     stmt.line, stmt.column);
            return false;
        }
        return false;
    }
    int expected = static_cast<int>(info->columns.size());
    for (auto& cn : stmt.columns) {
        ok &= CheckColumnExists(resolved_target, cn, stmt.line, stmt.column);
    }
    int actual_cols = static_cast<int>(stmt.columns.size());
    if (actual_cols == 0) actual_cols = expected;
    for (auto& row : stmt.values_list) {
        if (static_cast<int>(row.size()) != actual_cols) {
            AddError(SemanticErrorKind::ArityMismatch,
                "INSERT column count mismatch: got " +
                std::to_string(row.size()) + " expected " +
                std::to_string(actual_cols),
                stmt.line, stmt.column);
            ok = false;
        }
    }
    // 43_upsert: ON DUPLICATE KEY UPDATE 仅在 VALUES 路径下合法；
    // INSERT ... SELECT 不支持（候选行不可枚举）。
    if (stmt.has_on_duplicate) {
        if (stmt.query) {
            AddError(SemanticErrorKind::Other,
                     "ON DUPLICATE KEY UPDATE is not supported with INSERT ... SELECT",
                     stmt.line, stmt.column);
            ok = false;
        }
        if (stmt.upsert_assignments.empty()) {
            AddError(SemanticErrorKind::Other,
                     "ON DUPLICATE KEY UPDATE requires at least one assignment",
                     stmt.line, stmt.column);
            ok = false;
        }
        // 校验每个被赋值的列存在于目标表中；表达式 CheckExpressionMulti 允许
        // 引用目标表的列 + 解析 VALUES(col)。
        std::vector<std::pair<std::string, ExprPtr>> valid_assigns;
        for (const auto& kv : stmt.upsert_assignments) {
            ok &= CheckColumnExists(stmt.table_name, kv.first, stmt.line, stmt.column);
            // 表达式里允许 ColumnRefExpr（目标表现有列）+ UpsertValuesRefExpr
            // （VALUES(col)）+ 其它表达式；统一交给 CheckExpression 校验。
            ok &= CheckExpression(kv.second, stmt.table_name);
        }
    }
    // 54_dml: REPLACE INTO 不支持 INSERT ... SELECT 数据源；当前解析路径已仅
    // 允许 VALUES 形式（见 Parser.cpp 中 KEYWORD_REPLACE 分支）。
    if (stmt.is_replace && stmt.query) {
        AddError(SemanticErrorKind::Other,
                 "REPLACE INTO is not supported with INSERT ... SELECT",
                 stmt.line, stmt.column);
        ok = false;
    }
    // 54_dml: RETURNING 表达式校验 —— 在目标表上下文中求值；
    // 接受 ColumnRef / 函数 / 字面量等。
    for (const auto& e : stmt.returning_exprs) {
        ok &= CheckExpression(e, stmt.table_name);
    }
    return ok;
}

bool SemanticAnalyzer::AnalyzeUpdate(const UpdateStatement& stmt) {
    bool ok = CheckTableExists(stmt.table_name, stmt.line, stmt.column);
    for (auto& kv : stmt.assignments) {
        ok &= CheckColumnExists(stmt.table_name, kv.first, stmt.line, stmt.column);
        ok &= CheckExpression(kv.second, stmt.table_name);
    }
    if (stmt.where_clause) ok &= CheckExpression(stmt.where_clause, stmt.table_name);
    // 54_dml: UPDATE ... FROM —— 校验 from_sources 中的表存在；WHERE 中的列引用
    // 解析由 ExpressionEvaluator 在执行期通过 combined cmap 兜底。
    for (const auto& src : stmt.from_sources) {
        ok &= CheckTableExists(src.table_name, stmt.line, stmt.column);
    }
    // 54_dml: RETURNING 表达式校验
    for (const auto& e : stmt.returning_exprs) {
        ok &= CheckExpression(e, stmt.table_name);
    }
    return ok;
}

bool SemanticAnalyzer::AnalyzeDelete(const DeleteStatement& stmt) {
    bool ok = CheckTableExists(stmt.table_name, stmt.line, stmt.column);
    if (stmt.where_clause) ok &= CheckExpression(stmt.where_clause, stmt.table_name);
    // 54_dml: RETURNING 表达式校验
    for (const auto& e : stmt.returning_exprs) {
        ok &= CheckExpression(e, stmt.table_name);
    }
    return ok;
}

bool SemanticAnalyzer::AnalyzeMerge(const MergeStatement& stmt) {
    bool ok = true;
    ok &= CheckTableExists(stmt.target_table, stmt.line, stmt.column);
    // 收集源表信息（用于 ON / SET / INSERT 的跨表列解析）。
    TableInfo source_info_dummy;
    const TableInfo* source_info = nullptr;
    std::vector<std::string> tables_for_check;
    SemanticAnalyzer::TableAliasMap aliases;
    tables_for_check.push_back(stmt.target_table);
    aliases.push_back({stmt.target_table, stmt.target_table});
    if (!stmt.target_alias.empty() && stmt.target_alias != stmt.target_table) {
        aliases.push_back({stmt.target_alias, stmt.target_table});
    }
    if (stmt.source_query) {
        // 派生表：递归校验内层 SELECT。item #18: 用 AnalyzeInternal 代替
        // AnalyzeSelect —— 前者能正确处理嵌套的 SET_OP / WITH / 子 SELECT，
        // 后者只走 SELECT_STMT 单路径，遇到 UNION/INTERSECT/EXCEPT 形式的
        // 派生表会漏掉子节点校验。
        bool inner_ok = true;
        AnalyzeInternal(stmt.source_query, inner_ok);
        ok &= inner_ok;
        // 用派生表的 alias 作为 "表名" —— 但 catalog 找不到该别名。
        // 我们仍把 source_alias 加入 tables_for_check，并允许任意列引用通过
        // （即 CheckExpressionMulti 在该别名上不会校验列存在性）。
        tables_for_check.push_back(stmt.source_alias);
        aliases.push_back({stmt.source_alias, stmt.source_alias});
    } else if (!stmt.source_table.empty()) {
        ok &= CheckTableExists(stmt.source_table, stmt.line, stmt.column);
        source_info = symbol_table_.GetTable(stmt.source_table);
        tables_for_check.push_back(stmt.source_table);
        aliases.push_back({stmt.source_table, stmt.source_table});
        if (!stmt.source_alias.empty() && stmt.source_alias != stmt.source_table) {
            aliases.push_back({stmt.source_alias, stmt.source_table});
        }
    } else {
        AddError(SemanticErrorKind::Other,
                 "MERGE requires a source (table or subquery)",
                 stmt.line, stmt.column);
        ok = false;
    }
    // 跨表 ON / SET / VALUES：用 CheckExpressionMulti 在 (target + source) 集合上
    // 校验。target_alias / source_alias 作为额外别名。
    if (stmt.on_condition) {
        ok &= CheckExpressionMulti(stmt.on_condition, tables_for_check, aliases);
    }
    if (stmt.has_matched_update) {
        for (const auto& kv : stmt.matched_assignments) {
            ok &= CheckColumnExists(stmt.target_table, kv.first);
            ok &= CheckExpressionMulti(kv.second, tables_for_check, aliases);
        }
    }
    if (stmt.has_not_matched_insert) {
        for (const auto& cn : stmt.insert_columns) {
            ok &= CheckColumnExists(stmt.target_table, cn, stmt.line, stmt.column);
        }
        // INSERT VALUES 引用 source 列，必须走跨表校验。
        if (source_info || !stmt.source_query) {
            ok &= CheckExpressionMulti(stmt.insert_values.empty() ? nullptr : stmt.insert_values.front(),
                                       tables_for_check, aliases);
            for (size_t i = 0; i < stmt.insert_values.size(); ++i) {
                ok &= CheckExpressionMulti(stmt.insert_values[i], tables_for_check, aliases);
            }
        }
    }
    (void)source_info_dummy;
    return ok;
}

bool SemanticAnalyzer::AnalyzeCreateTable(const CreateTableStatement& stmt) {
    bool ok = true;
    if (symbol_table_.HasTable(stmt.table_name) && !stmt.if_not_exists) {
        AddError(SemanticErrorKind::DuplicateName,
                 "table already exists: " + stmt.table_name,
                 stmt.line, stmt.column);
        ok = false;
    }
    // 预先构建大小写不敏感的列名集合：用于 O(1) 重复列检测 + PRIMARY KEY
    // 列查找 + CHECK 表达式的列存在性校验。一次遍历建立，三个用途共享。
    std::unordered_set<std::string,
                       CaseInsensitiveHash, CaseInsensitiveEq> seen_columns;
    seen_columns.reserve(stmt.columns.size());
    // Check duplicate column names — O(N) via hash set
    for (size_t i = 0; i < stmt.columns.size(); ++i) {
        const auto& cn = stmt.columns[i].column_name;
        if (!seen_columns.emplace(cn).second) {
            AddError(SemanticErrorKind::DuplicateName,
                     "duplicate column name: " + cn,
                     stmt.line, stmt.column);
            ok = false;
        }
        // Validate type (支持 BIGINT/INTEGER/DOUBLE/DECIMAL/CHAR/TEXT/STRING 等全部归一化类型)
        std::string up;
        for (char c : stmt.columns[i].data_type)
            up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        if (up != "INT" && up != "INTEGER" && up != "BIGINT" &&
            up != "FLOAT" && up != "DOUBLE" && up != "DECIMAL" && up != "NUMERIC" &&
            up != "REAL" && up != "SMALLINT" && up != "TINYINT" &&
            up != "VARCHAR" && up != "CHAR" && up != "TEXT" && up != "STRING" &&
            up != "DATE" && up != "TIMESTAMP" && up != "TIME" &&
            up != "BOOLEAN" && up != "BOOL" &&
            up != "JSON" && up != "UUID") {
            AddError(SemanticErrorKind::TypeMismatch,
                     "unsupported column type: " + stmt.columns[i].data_type,
                     stmt.line, stmt.column);
            ok = false;
        }
    }
    // 表级 PRIMARY KEY(a, b, ...) 引用的列必须存在于列定义中（O(1) hash 查询）。
    for (const auto& pk : stmt.primary_keys) {
        for (const auto& pk_col : pk) {
            if (seen_columns.find(pk_col) == seen_columns.end()) {
                AddError(SemanticErrorKind::ColumnNotFound,
                         "PRIMARY KEY references unknown column: " + pk_col,
                         stmt.line, stmt.column);
                ok = false;
            }
        }
    }
    // 58_constraints: 表级 CHECK(expr) 与列级 CHECK(expr) 的列引用必须落
    // 在本表的列定义里。注意此刻 symbol_table_ 还没加入本表（语义分析在
    // catalog 注册之前跑），所以不能直接调用 CheckExpressionMulti；这里
    // 用本表的列名集合做就地校验。命名约束的名字仅用于错误消息展示，
    // 不做唯一性校验以减少阻断面。
    auto check_local_expr = [&](const ExprPtr& expr) -> bool {
        if (!expr) return true;
        std::function<bool(const ExprPtr&)> walk = [&](const ExprPtr& e) -> bool {
            if (!e) return true;
            switch (e->GetType()) {
                case NodeType::LITERAL_EXPR:
                    return true;
                case NodeType::COLUMN_REF_EXPR: {
                    auto cr = std::static_pointer_cast<ColumnRefExpr>(e);
                    if (seen_columns.find(cr->column_name) == seen_columns.end()) {
                        AddError(SemanticErrorKind::ColumnNotFound,
                                 "CHECK references unknown column: " +
                                 cr->column_name,
                                 e->line, e->column);
                        return false;
                    }
                    return true;
                }
                case NodeType::BINARY_EXPR: {
                    auto be = std::static_pointer_cast<BinaryExpr>(e);
                    return walk(be->left) && walk(be->right);
                }
                case NodeType::UNARY_EXPR: {
                    auto ue = std::static_pointer_cast<UnaryExpr>(e);
                    return walk(ue->operand);
                }
                case NodeType::FUNCTION_CALL_EXPR: {
                    auto fc = std::static_pointer_cast<FunctionCallExpr>(e);
                    bool r = true;
                    for (auto& a : fc->arguments) r &= walk(a);
                    return r;
                }
                default:
                    return true;
            }
        };
        return walk(expr);
    };
    for (const auto& tc : stmt.table_checks) {
        ok &= check_local_expr(tc.expr);
    }
    for (const auto& cd : stmt.columns) {
        ok &= check_local_expr(cd.check_expr);
    }
    return ok;
}

bool SemanticAnalyzer::AnalyzeDropTable(const DropTableStatement& stmt) {
    // IF EXISTS 时不存在也不算错误，交由执行阶段静默跳过
    if (stmt.if_exists) return true;
    return CheckTableExists(stmt.table_name, stmt.line, stmt.column);
}

bool SemanticAnalyzer::AnalyzeCreateIndex(const CreateIndexStatement& stmt) {
    if (!CheckTableExists(stmt.table_name, stmt.line, stmt.column)) return false;
    const TableInfo* table = symbol_table_.GetTable(stmt.table_name);
    if (table == nullptr) return false;
    if (stmt.key_columns.empty()) {
        AddError(SemanticErrorKind::Other,
                 "index must have at least one column",
                 stmt.line, stmt.column);
        return false;
    }
    bool ok = true;
    for (const auto& col : stmt.key_columns) {
        if (table->GetColumn(col) == nullptr) {
            AddError(SemanticErrorKind::ColumnNotFound,
                     "column not found: " + stmt.table_name + "." + col,
                     stmt.line, stmt.column);
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
    return CheckTableExists(stmt.table_name, stmt.line, stmt.column);
}

bool SemanticAnalyzer::AnalyzeAlterTable(const AlterStatement& stmt) {
    // ALTER TABLE 现在由 AlterTableExecutor 真正改写 schema，因此语义层
    // 必须做完整校验，避免「错误参数也能 ALTER 成功」导致目录与数据脱节。
    if (!CheckTableExists(stmt.table_name, stmt.line, stmt.column)) return false;
    const TableInfo* info = symbol_table_.GetTable(stmt.table_name);
    if (!info) return false;  // CheckTableExists 已报错。
    switch (stmt.action) {
        case AlterAction::ADD_COLUMN: {
            const auto& cd = stmt.column_def;
            if (!cd || cd->column_name.empty()) {
                AddError(SemanticErrorKind::Other,
                         "ALTER TABLE ADD COLUMN requires a column name",
                         stmt.line, stmt.column);
                return false;
            }
            if (info->HasColumn(cd->column_name)) {
                AddError(SemanticErrorKind::ColumnAlreadyExists,
                         "column already exists: " + cd->column_name,
                         stmt.line, stmt.column);
                return false;
            }
            return true;
        }
        case AlterAction::DROP_COLUMN: {
            if (!info->HasColumn(stmt.drop_column_name)) {
                AddError(SemanticErrorKind::ColumnNotFound,
                         "column not found: " + stmt.drop_column_name,
                         stmt.line, stmt.column);
                return false;
            }
            return true;
        }
        case AlterAction::RENAME_TO: {
            if (stmt.new_table_name.empty()) {
                AddError(SemanticErrorKind::Other,
                         "ALTER TABLE RENAME TO requires a new table name",
                         stmt.line, stmt.column);
                return false;
            }
            if (stmt.new_table_name == stmt.table_name) return true;
            if (symbol_table_.HasTable(stmt.new_table_name)) {
                AddError(SemanticErrorKind::DuplicateName,
                         "table already exists: " + stmt.new_table_name,
                         stmt.line, stmt.column);
                return false;
            }
            return true;
        }
        case AlterAction::MODIFY_COLUMN: {
            const auto& cd = stmt.column_def;
            if (!cd || cd->column_name.empty()) {
                AddError(SemanticErrorKind::Other,
                         "ALTER TABLE MODIFY COLUMN requires a column name",
                         stmt.line, stmt.column);
                return false;
            }
            if (!info->HasColumn(cd->column_name)) {
                AddError(SemanticErrorKind::ColumnNotFound,
                         "column not found: " + cd->column_name,
                         stmt.line, stmt.column);
                return false;
            }
            return true;
        }
        case AlterAction::RENAME_COLUMN: {
            // 53_ddl：RENAME COLUMN 在目录中校验「旧列存在 + 新列不存在」。
            if (stmt.rename_column_old_name.empty() ||
                stmt.rename_column_new_name.empty()) {
                AddError(SemanticErrorKind::Other,
                         "ALTER TABLE RENAME COLUMN requires both old and new column names",
                         stmt.line, stmt.column);
                return false;
            }
            if (!info->HasColumn(stmt.rename_column_old_name)) {
                AddError(SemanticErrorKind::ColumnNotFound,
                         "column not found: " + stmt.rename_column_old_name,
                         stmt.line, stmt.column);
                return false;
            }
            if (info->HasColumn(stmt.rename_column_new_name)) {
                AddError(SemanticErrorKind::ColumnAlreadyExists,
                         "column already exists: " + stmt.rename_column_new_name,
                         stmt.line, stmt.column);
                return false;
            }
            return true;
        }
    }
    return true;
}

bool SemanticAnalyzer::CheckTableExists(const std::string& table_name,
                                       int line, int column) {
    if (symbol_table_.HasTable(table_name)) return true;
    std::string base = "table not found: " + table_name;
    std::string hint = SuggestClosestName(table_name, symbol_table_.GetAllTableNames());
    AddError(SemanticErrorKind::TableNotFound,
             hint.empty() ? base : (base + " " + hint),
             line, column);
    return false;
}

bool SemanticAnalyzer::CheckColumnExists(const std::string& table_name,
                                         const std::string& column_name,
                                         int line, int column) {
    // table_name may be a comma-separated list of table names (for JOIN).
    // 在第一次循环里同时做"存在性检查 + 候选列收集"，避免错误路径上再走一遍。
    // 收集候选列名（用于「Did you mean」提示）。多个表时取并集。
    std::vector<std::string> column_candidates;
    // item #8: 用 string_view span 切片代替 substr，避免每张 JOIN 表分配一次
    // 临时 std::string。GetTable 需要 std::string&，我们在调用现场再构造一次
    // —— 但每次只构造一个 slice，而不是为整个逗号分隔字符串反复 substr。
    auto process_one = [&](std::string_view t) -> bool {
        std::string t_str(t);  // 仅在需要 GetTable 时构造一次
        const TableInfo* info = symbol_table_.GetTable(t_str);
        if (info != nullptr) {
            if (info->HasColumnFast(column_name)) return true;
            for (const auto& c : info->columns) {
                column_candidates.push_back(c.name);
            }
            return false;
        }
        // 60_view_trigger: 物化视图在 SELECT FROM mv / ORDER BY mv.col 场景下
        // 也应可被识别 —— Planner 会把 from_table 替换为 backing table，但语义
        // 校验仍按 mv 名走，所以这里放行 MV 的 columns。
        if (catalog_ != nullptr) {
            const SystemCatalog::MaterializedViewInfo* mv =
                catalog_->LookupMaterializedView(t_str);
            if (mv != nullptr) {
                // item #2: MV columns 来自 ColumnDefinition，没有 column_index_，
                // 暂时保留 O(M*C) 扫描但用 IEquals 替换 byte-equal 比较，确保大小写
                // 不敏感与 TableInfo 路径一致。MV column 数一般 < 30，开销可忽略；
                // 真要 O(1) 可在 MaterializedViewInfo 上加个 hash（item #2 后续）。
                for (const auto& c : mv->columns) {
                    if (IEquals(c.column_name, column_name)) return true;
                }
                for (const auto& c : mv->columns) {
                    column_candidates.push_back(c.column_name);
                }
            }
        }
        return false;
    };
    if (table_name.find(',') != std::string::npos) {
        std::string_view remaining(table_name);
        while (!remaining.empty()) {
            size_t comma = remaining.find(',');
            std::string_view t = (comma == std::string_view::npos)
                ? remaining
                : remaining.substr(0, comma);
            if (process_one(t)) return true;
            if (comma == std::string_view::npos) break;
            remaining.remove_prefix(comma + 1);
        }
        std::string base = "column not found: " + column_name;
        std::string hint = SuggestClosestName(column_name, column_candidates);
        AddError(SemanticErrorKind::ColumnNotFound,
                 hint.empty() ? base : (base + " " + hint),
                 line, column);
        return false;
    }
    if (process_one(table_name)) return true;
    std::string base = "column not found: " + table_name + "." + column_name;
    std::string hint = SuggestClosestName(column_name, column_candidates);
    AddError(SemanticErrorKind::ColumnNotFound,
             hint.empty() ? base : (base + " " + hint),
             line, column);
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
                    AddError(SemanticErrorKind::InvalidReference,
                             "column not found: " + cr->column_name,
                             cr->line, cr->column);
                    return false;
                }
                const TableInfo* info = symbol_table_.GetTable(resolved);
                if (info == nullptr || !info->HasColumnFast(cr->column_name)) {
                    AddError(SemanticErrorKind::ColumnNotFound,
                             "column not found: " + cr->column_name,
                             cr->line, cr->column);
                    return false;
                }
                return true;
            }
            // 未限定列：在所有真实表中查找（含物化视图），同时累加候选列名。
            // 错误路径上不再走第二次 walk（item #9）。
            std::vector<std::string> column_candidates;
            for (const auto& t : tables) {
                const TableInfo* info = symbol_table_.GetTable(t);
                if (info != nullptr) {
                    if (info->HasColumnFast(cr->column_name)) return true;
                    for (const auto& c : info->columns) {
                        column_candidates.push_back(c.name);
                    }
                    continue;
                }
                // 60_view_trigger: 物化视图列也作为合法引用源。
                if (catalog_ != nullptr) {
                    const SystemCatalog::MaterializedViewInfo* mv =
                        catalog_->LookupMaterializedView(t);
                    if (mv != nullptr) {
                        // item #2: 大小写不敏感与 TableInfo 路径对齐（见 CheckColumnExists）。
                        for (const auto& c : mv->columns) {
                            if (IEquals(c.column_name, cr->column_name)) return true;
                        }
                        for (const auto& c : mv->columns) {
                            column_candidates.push_back(c.column_name);
                        }
                    }
                }
            }
            std::string base = "column not found: " + cr->column_name;
            std::string hint = SuggestClosestName(cr->column_name, column_candidates);
            AddError(SemanticErrorKind::ColumnNotFound,
                     hint.empty() ? base : (base + " " + hint),
                     cr->line, cr->column);
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
                    AddError(SemanticErrorKind::InvalidReference,
                             "column not found: " + cr->column_name,
                             cr->line, cr->column);
                    return false;
                }
                const TableInfo* info = symbol_table_.GetTable(resolved);
                if (info == nullptr || !info->HasColumnFast(cr->column_name)) {
                    AddError(SemanticErrorKind::ColumnNotFound,
                             "column not found: " + cr->column_name,
                             cr->line, cr->column);
                    return false;
                }
                return true;
            }
            // 未限定列：先看是不是 SELECT 别名
            for (const auto& a : aliases) {
                if (a == cr->column_name) return true;
            }
            // 在真实表中查找（含物化视图），同时累加候选列名（错误路径用）。
            std::vector<std::string> column_candidates;
            for (const auto& t : tables) {
                const TableInfo* info = symbol_table_.GetTable(t);
                if (info != nullptr) {
                    if (info->HasColumnFast(cr->column_name)) return true;
                    for (const auto& c : info->columns) {
                        column_candidates.push_back(c.name);
                    }
                    continue;
                }
                // 60_view_trigger: 物化视图列同样可作为合法引用源。
                if (catalog_ != nullptr) {
                    const SystemCatalog::MaterializedViewInfo* mv =
                        catalog_->LookupMaterializedView(t);
                    if (mv != nullptr) {
                        // item #2: 大小写不敏感与 TableInfo 路径对齐。
                        for (const auto& c : mv->columns) {
                            if (IEquals(c.column_name, cr->column_name)) return true;
                        }
                        for (const auto& c : mv->columns) {
                            column_candidates.push_back(c.column_name);
                        }
                    }
                }
            }
            // 别名也加入候选列名集合。
            for (const auto& a : aliases) column_candidates.push_back(a);
            std::string base = "column not found: " + cr->column_name;
            std::string hint = SuggestClosestName(cr->column_name, column_candidates);
            AddError(SemanticErrorKind::ColumnNotFound,
                     hint.empty() ? base : (base + " " + hint),
                     cr->line, cr->column);
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

void SemanticAnalyzer::AddError(SemanticErrorKind kind,
                                const std::string& message,
                                int line,
                                int column,
                                ErrorStage stage) {
    SemanticError err;
    err.stage = stage;
    err.kind = kind;
    err.message = message;
    err.line = line;
    err.column = column;
    errors_.push_back(std::move(err));
}

// "Did you mean" suggestion helper: filters `candidates` by Levenshtein
// distance <= 2 from `bad_name`, sorts the survivors by (distance, name)
// ascending, and returns a hint phrase suitable to append to the original
// error. Returns "" when nothing qualifies so callers can simply check
// emptiness before concatenating.
//
// Distance threshold matches the prompt's spec; SQL identifiers are short
// so 2 is enough to absorb a single transposition or doubled letter without
// flooding the message with noise.
std::string SemanticAnalyzer::SuggestClosestName(
    const std::string& bad_name,
    const std::vector<std::string>& candidates) const {
    constexpr int kMaxDistance = 2;
    std::vector<std::pair<int, std::string>> scored;
    scored.reserve(candidates.size());
    const int bn_size = static_cast<int>(bad_name.size());
    for (const auto& c : candidates) {
        if (c.empty()) continue;
        // item #19：长度差距超过 kMaxDistance 时，编辑距离必然 >= kMaxDistance，
        // 提前 skip 整个 DP 过程，避免对每个候选都跑一次 O(L1*L2) 算法。
        if (std::abs(bn_size - static_cast<int>(c.size())) > kMaxDistance) continue;
        int d = LevenshteinDistance(bad_name, c);
        if (d <= kMaxDistance) scored.emplace_back(d, c);
    }
    if (scored.empty()) return std::string();
    std::sort(scored.begin(), scored.end(),
              [](const std::pair<int, std::string>& a,
                 const std::pair<int, std::string>& b) {
                  if (a.first != b.first) return a.first < b.first;
                  return a.second < b.second;
              });
    // item #10：hint 字符串构造时一次性 reserve，避免 += 链式多次触发 realloc。
    // 每个候选最多 14 字节（逗号 + 2 个引号 + 10 字符以内的典型名字），
    // 加上前缀 "Did you mean: " 与尾部 "?" 共 15 字节，给出充足余量。
    std::string out;
    out.reserve(15 + scored.size() * 14);
    out += "Did you mean: ";
    for (size_t i = 0; i < scored.size(); ++i) {
        if (i > 0) out += ", ";
        out += "'";
        out += scored[i].second;
        out += "'";
    }
    out += "?";
    return out;
}

}  // namespace sqlcompiler