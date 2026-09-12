#include "execution/MaterializedViewExecutor.h"

#include "catalog/SystemCatalog.h"
#include "common/Error.h"
#include "execution/CreateTableExecutor.h"
#include "execution/ExecutionEngine.h"
#include "execution/InsertExecutor.h"
#include "execution/TruncateTableExecutor.h"
#include "storage_engine/TableHeap.h"

#include <cstring>
#include <unordered_map>
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

// 推断 SELECT 子计划输出列的类型（用来建 backing table）。
// V1 简化：先把子计划跑一遍"探针"获取首行的 ValueType；空集时回退为 VARCHAR。
std::vector<ColumnDefinition> InferColumns(ExecutorPtr& child) {
    std::vector<ColumnDefinition> cols;
    if (!child) return cols;
    child->Init();
    Tuple first;
    if (!child->Next(&first)) {
        // 空结果集时无法推断列类型；用 single VARCHAR 占位列。
        ColumnDefinition c;
        c.column_name = "col0";
        c.data_type = "VARCHAR";
        cols.push_back(c);
        return cols;
    }
    for (size_t i = 0; i < first.ColumnCount(); ++i) {
        ColumnDefinition c;
        c.column_name = "col" + std::to_string(i);
        const Value& v = first.GetValue(i);
        switch (v.GetType()) {
            case ValueType::INTEGER:
                c.data_type = "INT";
                break;
            case ValueType::FLOAT:
                c.data_type = "FLOAT";
                break;
            case ValueType::VARCHAR:
            default:
                c.data_type = "VARCHAR";
                c.char_length = 255;
                break;
        }
        cols.push_back(c);
    }
    return cols;
}

// 把子计划的全部行写入 backing table。失败抛错。
void BulkInsertIntoTable(ExecutionContext* ctx,
                         const std::string& table_name,
                         const std::vector<ColumnDefinition>& cols,
                         ExecutorPtr& child) {
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
    (void)cols;
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
        } else {
            auto* n = static_cast<const AlterMaterializedViewNode*>(plan_node_);
            view_name_ = n->view_name;
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
        // 推断 SELECT 输出列类型并建 backing table
        auto cols = InferColumns(child);
        std::string backing = SystemCatalog::MaterializedViewBackingTable(view_name_);
        // 如果已存在则跳过（IF NOT EXISTS）
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
        // REFRESH：truncate + 重新执行 SELECT
        std::string backing = SystemCatalog::MaterializedViewBackingTable(view_name_);
        if (catalog->HasTable(backing)) {
            catalog->TruncateTable(backing);
        }
        // 重新推断列（schema 假定不变；V1 简化）
        BulkInsertIntoTable(context_, backing, {}, child);
    }
}

bool MaterializedViewExecutor::Next(Tuple* tuple) {
    (void)tuple;
    // 物化视图执行是 Init 一次性副作用；Next 永远没有结果。
    return false;
}

}  // namespace sqlcompiler
