// LockManager 实现：S/X 锁 + 等待图死锁检测 + 可选阻塞超时。
// 参见 include/storage/LockManager.h 的设计说明。

#include "storage/LockManager.h"

#include <algorithm>

namespace sqlcompiler {

namespace {
// 锁升级基准阈值：中型表（登记行数 256..4096）在持有该数行写锁后尝试升级为表锁。
// 见 LockManager::ComputeEscalationThreshold 的自适应规则。
constexpr size_t kBaseEscalationThreshold = 128;
}  // namespace

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
bool LockManager::HierarchyConflicts(int64_t txn_id, int64_t res_id,
                                     LockMode mode) const {
    if (res_id < 0) {
        // 行锁：检查所属表的表级锁。
        auto g = row_group_.find(res_id);
        if (g == row_group_.end()) return false;  // 未登记归属
        auto tit = locks_.find(g->second);
        if (tit == locks_.end()) return false;
        for (const auto& [hid, hmode] : tit->second.holders) {
            if (hid == txn_id) continue;
            // 表 X 与任何行锁冲突；表 S 与行 X 冲突（行 S 兼容）。
            if (hmode == LockMode::kExclusive) return true;
            if (hmode == LockMode::kShared && mode == LockMode::kExclusive) return true;
        }
        return false;
    }
    // 表锁：检查本表下已登记行锁的持有者。
    auto rows = table_rows_.find(res_id);
    if (rows == table_rows_.end()) return false;
    for (int64_t row : rows->second) {
        auto rit = locks_.find(row);
        if (rit == locks_.end()) continue;
        for (const auto& [hid, hmode] : rit->second.holders) {
            if (hid == txn_id) continue;
            if (mode == LockMode::kExclusive) return true;  // 表 X 与任何行锁冲突
            if (hmode == LockMode::kExclusive) return true; // 表 S 与行 X 冲突
        }
    }
    return false;
}

void LockManager::LinkWaitEdges(int64_t txn_id, int64_t res_id, LockMode mode) {
    // 同资源冲突持有者。
    auto it = locks_.find(res_id);
    if (it != locks_.end()) {
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
            auto tit = locks_.find(g->second);
            if (tit != locks_.end()) {
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
                auto rit = locks_.find(row);
                if (rit == locks_.end()) continue;
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
    std::unordered_set<int64_t> visited, onpath;
    onpath.insert(txn_id);
    return Dfs(txn_id, txn_id, onpath, visited);
}

LockResult LockManager::Acquire(int64_t txn_id, int64_t res_id, LockMode mode,
                                int wait_ms, bool block_try) {
    if (txn_id < 0) return LockResult::kDeadlock;  // 非法 txn_id 一律不进锁表
    std::unique_lock<std::mutex> lk(mutex_);
    const auto deadline =
        (wait_ms > 0)
            ? std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(wait_ms)
            : std::chrono::steady_clock::time_point::max();

    for (;;) {
        LockState& st = locks_[res_id];
        // 授权条件：同资源不冲突 且 多粒度层级不冲突。
        if (!Conflicts(st, txn_id, mode) && !HierarchyConflicts(txn_id, res_id, mode)) {
            // 授予：成为持有者；清除本 txn 的全部等待边（含同资源与层级边，
            // 此刻它已不再等待任何人，残留边会污染后续死锁检测）。
            st.holders.emplace(txn_id, mode);
            waits_on_.erase(txn_id);
            return LockResult::kGranted;
        }

        // 冲突：登记自己为等待者，并链接等待边（供死锁检测）。
        bool already_waiting = false;
        for (auto& w : st.waiters) {
            if (w.first == txn_id) { already_waiting = true; break; }
        }
        if (!already_waiting) st.waiters.emplace_back(txn_id, mode);
        LinkWaitEdges(txn_id, res_id, mode);

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
            cv_.wait(lk);
            continue;
        }
        if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) {
            // 超时：撤销等待登记并返回。
            for (auto it = st.waiters.begin(); it != st.waiters.end(); ++it) {
                if (it->first == txn_id) { st.waiters.erase(it); break; }
            }
            UnlinkWaitEdges(txn_id);
            return LockResult::kTimeout;
        }
    }
}

LockResult LockManager::LockShared(int64_t txn_id, int64_t res_id, int wait_ms) {
    return Acquire(txn_id, res_id, LockMode::kShared, wait_ms, false);
}

LockResult LockManager::LockExclusive(int64_t txn_id, int64_t res_id, int wait_ms) {
    return Acquire(txn_id, res_id, LockMode::kExclusive, wait_ms, false);
}

LockResult LockManager::TryLockShared(int64_t txn_id, int64_t res_id) {
    return Acquire(txn_id, res_id, LockMode::kShared, 0, true);
}

LockResult LockManager::TryLockExclusive(int64_t txn_id, int64_t res_id) {
    return Acquire(txn_id, res_id, LockMode::kExclusive, 0, true);
}

void LockManager::Unlock(int64_t txn_id, int64_t res_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = locks_.find(res_id);
    if (it == locks_.end()) return;
    LockState& st = it->second;
    st.holders.erase(txn_id);

    // 依次为可授予的等待者重新评估（每授予一个就重查，保持兼容性）。
    bool progress = true;
    while (progress) {
        progress = false;
        for (auto w = st.waiters.begin(); w != st.waiters.end(); ++w) {
            if (!Conflicts(st, w->first, w->second) &&
                !HierarchyConflicts(w->first, res_id, w->second)) {
                st.holders.emplace(w->first, w->second);
                // 释放该资源上被解除的等待边。
                for (const auto& [hid, hmode] : st.holders) {
                    (void)hmode;
                    if (hid != w->first) waits_on_[w->first].erase(hid);
                }
                st.waiters.erase(w);
                progress = true;
                break;
            }
        }
    }
    if (st.waiters.empty() && st.holders.empty()) locks_.erase(res_id);
    cv_.notify_all();
}

void LockManager::UnlockAll(int64_t txn_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (txn_id < 0) return;
    std::vector<int64_t> owned;
    std::vector<int64_t> waiting_on;
    for (const auto& [res, st] : locks_) {
        if (st.holders.count(txn_id)) owned.push_back(res);
        for (const auto& w : st.waiters) {
            if (w.first == txn_id) { waiting_on.push_back(res); break; }
        }
    }
    for (int64_t res : owned) {
        LockState& st = locks_[res];
        st.holders.erase(txn_id);
        // 重新评估该资源的等待者。
        bool progress = true;
        while (progress) {
            progress = false;
            for (auto w = st.waiters.begin(); w != st.waiters.end(); ++w) {
                if (!Conflicts(st, w->first, w->second) &&
                    !HierarchyConflicts(w->first, res, w->second)) {
                    st.holders.emplace(w->first, w->second);
                    for (const auto& [hid, hmode] : st.holders) {
                        (void)hmode;
                        if (hid != w->first) waits_on_[w->first].erase(hid);
                    }
                    st.waiters.erase(w);
                    progress = true;
                    break;
                }
            }
        }
        if (st.waiters.empty() && st.holders.empty()) locks_.erase(res);
    }
    // 撤销 txn 在所有资源上的等待登记。
    for (int64_t res : waiting_on) {
        auto it = locks_.find(res);
        if (it != locks_.end()) {
            for (auto w = it->second.waiters.begin(); w != it->second.waiters.end(); ++w) {
                if (w->first == txn_id) { it->second.waiters.erase(w); break; }
            }
        }
    }
    // 撤销 txn 持有的 SERIALIZABLE 谓词锁，并唤醒在谓词上阻塞的写者。
    // Phase 5：谓词按 (表, 列) 存于 pred_tables_（表级全表谓词 + 每列区间树），
    // 逐表剔除该事务：先删表级全表哨兵，再逐列删区间、清空空列容器。
    for (auto it = pred_tables_.begin(); it != pred_tables_.end();) {
        PredicateTableGroup& g = it->second;
        g.full_holders.erase(
            std::remove(g.full_holders.begin(), g.full_holders.end(), txn_id),
            g.full_holders.end());
        for (auto cit = g.columns.begin(); cit != g.columns.end();) {
            PredicateTable& t = cit->second;
            t.intervals.erase(
                std::remove_if(t.intervals.begin(), t.intervals.end(),
                               [txn_id](const Interval& iv) {
                                   return iv.txn_id == txn_id;
                               }),
                t.intervals.end());
            if (t.intervals.empty()) {
                cit = g.columns.erase(cit);
            } else {
                t.dirty = true;  // 源变了：下次查询惰性重建区间树
                ++cit;
            }
        }
        if (g.full_holders.empty() && g.columns.empty()) {
            it = pred_tables_.erase(it);
        } else {
            ++it;
        }
    }
    // 撤销 txn 的行级锁升级标记（事务结束后表锁随 locks_ 释放，标记不再有效）。
    escalated_tables_.erase(txn_id);
    UnlinkWaitEdges(txn_id);
    cv_.notify_all();
}

bool LockManager::IsLockHeld(int64_t txn_id, int64_t res_id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = locks_.find(res_id);
    if (it == locks_.end()) return false;
    return it->second.holders.count(txn_id) > 0;
}

void LockManager::RegisterRowGroup(int64_t row_res, int64_t table_res) {
    std::lock_guard<std::mutex> lk(mutex_);
    row_group_[row_res] = table_res;
    table_rows_[table_res].insert(row_res);
}

size_t LockManager::CountRowLocks(int64_t txn_id, int64_t table_res) const {
    std::lock_guard<std::mutex> lk(mutex_);
    size_t n = 0;
    auto rows = table_rows_.find(table_res);
    if (rows == table_rows_.end()) return 0;
    for (int64_t row : rows->second) {
        auto it = locks_.find(row);
        if (it == locks_.end()) continue;
        if (it->second.holders.count(txn_id) > 0) ++n;
    }
    return n;
}

size_t LockManager::CountPredicateLocks(int64_t txn_id, int64_t table_rid) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = pred_tables_.find(table_rid);
    if (it == pred_tables_.end()) return 0;
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
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = pred_tables_.find(table_rid);
    if (it == pred_tables_.end()) return 0;
    const PredicateTableGroup& g = it->second;
    size_t n = g.full_holders.size();
    for (const auto& kv : g.columns) {
        n += kv.second.intervals.size();
    }
    return n;
}

