#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "storage/DiskManager.h"
#include "storage/Page.h"
#include "storage/Replacer.h"

namespace sqlcompiler {

// 前向声明：避免 BufferPoolManager.h 引入事务模块的 <mutex> 与 LogRecord 头。
class LogManager;

// 可选的替换策略种类
enum class ReplacementPolicy { LRU, FIFO, CLOCK, LRUK };

// 缓存命中统计信息
struct BufferPoolStats {
    long hit_count = 0;
    long miss_count = 0;
    long replacement_count = 0;
    // 脏页写回次数：脏页内容被真正写回磁盘的次数（含淘汰换出、显式 Flush、
    // 全量刷脏、删除退页等路径）。供 \stats 输出「脏页写回 / IO 统计」。
    long writeback_count = 0;
    // Phase 4（创新特性 F）：刷盘温度分布——温度感知刷盘下，写回的脏页按访问
    // 温度分档：writeback_cold_count 记录「冷页（访问数 < 热阈值）写回」次数，
    // writeback_hot_count 记录「热页写回」次数（温度感知下热页被延后，只有后续
    // 显式刷/淘汰/关库才计一次）。关闭温度感知时全部记入 writeback_cold_count。
    long writeback_cold_count = 0;
    long writeback_hot_count = 0;
    // T4 可观测性：命中构成——缓冲池命中按「命中页的访问温度」分桶计数：
    //   hot = 访问数 >= 热阈值；warm = 访问数 >= 温阈值（= 热阈值/2，至少 1）；
    //   cold = 其余。用于 \stats 展示「替换策略命中构成」（替换目标 = 冷页优先）。
    long hit_cold_count = 0;
    long hit_warm_count = 0;
    long hit_hot_count = 0;

    double HitRate() const;
};

// 一条替换日志记录，便于调试与展示缓存替换过程
struct ReplacementLogEntry {
    page_id_t evicted_page_id;  // 被淘汰的页（INVALID_PAGE_ID表示淘汰的是空槽位）
    page_id_t loaded_page_id;   // 本次换入的页
    bool evicted_was_dirty;     // 被淘汰的页在换出前是否为脏页
};

// T4 诊断（\analyze）：缓冲池页映射快照条目——逻辑页在池中的帧号、脏标志与访问温度。
struct PageMapEntry {
    page_id_t page_id;
    int frame_id;
    bool dirty;
    uint64_t access_count;
};

// 缓冲池管理器：在内存中缓存固定数量的页，减少磁盘IO次数
// 对上层（存储引擎）提供 get_page / flush_page 等接口，
// 并在缓存满时依据Replacer选择的策略（LRU/FIFO）淘汰旧页
class BufferPoolManager {
public:
    BufferPoolManager(size_t pool_size, DiskManager* disk_manager,
                       ReplacementPolicy policy = ReplacementPolicy::LRU,
                       size_t lru_k = 2);
    ~BufferPoolManager();

    // 获取page_id对应的页（若不在缓存中则从磁盘加载，必要时淘汰旧页）
    // 返回的Page指针在使用完毕后必须调用UnpinPage释放，返回nullptr表示获取失败
    Page* GetPage(page_id_t page_id);

    // 通过DiskManager分配一个新页并将其放入缓冲池，page_id通过输出参数返回
    Page* NewPage(page_id_t* page_id);

    // 释放对某页的占用（pin计数减一），is_dirty表示本次使用是否修改了该页
    bool UnpinPage(page_id_t page_id, bool is_dirty);

    // 将指定页强制写回磁盘（无论是否脏页）。
    // Phase B：若 page.page_lsn_ > log.durable_lsn_，必须先 Flush 日志，
    // 否则磁盘上的 page-image 会「领先」日志，导致重启时 redo 不到原始写入。
    // 没设置 LogManager 时回退到 Phase A 行为（仅写盘）。
    bool FlushPage(page_id_t page_id);

    // 将缓冲池中所有脏页写回磁盘（不写干净页）。
    // Phase B：COMMIT 时调用此方法把脏页快速落盘，路径上自动尊重 WAL 规则。
    // 配合 DiskManager::Sync() 实现 COMMIT 语义。
    // 返回本次实际写回页数（T4：供后台刷脏直方图记账）。
    int FlushAllDirtyPages();

    // 将缓冲池中所有页写回磁盘（包括干净页）。Phase A 测试依赖此方法做
    // 测试收尾清理；保留接口。
    void FlushAllPages();

