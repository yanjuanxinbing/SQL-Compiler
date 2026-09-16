#pragma once

// =============================================================================
// StorageAccess — 页级存储的唯一门面（BufferPoolManager + DiskManager）
//
// 职责：为全库所有模块（catalog / executor / recovery / txn）提供统一的页访问入口，
//       避免它们各自直接依赖 BufferPoolManager 与 DiskManager：既把「存储 API 文档」
//       收敛到一处，也为将来插入横切能力（跟踪、二级缓存、I/O 调度、指标统计、
//       替换策略替换）留下唯一改造点。
//
// 关键设计决策：
//   * 纯转发（pass-through）：本类自身不缓存页、不实现替换、不持有可变状态，
//     每个方法都只是对被包裹对象的一次调用；
//   * 同时暴露「缓存优先」路径（GetPage / NewPage / UnpinPage / Flush*，走 BPM）
//     与「裸磁盘」路径（ReadPage / WritePage，绕过缓存直连 DiskManager），
//     由调用方按场景选择；
//   * 两个被包裹对象都是非拥有裸指针，其生命周期必须严格长于本门面。
//
// 必须维持的不变量与约束：
//   * 构造后 buffer_pool_manager_ / disk_manager_ 均不得为空，且不可重新绑定；
//   * 本类不引入新锁：并发语义完全由被包裹对象决定，锁序仍为
//     BufferPoolManager 的 frame latch → DiskManager 的 db_io_latch_（单向，不可逆）；
//   * 「裸磁盘」路径读到的内容可能与缓存中的副本不一致，调用方需自行保证不与
//     缓存持有者冲突（常规做法：仅在无缓存持有该页时走裸路径）；
//   * 转发不吞异常：被包裹对象抛出的 std::runtime_error 原样上抛给调用方。
// =============================================================================

#include "storage/BufferPoolManager.h"
#include "storage/DiskManager.h"

namespace sqlcompiler {

// StorageAccess is the single documented entry point for page-level storage
// operations on top of BufferPoolManager + DiskManager.
//
// All database modules (catalog, executors, recovery, txn) should call this
// facade instead of touching BufferPoolManager / DiskManager directly.
// Reasons:
//   (a) single documentation point — one place to learn the storage API;
//   (b) easy to add cross-cutting concerns (tracing, caching, IO scheduling,
//       instrumentation, second-tier cache, alternative replacers) without
//       rewriting call sites;
//   (c) keeps test setup simple (mock the facade).
//
// This class is a thin pass-through wrapper: it does NOT implement caching or
// replacement itself; it merely forwards to the wrapped BufferPoolManager and
// DiskManager. Construction takes raw pointers (not ownership); the caller
// must guarantee the underlying objects outlive this facade.
class StorageAccess {
public:
    // 构造门面：仅保存两个被包裹对象的裸指针，不触发任何 I/O 或初始化。
    // @param bpm 页缓存管理器，提供缓存优先路径；所有权不转移，不得为空
    //            （所有缓存路径方法都会直接解引用它）。
    // @param dm  磁盘管理器，提供裸页 I/O 与页号分配；所有权不转移，不得为空。
    // @note 调用方义务：bpm 与 dm 必须活得比本对象久（本类不持有、不销毁它们）。
    StorageAccess(BufferPoolManager* bpm, DiskManager* dm);

    // ---- Page access (cache-first) ----
    // 取页（缓存优先）：命中直接返回，未命中从磁盘装入并按需淘汰。
    // @param page_id 目标页号；< 0 为非法参数。
    // @return 成功 —— 已 pin 的帧指针（用完必须 UnpinPage 配对）；
    //         nullptr —— 页号非法，或池内无任何可淘汰帧。
    Page* GetPage(page_id_t page_id);
    // 分配新页并装入缓存（内容清零、pin_count == 1）。
    // @param new_page_id 输出参数，成功时写入新页号；传 nullptr 则跳过回写。
    // @return 成功 —— 已 pin 的新帧指针（用完必须 UnpinPage 配对）；
    //         nullptr —— 无可用帧或页号分配失败。
    Page* NewPage(page_id_t* new_page_id);
    // 释放对某页的 pin（计数减一）；计数归零后该帧才可能被淘汰。
    // @param page_id  目标页号；不在帧表中视为无效请求。
    // @param is_dirty 本次使用是否修改过该页；true 置脏（写回推迟到刷盘/淘汰）。
    // @return true  —— 命中帧表并完成递减；false —— 该页当前不在缓冲池中。
    // @note 每个 GetPage / NewPage 必须恰有一次对应的 UnpinPage，否则该帧永不被淘汰。
    bool UnpinPage(page_id_t page_id, bool is_dirty);
    // 把指定页强制写回磁盘（无论是否脏），尊重 WAL-before-data 规则。
    // @param page_id 目标页号；仅在该页已缓存时有效（不会为写回而触发读盘）。
    // @return true  —— 已写盘并清除脏标记；false —— 该页不在缓冲池中，未发生 I/O。
    bool FlushPage(page_id_t page_id);
    // 把缓冲池中所有脏页写回磁盘（不写干净页）。
    // @note 遍历顺序为帧表哈希顺序，无定义；常与 DiskManager::Sync() 配合实现 COMMIT 语义。
    void FlushAllDirtyPages();
    // 把缓冲池中所有页写回磁盘（含干净页），用于测试/关闭时收尾。
    // @note 比 FlushAllDirtyPages 多做无谓的干净页写入，常规路径应优先用后者。
    void FlushAllPages();

