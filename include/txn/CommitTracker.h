#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace sqlcompiler {

// MVCC 提交跟踪器（header-only，避免新增 .cpp 触发 cmake 重配置）。
//
// 职责：
//   * 为每个 COMMIT 的事务分配唯一、单调递增的提交序号（Commit Sequence Number,
//     CSN），作为版本可见性的时间基准。
//   * 为 ABORT 的 txn 登记，使读者判定其写出的版本不可见。
//   * 供快照读者查询某 txn 是否已提交及其 CSN，据此决定某版本对快照是否可见。
//
// 线程安全：`Commit()`/`CurrentCSN()` 在同一把互斥锁下串行化，保证「提交序」
// 正确：先提交者拿到更小 CSN，快照读 CurrentCSN() 恰好停留在某个提交边界。
// 读者在 ReadLock 内查 LookupCommitted 与取 CurrentCSN，保证可见性判定原子一致。
class CommitTracker {
public:
    // 提交事务 x，分配并返回其 CSN。
    int64_t Commit(int64_t xid) {
        std::lock_guard<std::mutex> lk(m_);
        int64_t csn = ++next_csn_;
        committed_[xid] = csn;
        aborted_.erase(xid);
        return csn;
    }

    // 登记事务 x 已中止（其写出的版本对外不可见）。
    void Abort(int64_t xid) {
        std::lock_guard<std::mutex> lk(m_);
        aborted_.insert(xid);
        committed_.erase(xid);
    }

    // 查询 x 是否已提交；已提交则 *csn 填充其 CSN 并返回 true。
    bool LookupCommitted(int64_t xid, int64_t* csn) const {
        std::lock_guard<std::mutex> lk(m_);
        auto it = committed_.find(xid);
        if (it == committed_.end()) return false;
        if (csn) *csn = it->second;
        return true;
    }

    // 查询 x 是否已中止。
    bool IsAborted(int64_t xid) const {
        std::lock_guard<std::mutex> lk(m_);
        return aborted_.count(xid) > 0;
    }

    // 当前提交水位。快照读者在读前采集，作为其快照值 S。
    int64_t CurrentCSN() const {
        std::lock_guard<std::mutex> lk(m_);
        return next_csn_;
    }

    // 当前已登记的最老提交 CSN（可作真空回收的地板）；无提交返回 0。
    int64_t OldestCommittedCSN() const {
        std::lock_guard<std::mutex> lk(m_);
        int64_t old = 0;
        for (const auto& kv : committed_) {
            if (old == 0 || kv.second < old) old = kv.second;
        }
        return old;
    }

    // ---- 活动快照登记（Vacuum 的地板）----
    // 快照读者/写者在 BEGIN 后注册其快照水位，事务结束时注销。Vacuum 以
    // 「最老活动快照」为界回收更旧的版本，避免误删仍可能被读取的版本。
    void RegisterSnapshot(int64_t csn) {
        std::lock_guard<std::mutex> lk(m_);
        ++active_snapshots_[csn];
    }
    void UnregisterSnapshot(int64_t csn) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = active_snapshots_.find(csn);
        if (it == active_snapshots_.end()) return;
        if (--it->second <= 0) active_snapshots_.erase(it);
    }
    // 最老活动快照水位；无活动快照返回 0。
    int64_t OldestActiveSnapshot() const {
        std::lock_guard<std::mutex> lk(m_);
        if (active_snapshots_.empty()) return 0;
        return active_snapshots_.begin()->first;
    }

private:
    mutable std::mutex m_;
    std::unordered_map<int64_t, int64_t> committed_;
    std::unordered_set<int64_t> aborted_;
    std::unordered_map<int64_t, int32_t> active_snapshots_;  // csn -> 活动快照计数
    int64_t next_csn_ = 0;
};

}  // namespace sqlcompiler