#pragma once

// =============================================================================
// LockManager —— 事务级 S/X 锁管理器（T2「锁粒度到事务 + 死锁处理」）
// =============================================================================
// 职责：
//   * 以任意「资源 id」（例：表堆首页 page_id 作为表锁、RID 编码作为行锁）为键，
//     提供共享锁（S，可叠加）与独占锁（X，互斥）的获取/释放；
//   * 锁按 txn_id 持有到事务级：UnlockAll(txn_id) 一次性释放某个事务的全部锁，
//     供 Commit / Rollback 时调用；
//   * 死锁检测：维护「等待图」（waits_on_：每事务阻塞等谁），每次新增阻塞边后做
//     DFS 找环；检测到环立即返回 kDeadlock（由调用方中止该事务，即 victim），
//     不实际阻塞，避免活锁/无限等待；
//   * 可选等待超时：locker 传入阻塞等待毫秒数，超时返回 kTimeout。
//
// 与既有的两层锁的关系：
//   * BufferPoolManager latch_ 与 PageReadGuard/PageWriteGuard 是「短暂」的物理
//     锁（单次帧访问），锁序约定「持页锁期间不调 BPM」；
//   * 本 LockManager 提供「事务长」的逻辑锁（S/X 兼容矩阵 + 等待图死锁回收），
//     粒度独立于页，供显式事务在 DML 算子边界声明访问集合并持有到 End。
//
// 线程安全：单实例内部一把 mutex_ 保护锁表、等待图与条件变量；锁授予对并发调用
// 线程安全。
// =============================================================================

#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "index/IndexKey.h"

namespace sqlcompiler {

// 锁模式：仅支持 S（共享）与 X（独占）。S 与 S 兼容，其余冲突。
enum class LockMode { kShared, kExclusive };

// 锁请求结果。
enum class LockResult {
    kGranted,      // 已授予
    kWouldBlock,   // 尝试模式（TryLock*）：当前冲突，未授予
    kDeadlock,     // 加入本次等待会形成等待环，作为 victim 返回（调用方应中止）
    kTimeout,      // 阻塞等待超时，未授予
};

// 行级锁资源 id 编码：页号进高 32 位、槽位进低 32 位，并置符号位标记「行锁」
// 命名空间。表锁 id = 表堆首页页号（非负），行锁 id 恒为负，两类 id 数值永不相交，
// 避免「某行 (page=0,slot=7) 与某表首页 7」这类数值撞车导致的伪冲突。
inline int64_t RowResourceId(int64_t page_id, int slot_num) {
    return ((int64_t)page_id << 32) | (int64_t)(uint32_t)slot_num | (1LL << 63);
}

class LockManager {
public:
    LockManager() = default;
    ~LockManager() = default;
    LockManager(const LockManager&) = delete;
    LockManager& operator=(const LockManager&) = delete;

    // 共享锁：当前无 X 持有者即可叠加授予。阻塞：等待毫秒数由 wait_ms 决定
    // （0 = 无限）。调用方需是 txn_id 对应事务。
    LockResult LockShared(int64_t txn_id, int64_t res_id, int wait_ms = 0);

    // 独占锁：当前无 任何 持有者（S 或 X）才可授予。
    LockResult LockExclusive(int64_t txn_id, int64_t res_id, int wait_ms = 0);

    // 非阻塞尝试版：冲突立即返回 kWouldBlock。用于单线程断言冲突/死锁场景。
    LockResult TryLockShared(int64_t txn_id, int64_t res_id);
    LockResult TryLockExclusive(int64_t txn_id, int64_t res_id);

    // 释放某个事务在指定资源上的锁，并把自己持有的等待图边清除；可唤醒等待者。
    void Unlock(int64_t txn_id, int64_t res_id);

    // 释放某个事务的全部锁（Commit / Rollback 时调用）。
    void UnlockAll(int64_t txn_id);

    // ---- SERIALIZABLE 谓词锁（防幻读）----
    // 谓词作用在主键键空间，隶属某张表（table_rid = 表堆首页页号）。
    // AcquireReadPredicate：SERIALIZABLE 范围/全扫描在读前注册谓词；多个读谓词
    //   共享共存，持有到提交（UnlockAll 一并释放）。不在此处判冲突。
    // CheckWritePredicate：SERIALIZABLE 写（插/删/改命中该范围的键）前调用；
    //   若其他活动事务持谓词覆盖该键则阻塞（或死锁/超时中止），防幻读。
    LockResult AcquireReadPredicate(int64_t txn_id, int64_t table_rid,
                                    bool is_full, const IndexKey& lo, const IndexKey& hi);
    LockResult CheckWritePredicate(int64_t txn_id, int64_t table_rid,
                                   const IndexKey& key, int wait_ms = 0);

    // 断言查询：事务 txn_id 当前是否持有 res_id 上的锁（不区分模式）。
    bool IsLockHeld(int64_t txn_id, int64_t res_id) const;

private:
    // 一份资源上的锁状态：持有者集合（模式）+ 等待者队列（模式）。
    struct LockState {
        std::unordered_map<int64_t, LockMode> holders;
        std::vector<int64_t> order;       // 持有者申请顺序（决定公平唤醒用，简化为忽略）
        std::vector<std::pair<int64_t, LockMode>> waiters;
    };

    LockResult Acquire(int64_t txn_id, int64_t res_id, LockMode mode,
                       int wait_ms, bool block_try);
    // 不持锁的前置判定：当前持有者中是否存在与 mode 冲突的事务（排除 txn_id 自身）。
    bool Conflicts(const LockState& st, int64_t txn_id, LockMode mode) const;
    bool ModeCompatible(LockMode a, LockMode b) const;
    // 把 txn 加入对 res 持有者集合中「冲突」事务的等待图边。
    void LinkWaitEdges(int64_t txn_id, int64_t res_id, LockMode mode);
    void UnlinkWaitEdges(int64_t txn_id);
    // 从 txn 出发，沿 waits_on_ 做 DFS；若能回到 txn 则说明形成死锁环。
    bool DeadlockCycle(int64_t txn_id) const;
    bool Dfs(int64_t cur, const int64_t start, std::unordered_set<int64_t>& onpath,
             std::unordered_set<int64_t>& visited) const;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<int64_t, LockState> locks_;          // res_id -> LockState
    std::unordered_map<int64_t, std::unordered_set<int64_t>> waits_on_;  // 等待图

    // SERIALIZABLE 谓词锁：某事务在某表主键键空间上的一条（可能全表）读谓词，
    // 持有到提交。is_full=false 时区间为 [lo, hi]（含端点）。
    struct PredicateLock {
        int64_t txn_id;
        int64_t table_rid;
        bool is_full;
        IndexKey lo;
        IndexKey hi;
    };
    std::vector<PredicateLock> pred_locks_;

    // 谓词 p 是否覆盖键 key（is_full 恒覆盖）。
    bool PredicateCovers(const PredicateLock& p, const IndexKey& key) const;
};

}  // namespace sqlcompiler