    // 删除一个页：从缓冲池中移除并通过DiskManager回收该page_id
    bool DeletePage(page_id_t page_id);

    // ---- Phase B：注入 LogManager，让 FlushPage 在写盘前尊重 WAL 规则 ----
    void SetLogManager(LogManager* lm) { log_manager_ = lm; }
    LogManager* GetLogManager() const { return log_manager_; }

    // 收集当前缓冲池中的脏页（page_id, page_lsn），供 COMMIT 路径强制落盘用。
    // 返回顺序无定义。
    std::vector<std::pair<page_id_t, uint64_t>> CollectDirtyPages();

    // 快照式读取（返回副本，而非内部引用），供 \stats 诊断命令与单测输出使用。
    // 调用方用 const-ref 绑定副本仍可编译（临时对象生命周期被延长），接口向后兼容。
    BufferPoolStats GetStats() const;
    std::vector<ReplacementLogEntry> GetReplacementLog() const;

    // 供 \stats 诊断命令输出存储统计信息使用。
    size_t GetPoolSize() const { return pool_size_; }
    static size_t GetReplacementLogCapacity() { return kMaxReplacementLog; }

    // ---- Phase 4（创新特性 F）：温度感知刷盘 ----
    // 把 FlushAllDirtyPages 的「全量刷脏」升级为「冷热分级」：
    //   * 温度 = 页帧自装载/复位以来的访问计数（GetPage 命中/装入、NewPage 各计一次）；
    //   * 开启后每次全量刷脏只写回「冷脏页」（访问数 < 热阈值），热脏页延后留在池内
    //     ——NO-FORCE + WAL 保证正确性（WAL-before-data：热脏页即便不落盘，崩溃后
    //     由 redo 从日志恢复），换来提交路径 I/O 削减与热页保留命中率；
    //   * 关闭（默认，SQLCOMPILER_TEMP_FLUSH 未设置）时行为与旧版逐页全量一致。
    // 阈值与开关：
    void SetTemperatureFlushEnabled(bool en) { temp_flush_enabled_ = en; }
    bool IsTemperatureFlushEnabled() const { return temp_flush_enabled_; }
    // 手动指定固定热阈值；调用后关闭自适应（手动优先级最高）。
    void SetHotAccessThreshold(uint64_t t) {
        hot_access_threshold_ = t;
        adaptive_threshold_enabled_ = false;
    }
    uint64_t GetHotAccessThreshold() const { return hot_access_threshold_; }
    static constexpr uint64_t kDefaultHotAccessThreshold = 8;

    // ---- Phase 5（G5）：温度阈值自适应 ----
    // 把静态热阈值升级为「按访问分布动态估计 + LRU-K 频率信号联动」：
    //   * 动态估计：每次全量刷脏前，以当前脏页访问计数分布（直方图）估计热阈值，
    //     目标是「约 hot_ratio_percent% 的高频脏页判定为热」（P 分位估计），冷页
    //     写回、热页留池，热页占比随工作集温度自动收敛；
    //   * 平坦分布保护：脏页访问计数几乎一致（同温）时阈值上推至全体判冷，避免
    //     「全部判定为热 → 整次刷盘空转、脏页长期滞留」；
    //   * LRU-K 联动：替换策略为 LRU-K 时阈值至少取 K——访问 < K 的页是 LRU-K
    //     语义下的「非相关」一次性引用，恒按冷页写回，热页判定与淘汰策略对齐；
    //   * 显式 SetHotAccessThreshold 手动设值会关闭自适应（手动优先级最高）。
    void SetAdaptiveThresholdEnabled(bool en) { adaptive_threshold_enabled_ = en; }
    bool IsAdaptiveThresholdEnabled() const { return adaptive_threshold_enabled_; }
    // 目标热页占比（百分比，1..99，默认 20）。自适应估计用该占比从访问分布
    // 高频端向下取分位作为热阈值。
    void SetHotRatioPercent(uint64_t pct) {
        hot_ratio_percent_ = (pct >= 1 && pct <= 99) ? pct : hot_ratio_percent_;
    }
    uint64_t GetHotRatioPercent() const { return hot_ratio_percent_; }
    // 最近一次动态估计出的热阈值（观测，供 \stats / 单测验证）。
    uint64_t GetLastEstimatedThreshold() const { return last_estimated_threshold_; }
    // 最近一次参与估计的脏页数（观测）。
    size_t GetLastDirtySampled() const { return last_dirty_sampled_; }

