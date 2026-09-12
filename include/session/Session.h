#pragma once

// =============================================================================
// Session —— 数据库会话抽象（T2 多连接并发事务：第一章）
// =============================================================================
// 在单连接（CLI 顺序执行）模型上引入「会话」边界：每个 Session 持有自己
// 独立的 TransactionManager，从而拥有独立的「当前事务 / 嵌套深度」状态。
// 多个会话可各自 Begin/Insert/Commit 而互不干扰；WAL/恢复的一致性由它们
// 共享的 TxnIdSequencer（全局唯一 txn_id）、BufferPoolManager（D7 全局锁）、
// LogManager（自身 mutex）共同保证。
//
// 设计取舍：
//   会话只持有「事务状态」这一个独立维度；缓冲池页缓存、WAL、系统目录仍是
//   全库共享的单例。这样并发化的侵入面最小，正确性聚焦在「每事务原子性 +
//   全局唯一 txn_id」上，而不尝试做隔离级别/锁等待（那是 T2 后续里程碑）。
//   因此两个会话并发写同一页时，仍由缓冲池全局锁精确串行化帧访问，结果
//   与顺序执行等价（正确性优先于吞吐）。
// =============================================================================

#include <memory>

#include "txn/TransactionManager.h"

namespace sqlcompiler {

class Session {
public:
    // 由 Database 创建：接管一个已接好共享资源 + 共享 txn_id 序列器的
    // TransactionManager。本类拥有其所有权。
    explicit Session(std::unique_ptr<TransactionManager> txn_manager)
        : txn_manager_(std::move(txn_manager)) {}
    ~Session() = default;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    TransactionManager* GetTransactionManager() const { return txn_manager_.get(); }

private:
    std::unique_ptr<TransactionManager> txn_manager_;
};

}  // namespace sqlcompiler