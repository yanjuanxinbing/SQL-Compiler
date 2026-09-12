#include "execution/MaterializedViewExecutor.h"

#include "catalog/SystemCatalog.h"
#include "common/Error.h"
#include "execution/ExecutionEngine.h"
#include "storage_engine/TableHeap.h"

#include <cctype>
#include <vector>

namespace sqlcompiler {

namespace {

// 从 ExecutionContext 拿到 catalog 与事务上下文，构造一个与子执行器配套的
// ExecutorPtr。走 ExecutionEngine::BuildExecutor 复用所有执行路径。
ExecutorPtr BuildChildExecutor(ExecutionContext* ctx,
                               const PlanNodePtr& plan) {
    if (!ctx || !plan) return nullptr;
    ExecutionEngine engine(ctx->GetCatalog());
    return engine.BuildExecutor(plan, ctx);
}

// 把子计划的全部行写入 backing table。失败抛错。
//
// schema 必须是 catalog 中 backing table 的列类型顺序（来自
// CreateMaterializedViewNode::columns），不能直接用 child tuple 的运行时
// 类型 —— 否则空表 MV 拿不到任何一行时无法启动。
void BulkInsertIntoTable(ExecutionContext* ctx,
                         const std::string& table_name,
                         const std::vector<ColumnDefinition>& cols,
                         ExecutorPtr& child) {
    (void)cols;
    if (!ctx || !child) return;
    TableHeap* heap = ctx->GetCatalog()->GetTableHeap(table_name);
    if (!heap) {
        throw CompilerException(ErrorStage::SEMANTIC,
            "backing table missing: " + table_name);
    }
    const TableInfo* info = ctx->GetCatalog()->GetTable(table_name);
    if (!info) return;

    std::vector<ValueType> col_types;
    col_types.reserve(info->columns.size());
    for (const auto& c : info->columns) {
        col_types.push_back(ValueTypeFromString(c.data_type));
    }

    child->Init();
    Tuple t;
    while (child->Next(&t)) {
        std::vector<Value> row;
        row.reserve(t.ColumnCount());
        for (size_t i = 0; i < t.ColumnCount(); ++i) {
            row.push_back(t.GetValue(i));
        }
        // 长度对齐
        if (row.size() < info->columns.size()) {
            row.resize(info->columns.size(), Value::MakeNull());
        }
        Tuple row_t(std::move(row));
        RID rid;
        heap->SetActiveTransaction(ctx->GetTransaction());
        if (!heap->InsertTuple(row_t, &rid, col_types)) {
            heap->SetActiveTransaction(nullptr);
            throw CompilerException(ErrorStage::SEMANTIC,
                "INSERT into materialized view failed (no space?)");
        }
        heap->SetActiveTransaction(nullptr);
    }
}

// 比较两组 ColumnDefinition 是否「列名 + 类型」一一对应。
// 注意：忽略 char_length 的差异 —— 同一 VARCHAR(50) 与 VARCHAR(255) 视为同一
// 类型；只在「用户实际换列」时报 drift，避免误伤。
bool ColumnsMatch(const std::vector<ColumnDefinition>& a,
                  const std::vector<ColumnDefinition>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].column_name != b[i].column_name) return false;
        // 大小写不敏感比较类型字符串（与 ValueTypeFromString 一致）。
        std::string ua = a[i].data_type;
        std::string ub = b[i].data_type;
        for (auto& c : ua) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        for (auto& c : ub) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (ua != ub) return false;
    }
    return true;
}

// 格式化「schema drift」错误信息，附带旧 vs 新列差异，便于用户排查。
std::string FormatDriftMessage(const std::string& view_name,
                               const std::vector<ColumnDefinition>& old_cols,
                               const std::vector<ColumnDefinition>& new_cols) {
    std::string out = "schema drift detected for materialized view '" +
                      view_name + "':\n  existing columns: [";
    for (size_t i = 0; i < old_cols.size(); ++i) {
        if (i) out += ", ";
        out += old_cols[i].column_name + ":" + old_cols[i].data_type;
    }
    out += "]\n  refreshed columns: [";
    for (size_t i = 0; i < new_cols.size(); ++i) {
        if (i) out += ", ";
        out += new_cols[i].column_name + ":" + new_cols[i].data_type;
    }
    out += "]\n  hint: DROP MATERIALIZED VIEW " + view_name +
           " then CREATE MATERIALIZED VIEW ... AS ... to apply the new schema.";
    return out;
}

}  // namespace

