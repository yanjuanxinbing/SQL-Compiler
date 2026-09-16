#pragma once

// =============================================================================
// Page — 缓冲池中的物理页帧（frame）
//
// 职责：在内存中承载磁盘上一页的内容（PAGE_SIZE = 4096 字节），并携带该帧的元信息：
//       绑定的 page_id、脏标记、pin 计数、页级 LSN、页级读写闩与若干观测计数。
//
// 持久化布局：
//   * 页大小固定 4KB；page_id 即数据文件中的第 page_id 块（偏移 page_id * PAGE_SIZE）；
//   * 磁盘页镜像只保存 data_ 的字节，不含 page_lsn_ / is_dirty_ / pin_count_ 等元信息，
//     因此页从磁盘重新装入后 page_lsn_ 必须归零（见 BufferPoolManager::GetPage）。
//
// 必须维持的不变量与约束：
//   * pin_count_ 恒 >= 0；pin_count_ > 0 的帧不得被替换器选为 victim（BPM 只在计数
//     归零时调用 Replacer::Unpin 把该帧交回候选集）；
//   * 页闩只保护 data_ 的并发访问，Page 自身不加锁、不自旋：GetData() 是无锁裸访问，
//     单线程路径可直接使用，多线程路径必须由上层 RAII 句柄持闩后访问；
//   * 锁序约定：页闩由上层 RAII 句柄（PageGuard / LatchedPageGuard）持有，且固定
//     「先释放页闩，再调用 BufferPoolManager 的任一方法（如 UnpinPage）」——即持页闩
//     期间不进入缓冲池。缓冲池自身不参与页闩协议，其写回/淘汰路径会不持页闩直接读写
//     帧数据，所以该顺序必须由调用方遵守；
//   * data_ 与 page_lsn_ 成对更新：页内容随某条 WAL 记录修改后，先把该记录的 LSN 写入
//     SetPageLsn()，写回磁盘时据此执行「先日志后数据」（WAL-before-data）检查；
//   * ResetMemory() 清零数据与全部元信息（含 page_lsn_ 与观测计数），但不重建 latch_。
//
// 线程模型：只有 access_count_ 是原子量（允许多线程累加）；data_ / page_id_ /
//   is_dirty_ / pin_count_ / page_lsn_ / dirty_since_tick_ 均需外部同步（页闩或单线程）。
// =============================================================================

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>

namespace sqlcompiler {

// 页号类型：数据文件中页块的逻辑编号，从 0 起单调递增。
using page_id_t = int32_t;
// 无效页号哨兵：表示「尚未绑定任何磁盘页」或「分配/查找失败」。
constexpr page_id_t INVALID_PAGE_ID = -1;
constexpr size_t PAGE_SIZE = 4096;  // 固定页大小：4KB

// 表示内存中的一个物理页帧（frame），承载磁盘上某一页的数据
class Page {
public:
    // 构造一个未绑定磁盘页的空帧：data_ 清零，page_id_ = INVALID_PAGE_ID，
    // 脏标记 / pin 计数 / page_lsn_ / 观测计数全部归零。
    Page();

    // 返回本帧当前绑定的页号。
    // @return 绑定的 page_id；空帧（未被任何页占用）返回 INVALID_PAGE_ID。
    page_id_t GetPageId() const;
    // 把本帧绑定到指定页号。
    // @param page_id 目标页号，合法值 >= 0；空帧可用 INVALID_PAGE_ID 表示未绑定。
    // @note 纯元信息赋值：不改动 data_ 内容，也不触碰脏标记与 LSN。
    void SetPageId(page_id_t page_id);

    // 取得可写的页数据缓冲区。
    // @return 指向帧内缓冲区的裸指针，长度恒为 PAGE_SIZE，永不为 nullptr。
    // @note 无锁访问：并发修改必须由调用方持页写闩（WLatch）串行化。
    char* GetData();
    // 取得只读的页数据缓冲区。
    // @return 指向帧内缓冲区的常量指针，长度恒为 PAGE_SIZE，永不为 nullptr。
    // @note 同样是无锁裸访问；并发场景请持页读闩（RLatch）后使用。
    const char* GetData() const;