size_t LockManager::GetPredicateQueryComparisons() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return predicate_query_comparisons_;
}

void LockManager::ResetPredicateQueryComparisons() {
    std::lock_guard<std::mutex> lk(mutex_);
    predicate_query_comparisons_ = 0;
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
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = table_rows_.find(table_res);
    return (it == table_rows_.end()) ? 0 : it->second.size();
}

size_t LockManager::GetTableConflictCount(int64_t table_res) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = table_escalation_conflicts_.find(table_res);
    return (it == table_escalation_conflicts_.end()) ? 0 : it->second;
}

bool LockManager::IsTableEscalated(int64_t txn_id, int64_t table_res) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = escalated_tables_.find(txn_id);
    if (it == escalated_tables_.end()) return false;
    return it->second.count(table_res) > 0;
}

LockResult LockManager::TryEscalateTable(int64_t txn_id, int64_t table_res,
                                         LockMode mode) {
    if (txn_id < 0) return LockResult::kDeadlock;
    std::lock_guard<std::mutex> lk(mutex_);

    // 已升级/已持表锁：幂等成功。
    auto et = escalated_tables_.find(txn_id);
    if (et != escalated_tables_.end() && et->second.count(table_res) > 0) {
        return LockResult::kGranted;
    }
    auto tit = locks_.find(table_res);
    if (tit != locks_.end() && tit->second.holders.count(txn_id) > 0) {
        escalated_tables_[txn_id].insert(table_res);
        return LockResult::kGranted;
    }

    // 升级条件：表锁与层级（本表行锁）都不能有他人冲突持有。
    // 层级检查：本表下任一被他人持有的行锁都会挡住表级 X；表级 S 仅被行 X 挡住。
    if (tit != locks_.end() && Conflicts(tit->second, txn_id, mode)) {
        ++table_escalation_conflicts_[table_res];  // 冲突采样：他人持表锁
        return LockResult::kWouldBlock;  // 他人持表锁
    }
    if (HierarchyConflicts(txn_id, table_res, mode)) {
        ++table_escalation_conflicts_[table_res];  // 冲突采样：他人持本表行锁
        return LockResult::kWouldBlock;  // 他人持本表行锁
    }

    // 授予表锁。
    LockState& st = locks_[table_res];
    st.holders.emplace(txn_id, mode);
    waits_on_.erase(txn_id);

    // 释放本事务在本表上的全部行锁（升级后由表锁覆盖）。
    auto rows = table_rows_.find(table_res);
    if (rows != table_rows_.end()) {
        std::vector<int64_t> to_unlock;
        for (int64_t row : rows->second) {
            auto rit = locks_.find(row);
            if (rit != locks_.end() && rit->second.holders.count(txn_id) > 0) {
                to_unlock.push_back(row);
            }
        }
        for (int64_t row : to_unlock) {
            auto it = locks_.find(row);
            if (it == locks_.end()) continue;
            it->second.holders.erase(txn_id);
            // 重新评估该行上的等待者。
            bool progress = true;
            while (progress) {
                progress = false;
                for (auto w = it->second.waiters.begin(); w != it->second.waiters.end(); ++w) {
                    if (!Conflicts(it->second, w->first, w->second) &&
                        !HierarchyConflicts(w->first, row, w->second)) {
                        it->second.holders.emplace(w->first, w->second);
                        for (const auto& [hid, hmode] : it->second.holders) {
                            (void)hmode;
                            if (hid != w->first) waits_on_[w->first].erase(hid);
                        }
                        it->second.waiters.erase(w);
                        progress = true;
                        break;
                    }
                }
            }
            if (it->second.waiters.empty() && it->second.holders.empty()) locks_.erase(row);
        }
    }
    escalated_tables_[txn_id].insert(table_res);
    cv_.notify_all();
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
    std::lock_guard<std::mutex> lk(mutex_);

    // Phase 5：谓词按 (表, 列) 组织为 PredicateTableGroup{表级全表谓词 + 每列区间树}。
    // 合并/继承规约逻辑与 v2 一致，仅存储容器泛化到「列」维度。
    PredicateTableGroup& g = pred_tables_[table_rid];

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
            std::vector<Interval> kept;
            for (const Interval& iv : t.intervals) {
                if (iv.txn_id != txn_id) kept.push_back(iv);
            }
            if (kept.empty()) {
                cit = g.columns.erase(cit);
            } else {
                t.intervals = std::move(kept);
                t.dirty = true;
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
    t.dirty = true;
    return LockResult::kGranted;
}

void LockManager::GatherColumnConflicts(int64_t table_rid, int32_t column,
                                        const IndexKey& key,
                                        std::vector<int64_t>* out) {
    auto tit = pred_tables_.find(table_rid);
    if (tit == pred_tables_.end()) return;
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
    std::unique_lock<std::mutex> lk(mutex_);
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
        GatherColumnConflicts(table_rid, column, key, &conflicts);
        std::sort(conflicts.begin(), conflicts.end());
        conflicts.erase(std::unique(conflicts.begin(), conflicts.end()),
                        conflicts.end());
        conflicts.erase(std::remove(conflicts.begin(), conflicts.end(), txn_id),
                        conflicts.end());
        if (conflicts.empty()) return LockResult::kGranted;

        // 建立本事务指向各冲突谓词持有者的等待边。
        for (int64_t c : conflicts) waits_on_[txn_id].insert(c);
        if (DeadlockCycle(txn_id)) {
            UnlinkWaitEdges(txn_id);
            return LockResult::kDeadlock;
        }
        if (wait_ms == 0) {
            // 无限阻塞至谓词持有者提交（UnlockAll 唤醒后重试）。
            cv_.wait(lk);
        } else if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) {
            UnlinkWaitEdges(txn_id);
            return LockResult::kTimeout;
        }
        // 被唤醒后清除指向冲突持有者的边，重查谓词集合（可能已提交）。
        for (int64_t c : conflicts) waits_on_[txn_id].erase(c);
    }
}