    // ---- E6：缓冲池内存上限可配置与统计 ----
    // 缓冲池内存 ≈ 帧数 × 页大小（固定池，构造时一次性预分配 pages_ 帧数组）。
    // 这里把「内存上限」以字节为单位暴露，并给出当前实际占用，供 \stats / 外部
    // 按字节配置与观测；帧数 <-> 字节的换算由 FramesForBytes 负责。
    static constexpr size_t kPageSize = PAGE_SIZE;

    // 缓冲池配置的内存上限（字节）：pool_size_ × PAGE_SIZE。
    size_t GetMemoryCapBytes() const { return pool_size_ * PAGE_SIZE; }

    // 当前已映射逻辑页的帧数（pool_size_ - 空闲帧数）与所占内存字节数。
    size_t GetMemoryUsageFrames() const;  // 需持锁读 free_list_，实现在 cpp
    size_t GetMemoryUsageBytes() const { return GetMemoryUsageFrames() * PAGE_SIZE; }

    // 把「缓冲池内存上限（字节）」换算为帧数；不足一页时按 1 页计，保证缓存非空。
    static size_t FramesForBytes(size_t bytes) {
        size_t frames = bytes / PAGE_SIZE;
        return (frames == 0) ? 1 : frames;
    }

    // ---- E5 后台异步刷脏页线程 ----
    // 启动一个后台线程，每隔 interval 把当前脏页写回 OS 缓存（复用免锁
    // FlushAllDirtyUnlocked，因此天然遵守 WAL-before-data）。写回不调
    // DiskManager::Sync()：真正落盘仍由 COMMIT 路径的 Sync() 负责，故组提交的
    // fsync 次数不变。interval <= 0 表示禁用（默认），未启用时行为与旧版逐字节一致。
    // 线程在 ~BufferPoolManager 与 Shutdown 时经 StopBackgroundFlush 干净收尾。
    void StartBackgroundFlush(std::chrono::milliseconds interval);
    void StopBackgroundFlush();  // 幂等：未运行时 no-op

    bool IsBackgroundFlushEnabled() const;   // 线程是否在运行
    long GetBackgroundFlushTicks() const;    // 累计被唤醒并尝试刷脏的次数
    std::chrono::milliseconds GetBackgroundFlushInterval() const;

    // ---- T4：可观测性 ----
    // 温阈值（命中构成分档与热阈值一起使用）：热阈值/2（至少 1）。由 latch_ 保护。
    uint64_t GetWarmAccessThreshold() const;
    // 脏页年龄分布（5 桶，单位 = 池操作序号 op_tick）：
    //   [0] 刚变脏；[1,4) [4,10) [10,30) [30,∞)。只统计当前仍脏的帧。
    std::vector<long> GetDirtyAgeDistribution() const;
    // 当前待刷脏帧数（脏页数）——「IO 队列」观测的一部分（同步模型下排队 = 脏帧）。
    size_t GetDirtyFrameCount() const;
    // 后台刷脏直方图（6 桶，每次后台刷脏实际写回页数 n）：
    //   [0] [1] [2,4) [4,8) [8,16) [16,∞)。
    std::vector<long> GetBackgroundFlushHistogram() const;
    // 诊断（\analyze）：页在缓冲池中的帧号；-1 = 不在池。
    int GetFrameOfPage(page_id_t page_id) const;
    // 诊断（\analyze）：页当前是否脏（仅对在池中的页有效；不在池返回 false）。
    bool IsPageDirtyInPool(page_id_t page_id) const;
    // 诊断（\analyze）：缓冲池页映射快照（page_id → 帧号/脏/访问温度），
    // 按 page_id 升序。持 latch_ 返回副本，供 \analyze 打印页映射。
    std::vector<PageMapEntry> GetPageMapSnapshot() const;

private:
    void BackgroundFlushLoop();
    // 全局锁：串行化所有对 frames / page_table_ / free_list_ 等共享状态的访问。
    // 锁序约定（防止死锁，见 BufferPoolManager.cpp 顶部注释）：
    //   BPM::latch_ -> LogManager::mutex_   （BPM 内 flush 时先拿本锁再调 log->Flush()）
    //   BPM::latch_ -> DiskManager::db_io_latch_
    // 恒不允许反向（任何持 LogManager/DiskManager 锁再进入 BPM 的嵌套路径都不存在）。
    mutable std::mutex latch_;

