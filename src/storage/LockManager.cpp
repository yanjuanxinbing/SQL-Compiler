// LockManager 实现：S/X 锁 + 等待图死锁检测 + 可选阻塞超时。
// 参见 include/storage/LockManager.h 的设计说明。

#include "storage/LockManager.h"

#include <algorithm>

namespace sqlcompiler {

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

void LockManager::LinkWaitEdges(int64_t txn_id, int64_t res_id, LockMode mode) {
    auto it = locks_.find(res_id);
    if (it == locks_.end()) return;
    for (const auto& [hid, hmode] : it->second.holders) {
        if (hid != txn_id && !ModeCompatible(mode, hmode)) {
            waits_on_[txn_id].insert(hid);
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
        if (!Conflicts(st, txn_id, mode)) {
            // 授予：成为持有者；清除本 txn 因等待该资源而建立的等待边。
            st.holders.emplace(txn_id, mode);
            for (const auto& [hid, hmode] : st.holders) {
                (void)hmode;
                if (hid != txn_id) waits_on_[txn_id].erase(hid);
            }
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
            if (!Conflicts(st, w->first, w->second)) {
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
                if (!Conflicts(st, w->first, w->second)) {
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
    for (auto it = pred_locks_.begin(); it != pred_locks_.end();) {
        if (it->txn_id == txn_id) it = pred_locks_.erase(it);
        else ++it;
    }
    UnlinkWaitEdges(txn_id);
    cv_.notify_all();
}

bool LockManager::IsLockHeld(int64_t txn_id, int64_t res_id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = locks_.find(res_id);
    if (it == locks_.end()) return false;
    return it->second.holders.count(txn_id) > 0;
}

bool LockManager::PredicateCovers(const PredicateLock& p, const IndexKey& key) const {
    if (p.is_full) return true;
    return CompareKeyOnly(key, p.lo) >= 0 && CompareKeyOnly(key, p.hi) <= 0;
}

LockResult LockManager::AcquireReadPredicate(int64_t txn_id, int64_t table_rid,
                                             bool is_full, const IndexKey& lo,
                                             const IndexKey& hi) {
    if (txn_id < 0) return LockResult::kDeadlock;
    std::lock_guard<std::mutex> lk(mutex_);
    pred_locks_.push_back(PredicateLock{txn_id, table_rid, is_full, lo, hi});
    return LockResult::kGranted;
}

LockResult LockManager::CheckWritePredicate(int64_t txn_id, int64_t table_rid,
                                            const IndexKey& key, int wait_ms) {
    if (txn_id < 0) return LockResult::kDeadlock;
    std::unique_lock<std::mutex> lk(mutex_);
    const auto deadline =
        (wait_ms > 0)
            ? std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(wait_ms)
            : std::chrono::steady_clock::time_point::max();

    for (;;) {
        // 收集其他活动事务持有、且覆盖该键的谓词 → 冲突持有者集合（去重）。
        std::vector<int64_t> conflicts;
        for (const PredicateLock& p : pred_locks_) {
            if (p.txn_id == txn_id) continue;
            if (p.table_rid != table_rid) continue;
            if (PredicateCovers(p, key) &&
                std::find(conflicts.begin(), conflicts.end(), p.txn_id) == conflicts.end()) {
                conflicts.push_back(p.txn_id);
            }
        }
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

}  // namespace sqlcompiler