    // 本帧内容自上次写回后是否被修改过；决定淘汰/写回时是否必须执行磁盘写入。
    bool IsDirty() const;
    // 设置脏标记。
    // @param dirty true —— 内容与磁盘不一致；false —— 通常表示「刚写回，与磁盘一致」。
    // @note 直接赋值，不更新 dirty_since_tick_（需要年龄观测请用 MarkDirtyFromClean）。
    void SetDirty(bool dirty);

    // 返回当前 pin 计数（本帧被上层持有的引用数）。
    int GetPinCount() const;
    // pin 计数加一；BufferPoolManager::GetPage / NewPage 在返回帧指针前会调用一次。
    // @note 无上限，也不与页闩联动：调用方须配对调用 DecPinCount / UnpinPage。
    void IncPinCount();
    // pin 计数减一。
    // @note 计数已为 0 时保持 0（防御性钳制，不会变负）；计数归零后该帧才可被淘汰，
    //       把帧交回替换器候选集的动作由 BufferPoolManager::UnpinPage 负责。
    void DecPinCount();

    // ---- Phase B：页级 LSN（Log Sequence Number） ----
    // 每个 page 帧记录最后一次写入该页的 log record 的 LSN。
    // 当 WAL 的 durability 要求「先 log 再 page」时（ARIES 的 WAL-before-data
    // 规则），BufferPool 在 FlushPage 之前必须保证 page.page_lsn_ <= log.durable_lsn_。
    // 反过来，恢复时 redo 阶段用 (page.page_lsn_ < record.lsn) 来判断「这条日志
    // 之后该页没被写过」——避免把同一个 page-image 重放多次。
    // 读取本帧的页 LSN。
    // @return 最后一次修改本帧的 WAL 记录 LSN；0 表示「新帧或刚从磁盘装入，未参与 redo」。
    uint64_t GetPageLsn() const { return page_lsn_; }
    // 写入本帧的页 LSN。
    // @param lsn 本次修改对应的 WAL 记录 LSN（由 WAL 写出方设置，PageGuard::SetPageLsn
    //            即转发到此处）。
    // @note 调用方义务：必须在页内容被修改后、写回磁盘之前设置，否则 WAL-before-data
    //       检查会漏判，崩溃后该页的修改无法被 redo 恢复。
    void SetPageLsn(uint64_t lsn) { page_lsn_ = lsn; }

    // ---- E4：页级读写锁（latch） ----
    // 每个物理帧带一把读写锁（std::shared_mutex），支持共享读 / 独占写两种访问。
    // 它保护「页数据 data_ 的并发访问」：多线程可持读锁共享读同一帧，写修改须持
    // 写锁独占。锁由 RAII 句柄（PageReadGuard / PageWriteGuard）在访问期间持有，
    // 不直接由 Page 自锁（GetData 保持无锁裸访问，向后兼容单线程路径）。
    // 加共享读闩（可重入共享，不可与写闩并存）。
    void RLatch() { latch_.lock_shared(); }
    // 释放共享读闩；必须与 RLatch() 在同一线程配对调用。
    void RUnlatch() { latch_.unlock_shared(); }
    // 加独占写（同一帧的读写闩互斥）。
    void WLatch() { latch_.lock(); }
    // 释放独占写闩；必须与 WLatch() 在同一线程配对调用。
    void WUnlatch() { latch_.unlock(); }
    // 取得本帧的读写闩本体，供上层 RAII 句柄（如 std::shared_lock）托管。
    // @return 帧内 latch_ 的可变引用（生命周期等于本 Page 对象）。
    std::shared_mutex& GetLatch() { return latch_; }
    // 只读重载：返回 const 引用，供只读场景取闩。
    const std::shared_mutex& GetLatch() const { return latch_; }

