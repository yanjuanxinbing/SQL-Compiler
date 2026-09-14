// LockManager 实现：S/X 锁 + 等待图死锁检测 + 可选阻塞超时。
// Phase 5（周期 3，G6）：锁表与谓词表按「所属表/命名空间」分片（kLockShardCount），
// 行锁与其所属表锁路由到同一分片；跨分片等待图与归属登记由 meta_mutex_ 保护。
// 锁序约定：分片互斥 -> meta_mutex_，恒不允许反向。
// 参见 include/storage/LockManager.h 的设计说明。

#include "storage/LockManager.h"

#include <algorithm>

namespace sqlcompiler {

namespace {
// 锁升级基准阈值：中型表（登记行数 256..4096）在持有该数行写锁后尝试升级为表锁。
// 见 LockManager::ComputeEscalationThreshold 的自适应规则。
constexpr size_t kBaseEscalationThreshold = 128;

// 稳定的 64 位散列（splitmix64）：表/资源 id → 分片下标。
size_t HashI64(int64_t x) {
    uint64_t h = static_cast<uint64_t>(x);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return static_cast<size_t>(h);
}
}  // namespace

size_t LockManager::TableShard(int64_t table_res) const {
    return HashI64(table_res) & kLockShardMask;
}

size_t LockManager::ShardOf(int64_t res_id, int64_t table_hint) const {
    if (res_id >= 0) return TableShard(res_id);          // 表资源：按自身哈希
    if (table_hint >= 0) return TableShard(table_hint);  // 行资源：优先表提示
    {   // 行资源：回退到已登记归属表（读 meta_mutex_，取完即放——不在持 meta 期间
        // 取分片锁，避免「meta -> 分片」的反序）。
        std::lock_guard<std::mutex> m(meta_mutex_);
        auto it = row_group_.find(res_id);
        if (it != row_group_.end()) return TableShard(it->second);
    }
    return HashI64(res_id) & kLockShardMask;             // 未登记：按资源自身哈希
}

bool LockManager::ModeCompatible(LockMode a, LockMode b) const {
    // 仅 S & S 兼容，其余冲突（S-X、X-S、X-X）。
    return a == LockMode::kShared && b == LockMode::kShared;
}

bool LockManager::Conflicts(const LockState& st, int64_t txn_id, LockMode mode) const {
    for (const auto& [hid, hmode] : st.holders) {
        if (hid == txn_id) continue;  // 自身的锁不与自己冲突
        if (!ModeCompatible(mode, hmode)) return true;
    }
    return false;
}

// 多粒度层级冲突（行级锁升级的冲突矩阵）：
//   * 行锁（res<0）→ 查其所属表的表级锁持有者：表 X 与任何行锁冲突；表 S 与行 X 冲突。
//   * 表锁（res>=0）→ 查本表下所有行锁持有者：表 X 与任何行锁冲突；表 S 与行 X 冲突。
// 无归属登记的行锁 / 未知表 → 不参与层级冲突（兼容裸 LockManager 用法）。
// 前置：调用方已持有 shard 分片互斥；res 的相关资源（行锁+其表锁）与该分片同片
//（分片路由不变量，见 ShardOf），本函数只在同一分片内查锁。
// table_hint >= 0：行锁的所属表由提示直接给出，免读 meta_mutex_（行锁热路径免
// 全局锁，分片收益不被 meta 全局互斥稀释）；<0 才按 row_group_ 登记查表。
bool LockManager::HierarchyConflicts(size_t shard, int64_t txn_id, int64_t res_id,
                                     LockMode mode, int64_t table_hint) const {
    LockShard& s = shards_[shard];
    if (res_id < 0) {
        // 行锁：检查所属表的表级锁。
        int64_t table = -1;
        if (table_hint >= 0) {
            table = table_hint;
        } else {
            std::lock_guard<std::mutex> m(meta_mutex_);
            auto g = row_group_.find(res_id);
            if (g == row_group_.end()) return false;  // 未登记归属
            table = g->second;
        }
        auto tit = s.locks.find(table);
        if (tit == s.locks.end()) return false;
        for (const auto& [hid, hmode] : tit->second.holders) {
            if (hid == txn_id) continue;
            // 表 X 与任何行锁冲突；表 S 与行 X 冲突（行 S 兼容）。
            if (hmode == LockMode::kExclusive) return true;
            if (hmode == LockMode::kShared && mode == LockMode::kExclusive) return true;
        }
        return false;
    }
    // 表锁：检查本表下已登记行锁的持有者。
    std::lock_guard<std::mutex> m(meta_mutex_);
    auto rows = table_rows_.find(res_id);
    if (rows == table_rows_.end()) return false;
    for (int64_t row : rows->second) {
        auto rit = s.locks.find(row);
        if (rit == s.locks.end()) continue;
        for (const auto& [hid, hmode] : rit->second.holders) {
            if (hid == txn_id) continue;
            if (mode == LockMode::kExclusive) return true;  // 表 X 与任何行锁冲突
            if (hmode == LockMode::kExclusive) return true; // 表 S 与行 X 冲突
        }
    }
    return false;
}

void LockManager::LinkWaitEdges(size_t shard, int64_t txn_id, int64_t res_id,
                                LockMode mode) {
    LockShard& s = shards_[shard];
    std::lock_guard<std::mutex> m(meta_mutex_);
    // 同资源冲突持有者。
    auto it = s.locks.find(res_id);
    if (it != s.locks.end()) {
        for (const auto& [hid, hmode] : it->second.holders) {
            if (hid != txn_id && !ModeCompatible(mode, hmode)) {
                waits_on_[txn_id].insert(hid);
            }
        }
    }
    // 层级冲突持有者（行锁←表锁、表锁←行锁）。
    if (res_id < 0) {
        auto g = row_group_.find(res_id);
        if (g != row_group_.end()) {
            auto tit = s.locks.find(g->second);
            if (tit != s.locks.end()) {
                for (const auto& [hid, hmode] : tit->second.holders) {
                    if (hid == txn_id) continue;
                    if (hmode == LockMode::kExclusive ||
                        (hmode == LockMode::kShared && mode == LockMode::kExclusive)) {
                        waits_on_[txn_id].insert(hid);
                    }
                }
            }
        }
    } else {
        auto rows = table_rows_.find(res_id);
        if (rows != table_rows_.end()) {
            for (int64_t row : rows->second) {
                auto rit = s.locks.find(row);
                if (rit == s.locks.end()) continue;
                for (const auto& [hid, hmode] : rit->second.holders) {
                    if (hid == txn_id) continue;
                    if (mode == LockMode::kExclusive ||
                        hmode == LockMode::kExclusive) {
                        waits_on_[txn_id].insert(hid);
                    }
                }
            }
        }
    }
}

void LockManager::UnlinkWaitEdges(int64_t txn_id) {
    // 清除 txn 自身指向谁（它持有的等待边）以及别人等它（从他人集合里移除）。
    std::lock_guard<std::mutex> m(meta_mutex_);
    waits_on_.erase(txn_id);
    for (auto& [_, who] : waits_on_) who.erase(txn_id);
}

bool LockManager::Dfs(int64_t cur, const int64_t start,
                      std::unordered_set<int64_t>& onpath,
                      std::unordered_set<int64_t>& visited) const {
    auto it = waits_on_.find(cur);
    if (it == waits_on_.end()) return false;
    for (int64_t nxt : it->second) {
        if (nxt == start) return true;               // 回到起点 -> 环
        if (visited.count(nxt)) continue;
        visited.insert(nxt);
        onpath.insert(nxt);
        if (Dfs(nxt, start, onpath, visited)) return true;
        onpath.erase(nxt);
    }
    return false;
}

bool LockManager::DeadlockCycle(int64_t txn_id) const {
    std::lock_guard<std::mutex> m(meta_mutex_);
    std::unordered_set<int64_t> visited, onpath;
    onpath.insert(txn_id);
    return Dfs(txn_id, txn_id, onpath, visited);
}

LockResult LockManager::Acquire(int64_t txn_id, int64_t res_id, LockMode mode,
                                int wait_ms, bool block_try, int64_t table_hint) {
    if (txn_id < 0) return LockResult::kDeadlock;  // 非法 txn_id 一律不进锁表
    const size_t shard = ShardOf(res_id, table_hint);
    LockShard& s = shards_[shard];
    std::unique_lock<std::mutex> lk(s.mutex);
    const auto deadline =
        (wait_ms > 0)
            ? std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(wait_ms)
            : std::chrono::steady_clock::time_point::max();

    for (;;) {
        LockState& st = s.locks[res_id];
        // 授权条件：同资源不冲突 且 多粒度层级不冲突（二者都在本分片内完成）。
        if (!Conflicts(st, txn_id, mode) &&
            !HierarchyConflicts(shard, txn_id, res_id, mode, table_hint)) {
            // 授予：成为持有者；清除本 txn 的全部等待边（含同资源与层级边，
            // 此刻它已不再等待任何人，残留边会污染后续死锁检测）。
            st.holders.emplace(txn_id, mode);
            { std::lock_guard<std::mutex> m(meta_mutex_); waits_on_.erase(txn_id); }
            return LockResult::kGranted;
        }

        // 冲突：登记自己为等待者，并链接等待边（供死锁检测）。
        bool already_waiting = false;
        for (auto& w : st.waiters) {
            if (w.first == txn_id) { already_waiting = true; break; }
        }
        if (!already_waiting) st.waiters.emplace_back(txn_id, mode);
        LinkWaitEdges(shard, txn_id, res_id, mode);

        // 死锁检测：本次加入让等待图成环 -> 本事务作为 victim 中止。
        if (DeadlockCycle(txn_id)) {
            // 撤销本事务刚登记的等待，避免残留边污染后续检测。
            for (auto it = st.waiters.begin(); it != st.waiters.end(); ++it) {
                if (it->first == txn_id) { st.waiters.erase(it); break; }
            }
            UnlinkWaitEdges(txn_id);
            return LockResult::kDeadlock;
        }

        if (block_try) {
            // 非阻塞探针：不阻塞、保留等待登记，返回 WouldBlock。
            return LockResult::kWouldBlock;
        }
        if (wait_ms == 0) {
            // 无限阻塞等待至可授予（死锁已在上面提前返回）。
            s.cv.wait(lk);
            continue;
        }
        if (s.cv.wait_until(lk, deadline) == std::cv_status::timeout) {
            // 超时：撤销等待登记并返回。
            for (auto it = st.waiters.begin(); it != st.waiters.end(); ++it) {
                if (it->first == txn_id) { st.waiters.erase(it); break; }
            }
            UnlinkWaitEdges(txn_id);
            return LockResult::kTimeout;
        }
    }
}

LockResult LockManager::LockShared(int64_t txn_id, int64_t res_id, int wait_ms,
                                   int64_t table_hint) {
    return Acquire(txn_id, res_id, LockMode::kShared, wait_ms, false, table_hint);
}

LockResult LockManager::LockExclusive(int64_t txn_id, int64_t res_id, int wait_ms,
                                      int64_t table_hint) {
    return Acquire(txn_id, res_id, LockMode::kExclusive, wait_ms, false, table_hint);
}

LockResult LockManager::TryLockShared(int64_t txn_id, int64_t res_id,
                                      int64_t table_hint) {
    return Acquire(txn_id, res_id, LockMode::kShared, 0, true, table_hint);
}

LockResult LockManager::TryLockExclusive(int64_t txn_id, int64_t res_id,
                                         int64_t table_hint) {
    return Acquire(txn_id, res_id, LockMode::kExclusive, 0, true, table_hint);
}

void LockManager::Unlock(int64_t txn_id, int64_t res_id, int64_t table_hint) {
    const size_t shard = ShardOf(res_id, table_hint);
    LockShard& s = shards_[shard];
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.locks.find(res_id);
    if (it == s.locks.end()) return;
    LockState& st = it->second;
    st.holders.erase(txn_id);

    // 依次为可授予的等待者重新评估（每授予一个就重查，保持兼容性）。
    bool progress = true;
    while (progress) {
        progress = false;
        for (auto w = st.waiters.begin(); w != st.waiters.end(); ++w) {
            if (!Conflicts(st, w->first, w->second) &&
                !HierarchyConflicts(shard, w->first, res_id, w->second)) {
                st.holders.emplace(w->first, w->second);
                // 释放该资源上被解除的等待边。
                { std::lock_guard<std::mutex> m(meta_mutex_);
                  for (const auto& [hid, hmode] : st.holders) {
                      (void)hmode;
                      if (hid != w->first) waits_on_[w->first].erase(hid);
                  } }
                st.waiters.erase(w);
                progress = true;
                break;
            }
        }
    }
    if (st.waiters.empty() && st.holders.empty()) s.locks.erase(res_id);
    s.cv.notify_all();
}

void LockManager::UnlockAll(int64_t txn_id) {
    if (txn_id < 0) return;
    // 逐分片处理（每次只持一把分片互斥，恒不跨分片持锁）：
    // 释放持有、撤销等待、剔除 SERIALIZABLE 谓词。
    for (size_t i = 0; i < kLockShardCount; ++i) {
        LockShard& s = shards_[i];
        std::lock_guard<std::mutex> lk(s.mutex);
        std::vector<int64_t> owned;
        std::vector<int64_t> waiting_on;
        for (const auto& [res, st] : s.locks) {
            if (st.holders.count(txn_id)) owned.push_back(res);
            for (const auto& w : st.waiters) {
                if (w.first == txn_id) { waiting_on.push_back(res); break; }
            }
        }
        for (int64_t res : owned) {
            auto it = s.locks.find(res);
            if (it == s.locks.end()) continue;
            LockState& st = it->second;
            st.holders.erase(txn_id);
            // 重新评估该资源的等待者。
            bool progress = true;
            while (progress) {
                progress = false;
                for (auto w = st.waiters.begin(); w != st.waiters.end(); ++w) {
                    if (!Conflicts(st, w->first, w->second) &&
                        !HierarchyConflicts(i, w->first, res, w->second)) {
                        st.holders.emplace(w->first, w->second);
                        { std::lock_guard<std::mutex> m(meta_mutex_);
                          for (const auto& [hid, hmode] : st.holders) {
                              (void)hmode;
                              if (hid != w->first) waits_on_[w->first].erase(hid);
                          } }
                        st.waiters.erase(w);
                        progress = true;
                        break;
                    }
                }
            }
            if (st.waiters.empty() && st.holders.empty()) s.locks.erase(res);
        }
        // 撤销 txn 在本分片所有资源上的等待登记。
        for (int64_t res : waiting_on) {
            auto it = s.locks.find(res);
            if (it != s.locks.end()) {
                for (auto w = it->second.waiters.begin(); w != it->second.waiters.end(); ++w) {
                    if (w->first == txn_id) { it->second.waiters.erase(w); break; }
                }
            }
        }
        // 撤销 txn 在本分片持有的 SERIALIZABLE 谓词锁，并唤醒在谓词上阻塞的写者。
        // Phase 5：谓词按 (表, 列) 存于分片 pred_tables（表级全表谓词 + 每列区间树），
        // 逐表剔除该事务：先删表级全表哨兵，再逐列删区间、清空空列容器。
        for (auto it = s.pred_tables.begin(); it != s.pred_tables.end();) {
            PredicateTableGroup& g = it->second;
            g.full_holders.erase(
                std::remove(g.full_holders.begin(), g.full_holders.end(), txn_id),
                g.full_holders.end());
            for (auto cit = g.columns.begin(); cit != g.columns.end();) {
                PredicateTable& t = cit->second;
                const std::vector<Interval> old_set = t.intervals;  // 周期 2：增量同步
                t.intervals.erase(
                    std::remove_if(t.intervals.begin(), t.intervals.end(),
                                   [txn_id](const Interval& iv) {
                                       return iv.txn_id == txn_id;
                                   }),
                    t.intervals.end());
                if (t.intervals.empty()) {
                    cit = g.columns.erase(cit);  // 列清空：整树随容器丢弃
                } else {
                    // 周期 2：树已构建时增量删除该事务区间（O(D log P)），不整树重建。
                    PredicateTreeSync(t, old_set, t.intervals);
                    ++cit;
                }
            }
            if (g.full_holders.empty() && g.columns.empty()) {
                it = s.pred_tables.erase(it);
            } else {
                ++it;
            }
        }
        s.cv.notify_all();
    }
    // 跨分片元数据清理：升级标记 + 等待图边。
    { std::lock_guard<std::mutex> m(meta_mutex_);
      escalated_tables_.erase(txn_id); }
    UnlinkWaitEdges(txn_id);
}

bool LockManager::IsLockHeld(int64_t txn_id, int64_t res_id) const {
    const size_t shard = ShardOf(res_id, -1);
    std::lock_guard<std::mutex> lk(shards_[shard].mutex);
    auto it = shards_[shard].locks.find(res_id);
    if (it == shards_[shard].locks.end()) return false;
    return it->second.holders.count(txn_id) > 0;
}

void LockManager::RegisterRowGroup(int64_t row_res, int64_t table_res) {
    std::lock_guard<std::mutex> m(meta_mutex_);
    row_group_[row_res] = table_res;
    table_rows_[table_res].insert(row_res);
}

size_t LockManager::CountRowLocks(int64_t txn_id, int64_t table_res) const {
    const size_t shard = TableShard(table_res);
    std::lock_guard<std::mutex> lk(shards_[shard].mutex);
    std::lock_guard<std::mutex> m(meta_mutex_);
    size_t n = 0;
    auto rows = table_rows_.find(table_res);
    if (rows == table_rows_.end()) return 0;
    for (int64_t row : rows->second) {
        auto it = shards_[shard].locks.find(row);
        if (it == shards_[shard].locks.end()) continue;
        if (it->second.holders.count(txn_id) > 0) ++n;
    }
    return n;
}

size_t LockManager::CountPredicateLocks(int64_t txn_id, int64_t table_rid) const {
    const size_t shard = TableShard(table_rid);
    std::lock_guard<std::mutex> lk(shards_[shard].mutex);
    auto it = shards_[shard].pred_tables.find(table_rid);
    if (it == shards_[shard].pred_tables.end()) return 0;
    const PredicateTableGroup& g = it->second;
    size_t n = 0;
    if (std::find(g.full_holders.begin(), g.full_holders.end(), txn_id) !=
        g.full_holders.end()) {
        ++n;  // 表级全表谓词哨兵计 1 条（与旧 pred_locks_ 语义一致）
    }
    for (const auto& kv : g.columns) {
        for (const Interval& iv : kv.second.intervals) {
            if (iv.txn_id == txn_id) ++n;
        }
    }
    return n;
}

size_t LockManager::CountTotalPredicates(int64_t table_rid) const {
    const size_t shard = TableShard(table_rid);
    std::lock_guard<std::mutex> lk(shards_[shard].mutex);
    auto it = shards_[shard].pred_tables.find(table_rid);
    if (it == shards_[shard].pred_tables.end()) return 0;
    const PredicateTableGroup& g = it->second;
    size_t n = g.full_holders.size();
    for (const auto& kv : g.columns) {
        n += kv.second.intervals.size();
    }
    return n;
}

size_t LockManager::GetPredicateQueryComparisons() const {
    return predicate_query_comparisons_.load();
}

void LockManager::ResetPredicateQueryComparisons() {
    predicate_query_comparisons_.store(0);
}

size_t LockManager::ComputeEscalationThreshold(size_t registered_rows,
                                               size_t conflict_count) {
    size_t thr = kBaseEscalationThreshold;
    // 表规模自适应：小表（<256 行）提前升级（约一半行数即升，下限 8）；
    // 大表（>=4096 行）延后升级（每 4096 行上调 64，避免过早放大表级互斥）。
    if (registered_rows > 0 && registered_rows < 256) {
        thr = std::max<size_t>(8, registered_rows / 2);
    } else if (registered_rows >= 4096) {
        thr = kBaseEscalationThreshold + (registered_rows / 4096) * 64;
    }
    // 冲突采样：升级曾因他人持冲突锁失败（kWouldBlock）→ 降阈（更早再次尝试升级，
    // 冲突一消解立即把大量行锁收敛为表锁；下限 8 防止退化到逐行尝试升级）。
    if (conflict_count > 0) {
        thr = std::max<size_t>(8, thr / 2);
    }
    return thr;
}

size_t LockManager::GetRegisteredRowCount(int64_t table_res) const {
    std::lock_guard<std::mutex> m(meta_mutex_);
    auto it = table_rows_.find(table_res);
    return (it == table_rows_.end()) ? 0 : it->second.size();
}

size_t LockManager::GetTableConflictCount(int64_t table_res) const {
    std::lock_guard<std::mutex> m(meta_mutex_);
    auto it = table_escalation_conflicts_.find(table_res);
    return (it == table_escalation_conflicts_.end()) ? 0 : it->second;
}

bool LockManager::IsTableEscalated(int64_t txn_id, int64_t table_res) const {
    std::lock_guard<std::mutex> m(meta_mutex_);
    auto it = escalated_tables_.find(txn_id);
    if (it == escalated_tables_.end()) return false;
    return it->second.count(table_res) > 0;
}

LockResult LockManager::TryEscalateTable(int64_t txn_id, int64_t table_res,
                                         LockMode mode) {
    if (txn_id < 0) return LockResult::kDeadlock;
    const size_t shard = TableShard(table_res);
    LockShard& s = shards_[shard];
    std::lock_guard<std::mutex> lk(s.mutex);

    // 已升级：幂等成功。
    { std::lock_guard<std::mutex> m(meta_mutex_);
      auto et = escalated_tables_.find(txn_id);
      if (et != escalated_tables_.end() && et->second.count(table_res) > 0) {
          return LockResult::kGranted;
      } }
    auto tit = s.locks.find(table_res);
    if (tit != s.locks.end() && tit->second.holders.count(txn_id) > 0) {
        { std::lock_guard<std::mutex> m(meta_mutex_);
          escalated_tables_[txn_id].insert(table_res); }
        return LockResult::kGranted;
    }

    // 升级条件：表锁与层级（本表行锁）都不能有他人冲突持有。
    // 层级检查：本表下任一被他人持有的行锁都会挡住表级 X；表级 S 仅被行 X 挡住。
    if (tit != s.locks.end() && Conflicts(tit->second, txn_id, mode)) {
        { std::lock_guard<std::mutex> m(meta_mutex_);
          ++table_escalation_conflicts_[table_res]; }  // 冲突采样：他人持表锁
        return LockResult::kWouldBlock;  // 他人持表锁
    }
    if (HierarchyConflicts(shard, txn_id, table_res, mode)) {
        { std::lock_guard<std::mutex> m(meta_mutex_);
          ++table_escalation_conflicts_[table_res]; }  // 冲突采样：他人持本表行锁
        return LockResult::kWouldBlock;  // 他人持本表行锁
    }

    // 授予表锁。
    LockState& st = s.locks[table_res];
    st.holders.emplace(txn_id, mode);
    { std::lock_guard<std::mutex> m(meta_mutex_); waits_on_.erase(txn_id); }

    // 释放本事务在本表上的全部行锁（升级后由表锁覆盖）。行锁与其表锁同片，
    // 故都在本分片内完成。
    std::vector<int64_t> to_unlock;
    { std::lock_guard<std::mutex> m(meta_mutex_);
      auto rows = table_rows_.find(table_res);
      if (rows != table_rows_.end()) {
          for (int64_t row : rows->second) {
              auto rit = s.locks.find(row);
              if (rit != s.locks.end() && rit->second.holders.count(txn_id) > 0) {
                  to_unlock.push_back(row);
              }
          }
      } }
    for (int64_t row : to_unlock) {
        auto it = s.locks.find(row);
        if (it == s.locks.end()) continue;
        it->second.holders.erase(txn_id);
        // 重新评估该行上的等待者。升级后表 X 已持有：层级冲突让行等待者仍被
        // 挡住（表 X 覆盖整表），与「升级后由表锁接管行互斥」的语义一致。
        bool progress = true;
        while (progress) {
            progress = false;
            for (auto w = it->second.waiters.begin(); w != it->second.waiters.end(); ++w) {
                if (!Conflicts(it->second, w->first, w->second) &&
                    !HierarchyConflicts(shard, w->first, row, w->second)) {
                    it->second.holders.emplace(w->first, w->second);
                    { std::lock_guard<std::mutex> m(meta_mutex_);
                      for (const auto& [hid, hmode] : it->second.holders) {
                          (void)hmode;
                          if (hid != w->first) waits_on_[w->first].erase(hid);
                      } }
                    it->second.waiters.erase(w);
                    progress = true;
                    break;
                }
            }
        }
        if (it->second.waiters.empty() && it->second.holders.empty()) s.locks.erase(row);
    }
    { std::lock_guard<std::mutex> m(meta_mutex_);
      escalated_tables_[txn_id].insert(table_res); }
    s.cv.notify_all();
    return LockResult::kGranted;
}

namespace {

// 谓词区间端点比较辅助：lo/hi 的 values 为空表示开边界（lo 空 = -inf，hi 空 = +inf）。
// a.lo <= b.hi（任一端开放即成立）。
bool PredicateLoLeHi(const IndexKey& lo, const IndexKey& hi) {
    return lo.values.empty() || hi.values.empty() || CompareKeyOnly(lo, hi) <= 0;
}

// 两个谓词区间重叠：a.lo <= b.hi && b.lo <= a.hi。
// 只依赖 IndexKey 端点（PredicateLock 是 LockManager 私有嵌套类型，辅助函数
// 不能引用它，避免类外访问控制错误）。
bool PredicateIntervalsOverlap(const IndexKey& a_lo, const IndexKey& a_hi,
                               const IndexKey& b_lo, const IndexKey& b_hi) {
    return PredicateLoLeHi(a_lo, b_hi) && PredicateLoLeHi(b_lo, a_hi);
}

}  // namespace

LockResult LockManager::AcquireReadPredicate(int64_t txn_id, int64_t table_rid,
                                             int32_t column, bool is_full,
                                             const IndexKey& lo, const IndexKey& hi) {
    if (txn_id < 0) return LockResult::kDeadlock;
    const size_t shard = TableShard(table_rid);
    LockShard& s = shards_[shard];
    std::lock_guard<std::mutex> lk(s.mutex);

    // Phase 5：谓词按 (表, 列) 组织为 PredicateTableGroup{表级全表谓词 + 每列区间树}。
    // 合并/继承规约逻辑与 v2 一致，仅存储容器泛化到「列」维度。
    PredicateTableGroup& g = s.pred_tables[table_rid];

    // ---- 父子区间继承（表级全表谓词）----
    // 已持表级全表谓词 → 任何子谓词（任意列任意区间）被父谓词覆盖，直接丢弃。
    const bool has_full =
        std::find(g.full_holders.begin(), g.full_holders.end(), txn_id) !=
        g.full_holders.end();
    if (has_full) {
        return LockResult::kGranted;
    }
    if (is_full) {
        // 新谓词为表级全表：删除本事务在全部列上的区间谓词，只保留全表哨兵。
        // 其他事务的区间必须原样保留——全表谓词只覆盖本事务的读范围，无权撤销
        // 他事务已注册的谓词（否则他事务的防幻读能力会凭空丢失）。
        for (auto cit = g.columns.begin(); cit != g.columns.end();) {
            PredicateTable& t = cit->second;
            const std::vector<Interval> old_set = t.intervals;  // 周期 2：增量同步
            std::vector<Interval> kept;
            for (const Interval& iv : t.intervals) {
                if (iv.txn_id != txn_id) kept.push_back(iv);
            }
            if (kept.empty()) {
                cit = g.columns.erase(cit);  // 列清空：整树随容器丢弃
            } else {
                t.intervals = std::move(kept);
                PredicateTreeSync(t, old_set, t.intervals);
                ++cit;
            }
        }
        g.full_holders.push_back(txn_id);
        return LockResult::kGranted;
    }

    // ---- 同列区间合并：本事务本列既有区间 + 新区间按 lo 升序，合并真重叠 ----
    // 注意：合并输出必须用独立容器 merged —— kept 中还保留着其他事务的谓词，
    // 直接复用 kept.back() 会让新区间与「最后一条被保留的谓词」误判重叠，
    // 导致新区间被丢弃（如跨列/跨事务注册时覆盖能力丢失）。
    PredicateTable& t = g.columns[column];
    const std::vector<Interval> old_set = t.intervals;  // 周期 2：增量同步备份
    std::vector<PredicateLock> kept;   // 其他事务同列谓词（原样保留）
    std::vector<PredicateLock> group;  // 本事务同列既有区间谓词
    for (const Interval& iv : t.intervals) {
        PredicateLock p{iv.txn_id, table_rid, column, false, iv.lo, iv.hi};
        if (iv.txn_id == txn_id) {
            group.push_back(std::move(p));
        } else {
            kept.push_back(std::move(p));
        }
    }
    std::vector<PredicateLock> intervals = std::move(group);
    intervals.push_back(PredicateLock{txn_id, table_rid, column, false, lo, hi});
    std::sort(intervals.begin(), intervals.end(),
              [](const PredicateLock& a, const PredicateLock& b) {
                  const bool ae = a.lo.values.empty(), be = b.lo.values.empty();
                  if (ae != be) return ae;  // 开下界（-inf）排最前
                  return CompareKeyOnly(a.lo, b.lo) < 0;
              });
    std::vector<PredicateLock> merged;
    for (size_t i = 0; i < intervals.size(); ++i) {
        const PredicateLock& cur = intervals[i];
        if (merged.empty()) {
            merged.push_back(cur);
            continue;
        }
        PredicateLock& last = merged.back();
        // 真重叠 → 合并为并集（并集的防幻读能力不小于原各区间）。
        if (PredicateIntervalsOverlap(last.lo, last.hi, cur.lo, cur.hi)) {
            if (last.lo.values.empty()) {
                // 已是 -inf，无需更新
            } else if (cur.lo.values.empty()) {
                last.lo = cur.lo;  // cur 下界开放 → 并集下界为 -inf
            } else if (CompareKeyOnly(cur.lo, last.lo) < 0) {
                last.lo = cur.lo;
            }
            if (last.hi.values.empty()) {
                // 已是 +inf，无需更新
            } else if (cur.hi.values.empty()) {
                last.hi = cur.hi;  // cur 上界开放 → 并集上界为 +inf
            } else if (CompareKeyOnly(cur.hi, last.hi) > 0) {
                last.hi = cur.hi;
            }
        } else {
            merged.push_back(cur);
        }
    }
    t.intervals.clear();
    for (const PredicateLock& m : merged) {
        t.intervals.push_back(Interval{m.lo, m.hi, m.txn_id});
    }
    for (const PredicateLock& o : kept) {
        t.intervals.push_back(Interval{o.lo, o.hi, o.txn_id});
    }
    // 周期 2：树已构建时增量同步（O(D log P)），避免每次注册整树重建（O(P log P)）。
    PredicateTreeSync(t, old_set, t.intervals);
    return LockResult::kGranted;
}

void LockManager::GatherColumnConflicts(size_t shard, int64_t table_rid, int32_t column,
                                        const IndexKey& key,
                                        std::vector<int64_t>* out) {
    LockShard& s = shards_[shard];
    auto tit = s.pred_tables.find(table_rid);
    if (tit == s.pred_tables.end()) return;
    PredicateTableGroup& g = tit->second;
    for (int64_t f : g.full_holders) out->push_back(f);
    auto cit = g.columns.find(column);
    if (cit == g.columns.end()) return;
    PredicateTable& t = cit->second;
    if (t.dirty) PredicateTreeRebuild(t);
    PredicateTreeQuery(t, key, out);
}

LockResult LockManager::CheckWritePredicate(int64_t txn_id, int64_t table_rid,
                                            int32_t column, const IndexKey& key,
                                            int wait_ms) {
    if (txn_id < 0) return LockResult::kDeadlock;
    const size_t shard = TableShard(table_rid);
    LockShard& s = shards_[shard];
    std::unique_lock<std::mutex> lk(s.mutex);
    const auto deadline =
        (wait_ms > 0)
            ? std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(wait_ms)
            : std::chrono::steady_clock::time_point::max();

    for (;;) {
        // 收集其他活动事务持有、且覆盖该键的谓词 → 冲突持有者集合（去重）。
        // Phase 5：表级全表哨兵 O(#full) + 该列区间树 stabbing O(log P + K)；
        // 旧实现为对全部谓词线性 PredicateCovers → O(P)。
        std::vector<int64_t> conflicts;
        GatherColumnConflicts(shard, table_rid, column, key, &conflicts);
        std::sort(conflicts.begin(), conflicts.end());
        conflicts.erase(std::unique(conflicts.begin(), conflicts.end()),
                        conflicts.end());
        conflicts.erase(std::remove(conflicts.begin(), conflicts.end(), txn_id),
                        conflicts.end());
        if (conflicts.empty()) return LockResult::kGranted;

        // 建立本事务指向各冲突谓词持有者的等待边。
        { std::lock_guard<std::mutex> m(meta_mutex_);
          for (int64_t c : conflicts) waits_on_[txn_id].insert(c); }
        if (DeadlockCycle(txn_id)) {
            UnlinkWaitEdges(txn_id);
            return LockResult::kDeadlock;
        }
        if (wait_ms == 0) {
            // 无限阻塞至谓词持有者提交（UnlockAll 唤醒后重试）。
            s.cv.wait(lk);
        } else if (s.cv.wait_until(lk, deadline) == std::cv_status::timeout) {
            UnlinkWaitEdges(txn_id);
            return LockResult::kTimeout;
        }
        // 被唤醒后清除指向冲突持有者的边，重查谓词集合（可能已提交）。
        { std::lock_guard<std::mutex> m(meta_mutex_);
          for (int64_t c : conflicts) waits_on_[txn_id].erase(c); }
    }
}

LockResult LockManager::CheckWritePredicateRow(int64_t txn_id, int64_t table_rid,
                                               const std::vector<Value>& row,
                                               int wait_ms) {
    if (txn_id < 0) return LockResult::kDeadlock;
    const size_t shard = TableShard(table_rid);
    LockShard& s = shards_[shard];
    std::unique_lock<std::mutex> lk(s.mutex);
    const auto deadline =
        (wait_ms > 0)
            ? std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(wait_ms)
            : std::chrono::steady_clock::time_point::max();

    for (;;) {
        // 整行写检查：表级全表谓词命中任意写；逐列对该列值做区间树 stabbing。
        // 一次加锁内收集全部冲突持有者（不重复逐列取全局互斥）。
        std::vector<int64_t> conflicts;
        auto tit = s.pred_tables.find(table_rid);
        if (tit != s.pred_tables.end()) {
            PredicateTableGroup& g = tit->second;
            for (int64_t f : g.full_holders) conflicts.push_back(f);
            for (auto& kv : g.columns) {
                const int32_t col = kv.first;
                if (col < 0 || static_cast<size_t>(col) >= row.size()) continue;
                const Value& v = row[static_cast<size_t>(col)];
                if (v.IsNull()) continue;  // NULL 不进入索引值域，不可能命中区间
                PredicateTable& t = kv.second;
                if (t.dirty) PredicateTreeRebuild(t);
                PredicateTreeQuery(t, IndexKey{std::vector<Value>{v}}, &conflicts);
            }
        }
        std::sort(conflicts.begin(), conflicts.end());
        conflicts.erase(std::unique(conflicts.begin(), conflicts.end()),
                        conflicts.end());
        conflicts.erase(std::remove(conflicts.begin(), conflicts.end(), txn_id),
                        conflicts.end());
        if (conflicts.empty()) return LockResult::kGranted;

        // 建立本事务指向各冲突谓词持有者的等待边（与单列检查同一套死锁/超时逻辑）。
        { std::lock_guard<std::mutex> m(meta_mutex_);
          for (int64_t c : conflicts) waits_on_[txn_id].insert(c); }
        if (DeadlockCycle(txn_id)) {
            UnlinkWaitEdges(txn_id);
            return LockResult::kDeadlock;
        }
        if (wait_ms == 0) {
            s.cv.wait(lk);
        } else if (s.cv.wait_until(lk, deadline) == std::cv_status::timeout) {
            UnlinkWaitEdges(txn_id);
            return LockResult::kTimeout;
        }
        { std::lock_guard<std::mutex> m(meta_mutex_);
          for (int64_t c : conflicts) waits_on_[txn_id].erase(c); }
    }
}

// ---- Phase 4：谓词锁居中区间树（惰性重建） ----

// 重建 t 的区间树：intervals（源）→ lo_values（去重升序分裂点）+ nodes（区间树）。
// 树形态：节点分裂点 split = 区间 lo 值集合的中位数；
//   * 跨过 split 的区间（lo <= split <= hi）存于节点自身（by_lo_asc / by_hi_desc）；
//   * hi < split 的区间递归到左子树；lo > split 的区间递归到右子树。
// 每个区间恰好归属一个节点；stabbing 点查询沿分裂点二分下降，每层输出必然覆盖
// 该键的区间前缀 → O(log P + K)。
// Phase 5：lo/hi 的 values 为空表示开边界（lo 空 = -inf、hi 空 = +inf）：
//   * 开下界不参与分裂点集合（-inf 无法做分裂点），开下界区间始终「跨过」任意分裂点；
//   * 开上界区间恒满足 hi >= split（+inf 不小于任何分裂点）。
void LockManager::PredicateTreeRebuild(PredicateTable& t) const {
    ++predicate_rebuild_count_;  // 周期 2 观测：整树重建次数
    t.lo_values.clear();
    t.nodes.clear();
    if (t.intervals.empty()) {
        t.dirty = false;
        return;
    }
    std::vector<IndexKey> los;
    los.reserve(t.intervals.size());
    for (const Interval& iv : t.intervals) {
        if (iv.lo.values.empty()) continue;  // 开下界（-inf）不参与分裂点
        los.push_back(iv.lo);
    }
    std::sort(los.begin(), los.end(),
              [](const IndexKey& a, const IndexKey& b) {
                  return CompareKeyOnly(a, b) < 0;
              });
    los.erase(std::unique(los.begin(), los.end(),
                          [](const IndexKey& a, const IndexKey& b) {
                              return CompareKeyOnly(a, b) == 0;
                          }),
              los.end());
    t.lo_values = std::move(los);
    if (t.lo_values.empty()) {
        // 全部区间下界开放：无分裂点可用，退化单节点（split_idx = -1）。
        // 查询走 fallback 双端过滤（lo 开放恒满足 lo 侧，只需 hi >= key）。
        IntervalNode n;
        for (const Interval& iv : t.intervals) n.fallback.push_back(iv);
        t.nodes.push_back(std::move(n));
        t.dirty = false;
        t.pending_inserts_ = 0;
        return;
    }
    PredicateTreeBuildRange(t, 0, t.lo_values.size(), t.intervals);
    t.dirty = false;
    t.pending_inserts_ = 0;
}

int32_t LockManager::PredicateTreeBuildRange(PredicateTable& t, size_t lo_begin,
                                             size_t lo_end,
                                             const std::vector<Interval>& cands) const {
    const int32_t idx = static_cast<int32_t>(t.nodes.size());
    t.nodes.push_back(IntervalNode{});
    int32_t left = -1, right = -1;
    {
        // 注意：不能把「n = t.nodes[idx]」引用跨递归调用持有——递归 push_back 可能
        // 触发 nodes 向量重分配，令引用悬垂，随后 n.left/n.right 赋值将写入已释放
        // 内存（堆损坏，崩溃延后到后续任意代码）。因此先在本作用域完成节点填充，
        // 子树下标在递归结束后用下标重新取引用回填。
        IntervalNode& n = t.nodes[static_cast<size_t>(idx)];
        n.split_idx = static_cast<int32_t>((lo_begin + lo_end) / 2);
        const IndexKey& split = t.lo_values[static_cast<size_t>(n.split_idx)];

        std::vector<Interval> left_c, right_c;
        for (const Interval& iv : cands) {
            const bool lo_le =
                iv.lo.values.empty() || CompareKeyOnly(iv.lo, split) <= 0;
            const bool hi_ge =
                iv.hi.values.empty() || CompareKeyOnly(iv.hi, split) >= 0;
            if (lo_le && hi_ge) {
                // 跨过分裂点：存于本节点（lo <= split <= hi）。
                n.by_lo_asc.emplace_back(iv.lo, iv.txn_id);
                n.by_hi_desc.emplace_back(iv.hi, iv.txn_id);
            } else if (!iv.hi.values.empty() &&
                       CompareKeyOnly(iv.hi, split) < 0) {
                left_c.push_back(iv);   // 完全在分裂点左侧（hi 非空才可能）
            } else {
                right_c.push_back(iv);  // 完全在分裂点右侧（lo > split）
            }
        }
        std::sort(n.by_lo_asc.begin(), n.by_lo_asc.end(),
                  [](const std::pair<IndexKey, int64_t>& a,
                     const std::pair<IndexKey, int64_t>& b) {
                      const bool ae = a.first.values.empty();
                      const bool be = b.first.values.empty();
                      if (ae != be) return ae;  // 开下界（-inf）在前
                      return CompareKeyOnly(a.first, b.first) < 0;
                  });
        std::sort(n.by_hi_desc.begin(), n.by_hi_desc.end(),
                  [](const std::pair<IndexKey, int64_t>& a,
                     const std::pair<IndexKey, int64_t>& b) {
                      const bool ae = a.first.values.empty();
                      const bool be = b.first.values.empty();
                      if (ae != be) return ae;  // 开上界（+inf）在前
                      return CompareKeyOnly(a.first, b.first) > 0;  // hi 降序
                  });
        const int32_t split_pos = n.split_idx;  // 捕获：递归后 n 可能悬垂，不得再读
        // 周期 2：子范围为空但仍有候选时建退化叶子（split_idx=-1）兜底——
        // 旧实现直接跳过递归，这些区间被静默丢弃（开下界区间 hi < 最左分裂点、
        // 或 lo 落在分裂点间隙且无更右分裂点），等于谓词凭空消失，SERIALIZABLE
        // 防幻读会出现漏洞。退化叶子的查询走 fallback 双端过滤 lo<=key<=hi。
        const auto build_degenerate = [&](const std::vector<Interval>& cands) {
            const int32_t didx = static_cast<int32_t>(t.nodes.size());
            t.nodes.push_back(IntervalNode{});
            IntervalNode& dn = t.nodes[static_cast<size_t>(didx)];
            for (const Interval& iv : cands) dn.fallback.push_back(iv);
            return didx;
        };
        if (lo_begin < static_cast<size_t>(split_pos)) {
            left = PredicateTreeBuildRange(t, lo_begin,
                                           static_cast<size_t>(split_pos), left_c);
        } else if (!left_c.empty()) {
            left = build_degenerate(left_c);
        }
        if (static_cast<size_t>(split_pos) + 1 < lo_end) {
            right = PredicateTreeBuildRange(t, static_cast<size_t>(split_pos) + 1,
                                            lo_end, right_c);
        } else if (!right_c.empty()) {
            right = build_degenerate(right_c);
        }
    }
    t.nodes[static_cast<size_t>(idx)].left = left;
    t.nodes[static_cast<size_t>(idx)].right = right;
    return idx;
}

void LockManager::PredicateTreeQuery(const PredicateTable& t, const IndexKey& key,
                                     std::vector<int64_t>* out) const {
    if (t.nodes.empty()) return;
    int32_t idx = 0;  // 根
    while (idx >= 0) {
        const IntervalNode& n = t.nodes[static_cast<size_t>(idx)];
        if (n.split_idx < 0) {
            // 退化节点：区间无法归属任何分裂点（lo 落在分裂点间隙、或开下界且
            // hi 小于某范围最左分裂点）。到达路径已约束一侧（经 right 分支
            // key > 某分裂点、经 left 分支 key < 某分裂点），但区间内部仍可能
            // 不覆盖 key（如 [12,15] 与 key=11），必须双端过滤 lo <= key <= hi。
            for (const Interval& iv : n.fallback) {
                if (!iv.lo.values.empty() && CompareKeyOnly(iv.lo, key) > 0) continue;
                if (!iv.hi.values.empty() && CompareKeyOnly(iv.hi, key) < 0) continue;
                out->push_back(iv.txn_id);
            }
            return;
        }
        const IndexKey& split = t.lo_values[static_cast<size_t>(n.split_idx)];
        const int c = CompareKeyOnly(key, split);
        if (c < 0) {
            // key < split：本节点区间都有 hi >= split > key，只需 lo <= key。
            for (const auto& pr : n.by_lo_asc) {
                if (pr.first.values.empty() || CompareKeyOnly(pr.first, key) <= 0)
                    out->push_back(pr.second);
                else break;
            }
            idx = n.left;
        } else if (c > 0) {
            // key > split：本节点区间都有 lo <= split < key，只需 hi >= key。
            for (const auto& pr : n.by_hi_desc) {
                if (pr.first.values.empty() || CompareKeyOnly(pr.first, key) >= 0)
                    out->push_back(pr.second);
                else break;
            }
            idx = n.right;
        } else {
            // key == split：本节点全部区间覆盖该键；左右子树分别 hi < split / lo > split，
            // 不可能覆盖，查询结束。
            for (const auto& pr : n.by_lo_asc) out->push_back(pr.second);
            break;
        }
        ++predicate_query_comparisons_;  // 每下降一层记一次分裂点比较
    }
}

namespace {

// 区间端点相等：都开放（空）算相等；一开一闭必不等；否则按值比较。
bool IntervalKeyEquals(const IndexKey& a, const IndexKey& b) {
    const bool ae = a.values.empty(), be = b.values.empty();
    if (ae || be) return ae == be;
    return CompareKeyOnly(a, b) == 0;
}

// 有序表二分定位插入。两种表序：
//   * by_lo_asc：lo 升序，空 lo（-inf）恒在最前；
//   * by_hi_desc：hi 降序，空 hi（+inf）恒在最前。
// 统一比较器：comp(a, key) = a 应排在 key 之前（a < key 的偏序语义）。
//   * 都空 / 一空一非空：空侧恒前（ae && !ke）；
//   * 都非空：升序表 CompareKeyOnly < 0，降序表 CompareKeyOnly > 0。
template <bool Ascending>
bool KeyLess(const std::pair<IndexKey, int64_t>& a, const IndexKey& key) {
    const bool ae = a.first.values.empty(), ke = key.values.empty();
    if (ae || ke) return ae && !ke;
    return Ascending ? CompareKeyOnly(a.first, key) < 0
                     : CompareKeyOnly(a.first, key) > 0;
}

void InsertOrdered(std::vector<std::pair<IndexKey, int64_t>>& v,
                   const IndexKey& key, int64_t txn, bool ascending) {
    auto it = ascending
                  ? std::lower_bound(v.begin(), v.end(), key,
                                     [](const std::pair<IndexKey, int64_t>& a,
                                        const IndexKey& k) {
                                         return KeyLess<true>(a, k);
                                     })
                  : std::lower_bound(v.begin(), v.end(), key,
                                     [](const std::pair<IndexKey, int64_t>& a,
                                        const IndexKey& k) {
                                         return KeyLess<false>(a, k);
                                     });
    v.insert(it, std::make_pair(key, txn));
}

// 有序表精确删除 (key, txn)：定位同值段，段内匹配 txn。找到返回 true。
bool EraseOrdered(std::vector<std::pair<IndexKey, int64_t>>& v,
                  const IndexKey& key, int64_t txn, bool ascending) {
    auto it = ascending
                  ? std::lower_bound(v.begin(), v.end(), key,
                                     [](const std::pair<IndexKey, int64_t>& a,
                                        const IndexKey& k) {
                                         return KeyLess<true>(a, k);
                                     })
                  : std::lower_bound(v.begin(), v.end(), key,
                                     [](const std::pair<IndexKey, int64_t>& a,
                                        const IndexKey& k) {
                                         return KeyLess<false>(a, k);
                                     });
    for (; it != v.end(); ++it) {
        if (!IntervalKeyEquals(it->first, key)) break;  // 已过同值段
        if (it->second == txn) {
            v.erase(it);
            return true;
        }
    }
    return false;
}

}  // namespace

// 周期 2（G3）：增量插入。树形与分裂点保持静态，新区间按与构建相同的归属规则
// 二分下降；跨过分裂点 → 二分定位插入节点有序表；落到空子树 → 建退化叶子兜底。
size_t LockManager::PredicateTreeInsert(PredicateTable& t,
                                        const Interval& iv) const {
    int32_t idx = 0;
    size_t steps = 0;
    for (;;) {
        ++steps;
        const IntervalNode& n0 = t.nodes[static_cast<size_t>(idx)];
        if (n0.split_idx < 0) {
            // 退化节点：直接追加 fallback（区间少，线性）。
            t.nodes[static_cast<size_t>(idx)].fallback.push_back(iv);
            return steps;
        }
        const IndexKey& split = t.lo_values[static_cast<size_t>(n0.split_idx)];
        const bool lo_le =
            iv.lo.values.empty() || CompareKeyOnly(iv.lo, split) <= 0;
        const bool hi_ge =
            iv.hi.values.empty() || CompareKeyOnly(iv.hi, split) >= 0;
        if (lo_le && hi_ge) {
            IntervalNode& n = t.nodes[static_cast<size_t>(idx)];
            InsertOrdered(n.by_lo_asc, iv.lo, iv.txn_id, true);
            InsertOrdered(n.by_hi_desc, iv.hi, iv.txn_id, false);
            return steps;
        }
        if (!iv.hi.values.empty() && CompareKeyOnly(iv.hi, split) < 0) {
            if (n0.left >= 0) { idx = n0.left; continue; }
            // 无左子树可落：建退化叶子。注意 push_back 会让 n0 悬垂，故用下标回填。
            const int32_t leaf = static_cast<int32_t>(t.nodes.size());
            t.nodes.push_back(IntervalNode{});
            t.nodes[static_cast<size_t>(leaf)].fallback.push_back(iv);
            t.nodes[static_cast<size_t>(idx)].left = leaf;
            return steps;
        } else {
            if (n0.right >= 0) { idx = n0.right; continue; }
            const int32_t leaf = static_cast<int32_t>(t.nodes.size());
            t.nodes.push_back(IntervalNode{});
            t.nodes[static_cast<size_t>(leaf)].fallback.push_back(iv);
            t.nodes[static_cast<size_t>(idx)].right = leaf;
            return steps;
        }
    }
}

// 周期 2（G3）：增量删除。同规则二分下降，从节点有序表 / 退化 fallback 精确剔除。
bool LockManager::PredicateTreeRemove(PredicateTable& t,
                                      const Interval& iv) const {
    int32_t idx = 0;
    for (;;) {
        const IntervalNode& n0 = t.nodes[static_cast<size_t>(idx)];
        if (n0.split_idx < 0) {
            std::vector<Interval>& fb = t.nodes[static_cast<size_t>(idx)].fallback;
            for (auto it = fb.begin(); it != fb.end(); ++it) {
                if (it->txn_id == iv.txn_id &&
                    IntervalKeyEquals(it->lo, iv.lo) &&
                    IntervalKeyEquals(it->hi, iv.hi)) {
                    fb.erase(it);
                    return true;
                }
            }
            return false;
        }
        const IndexKey& split = t.lo_values[static_cast<size_t>(n0.split_idx)];
        const bool lo_le =
            iv.lo.values.empty() || CompareKeyOnly(iv.lo, split) <= 0;
        const bool hi_ge =
            iv.hi.values.empty() || CompareKeyOnly(iv.hi, split) >= 0;
        if (lo_le && hi_ge) {
            IntervalNode& n = t.nodes[static_cast<size_t>(idx)];
            const bool r1 = EraseOrdered(n.by_lo_asc, iv.lo, iv.txn_id, true);
            const bool r2 = EraseOrdered(n.by_hi_desc, iv.hi, iv.txn_id, false);
            return r1 && r2;
        }
        if (!iv.hi.values.empty() && CompareKeyOnly(iv.hi, split) < 0) {
            if (n0.left >= 0) { idx = n0.left; continue; }
            return false;
        } else {
            if (n0.right >= 0) { idx = n0.right; continue; }
            return false;
        }
    }
}

// 周期 2（G3）：增量同步。把已构建的树从 old_set 更新到 new_set：
// 先删消失的区间、再插新增的区间，均按二分定位 O(D log P)；树未构建或
// 删除失配（树不同步）时置 dirty，下次查询整树重建兜底。
void LockManager::PredicateTreeSync(PredicateTable& t,
                                    const std::vector<Interval>& old_set,
                                    const std::vector<Interval>& new_set) const {
    if (t.dirty || t.nodes.empty()) {
        t.dirty = true;
        return;
    }
    // 差异计算：以 (lo|hi|txn) 为键做集合差值。P 通常不大（同事务区间合并后
    // 收敛），O(P) 建哈希可接受；增量更新本身 O(D log P)。
    const auto key_of = [](const Interval& iv) {
        std::string k = iv.lo.ToString();
        k.push_back('|');
        k += iv.hi.ToString();
        k.push_back('|');
        k += std::to_string(iv.txn_id);
        return k;
    };
    std::unordered_set<std::string> old_keys, new_keys;
    old_keys.reserve(old_set.size());
    new_keys.reserve(new_set.size());
    for (const Interval& iv : old_set) old_keys.insert(key_of(iv));
    for (const Interval& iv : new_set) new_keys.insert(key_of(iv));

    for (const Interval& iv : old_set) {
        if (new_keys.count(key_of(iv))) continue;
        if (!PredicateTreeRemove(t, iv)) {
            t.dirty = true;  // 树与源不同步（理论不应发生）：重建兜底。
            return;
        }
    }
    size_t inserts = 0;
    for (const Interval& iv : new_set) {
        if (old_keys.count(key_of(iv))) continue;
        predicate_insert_steps_ += PredicateTreeInsert(t, iv);
        ++inserts;
    }
    // 退化控制：累计增量插入超过阈值（64 条且超过当前区间数）→ 置 dirty，
    // 让下次查询整树重建，摊薄长期增量导致的树形失衡。
    t.pending_inserts_ += inserts;
    if (t.pending_inserts_ > 64 && t.pending_inserts_ > t.intervals.size()) {
        t.dirty = true;
    }
}

size_t LockManager::GetPredicateRebuildCount() const {
    return predicate_rebuild_count_.load();
}

size_t LockManager::GetPredicateInsertSteps() const {
    return predicate_insert_steps_.load();
}

void LockManager::ResetPredicateInsertCounters() {
    predicate_rebuild_count_.store(0);
    predicate_insert_steps_.store(0);
}

}  // namespace sqlcompiler
