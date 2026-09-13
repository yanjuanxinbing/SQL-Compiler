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
    //   【区间继承与合并（v2）】同事务同表上注册的谓词做集合规约：
    //     * 父谓词继承：已持全表谓词（is_full）或某区间完整覆盖新区间时，
    //       新区间被父谓词覆盖，直接丢弃（子谓词继承父谓词的防幻读能力）。
    //     * 区间合并：新区间与已有区间重叠时，合并为二者的并集（一个更宽的
    //       区间），覆盖能力是原有并集的上近似（更保守，不破坏防幻读正确性），
    //       同时把谓词条目数从 N 收敛到不重叠的最小区间数。
    //     * 全表化收敛：新谓词为 is_full 时，删除本事务本表全部区间谓词，
    //       只保留全表谓词（父谓词统一覆盖）。
    //   收益：长事务多次范围扫描不再线性累积谓词条目（内存），CheckWritePredicate
    //   的冲突扫描量随之下降（延迟）。
    LockResult AcquireReadPredicate(int64_t txn_id, int64_t table_rid,
                                    bool is_full, const IndexKey& lo, const IndexKey& hi);
    LockResult CheckWritePredicate(int64_t txn_id, int64_t table_rid,
                                   const IndexKey& key, int wait_ms = 0);

    // 断言查询：事务 txn_id 当前是否持有 res_id 上的锁（不区分模式）。
    bool IsLockHeld(int64_t txn_id, int64_t res_id) const;

    // ---- 行级锁升级（v2）：把一张表上的大量行锁合并为表级锁 ----
    // 背景：长事务（如无 WHERE 的 UPDATE/DELETE、大批量 INSERT）对同一张表逐行
    // 取锁，行锁条目数与行数成正比：locks_ 表膨胀、UnlockAll 释放慢、死锁检测
    // 等待图边长。升级把「该表已持的行锁集合」规约为一条表级锁，锁条目数从 O(行)
    // 收敛到 O(表)。
    //
    // 层次关系（多粒度锁）：行锁（res < 0）注册到其所属表（res = 表堆首页页号，
    // 非负）。冲突矩阵扩展为：
    //   * 表级 X 锁 与 本表任意行锁（S/X）冲突；
    //   * 表级 S 锁 与 本表行 X 锁冲突（行 S 锁兼容）；
    //   * 行 S/X 锁 与 本表表级 X 锁冲突；行 X 锁还与表级 S 锁冲突。
    // 这样升级后表锁与其他事务仍在行粒度的访问互斥，语义与逐行持锁一致。
    //
    // 使用方式（由 ExecutionContext 驱动）：
    //   1. 每次行锁授予后调用 RegisterRowGroup(row_res, table_res) 登记归属；
    //   2. 某表行锁计数超过阈值时调用 TryEscalateTable(txn, table_res, mode)；
    //   3. 升级成功（返回 kGranted）后，该表后续行访问由表锁覆盖（无需逐行取锁），
    //      表锁随 UnlockAll 在 Commit/Rollback 时释放。
    //   4. 升级失败（kWouldBlock：他事务持冲突行锁/表锁）→ 回退为继续逐行持锁，
    //      正确性不受影响（只是不享受收敛收益）。
    // 登记行锁归属（res 为行锁 id，table_res 为表资源 id，非负）。
    void RegisterRowGroup(int64_t row_res, int64_t table_res);
    // 尝试把 txn 在 table_res 上的行锁升级为表级 mode 锁（非阻塞 TryLock 语义）：
    // 成功即已释放本事务全部该表行锁、仅持表锁。若 txn 已持该表表锁则直接成功。
    LockResult TryEscalateTable(int64_t txn_id, int64_t table_res, LockMode mode);
    // 查询 txn 是否已对 table_res 升级为表级锁（用于后续行访问直接放行）。
    bool IsTableEscalated(int64_t txn_id, int64_t table_res) const;
    // 查询 txn 在某表（table_res）当前持有的行锁数量（供升级阈值判断）。
    size_t CountRowLocks(int64_t txn_id, int64_t table_res) const;
    // 查询 txn 在 table_rid 上注册的谓词锁条目数（供区间继承/合并的收敛验证）。
    size_t CountPredicateLocks(int64_t txn_id, int64_t table_rid) const;

    // ---- Phase 4（创新特性 E）：谓词锁区间树观测接口 ----
    // 某表当前谓词总数（全表哨兵 + 区间条目），供区间树规模验证。
    size_t CountTotalPredicates(int64_t table_rid) const;
    // CheckWritePredicate 自上次重置以来累计执行的「键比较」次数。
    // 用于验证区间树 O(log P) 命中：1 万条目下未覆盖键的检查次数远小于 P。
    size_t GetPredicateQueryComparisons() const;
    void ResetPredicateQueryComparisons();

    // ---- 自适应锁升级阈值（v3）----
    // 固定阈值（128 行）对所有表一刀切并不合适：小表（几十行）批量写时行锁条目数
    // 已占表的大半，应提前升级收敛；大表（上万行）128 行远未覆盖足够比例，过早升级
    // 反而放大表级互斥。自适应以「表已登记行数」（表规模近似）与「历史升级冲突采样」
    // 动态求阈值：
    //   * 小表（登记行 < 256）→ 提前升级，阈值 ≈ 登记行数/2（下限 8）；
    //   * 大表（登记行 >= 4096）→ 延后升级，每 4096 行在基准 128 上再上调 64；
    //   * 冲突采样 → 升级曾因他人持冲突锁而失败（kWouldBlock）的次数越多，阈值越低
    //     （更早再次尝试升级，冲突一消解立即收敛为表锁）。
    static size_t ComputeEscalationThreshold(size_t registered_rows,
                                             size_t conflict_count);
    // 该表已登记（记录过归属）的行资源总数（表规模近似）。
    size_t GetRegisteredRowCount(int64_t table_res) const;
    // 该表累计的升级冲突次数（TryEscalateTable 返回 kWouldBlock 的次数）。
    size_t GetTableConflictCount(int64_t table_res) const;

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
    // 多粒度层级冲突：行锁（res<0）检查其所属表的表级锁持有者；表锁（res>=0）
    // 检查本表下所有行锁的持有者。无登记归属的行锁/未知表不参与层级冲突。
    bool HierarchyConflicts(int64_t txn_id, int64_t res_id, LockMode mode) const;
    // 把 txn 加入对 res 持有者集合中「冲突」事务的等待图边（含层级冲突的边）。
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

    // ---- 行级锁升级的层次映射 ----
    // row_group_：行锁 res（<0）→ 所属表资源 id（非负）。由 ExecutionContext 在
    // 行锁授予后登记；无登记的行锁不参与层级冲突（兼容裸 LockManager 用法）。
    std::unordered_map<int64_t, int64_t> row_group_;
    // table_rows_：表资源 id（非负）→ 该表下已登记的行锁集合（反查索引，供表级
    // 锁请求时检查本表所有行锁持有者）。
    std::unordered_map<int64_t, std::unordered_set<int64_t>> table_rows_;
    // escalated_tables_：txn_id → 已升级为表级锁的表集合（升级成功标记）。
    std::unordered_map<int64_t, std::unordered_set<int64_t>> escalated_tables_;
    // table_escalation_conflicts_：表资源 id → 该表累计的升级冲突次数
    // （TryEscalateTable 因他人持冲突行/表锁返回 kWouldBlock 的次数）。
    std::unordered_map<int64_t, size_t> table_escalation_conflicts_;

    // SERIALIZABLE 谓词锁：某事务在某表主键键空间上的一条（可能全表）读谓词，
    // 持有到提交。is_full=false 时区间为 [lo, hi]（含端点）。
    // 本结构仅供 AcquireReadPredicate 的「父子区间继承与合并」规约逻辑使用；
    // 实际存储按 (表, 区间) 组织为 Phase 4 的居中区间树（见 pred_tables_）。
    struct PredicateLock {
        int64_t txn_id;
        int64_t table_rid;
        bool is_full;
        IndexKey lo;
        IndexKey hi;
    };

    // ---- Phase 4（创新特性 E）：谓词锁区间树 ----
    // 旧实现把所有谓词线性存于 pred_locks_ 向量，CheckWritePredicate 逐条
    // PredicateCovers 扫描 → O(P)。改为按表组织「居中区间树」（centered
    // interval tree，教科书 CLRS 区间树形态）：
    //   * 每张表一棵树；节点分裂点取 lo 值集合的中位数；
    //   * 跨过分裂点的区间存于节点本身（by_lo_asc / by_hi_desc 双有序表），
    //     完全在左/右的区间递归到左右子树；
    //   * 点查询 stabbing query 沿分裂点二分下降，每层只输出必然覆盖该键的
    //     区间前缀 → O(log P + K)（K 为命中区间数），全表谓词作哨兵单独存放；
    //   * 谓词注册/注销频率低（每语句级），采用「源向量 + 脏标记 + 惰性重建」
    //     （rebuild O(P log P) 摊薄在热路径之外）。
    struct Interval {
        IndexKey lo;      // 含端点
        IndexKey hi;
        int64_t txn_id;
    };
    struct IntervalNode {
        int32_t split_idx = -1;   // lo_values 中分裂点的下标
        std::vector<std::pair<IndexKey, int64_t>> by_lo_asc;   // (lo, txn) 升序
        std::vector<std::pair<IndexKey, int64_t>> by_hi_desc;  // (hi, txn) 降序
        int32_t left = -1, right = -1;   // 子树节点下标（nodes 向量）
    };
    struct PredicateTable {
        std::vector<int64_t> full_holders;   // 全表谓词哨兵持有者（is_full）
        std::vector<Interval> intervals;      // 源：合并后的区间条目（source of truth）
        std::vector<IndexKey> lo_values;      // 去重升序的 lo 集合（分裂点序列）
        std::vector<IntervalNode> nodes;      // 惰性重建的区间树（后序遍历下标）
        bool dirty = true;
    };
    std::unordered_map<int64_t, PredicateTable> pred_tables_;  // table_rid -> 树
    mutable size_t predicate_query_comparisons_ = 0;  // 累计键比较次数（观测用）

    // 惰性重建 t 的区间树（intervals → lo_values + nodes）。
    void PredicateTreeRebuild(PredicateTable& t) const;
    // 递归建树 [lo_begin, lo_end)：把 cands 按分裂点分派到节点/左右子树，返回节点下标。
    int32_t PredicateTreeBuildRange(PredicateTable& t, size_t lo_begin, size_t lo_end,
                                    const std::vector<Interval>& cands) const;
    // 点查询：把 t 中覆盖 key 的区间持有者（txn_id）追加进 out（不去重、不含自己，
    // 由调用方过滤）。空树时 no-op。
    void PredicateTreeQuery(const PredicateTable& t, const IndexKey& key,
                            std::vector<int64_t>* out) const;
};

}  // namespace sqlcompiler