    // ---- Phase 4（创新特性 F）：页访问温度 ----
    // 记录本帧的累计访问次数，作为「温度」近似（访问越频繁 = 越热）。计数由调用方
    // 显式调用 RecordAccess() 维护：Page 不感知缓冲池的取页路径，不会自动累加。
    // 潜在用途：让刷盘/淘汰优先保留热页（NO-FORCE + WAL 保证：热脏页即便不落盘，
    // 崩溃后也能由 WAL redo 恢复）。
    // 访问计数加一（原子自增，多线程安全）。
    void RecordAccess() { ++access_count_; }
    // 读取累计访问次数（原子读）。
    // @return 自上次 ResetMemory 以来的访问次数；新装入/新分配的帧为 0。
    uint64_t GetAccessCount() const { return access_count_.load(); }

    // ---- T4 可观测性：脏页年龄 ----
    // 记录「变脏时刻」的逻辑时钟刻度（由调用方提供的 op_tick，例如池操作序号）；
    // 仅当页从干净变脏时更新一次（幂等）。写回（SetDirty(false)）不清零——年龄
    // 分布只统计「当前仍脏」的帧，读取方需先判 IsDirty()。ResetMemory 时清零。
    // 注：绕过本方法直接 SetDirty(true) 的路径（恢复/undo 等）不更新本字段，年龄
    // 会被计为 0（即「很久以前」），仅影响观测分桶、不影响正确性。
    // 从「干净」变为「脏」时记录一次变脏刻度。
    // @param op_tick 调用方自定的逻辑时钟刻度（单位由调用方决定，仅用于分桶比较）。
    // @note 幂等：页已脏时不覆盖原有刻度；若设置为 true 的路径绕过本方法，
    //       dirty_since_tick_ 保持原值（初始为 0）。
    void MarkDirtyFromClean(int64_t op_tick);
    // 读取变脏时刻的逻辑刻度。
    // @return 变脏刻度；只有在 IsDirty() == true 时才有意义（已写回或从未变脏的帧
    //         返回上次记录值，ResetMemory 后为 0）。
    int64_t GetDirtySinceTick() const { return dirty_since_tick_; }

    // 重置页内容与元信息为初始状态，供缓冲池复用该帧时调用
    // @note 清零 data_ 全部 PAGE_SIZE 字节，并把 page_id_ / is_dirty_ / pin_count_ /
    //       page_lsn_ / access_count_ / dirty_since_tick_ 复位；不触碰 latch_（锁不随
    //       帧内容重建，也不可被清零）。调用方义务：帧必须已不被任何线程持有
    //       （pin_count_ 归零且不在替换器/使用中状态）才可复位。
    void ResetMemory();

private:
    page_id_t page_id_;    // 本帧当前承载的页号；INVALID_PAGE_ID 表示空帧（未绑定）
    char data_[PAGE_SIZE]; // 页内容缓冲区，长度恒为 PAGE_SIZE，无锁裸访问（靠页闩保护）
    bool is_dirty_;        // 脏标记：内容与磁盘是否不一致；决定写回时是否需要落盘
    int pin_count_;        // 引用计数：> 0 表示被上层占用，不可被替换器淘汰；恒 >= 0
    // Phase B：当前帧对应 page 上一次被任何 log record 写入时的 LSN。
    // ResetMemory 中归零表示「这是全新页，未参与过 redo」。
    uint64_t page_lsn_ = 0;
    // E4：页级读写锁。与 data_ 独立存在，不参与 ResetMemory（锁不随帧内容清零）。
    std::shared_mutex latch_;
    // Phase 4：访问温度计数。原子：调用方（可能多线程）可并发调用 RecordAccess()；
    // ResetMemory 时清零，避免帧复用后继承上一页的温度。
    std::atomic<uint64_t> access_count_{0};
    // T4：脏页年龄基准（变脏时刻的逻辑刻度；仅在 IsDirty()==true 时有效）。
    int64_t dirty_since_tick_ = 0;
};

}  // namespace sqlcompiler