// =============================================================================
// Phase D — in-memory undo log + nested savepoint stack.
//
// IMPORTANT: This file implements the in-memory undo log as a
// `std::vector<UndoRecord>`. The WAL hook is in TableHeap / BPlusTree
// (Phase B) plus TransactionManager's Rollback / RollbackToSavepoint
// (which writes CLR chains and SAVEPOINT_ROLLBACK records).
//
// All methods here are no-ops when called on a Transaction whose
// IsActive() == false (i.e. already committed or rolled back).
//
// Nested-savepoint semantics（嵌套保存点栈语义）：
//   savepoints_ 是一个 std::vector<Savepoint>，back() 是最近（最内层）的保存点。
//
//   * ROLLBACK TO name：
//       - 把 undo_log_ 截断到该保存点的 offset（更靠后的 undo 记录一并丢弃）；
//       - 把 savepoints_ 中「该名称的最内层条目及之后」所有 entry 弹出
//         （外层同名保存点继续保留）。
//       - TransactionManager 负责：①按逆序应用 [offset, total) 区间 undo，
//         每步写一条 CLR；②写一条 SAVEPOINT_ROLLBACK 信息性记录；③调用本类
//         的 TruncateUndoLog + PopSavepointStack 完成内存侧收尾。
//
//   * RELEASE name：
//       - 仅从栈中弹出该名称的最内层条目（不影响外层保存点）；
//       - 不动 undo_log_——保存点区间内的写入被「升级」为本事务顶层写入。
//       - TransactionManager 写一条 SAVEPOINT_RELEASE 信息性记录，不写 CLR。
//
//   * 同名保存点允许重复出现（用户错误）；所有匹配都从栈顶向栈底搜索，命中
//     第一个同名条目。
// =============================================================================

#include "txn/Transaction.h"

#include <cstring>
#include <utility>

namespace sqlcompiler {

Transaction::Transaction(int64_t txn_id) : txn_id_(txn_id), active_(true) {
}

void Transaction::AppendUndo(page_id_t pid, const char* page_data, size_t bytes,
                             const std::string& desc) {
    if (!active_) return;
    UndoRecord rec;
    rec.page_id = pid;
    rec.description = desc;
    rec.before_image.assign(page_data, page_data + bytes);
    rec.lsn = INVALID_LSN;  // Phase C：AppendRecord 返回后由调用方回填。
    undo_log_.push_back(std::move(rec));
}

void Transaction::SetLastUndoLSN(lsn_t lsn) {
    if (!active_ || undo_log_.empty()) return;
    undo_log_.back().lsn = lsn;
}

void Transaction::PushSavepoint(const std::string& name) {
    if (!active_) return;
    Savepoint sp;
    sp.name = name;
    sp.undo_log_offset = undo_log_.size();
    savepoints_.push_back(std::move(sp));
}

bool Transaction::PopSavepoint(const std::string& name) {
    for (auto it = savepoints_.rbegin(); it != savepoints_.rend(); ++it) {
        if (it->name == name) {
            // reverse_iterator 转回正向 iterator 再 erase。
            auto fwd = std::next(it).base();
            savepoints_.erase(fwd);
            return true;
        }
    }
    return false;
}

// Phase D：ROLLBACK TO name 时的栈收尾——弹出该名称的最内层条目以及其后
// （更内层）的所有条目。外层、靠前的同名条目保留。
// 不修改 undo_log_，由 TransactionManager 显式调用 TruncateUndoLog 完成。
bool Transaction::PopSavepointStack(const std::string& name) {
    for (auto it = savepoints_.rbegin(); it != savepoints_.rend(); ++it) {
        if (it->name != name) continue;
        auto fwd = std::next(it).base();
        savepoints_.erase(fwd, savepoints_.end());
        return true;
    }
    return false;
}

bool Transaction::RollbackToSavepoint(const std::string& name) {
    // 从栈顶往下找同名保存点。SQL 语义下，保存点按名称标识，可能存在同名
    // 嵌套；这里约定 ROLLBACK TO 命中「最近」的同名保存点。
    for (auto it = savepoints_.rbegin(); it != savepoints_.rend(); ++it) {
        if (it->name != name) continue;
        const size_t offset = it->undo_log_offset;
        // 删除栈顶到（含）该保存点的所有 entry：
        //   反向迭代器 it 转正向 = std::next(it).base()，erase 到 savepoints_.end()。
        auto fwd = std::next(it).base();
        savepoints_.erase(fwd, savepoints_.end());
        // 截断 undo log。Phase A 直接丢弃 offset 之后的所有记录——
        // Rollback 由调用方（TransactionExecutor）反向应用 offset 之前的记录。
        if (offset <= undo_log_.size()) {
            undo_log_.resize(offset);
        }
        return true;
    }
    return false;
}

bool Transaction::HasSavepoint(const std::string& name) const {
    for (const auto& sp : savepoints_) {
        if (sp.name == name) return true;
    }
    return false;
}

size_t Transaction::GetSavepointOffset(const std::string& name) const {
    for (auto it = savepoints_.rbegin(); it != savepoints_.rend(); ++it) {
        if (it->name == name) return it->undo_log_offset;
    }
    return 0;
}

void Transaction::TruncateUndoLog(size_t new_size) {
    if (new_size <= undo_log_.size()) {
        undo_log_.resize(new_size);
    }
}

}  // namespace sqlcompiler