LockResult LockManager::CheckWritePredicateRow(int64_t txn_id, int64_t table_rid,
                                               const std::vector<Value>& row,
                                               int wait_ms) {
    if (txn_id < 0) return LockResult::kDeadlock;
    std::unique_lock<std::mutex> lk(mutex_);
    const auto deadline =
        (wait_ms > 0)
            ? std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(wait_ms)
            : std::chrono::steady_clock::time_point::max();

    for (;;) {
        // 整行写检查：表级全表谓词命中任意写；逐列对该列值做区间树 stabbing。
        // 一次加锁内收集全部冲突持有者（不重复逐列取全局互斥）。
        std::vector<int64_t> conflicts;
        auto tit = pred_tables_.find(table_rid);
        if (tit != pred_tables_.end()) {
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
        for (int64_t c : conflicts) waits_on_[txn_id].insert(c);
        if (DeadlockCycle(txn_id)) {
            UnlinkWaitEdges(txn_id);
            return LockResult::kDeadlock;
        }
        if (wait_ms == 0) {
            cv_.wait(lk);
        } else if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) {
            UnlinkWaitEdges(txn_id);
            return LockResult::kTimeout;
        }
        for (int64_t c : conflicts) waits_on_[txn_id].erase(c);
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
        // 查询只按 hi >= key 过滤（lo 开放恒满足）。
        IntervalNode n;
        for (const Interval& iv : t.intervals) {
            n.by_lo_asc.emplace_back(iv.lo, iv.txn_id);
            n.by_hi_desc.emplace_back(iv.hi, iv.txn_id);
        }
        std::sort(n.by_lo_asc.begin(), n.by_lo_asc.end(),
                  [](const std::pair<IndexKey, int64_t>& a,
                     const std::pair<IndexKey, int64_t>& b) {
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
        t.nodes.push_back(std::move(n));
        t.dirty = false;
        return;
    }
    PredicateTreeBuildRange(t, 0, t.lo_values.size(), t.intervals);
    t.dirty = false;
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
        if (lo_begin < static_cast<size_t>(split_pos)) {
            left = PredicateTreeBuildRange(t, lo_begin,
                                           static_cast<size_t>(split_pos), left_c);
        }
        if (static_cast<size_t>(split_pos) + 1 < lo_end) {
            right = PredicateTreeBuildRange(t, static_cast<size_t>(split_pos) + 1,
                                            lo_end, right_c);
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
            // 退化单节点：全区间下界开放（-inf），只需 hi >= key。
            for (const auto& pr : n.by_hi_desc) {
                if (pr.first.values.empty() || CompareKeyOnly(pr.first, key) >= 0)
                    out->push_back(pr.second);
                else break;
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

}  // namespace sqlcompiler