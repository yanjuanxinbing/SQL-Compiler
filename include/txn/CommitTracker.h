#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    //
    // Phase 2（O(1) 快照水位）：维护单调低水位缓存 low_water_mark_ = 所有活动
    // 快照 CSN 的最小值，OldestActiveSnapshot() 退化为 O(1) 缓存读。
    //   * RegisterSnapshot：仅当它是首个快照或 CSN < 当前水位时降水位（O(1)）；
    //   * UnregisterSnapshot：仅当被注销的恰是最小水位且该 CSN 计数归零时置
    //     low_water_dirty_，由下次 OldestActiveSnapshot() 惰性重算推进（O(N) 重算
    //     只在「最小值快照注销」后发生一次，摊薄后读路径恒定 O(1)）；
    //   * 单调性：快照 CSN 由全局序列单调分配，新登记 CSN 恒 ≥ 已登记最小值，
    //     故水位只升不降（惰性重算推进方向安全）；测试可直接注册任意 CSN。
    void RegisterSnapshot(int64_t csn) {
        std::lock_guard<std::mutex> lk(m_);
        ++active_count_;
        if (active_count_ == 1) {
            low_water_mark_ = csn;
            low_water_dirty_ = false;
        } else if (csn < low_water_mark_) {
            low_water_mark_ = csn;  // 测试可注册任意 CSN；生产单调递增不会触发
            low_water_dirty_ = false;
        } else if (csn == low_water_mark_) {
            // 最小值重新出现（此前被注销过）：缓存值仍有效。
            low_water_dirty_ = false;
        }
        ++active_snapshots_[csn];
    }
    void UnregisterSnapshot(int64_t csn) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = active_snapshots_.find(csn);
        if (it == active_snapshots_.end()) return;
        if (--it->second <= 0) {
            active_snapshots_.erase(it);
            // 被注销的是当前最小值且计数归零：缓存失效，下次读取时惰性重算。
            if (csn == low_water_mark_) low_water_dirty_ = true;
        }
        --active_count_;
    }
    // 最老活动快照水位；无活动快照返回 0。
    // 注意：必须显式求最小 CSN——active_snapshots_ 是 unordered_map，begin()
    // 不保证是最小键；曾用 begin()->first 导致真空边界随机取到一个较大的快照，
    // 误删老快照读者仍需要的版本（脏读/丢行）。Phase 2 起改为低水位缓存：
    // 仅当 low_water_dirty_（最小值快照注销后首次读取）才做一次 O(N) 惰性重算，
    // 其余调用都是 O(1) 缓存读，不再每次扫描活动快照表。
    int64_t OldestActiveSnapshot() const {
        std::lock_guard<std::mutex> lk(m_);
        if (active_count_ <= 0) {
            low_water_mark_ = 0;
            low_water_dirty_ = false;
            return 0;
        }
        if (low_water_dirty_) {
            int64_t oldest = 0;
            for (const auto& kv : active_snapshots_) {
                if (oldest == 0 || kv.first < oldest) oldest = kv.first;
            }
            low_water_mark_ = oldest;
            low_water_dirty_ = false;
            ++low_water_recompute_count_;
        }
        return low_water_mark_;
    }

    // 当前缓存的低水位（不触发惰性重算）；仅用于观测/测试。
    int64_t GetLowWaterMark() const {
        std::lock_guard<std::mutex> lk(m_);
        return low_water_mark_;
    }
    // 惰性重算次数（仅在「最小值快照注销后首次 OldestActiveSnapshot」时 +1）；
    // 用于验证 O(1) 读缓存性质：稳定态下重算计数不变。
    int64_t GetLowWaterRecomputeCount() const {
        std::lock_guard<std::mutex> lk(m_);
        return low_water_recompute_count_;
    }

    // 活动快照 CSN 的升序列表（含重复 CSN 的多读者）。
    // Phase 3（t4 精确化）：索引真空需要的不只是「最老」快照，而是全部活动快照
    // 的 CSN——判据 (ii) 要检查「每个活动快照的可见版本键」是否等于条目键，仅凭
    // 最老水位无法覆盖（可见版本随快照 CSN 在链上推移）。低频路径（每 100 语句 /
    // 后台真空），O(N) 拷贝可接受。
    std::vector<int64_t> ActiveSnapshotList() const {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<int64_t> out;
        out.reserve(active_snapshots_.size());
        for (const auto& kv : active_snapshots_) {
            for (int32_t i = 0; i < kv.second; ++i) out.push_back(kv.first);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

private:
    mutable std::mutex m_;
    std::unordered_map<int64_t, int64_t> committed_;
    std::unordered_set<int64_t> aborted_;
    std::unordered_map<int64_t, int32_t> active_snapshots_;  // csn -> 活动快照计数
    int64_t next_csn_ = 0;
    // Phase 2：低水位缓存（mutable：OldestActiveSnapshot 为 const 但可惰性刷新）。
    mutable int64_t low_water_mark_ = 0;       // 活动快照最小 CSN 的缓存
    mutable int64_t active_count_ = 0;         // 活动快照总数（0 ⇔ 空集）
    mutable bool low_water_dirty_ = false;     // 缓存失效待重算
    mutable int64_t low_water_recompute_count_ = 0;
};

}  // namespace sqlcompiler