MaterializedViewExecutor::MaterializedViewExecutor(ExecutionContext* context,
                                                   Kind kind,
                                                   const PlanNode* plan_node)
    : Executor(context), kind_(kind), plan_node_(plan_node) {
    if (plan_node_) {
        if (kind_ == Kind::CREATE) {
            auto* n = static_cast<const CreateMaterializedViewNode*>(plan_node_);
            view_name_ = n->view_name;
            // Planner 通过 InferSelectOutputSchema 静态产出输出 schema —— 不依赖
            // 执行期求值，因此空表 MV 也能正确建表。
            cols_ = n->columns;
        } else {
            auto* n = static_cast<const AlterMaterializedViewNode*>(plan_node_);
            view_name_ = n->view_name;
            cols_ = n->columns;
        }
    }
}

void MaterializedViewExecutor::Init() {
    if (done_) return;
    done_ = true;
    SystemCatalog* catalog = context_->GetCatalog();
    if (catalog == nullptr || plan_node_ == nullptr) return;

    PlanNodePtr child_plan = plan_node_->children.empty()
                                ? nullptr
                                : plan_node_->children[0];
    if (!child_plan) {
        throw CompilerException(ErrorStage::SEMANTIC,
            "materialized view " + view_name_ +
                " missing child SELECT plan");
    }
    ExecutorPtr child = BuildChildExecutor(context_, child_plan);
    if (!child) {
        throw CompilerException(ErrorStage::SEMANTIC,
            "failed to build child executor for materialized view " + view_name_);
    }

    if (kind_ == Kind::CREATE) {
        // Planner 已经把输出列定型写到 node->columns；若 Planner 漏写（极少见，
        // 比如 SELECT 子句根本不存在），用兜底的单 VARCHAR 列以保留向后兼容。
        std::vector<ColumnDefinition> cols = cols_;
        if (cols.empty()) {
            ColumnDefinition c;
            c.column_name = "col0";
            c.data_type = "VARCHAR";
            c.char_length = 255;
            cols.push_back(c);
        }
        std::string backing = SystemCatalog::MaterializedViewBackingTable(view_name_);
        // 如果已存在则跳过（IF NOT EXISTS）。
        if (!catalog->HasTable(backing)) {
            TableInfo info;
            info.table_name = backing;
            // 把 ColumnDefinition 映射到 ColumnInfo（catalog 接受这种结构）。
            for (const auto& c : cols) {
                ColumnInfo ci;
                ci.name = c.column_name;
                ci.data_type = c.data_type;
                ci.char_length = c.char_length;
                ci.is_primary_key = c.is_primary_key;
                ci.is_not_null = c.is_not_null;
                ci.is_unique = c.is_unique;
                info.columns.push_back(std::move(ci));
            }
            if (!catalog->CreateTable(info)) {
                throw CompilerException(ErrorStage::SEMANTIC,
                    "failed to create backing table: " + backing);
            }
        }
        // 注册 MaterializedViewInfo（带 query_text 用于 REFRESH 重解析）。
        // 注意：query_text 由 Planner::PlanCreateMaterializedView 在规划阶段
        // 写入（原始 SELECT 文本）。若 catalog 中已有此视图（Planner 已注册），
        // 不要覆盖 query_text 以免 REFRESH 时无法重新解析。
        SystemCatalog::MaterializedViewInfo mv;
        mv.view_name = view_name_;
        mv.backing_table = backing;
        mv.columns = cols;
        if (!catalog->HasMaterializedView(view_name_)) {
            mv.query_text = child_plan ? child_plan->ToString() : "";
            catalog->CreateMaterializedView(mv);
        }
        // 物化数据
        BulkInsertIntoTable(context_, backing, cols, child);
    } else {
        // REFRESH：truncate + 重新执行 SELECT。
        //   1) Schema drift 检查：把 planner 重新推断出的 columns 与 catalog 中
        //      已存的 MaterializedViewInfo.columns 对比；不一致则报错，让用户
        //      drop + create（与 PostgreSQL 行为一致）。
        const SystemCatalog::MaterializedViewInfo* info =
            catalog->GetMaterializedView(view_name_);
        if (info != nullptr && !info->columns.empty() && !cols_.empty() &&
            !ColumnsMatch(info->columns, cols_)) {
            throw CompilerException(ErrorStage::SEMANTIC,
                FormatDriftMessage(view_name_, info->columns, cols_));
        }
        std::string backing = SystemCatalog::MaterializedViewBackingTable(view_name_);
        if (catalog->HasTable(backing)) {
            catalog->TruncateTable(backing);
        }
        // 重新物化（schema 不变，由 drift 检查保证）。
        BulkInsertIntoTable(context_, backing, cols_, child);
    }
}

bool MaterializedViewExecutor::Next(Tuple* tuple) {
    (void)tuple;
    // 物化视图执行是 Init 一次性副作用；Next 永远没有结果。
    return false;
}

}  // namespace sqlcompiler