    // ---- Raw disk IO (no cache) ----
    // 裸读一页，绕过缓冲池直连磁盘（不改变 pin 计数、不进入帧表）。
    // @param page_id 目标页号；< 0 时仅把 data 清零（见 DiskManager::ReadPage）。
    // @param data    输出缓冲，须至少 PAGE_SIZE 字节且非空。
    // @note 未分配页返回全零；若该页有 CRC 记录且校验失败会抛 std::runtime_error。
    void ReadPage(page_id_t page_id, char* data);
    // 裸写一页，绕过缓冲池直连磁盘。
    // @param page_id 目标页号，调用方须保证 ≥ 0。
    // @param data    长度为 PAGE_SIZE 的页镜像，不得为空。
    // @param force   true 表示写完立即落盘（COMMIT 路径）；默认 false 由 Sync 兜底。
    // @note 若该页正被缓存持有，本调用可能与其副本冲突：调用方需自行保证一致性。
    void WritePage(page_id_t page_id, const char* data, bool force = false);

    // ---- Page lifecycle ----
    // 分配一个页号（优先复用回收页），不装入缓存。
    // @return 新页号，恒 ≥ 0；页内容尚未初始化，需由调用方写入。
    page_id_t AllocatePage();
    // 回收一个页号到空闲列表（并持久化到 <db>.fpl）；< 0 时无操作。
    // @note 不涉及缓冲池：若该页仍在缓存中，调用方须自行保证不再访问它。
    void DeallocatePage(page_id_t page_id);
    // 删除一个页：先从缓冲池移除，再把页号交还 DiskManager 回收。
    // @param page_id 待删除页号；未缓存的页直接走回收。
    // @return true  —— 删除完成且页号已归还空闲列表；
    //         false —— 该页仍在池中且被 pin 住（pin_count > 0），拒绝删除。
    // @note 调用方须保证删除后不再访问该页，否则会重新装入（全零或复用后的内容）。
    bool DeletePage(page_id_t page_id);

    // ---- Diagnostics ----
    // 取得缓冲池累计统计（命中/未命中/淘汰次数）。
    // @return 被包裹 BPM 内部统计对象的常引用（生命周期随 BPM，而非本门面）。
    // @note 统计读写不加锁，并发下取值可能不是某一时刻的稳定快照。
    const BufferPoolStats& GetStats() const;
    // 取得替换日志（按事件发生顺序追加，仅记录真正的「淘汰换出 → 换入」事件）。
    // @return 被包裹 BPM 内部日志向量的常引用（生命周期随 BPM）。
    const std::vector<ReplacementLogEntry>& GetReplacementLog() const;

    // ---- 低层直接访问（BPlusTree / PageGuard 等需要直接持有 BPM 的模块）----
    // 大多数调用方应该走 GetPage / NewPage 等缓存优先路径，不直接拿 BPM/DM。
    // 仅当模块需要与 BPM 的具体替换策略 / 帧管理耦合时才使用。
    // @return 构造时注入的缓冲池指针（非拥有，恒非空）。
    BufferPoolManager* GetBufferPoolManager() const { return buffer_pool_manager_; }
    // @return 构造时注入的磁盘管理器指针（非拥有，恒非空）。
    DiskManager* GetDiskManager() const { return disk_manager_; }

private:
    // 被包裹对象（均为非拥有裸指针）：构造时注入，此后不再变更，且生命周期必须
    // 长于本门面；本类不做空指针校验，调用方必须注入有效对象。
    // buffer_pool_manager_ 提供缓存优先路径；disk_manager_ 提供裸页 I/O 与页号分配。
    BufferPoolManager* buffer_pool_manager_;  // not owned
    DiskManager* disk_manager_;                // not owned
};

}  // namespace sqlcompiler