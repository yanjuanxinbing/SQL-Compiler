#include "execution/IndexScanExecutor.h"

#include "execution/ConstraintChecker.h"
#include "execution/ExpressionEvaluator.h"

namespace sqlcompiler {

IndexScanExecutor::IndexScanExecutor(
    ExecutionContext* context, std::shared_ptr<IndexScanNode> node,
    std::unordered_map<std::string, size_t> column_index_map)
    : Executor(context), node_(std::move(node)),
      column_index_map_(std::move(column_index_map)) {
}

void IndexScanExecutor::Init() {
    SystemCatalog* catalog = context_->GetCatalog();
    table_heap_ = catalog->GetTableHeap(node_->table_name);
    tree_ = catalog->GetIndexTree(node_->index_name);
    const TableInfo* info = catalog->GetTable(node_->table_name);
    if (info != nullptr) column_types_ = BuildColumnTypes(*info);
    if (tree_ == nullptr) return;

    if (node_->low_key.empty()) {
        cursor_ = tree_->Begin();
    } else {
        cursor_ = tree_->LowerBound(IndexKey(node_->low_key));
    }
}

bool IndexScanExecutor::BeyondUpperBound(const IndexKey& key) const {
    if (node_->high_key.empty()) return false;
    const IndexKey high(node_->high_key);
    const int c = CompareKeyOnly(key, high);
    return node_->high_inclusive ? (c > 0) : (c >= 0);
}

bool IndexScanExecutor::Next(Tuple* tuple) {
    if (!cursor_ || table_heap_ == nullptr) return false;

    ExpressionEvaluator eval(column_index_map_);
    IndexKey key;
    RID rid;
    while (cursor_->Next(&key, &rid)) {
        // 下界是开区间时，LowerBound 会把等于下界的项也带出来，这里跳过
        if (!node_->low_key.empty() && !node_->low_inclusive) {
            if (CompareKeyOnly(key, IndexKey(node_->low_key)) == 0) continue;
        }
        if (BeyondUpperBound(key)) return false;

        Tuple t;
        // 回表。取不到说明索引项指向的记录已不存在——正常情况下不该发生
        // （删除路径会同步摘掉索引项），这里跳过而不是报错，避免一条陈旧索引项
        // 让整条查询失败。
        if (!table_heap_->GetTuple(rid, &t, column_types_)) continue;

        if (node_->residual_predicate) {
            Value v = eval.Evaluate(node_->residual_predicate, t);
            if (v.IsNull() || v.AsInt() == 0) continue;
        }
        if (tuple != nullptr) *tuple = std::move(t);
        return true;
    }
    return false;
}

}  // namespace sqlcompiler
