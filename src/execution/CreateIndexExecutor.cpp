#include "execution/CreateIndexExecutor.h"

#include "common/Error.h"
#include "execution/ConstraintChecker.h"
#include "execution/IndexMaintenance.h"
#include "index/BPlusTree.h"

namespace sqlcompiler {

CreateIndexExecutor::CreateIndexExecutor(ExecutionContext* context,
                                         std::string index_name,
                                         std::string table_name,
                                         std::vector<std::string> key_columns,
                                         bool is_unique)
    : Executor(context), index_name_(std::move(index_name)),
      table_name_(std::move(table_name)), key_columns_(std::move(key_columns)),
      is_unique_(is_unique), executed_(false) {
}

void CreateIndexExecutor::Init() {
    if (executed_) return;
    SystemCatalog* catalog = context_->GetCatalog();

    IndexInfo info;
    info.index_name = index_name_;
    info.table_name = table_name_;
    info.key_columns = key_columns_;
    info.is_unique = is_unique_;

    // Phase B：让 catalog 内部 sys_indexes 写入带上当前事务。
    catalog->SetActiveTransaction(context_->GetTransaction());
    std::string error;
    if (!catalog->CreateIndex(info, &error)) {
        catalog->SetActiveTransaction(nullptr);
        throw CompilerException(ErrorStage::SEMANTIC, error);
    }

    // ---- 回填已有数据 ----
    const TableInfo* table = catalog->GetTable(table_name_);
    TableHeap* heap = catalog->GetTableHeap(table_name_);
    const IndexInfo* created = catalog->GetIndex(index_name_);
    BPlusTree* tree = catalog->GetIndexTree(index_name_);
    if (table == nullptr || heap == nullptr || created == nullptr || tree == nullptr) {
        catalog->SetActiveTransaction(nullptr);
        return;
    }

    const std::vector<ValueType> col_types = BuildColumnTypes(*table);
    auto iter = heap->Begin();
    while (iter.HasNext()) {
        Tuple t = iter.Next(col_types);
        if (t.ColumnCount() != table->columns.size()) continue;
        IndexKey key;
        if (!BuildIndexKeyFromRow(*table, *created, t.GetValues(), &key)) {
            // 已有数据里存在 NULL 或缺列，无法建立完整索引。回滚掉刚建的索引，
            // 否则会留下一个「看起来存在但内容不全」的索引——那比没有索引更危险，
            // 因为查询会信任它并漏掉数据。
            catalog->SetActiveTransaction(context_->GetTransaction());
            catalog->DropIndex(index_name_);
            catalog->SetActiveTransaction(nullptr);
            throw CompilerException(
                ErrorStage::SEMANTIC,
                "cannot build index '" + index_name_ +
                    "': existing rows contain NULL in the indexed columns");
        }
        // Insert 也走 active_txn_ 路径，但这里是用户表 + 用户索引，回填期间
        // 索引本身的 WAL 由 catalog->SetActiveTransaction 推过来的 txn 决定。
        if (!tree->Insert(key, t.GetRid())) {
            catalog->SetActiveTransaction(context_->GetTransaction());
            catalog->DropIndex(index_name_);
            catalog->SetActiveTransaction(nullptr);
            throw CompilerException(
                ErrorStage::SEMANTIC,
                "cannot build unique index '" + index_name_ +
                    "': existing rows contain duplicate keys");
        }
    }
    catalog->SetActiveTransaction(nullptr);
    executed_ = true;
}

bool CreateIndexExecutor::Next(Tuple* tuple) {
    (void)tuple;
    return false;
}

}  // namespace sqlcompiler