    // 免锁内部实现：公共方法拿锁后委托给这些私有函数，避免非递归锁重入死锁。
    void FlushPageUnlocked(page_id_t page_id);   // FlushPage 的免锁主体
    int FlushAllDirtyUnlocked();                 // FlushAllDirtyPages 的免锁主体（返回写回页数）
    // Phase 5（G5）：按当前脏页访问计数分布动态估计热阈值并写入
    // hot_access_threshold_（观测：last_estimated_threshold_ / last_dirty_sampled_）。
    // 前提：已持 latch_；仅当温度刷盘 + 自适应均开启时由 FlushAllDirtyUnlocked 调用。
    void EstimateAdaptiveThreshold();
    // Phase 4（F）：给一次写回记账（writeback_count + 冷/热分档）。前提：已持 latch_。
    void RecordWritebackStat(const Page& page);
    // T4：给一次命中按温度分档记账（hit_cold/warm/hot_count）。前提：已持 latch_。
    void RecordHitTemperatureStat(const Page& page);
    // T4：给一次后台刷脏写回页数记账（bg_flush_hist_ 直方图）。前提：已持 latch_。
    void RecordBackgroundFlushStat(int written);

    size_t pool_size_;
    DiskManager* disk_manager_;
    std::unique_ptr<Replacer> replacer_;

    std::vector<Page> pages_;                        // 帧数组（frame slots），下标即frame_id
    std::unordered_map<page_id_t, int> page_table_;   // page_id -> frame_id
    std::vector<int> free_list_;                      // 尚未使用的空闲帧id列表

    BufferPoolStats stats_;
    // 替换日志采用环形上限，避免长会话中无限增长导致内存泄漏。
    static constexpr size_t kMaxReplacementLog = 1024;
    std::vector<ReplacementLogEntry> replacement_log_;

    // ---- T4 可观测性状态（由 latch_ 保护）----
    // 池操作序号：每个公共变更方法（GetPage/NewPage/UnpinPage/Flush*/DeletePage/
    // CollectDirtyPages）递增一次。用作「脏页年龄」的逻辑时钟（变脏后经历的操作数）。
    int64_t op_tick_ = 0;
    // 后台刷脏直方图：每次后台刷脏实际写回页数的分布（6 桶，见
    // GetBackgroundFlushHistogram 注释）。仅后台线程记账。
    std::vector<long> bg_flush_hist_;

    // Phase B：可空；非空时 FlushPage / FlushAllDirtyPages 走 LSN 检查。
    LogManager* log_manager_ = nullptr;

    // Phase 4：温度感知刷盘开关与热阈值（由 latch_ 保护；默认关闭 = 旧行为）。
    bool temp_flush_enabled_ = false;
    uint64_t hot_access_threshold_ = kDefaultHotAccessThreshold;

    // ---- Phase 5（G5）：温度阈值自适应状态（由 latch_ 保护）----
    bool adaptive_threshold_enabled_ = false;   // 自适应估计开关（默认关 = 静态阈值）
    uint64_t hot_ratio_percent_ = 20;           // 目标热页占比（%，1..99）
    uint64_t last_estimated_threshold_ = 0;     // 最近一次动态估计的热阈值（观测）
    size_t last_dirty_sampled_ = 0;             // 最近一次参与估计的脏页数（观测）
    // 替换策略与 LRU-K 的 K（LRU-K 联动用；构造时固化，仅观测读取）。
    ReplacementPolicy policy_ = ReplacementPolicy::LRU;
    size_t lru_k_ = 0;

    // ---- E5 后台刷脏线程状态 ----
    std::thread background_flusher_;     // 后台线程；未运行时为空
    mutable std::mutex bg_mutex_;        // 保护 bg_running_ / bg_stop_ / bg_interval_
    std::condition_variable bg_cv_;
    bool bg_running_ = false;            // 线程是否处于运行态
    bool bg_stop_ = false;               // 请求线程退出
    std::chrono::milliseconds bg_interval_{0};
    std::atomic<long> bg_flush_ticks_{0};  // 被唤醒并尝试刷脏的次数（可观测）

    // 找到一个可用帧：优先从free_list_取空闲帧，否则调用replacer_->Victim()淘汰一页；
    // 若淘汰的页为脏页需先写回磁盘。发生淘汰时以 loaded_page_id 填写替换日志的
    // loaded 字段。成功返回true并写入frame_id
    bool FindFreeFrame(page_id_t loaded_page_id, int* frame_id);
};

}  // namespace sqlcompiler