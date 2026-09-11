#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "storage/BufferPoolManager.h"
#include "txn/LogRecord.h"  // Phase C：UndoRecord 需要 lsn_t

namespace sqlcompiler {

// 事务（Phase A：在内存中持有 undo log）。
//
// 设计要点：
//   * Phase A 不写 WAL：undo 记录保存在进程的 std::vector 中，崩溃即丢失。
//   * 写入流程：TableHeap / BPlusTree 在每次修改 page 前把「整页 before-image」
//     压进 txn->AppendUndo()。Rollback 时从尾部往前逐条把 page 恢复。
//   * COMMIT 不落 log，但要把 undo_log_ 清空、active_ 置 false，让后续 BEGIN
//     重新分配 txn。
//   * Phase C：每条 UndoRecord 携带其对应的 WAL UPDATE 记录的 LSN（由
//     TableHeap / BPlusTree 在 AppendRecord 返回后回填），让 Rollback 能
//     写出「undo_next_lsn」链接 CLR 链。
//
// 字段命名与 ARIES 经典约定对应：undo_log_ 相当于 ARIES 的「per-txn log chain」，
// savepoints_ 相当于「nested savepoint stack」。LSN 由 Phase B 引入。
class Transaction {
public:
    // 一条 undo 记录：覆盖 [page_id] 的整页 before-image。
    // 同一个 page 多次修改只在 undo_log_ 里留下最新一条就够了——Rollback 时
    // 我们总是恢复到最早的状态，所以一条 page 一份最早 before-image 即可。
    // 但为了实现简洁，Phase A 暂不过滤重复：每次 write 都 push 一条，
    // Rollback 按相反顺序应用；多写多恢复，最坏情况是把后面的恢复回去又恢复
    // 回来，幂等，最终状态仍正确。内存代价是日志放大，但教学规模可接受。
    struct UndoRecord {
        page_id_t page_id = INVALID_PAGE_ID;
        // 整页 before-image（PAGE_SIZE 字节）。即使 page 实际只改了几字节，
        // 也存全页：undo 应用走 memcpy，避免依赖「改动范围」的元信息。
        std::vector<char> before_image;
        // 可选：写入 undo 时给本记录的「人类可读描述」，仅用于调试。
        std::string description;
        // Phase C：该 undo 记录对应的 WAL UPDATE 记录 LSN。AppendUndo
        // 时暂时为 INVALID_LSN；TableHeap / BPlusTree 在 AppendRecord 返回
        // 后通过 SetLastUndoLSN() 回填，让 Rollback 能算出 undo_next_lsn。
        lsn_t lsn = INVALID_LSN;
    };

    // 一个保存点：记录 undo_log_ 在创建时的长度，便于 ROLLBACK TO 时截断。
    //
    // 嵌套保存点的栈式语义（Phase D）：
    //   * savepoints_ 是一个栈，back() 是最近（最内层）的保存点。
    //   * 同名保存点允许重复出现但属用户错误；语义上 ROLLBACK TO / RELEASE
    //     都只命中最近的一个同名条目（从栈顶向栈底搜索）。
    //   * ROLLBACK TO name：
    //       1) 把 undo_log_ 截断到该保存点记录的 offset；
    //       2) 把 savepoints_ 中「该条目及之后」的所有 entry 一并弹出
    //          （外层、靠前的保存点保留）；
    //       3) 不修改 undo_log_ 中 [0, offset) 的内容——它们仍是本事务
    //          「未被回滚」的写入。
    //   * RELEASE name：
    //       1) 仅从栈中弹出该名称的最内层条目；
    //       2) 不动 undo_log_——保存点区间内的所有写入被「升级」为本事务
    //          顶层写入。
    //   * 没有任何 SAVEPOINT_ROLLBACK / RELEASE 触发的 undo / CLR 写入：本类
    //     只维护 in-memory 栈；TransactionManager 负责落 WAL。
    struct Savepoint {
        std::string name;
        size_t undo_log_offset = 0;
    };

    explicit Transaction(int64_t txn_id);

    int64_t GetTxnId() const { return txn_id_; }
    bool IsActive() const { return active_; }
    void SetActive(bool a) { active_ = a; }

    // 把一条 undo 记录追加到日志末尾。
    void AppendUndo(page_id_t pid, const char* page_data, size_t bytes,
                    const std::string& desc);

    // Phase C：把最近追加的 undo 记录的 LSN 回填为 lsn。
    // TableHeap / BPlusTree 在 AppendRecord 返回后调用，让 Rollback 能按
    // 原始 UPDATE 的 LSN 排序、写出 undo_next_lsn。
    void SetLastUndoLSN(lsn_t lsn);

    // 取出 undo 日志，供 Rollback / RecoveryManager 反向应用。
    const std::vector<UndoRecord>& GetUndoLog() const { return undo_log_; }

    // ---- 保存点 ----
    // 压入一个新的保存点，记录当前 undo_log_ 长度。
    void PushSavepoint(const std::string& name);
    // 弹出（RELEASE）：仅从栈中移除，但不丢弃日志。
    // 返回 true 表示找到了同名保存点并已弹出。
    bool PopSavepoint(const std::string& name);
    // Phase D：ROLLBACK TO 语义——从栈中弹出该名称的最内层条目以及其后
    // （更内层）的所有条目。外层（更早）的同名条目继续保留。
    // 与 PopSavepoint 的区别：本方法不只弹一个，而是「matched 之后整段切掉」。
    // 返回 true 表示找到了同名保存点并已弹出。
    bool PopSavepointStack(const std::string& name);
    // 找到 ROLLBACK TO 目标保存点，把日志截断到该 offset，并从栈中移除。
    // 返回 true 表示找到了同名保存点。
    bool RollbackToSavepoint(const std::string& name);
    // 查询某名称的保存点是否在栈里。
    bool HasSavepoint(const std::string& name) const;

    // 标记事务已提交；调用后 IsActive() 返回 false，undo 日志可被释放。
    void MarkCommitted() { active_ = false; }
    void MarkAborted() { active_ = false; }

    // 查询最近同名保存点的 undo_log_offset；找不到返回 0。
    // TransactionManager 在 ROLLBACK TO 时用此值确定「应用逆序 undo 的区间」。
    size_t GetSavepointOffset(const std::string& name) const;

    // 截断 undo 日志到指定长度，供 TransactionManager 在应用完区间后调用。
    void TruncateUndoLog(size_t new_size);

private:
    int64_t txn_id_;
    bool active_ = true;
    std::vector<UndoRecord> undo_log_;
    // 保存点栈。栈顶即当前（最近）的保存点。
    std::vector<Savepoint> savepoints_;
};

}  // namespace sqlcompiler