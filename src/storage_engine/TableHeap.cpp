#include "storage_engine/TableHeap.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "index/PageGuard.h"
#include "storage/Page.h"
#include "storage/StorageAccess.h"
#include "storage_engine/MvccRecord.h"
#include "txn/CommitTracker.h"
#include "txn/LogManager.h"
#include "txn/LogRecord.h"
#include "txn/Transaction.h"

namespace sqlcompiler {

namespace {

// Slotted-page layout:
// offset 0  : int32_t next_page_id
// offset 4  : int32_t slot_count
// offset 8  : int32_t free_space_offset (initial = PAGE_SIZE)
// offset 12 : int32_t reserved
// offset 16 : slot entries (8 bytes each: int32_t offset, int32_t length)
constexpr int32_t kHeaderBytes = 16;
constexpr int32_t kSlotBytes = 8;
constexpr int32_t kMaxSlots = (PAGE_SIZE - kHeaderBytes) / kSlotBytes;  // 510
constexpr uint32_t kTombstone = 0xFFFFFFFFu;

inline int32_t ReadI32(const char* data, size_t off) {
    int32_t v;
    std::memcpy(&v, data + off, sizeof(int32_t));
    return v;
}

inline void WriteI32(char* data, size_t off, int32_t v) {
    std::memcpy(data + off, &v, sizeof(int32_t));
}

void ReadPageHeader(const char* data, int32_t& next_pid, int32_t& slot_count, int32_t& free_off) {
    next_pid   = ReadI32(data, 0);
    slot_count = ReadI32(data, 4);
    free_off   = ReadI32(data, 8);
}

void WritePageHeader(char* data, int32_t next_pid, int32_t slot_count, int32_t free_off) {
    WriteI32(data, 0, next_pid);
    WriteI32(data, 4, slot_count);
    WriteI32(data, 8, free_off);
    WriteI32(data, 12, 0);
}

void InitEmptyPageHeader(char* data) {
    WritePageHeader(data, INVALID_PAGE_ID, 0, PAGE_SIZE);
}

// 判断页头是否自洽。全零页（新分配但从未写入、或进程异常退出后残留在磁盘上的
// 页）会得到 next_pid = 0 / slot_count = 0 / free_off = 0，其中 next_pid = 0 与
// 「合法地指向 page 0」无法区分，会让链表遍历自指成环。这里以 free_off 作为
// 有效性判据：合法页的 free_off 恒在 (kHeaderBytes, PAGE_SIZE] 内。
bool IsValidPageHeader(int32_t slot_count, int32_t free_off) {
    if (free_off <= kHeaderBytes || free_off > static_cast<int32_t>(PAGE_SIZE)) {
        return false;
    }
    if (slot_count < 0 || slot_count > kMaxSlots) return false;
    // slot 目录不得与记录区重叠
    if (kHeaderBytes + slot_count * kSlotBytes > free_off) return false;
    return true;
}

// 若页头不自洽，就地重置为空页头，并返回 true（调用方需标脏）。
bool NormalizePageHeader(char* data, int32_t& next_pid, int32_t& slot_count,
                         int32_t& free_off) {
    if (IsValidPageHeader(slot_count, free_off)) return false;
    next_pid = INVALID_PAGE_ID;
    slot_count = 0;
    free_off = PAGE_SIZE;
    WritePageHeader(data, next_pid, slot_count, free_off);
    return true;
}

size_t SlotOffset(int slot_num) {
    return static_cast<size_t>(kHeaderBytes + slot_num * kSlotBytes);
}

void ReadSlot(const char* data, int slot_num, int32_t& off, int32_t& len) {
    size_t o = SlotOffset(slot_num);
    off = ReadI32(data, o);
    len = ReadI32(data, o + 4);
}

void WriteSlot(char* data, int slot_num, int32_t off, int32_t len) {
    size_t o = SlotOffset(slot_num);
    WriteI32(data, o, off);
    WriteI32(data, o + 4, len);
}

bool IsTombstone(int32_t len) {
    return static_cast<uint32_t>(len) == kTombstone;
}

// Phase 3（t3）：返回页内首个墓碑槽（目录项 len == kTombstone）的下标；无则 -1。
// 墓碑槽由 DeleteTuple（物理删除）或真空（链摘除后）产生，不再被任何版本引用，
// 覆写其目录项即可复用（免去新增目录项占用的 kSlotBytes，且 slot_count 不增长）。
int32_t FindTombstoneSlot(const char* data, int32_t slot_count) {
    for (int32_t s = 0; s < slot_count; ++s) {
        int32_t o, l;
        ReadSlot(data, s, o, l);
        if (IsTombstone(l)) return s;
    }
    return -1;
}

// Phase 3（t3）真空链摘除：把页内所有引用 (pid, slot) 为 prev 的后继版本，其
// prev 改接到被回收版本自身的 prev（跳过被回收版本，保持链可走、墓碑槽不被引用）。
// 仅改本页 prev 链段——建链路径（快照更新把旧 head 迁到同页新槽）保证后继必在
// 本页；扫描不到即无后继（被删 head）或理论上的跨页引用（保守：调用方仍可打
// 墓碑，可见性判据 end_csn<=低水位 已保证整段更旧版本对全部活动快照不可见）。
void UnlinkVersionInPage(char* data, int32_t slot_count, page_id_t pid, int32_t slot,
                         const MvccRecordHeader& v) {
    for (int32_t s = 0; s < slot_count; ++s) {
        if (s == slot) continue;
        int32_t o, l;
        ReadSlot(data, s, o, l);
        if (IsTombstone(l)) continue;
        MvccRecordHeader h;
        if (!ReadMvccHeader(data + o, &h)) continue;
        if (h.prev_page_id == pid && h.prev_slot_num == slot) {
            h.prev_page_id = v.prev_page_id;
            h.prev_slot_num = v.prev_slot_num;
            WriteMvccHeader(data + o, h);
        }
    }
}

// MVCC：版本可见性谓词——版本 v 对快照 S 是否可见。
//   * creation：v.begin_xid 非自身且未提交 -> 不可见（排斥脏读）；已提交但 CSN>S -> 不可见。
//   * supersession：v.end_xid==0 仍为最新 -> 可见；end_xid 为自身 -> 被自己删除/替代，不可见；
//                   end 写者未提交 -> 可见（删除尚未提交）；end CSN>S -> 删除发生在快照后，可见。
bool VisibleForSnapshot(const MvccRecordHeader& v, int64_t S, int64_t self_xid,
                        CommitTracker* tracker) {
    if (v.begin_xid != self_xid && v.begin_xid != 0) {
        // Phase 3：begin_csn 已回填（>0）→ CSN 直判，免 LookupCommitted 锁。
        // 回填发生在 Commit() 登记之后、CSN 一经分配不可变，直判与查表结果等价。
        if (v.begin_csn > 0) {
            if (v.begin_csn > S) return false;
        } else {
            int64_t bcsn = 0;
            if (!tracker->LookupCommitted(v.begin_xid, &bcsn)) return false;
            if (bcsn > S) return false;
        }
    }
    if (v.end_xid == 0) return true;
    if (v.end_xid == self_xid) return false;
    if (v.end_csn > 0) return v.end_csn > S;  // 端已回填 → CSN 直判
    int64_t ecsn = 0;
    if (!tracker->LookupCommitted(v.end_xid, &ecsn)) return true;
    return ecsn > S;
}

}  // namespace

TableHeap::TableHeap(BufferPoolManager* buffer_pool_manager, page_id_t first_page_id)
    : buffer_pool_manager_(buffer_pool_manager), first_page_id_(first_page_id) {
}

TableHeap* TableHeap::Create(BufferPoolManager* buffer_pool_manager) {
    // 用 RAII 句柄持有新页：作用域结束时自动 unpin（脏），杜绝裸 GetPage/UnpinPage
    // 成对遗漏导致的 pin 泄漏（泄漏表现为缓冲池逐渐耗尽，极难定位）。
    PageGuard guard = PageGuard::New(buffer_pool_manager);
    if (!guard.Valid()) return nullptr;
    InitEmptyPageHeader(guard.Data());
    guard.MarkDirty();
    return new TableHeap(buffer_pool_manager, guard.PageId());
}

TableHeap* TableHeap::Open(BufferPoolManager* buffer_pool_manager, page_id_t first_page_id) {
    return new TableHeap(buffer_pool_manager, first_page_id);
}

// ---- StorageAccess 门面重载：解包出被包裹的 BufferPoolManager 后走同一条路径 ----
TableHeap::TableHeap(StorageAccess* storage, page_id_t first_page_id)
    : buffer_pool_manager_(storage != nullptr ? storage->GetBufferPoolManager() : nullptr),
      first_page_id_(first_page_id) {
}

TableHeap* TableHeap::Create(StorageAccess* storage) {
    return Create(storage != nullptr ? storage->GetBufferPoolManager() : nullptr);
}

TableHeap* TableHeap::Open(StorageAccess* storage, page_id_t first_page_id) {
    return Open(storage != nullptr ? storage->GetBufferPoolManager() : nullptr, first_page_id);
}

page_id_t TableHeap::GetFirstPageId() const {
    return first_page_id_;
}

bool TableHeap::InsertIntoPage(page_id_t page_id, const Tuple& tuple, RID* rid,
                                 const std::vector<ValueType>& column_types) {
    // 写锁：并发下其他写者也可能把同一页当作追加目标，读-改-写页头/槽位目录必须
    // 在独占锁内原子完成，否则两个会话读到同一 free_off 会互相覆盖记录。
    PageWriteGuard guard = PageWriteGuard::Fetch(buffer_pool_manager_, page_id);
    if (!guard.Valid()) return false;
    char* data = guard.Data();
    int32_t next_pid, slot_count, free_off;
    ReadPageHeader(data, next_pid, slot_count, free_off);
    bool header_fixed = NormalizePageHeader(data, next_pid, slot_count, free_off);
    if (header_fixed) guard.MarkDirty();

    std::vector<char> serialized = tuple.Serialize(column_types);
    // 快照模式（写者事务为 kSnapshot）：记录前附加 MVCC 头，供快照读过滤/版本链。
    const bool mvcc_write =
        (active_txn_ != nullptr && active_txn_->IsActive() &&
         active_txn_->GetIsolationLevel() == IsolationLevel::kSnapshot);
    const int hdr = mvcc_write ? static_cast<int>(sizeof(MvccRecordHeader)) : 0;
    int32_t len = hdr + static_cast<int32_t>(serialized.size());

    // 目标槽位（t3 墓碑槽复用）：优先复用页内墓碑槽（kTombstone 目录项），无则追加
    // 新目录项。墓碑槽由物理删除或真空（链摘除后）产生、不再被任何版本引为 prev，
    // 覆写其目录项即可归还目录空间（slot_count 不增长，长写路径下目录膨胀有界）。
    // 复用比追加少占 kSlotBytes，空间判断按复用后的目录大小进行。
    int32_t target_slot = slot_count;
    int32_t slot_dir_end = kHeaderBytes + (slot_count + 1) * kSlotBytes;
    const int32_t ts = FindTombstoneSlot(data, slot_count);
    if (ts >= 0) {
        target_slot = ts;
        slot_dir_end = kHeaderBytes + slot_count * kSlotBytes;
        ++tombstone_reuse_count_;
    }
    if (slot_dir_end > free_off || len > free_off - slot_dir_end) {
        if (header_fixed) guard.MarkDirty();
        return false;
    }

    // Phase A：写之前抓整页 before-image，给事务的 undo log。
    if (active_txn_ != nullptr && active_txn_->IsActive()) {
        active_txn_->AppendUndo(page_id, data, PAGE_SIZE,
                                "TableHeap::InsertIntoPage");
    }
    // Phase B：抓整页 before-image 给 WAL。在写入页面之前记录 BEFORE，让 redo
    // 能精准重放到本条插入（before 是「该 slot 尚未被填充」的状态）。
    std::vector<char> before_image;
    if (log_manager_ != nullptr) {
        before_image.assign(data, data + PAGE_SIZE);
    }

    int32_t new_off = free_off - len;
    if (len > 0) {
        if (mvcc_write) {
            MvccRecordHeader h;
            std::memset(&h, 0, sizeof(h));
            h.magic = kMvccMagic;
            h.begin_xid = active_txn_->GetTxnId();
            h.end_xid = 0;
            h.prev_page_id = INVALID_PAGE_ID;
            h.prev_slot_num = -1;
            WriteMvccHeader(data + new_off, h);
            // 提交时回填 begin_csn（新版本）：记录 head 槽位（复用墓碑槽时即该槽）。
            active_txn_->AddVersionSlot(page_id, target_slot, /*is_end=*/false);
            // 写集基（纯 INSERT 无旧基）：FCW 提交时看到 head==自身即跳过。
            active_txn_->AddToWriteSet(RID{page_id, target_slot}, 0, 0);
        }
        std::memcpy(data + new_off + hdr, serialized.data(), serialized.size());
    }
    WriteSlot(data, target_slot, new_off, len);
    WritePageHeader(data, next_pid,
                    (target_slot == slot_count) ? slot_count + 1 : slot_count, new_off);
    guard.MarkDirty();

    // Phase B：写 WAL（UPDATE 记录；before/after 都是 PAGE_SIZE 字节）。
    if (log_manager_ != nullptr) {
        LogRecord rec;
        rec.type_ = LogRecordType::UPDATE;
        rec.txn_id_ = (active_txn_ != nullptr) ? active_txn_->GetTxnId() : 0;
        rec.page_id_ = page_id;
        rec.before_image_ = std::move(before_image);
        rec.after_image_.assign(data, data + PAGE_SIZE);
        lsn_t lsn = log_manager_->AppendRecord(std::move(rec));
        guard.SetPageLsn(lsn);
        // Phase C：回填 in-memory undo 记录的 LSN。
        if (active_txn_ != nullptr && active_txn_->IsActive()) {
            active_txn_->SetLastUndoLSN(lsn);
        }
    }
    if (rid) {
        rid->page_id = page_id;
        rid->slot_num = target_slot;
    }
    return true;
}

bool TableHeap::InsertTuple(const Tuple& tuple, RID* rid,
                            const std::vector<ValueType>& column_types) {
    // 串行化整段「找页->判满->新建页->链接->插入」，防止两个会话并发扩展尾部页
    // 造成链表分叉/丢行（跨多次缓冲池访问的 check-then-act 竞态）。
    std::lock_guard<std::recursive_mutex> lk(write_mutex_);
    page_id_t pid = first_page_id_;
    page_id_t prev_pid = INVALID_PAGE_ID;
    // 防止损坏的 next_pid 形成环导致死循环（例如全零页自指 page 0）
    std::unordered_set<page_id_t> visited;
    while (pid != INVALID_PAGE_ID && pid >= 0) {
        if (!visited.insert(pid).second) break;
        if (InsertIntoPage(pid, tuple, rid, column_types)) {
            return true;
        }
        // Walk to next page in chain（用 RAII 句柄读页头后立即释放）
        page_id_t next_pid = INVALID_PAGE_ID;
        {
            PageReadGuard guard = PageReadGuard::Fetch(buffer_pool_manager_, pid);
            if (!guard.Valid()) return false;
            int32_t slot_count, free_off;
            ReadPageHeader(guard.Data(), next_pid, slot_count, free_off);
        }
        prev_pid = pid;
        if (next_pid == pid) break;
        pid = next_pid;
    }
    // Allocate a new page and link from prev page
    PageGuard new_guard = PageGuard::New(buffer_pool_manager_);
    if (!new_guard.Valid()) return false;
    InitEmptyPageHeader(new_guard.Data());
    new_guard.MarkDirty();
    page_id_t new_pid = new_guard.PageId();
    if (prev_pid != INVALID_PAGE_ID) {
        // 写锁：并发下 prev 页可能被其他会话借用到链上，改 next_pid 需独占。
        PageWriteGuard prev = PageWriteGuard::Fetch(buffer_pool_manager_, prev_pid);
        if (prev.Valid()) {
            int32_t next_pid, slot_count, free_off;
            ReadPageHeader(prev.Data(), next_pid, slot_count, free_off);
            // Phase B：页链链接（prev.next_pid = new_pid）必须写 WAL。
            // page_lsn 是纯内存字段、不跨重启持久化，恢复时 redo 会把页重放
            // 成 WAL 中最后一条 UPDATE 的 after 状态；链接修改若无 WAL 记录，
            // 多页表跨重启后页链会断在未链接处（重放覆盖掉链接），只读到第一页。
            std::vector<char> before_image;
            if (log_manager_ != nullptr) {
                before_image.assign(prev.Data(), prev.Data() + PAGE_SIZE);
            }
            WritePageHeader(prev.Data(), new_pid, slot_count, free_off);
            prev.MarkDirty();
            if (log_manager_ != nullptr) {
                LogRecord rec;
                rec.type_ = LogRecordType::UPDATE;
                rec.txn_id_ = (active_txn_ != nullptr) ? active_txn_->GetTxnId() : 0;
                rec.page_id_ = prev_pid;
                rec.before_image_ = std::move(before_image);
                rec.after_image_.assign(prev.Data(), prev.Data() + PAGE_SIZE);
                lsn_t lsn = log_manager_->AppendRecord(std::move(rec));
                prev.SetPageLsn(lsn);
                if (active_txn_ != nullptr && active_txn_->IsActive()) {
                    active_txn_->SetLastUndoLSN(lsn);
                }
            }
        }
    } else {
        first_page_id_ = new_pid;
    }
    return InsertIntoPage(new_pid, tuple, rid, column_types);
}

bool TableHeap::GetTuple(const RID& rid, Tuple* tuple,
                          const std::vector<ValueType>& column_types) {
    if (!rid.IsValid()) return false;
    // 共享读锁：与写者（持独占页锁做 memcpy）协调，避免读到撕裂的记录。
    PageReadGuard guard = PageReadGuard::Fetch(buffer_pool_manager_, rid.page_id);
    if (!guard.Valid()) return false;
    const char* data = guard.Data();
    int32_t next_pid, slot_count, free_off;
    ReadPageHeader(data, next_pid, slot_count, free_off);
    if (rid.slot_num < 0 || rid.slot_num >= slot_count) {
        return false;
    }
    int32_t off, len;
    ReadSlot(data, rid.slot_num, off, len);
    if (IsTombstone(len)) {
        return false;
    }
    // 防御性边界校验：slot 中的 (off, len) 必须完整落在数据区内。
    if (off < kHeaderBytes || len <= 0 ||
        off > static_cast<int32_t>(PAGE_SIZE) - len) {
        return false;
    }

    MvccRecordHeader head;
    const bool has_head_hdr = ReadMvccHeader(data + off, &head);

    // 非快照（锁基）读：head 若已被 MVCC 逻辑删除（end_xid != 0 且已提交——
    // 未提交的删除写者要么被行 X 锁/回滚隔离，要么属 READ UNCOMMITTED 的既有
    // 限制），行不可见，返回 false。必要性（t4）：快照写者的逻辑删除在非唯一
    // 二级索引中延迟保留条目，索引回表会命中该 head；不在此过滤会让
    // SERIALIZABLE/READ COMMITTED 读者读到已删除的行（幽灵行）。
    if (has_head_hdr && commit_tracker_ == nullptr && head.end_xid != 0) {
        return false;
    }

    // 最终要反序列化的版本内容位置（默认 = head 自身）。快照模式下可能沿链
    // 回到更旧的版本页，data 会被换成该版本页指针。
    int32_t v_off = off;
    bool adopt_and_record_base = false;
    int64_t base_xid = 0, base_csn = 0;

    if (has_head_hdr && commit_tracker_ != nullptr) {
        // ---- 快照读：版本索引缓存（Phase 3）命中直取；未命中沿链找首个可见版本 ----
        const int64_t S = snapshot_csn_;
        Transaction* self = (active_txn_ != nullptr && active_txn_->IsActive())
                                ? active_txn_
                                : nullptr;
        const int64_t self_xid = self ? self->GetTxnId() : 0;

        // 自读防护：本事务自己写过该 rid（其未提交版本不可进缓存）→ 不走缓存，
        // 由链走路径处理 self 可见性。读者事务版本槽为空，恒 O(1) 跳过本检查。
        bool self_wrote_rid = false;
        if (self != nullptr) {
            for (const auto& ref : self->GetVersionSlots()) {
                if (ref.page_id == rid.page_id && ref.slot_num == rid.slot_num) {
                    self_wrote_rid = true;
                    break;
                }
            }
        }

        // (A) 缓存命中：head 标记一致 → 二分「最新 begin_csn<=S」的稳定版本，
        //     端可见性 CSN 直判（免逐版本 LookupCommitted 锁与跨页读取）。
        VersionIndexEntry cand;
        bool cache_hit = false;
        if (!self_wrote_rid) {
            std::lock_guard<std::mutex> lk(version_index_mutex_);
            auto it = version_index_.find(VersionIndexKey(rid));
            if (it != version_index_.end()) {
                const VersionIndex& vi = it->second;
                if (vi.head_begin_xid == head.begin_xid &&
                    vi.head_begin_csn == head.begin_csn &&
                    vi.head_end_xid == head.end_xid &&
                    vi.head_end_csn == head.end_csn &&
                    vi.head_prev_pid == head.prev_page_id &&
                    vi.head_prev_slot == head.prev_slot_num) {
                    // 二分：pick = 最后一个 begin_csn <= S 的版本下标（升序数组）。
                    size_t lo = 0, hi = vi.versions.size(), pick = vi.versions.size();
                    while (lo < hi) {
                        const size_t mid = lo + (hi - lo) / 2;
                        if (vi.versions[mid].begin_csn <= S) { pick = mid; lo = mid + 1; }
                        else hi = mid;
                    }
                    if (pick < vi.versions.size()) {
                        const VersionIndexEntry& e = vi.versions[pick];
                        if (e.end_csn == 0 || e.end_csn > S) {
                            cand = e;
                            cache_hit = true;
                        }
                    }
                }
            }
        }
        if (cache_hit) {
            // 候选槽校验：头部字段与缓存一致才采用（防真空墓碑/页复用漂移）。
            const char* cd = data;  // 默认同页（cand.page_id == rid.page_id）
            PageReadGuard cg;       // 跨页时的局部页读锁（作用域延至校验完成）
            bool ok = true;
            if (cand.page_id != rid.page_id) {
                cg = PageReadGuard::Fetch(buffer_pool_manager_, cand.page_id);
                if (!cg.Valid()) { ok = false; }
                else cd = cg.Data();
            }
            if (ok) {
                int32_t csc = 0, cnext = 0, cfree = 0;
                ReadPageHeader(cd, cnext, csc, cfree);
                ok = (cand.slot_num >= 0 && cand.slot_num < csc);
                int32_t co = 0, cl = 0;
                if (ok) ReadSlot(cd, cand.slot_num, co, cl);
                MvccRecordHeader ch;
                if (ok && !IsTombstone(cl) && ReadMvccHeader(cd + co, &ch) &&
                    ch.begin_xid == cand.begin_xid && ch.begin_csn == cand.begin_csn &&
                    ch.end_csn == cand.end_csn &&
                    static_cast<size_t>(co) + sizeof(MvccRecordHeader) <= PAGE_SIZE) {
                    // 命中：反序列化候选版本内容（跳过 48 字节头），并登记快照读基。
                    ++version_index_hits_;
                    if (tuple) {
                        *tuple = Tuple::Deserialize(
                            cd + co + static_cast<int32_t>(sizeof(MvccRecordHeader)),
                            column_types);
                        tuple->SetRid(rid);
                    }
                    if (self != nullptr) {
                        active_txn_->RecordSnapshotRead(rid, cand.begin_xid,
                                                       cand.begin_csn);
                    }
                    return true;
                }
                // 候选失效（墓碑/漂移）：回退链走（下方重建缓存，自愈）。
            }
        }

        // (B) 未命中/过期/自读：沿链（头 -> 越来越旧）找首个可见版本，同时收集
        //     稳定版本（写者已提交且端已回填）供重建缓存。
        MvccRecordHeader cur = head;
        page_id_t cpid = rid.page_id;
        int32_t cslot = rid.slot_num;
        int32_t coff = off;
        bool found = false;
        std::vector<VersionIndexEntry> collected;  // 访问序：新→旧
        while (true) {
            if (cur.begin_csn > 0 && (cur.end_xid == 0 || cur.end_csn > 0)) {
                collected.push_back(VersionIndexEntry{cur.begin_xid, cur.begin_csn,
                                                      cur.end_csn, cpid, cslot});
            }
            if (VisibleForSnapshot(cur, S, self_xid, commit_tracker_)) {
                found = true;
                break;
            }
            if (cur.prev_page_id == INVALID_PAGE_ID) break;
            const page_id_t np = cur.prev_page_id;
            const int32_t ns = cur.prev_slot_num;
            const char* pdata = nullptr;
            if (np == cpid) {
                pdata = data;  // 同页链（head 与旧版本同页）
            } else {
                guard.Release();
                PageReadGuard pg = PageReadGuard::Fetch(buffer_pool_manager_, np);
                if (!pg.Valid()) return false;
                guard = std::move(pg);
                pdata = guard.Data();
            }
            int32_t nslot_count, nnext, nfree;
            ReadPageHeader(pdata, nnext, nslot_count, nfree);
            if (ns < 0 || ns >= nslot_count) return false;
            int32_t no, nl;
            ReadSlot(pdata, ns, no, nl);
            if (IsTombstone(nl) || !ReadMvccHeader(pdata + no, &cur)) return false;
            cpid = np;
            cslot = ns;
            coff = no;
            data = pdata;
            // 防御：链循环保护（最多试探一个有限的深度，随链长增长即视为损坏）
            if (cslot == rid.slot_num && cpid == rid.page_id) return false;
        }
        // 链完整（走到链尾或找到可见版本）→ 重建缓存：collected 是 新→旧 序，
        // 反转成 旧→新 升序并记录 head 标记（供下次命中校验）。
        {
            std::lock_guard<std::mutex> lk(version_index_mutex_);
            VersionIndex& vi = version_index_[VersionIndexKey(rid)];
            vi.versions.assign(collected.rbegin(), collected.rend());
            vi.head_begin_xid = head.begin_xid;
            vi.head_begin_csn = head.begin_csn;
            vi.head_end_xid = head.end_xid;
            vi.head_end_csn = head.end_csn;
            vi.head_prev_pid = head.prev_page_id;
            vi.head_prev_slot = head.prev_slot_num;
            ++version_index_builds_;
        }
        if (!found) return false;  // 对快照不可见
        v_off = coff;
        // 版本位于 cpid 页（可能跨页）；data 已指向该页。
        adopt_and_record_base = (self != nullptr);
        base_xid = cur.begin_xid;
        base_csn = cur.begin_csn;
    }
    // 反序列化时跳过 48 字节头（有头记录）。
    const int rec_hdr = has_head_hdr ? static_cast<int>(sizeof(MvccRecordHeader)) : 0;
    // 防越界：合法记录必然含头部空间。
    if (rec_hdr > 0 && (v_off + rec_hdr > static_cast<int32_t>(PAGE_SIZE))) return false;
    if (tuple) {
        *tuple = Tuple::Deserialize(data + v_off + rec_hdr, column_types);
        tuple->SetRid(rid);
    }
    if (adopt_and_record_base) {
        // 快照读基：供该行后续 UPDATE 的 first-committer-wins 基比较。
        active_txn_->RecordSnapshotRead(rid, base_xid, base_csn);
    }
    return true;
}

bool TableHeap::DeleteTuple(const RID& rid) {
    if (!rid.IsValid()) return false;
    std::lock_guard<std::recursive_mutex> lk(write_mutex_);
    // 写锁：墓碑写入槽位目录需与并发写者协调。
    PageWriteGuard guard = PageWriteGuard::Fetch(buffer_pool_manager_, rid.page_id);
    if (!guard.Valid()) return false;
    char* data = guard.Data();
    int32_t next_pid, slot_count, free_off;
    ReadPageHeader(data, next_pid, slot_count, free_off);
    if (rid.slot_num < 0 || rid.slot_num >= slot_count) {
        return false;
    }
    int32_t off, len;
    ReadSlot(data, rid.slot_num, off, len);
    if (IsTombstone(len)) return false;  // 已删除（幂等）
    // Phase A：写之前抓整页 before-image。
    if (active_txn_ != nullptr && active_txn_->IsActive()) {
        active_txn_->AppendUndo(rid.page_id, data, PAGE_SIZE,
                                "TableHeap::DeleteTuple");
    }
    // Phase B：抓 before-image 给 WAL。
    std::vector<char> before_image;
    if (log_manager_ != nullptr) {
        before_image.assign(data, data + PAGE_SIZE);
    }

    const bool mvcc_del =
        (active_txn_ != nullptr && active_txn_->IsActive() &&
         active_txn_->GetIsolationLevel() == IsolationLevel::kSnapshot);
    if (mvcc_del) {
        // MVCC 删除：把 head 版本标记 end_xid=me（保持槽位与 RID 稳定），
        // 提交时回填 end_csn；物理空间交给 Vacuum 回收。
        MvccRecordHeader h;
        if (ReadMvccHeader(data + off, &h)) {
            h.end_xid = active_txn_->GetTxnId();
            h.end_csn = 0;  // 提交时回填
            WriteMvccHeader(data + off, h);
            guard.MarkDirty();
            // 记录待回填 end_csn 的版本槽位。
            active_txn_->AddVersionSlot(rid.page_id, rid.slot_num, /*is_end=*/true);
            // FCW 写集基：取快照读登记的可见版本写者。快照删除的语义是「删掉我读到的
            // 那个版本」，若删除操作实际作用于的 head 已被其他已提交事务改写（head 的
            // 写者 != base），first-committer-wins 在提交时判定冲突并中止本事务。
            // 无快照读基时跳过（不得退化为以物理 head 作基，那会恰好掩盖冲突）。
            int64_t bx = 0, bc = 0;
            if (active_txn_->TakeSnapshotRead(rid, &bx, &bc) && bx != 0) {
                active_txn_->AddToWriteSet(rid, bx, bc);
            }
        } else {
            // legacy 行：退化为物理墓碑（旧格式语义）。
            WriteSlot(data, rid.slot_num, 0, static_cast<int32_t>(kTombstone));
            guard.MarkDirty();
        }
    } else {
        WriteSlot(data, rid.slot_num, 0, static_cast<int32_t>(kTombstone));
        guard.MarkDirty();
    }

    // Phase 2 内联轻量真空：本页旧版本槽位（end_csn <= 低水位）在写路径上顺带
    // 回收（语句级预算内），WAL after-image 包含墓碑，重放语义一致。
    RunInlineVacuum(rid.page_id, data, slot_count);

    // Phase B：写 UPDATE 记录（before = 原 page，after = 带墓碑的 page）。
    if (log_manager_ != nullptr) {
        LogRecord rec;
        rec.type_ = LogRecordType::UPDATE;
        rec.txn_id_ = (active_txn_ != nullptr) ? active_txn_->GetTxnId() : 0;
        rec.page_id_ = rid.page_id;
        rec.before_image_ = std::move(before_image);
        rec.after_image_.assign(data, data + PAGE_SIZE);
        lsn_t lsn = log_manager_->AppendRecord(std::move(rec));
        guard.SetPageLsn(lsn);
        // Phase C：回填 in-memory undo 记录的 LSN。
        if (active_txn_ != nullptr && active_txn_->IsActive()) {
            active_txn_->SetLastUndoLSN(lsn);
        }
    }
    return true;
}

bool TableHeap::UpdateTuple(const RID& rid, const Tuple& new_tuple,
                             const std::vector<ValueType>& column_types,
                             RID* out_new_rid) {
    if (!rid.IsValid()) return false;
    std::lock_guard<std::recursive_mutex> lk(write_mutex_);
    // 写锁：in-place 更新槽位目录/记录需与并发写者协调。
    PageWriteGuard guard = PageWriteGuard::Fetch(buffer_pool_manager_, rid.page_id);
    if (!guard.Valid()) return false;
    char* data = guard.Data();
    int32_t next_pid, slot_count, free_off;
    ReadPageHeader(data, next_pid, slot_count, free_off);
    if (rid.slot_num < 0 || rid.slot_num >= slot_count) {
        return false;
    }
    int32_t off, len;
    ReadSlot(data, rid.slot_num, off, len);
    if (IsTombstone(len) || off < 0 || len < 0 ||
        off + len > static_cast<int32_t>(PAGE_SIZE)) {
        return false;
    }
    std::vector<char> serialized = new_tuple.Serialize(column_types);
    int32_t new_len = static_cast<int32_t>(serialized.size());
    if (new_len == 0) {
        return false;
    }
    // Phase A：写之前抓整页 before-image（即使后续走 relocate 路径，
    // 也会被 DeleteTuple/InsertTuple 各自再抓一份；总 undo 仍能恢复到原始状态）。
    if (active_txn_ != nullptr && active_txn_->IsActive()) {
        active_txn_->AppendUndo(rid.page_id, data, PAGE_SIZE,
                                "TableHeap::UpdateTuple(before)");
    }
    // Phase B：抓 before-image 给 WAL。
    std::vector<char> before_image;
    if (log_manager_ != nullptr) {
        before_image.assign(data, data + PAGE_SIZE);
    }

    const bool mvcc_update =
        (active_txn_ != nullptr && active_txn_->IsActive() &&
         active_txn_->GetIsolationLevel() == IsolationLevel::kSnapshot);
    MvccRecordHeader head_hdr;
    const bool has_head_hdr = mvcc_update && ReadMvccHeader(data + off, &head_hdr);
    const int hdr = static_cast<int>(sizeof(MvccRecordHeader));

    if (!has_head_hdr) {
        // ---- 非快照更新，或对 legacy（无头）行做快照更新：退化为既有 in-place/
        // relocate 语义，不建链（旧库行在快照下失去旧值属文档化局限）。 ----
        const int rec_hdr = mvcc_update ? hdr : 0;
        const int32_t payload_end = rec_hdr + new_len;
        if (payload_end <= len) {
            if (rec_hdr > 0) {
                MvccRecordHeader nh;
                std::memset(&nh, 0, sizeof(nh));
                nh.magic = kMvccMagic;
                nh.begin_xid = active_txn_->GetTxnId();
                nh.end_xid = 0;
                nh.prev_page_id = INVALID_PAGE_ID;
                nh.prev_slot_num = -1;
                WriteMvccHeader(data + off, nh);
                active_txn_->AddVersionSlot(rid.page_id, rid.slot_num, /*is_end=*/false);
            }
            if (payload_end < len) {
                std::memset(data + off + payload_end, 0, len - payload_end);
            }
            std::memcpy(data + off + rec_hdr, serialized.data(), new_len);
            WriteSlot(data, rid.slot_num, off, payload_end);
            guard.MarkDirty();
            // Phase 2 内联轻量真空（in-place 路径：仍可能有先前快照写遗留的旧版本）。
            RunInlineVacuum(rid.page_id, data, slot_count);
            if (log_manager_ != nullptr) {
                LogRecord rec;
                rec.type_ = LogRecordType::UPDATE;
                rec.txn_id_ = (active_txn_ != nullptr) ? active_txn_->GetTxnId() : 0;
                rec.page_id_ = rid.page_id;
                rec.before_image_ = std::move(before_image);
                rec.after_image_.assign(data, data + PAGE_SIZE);
                lsn_t lsn = log_manager_->AppendRecord(std::move(rec));
                guard.SetPageLsn(lsn);
                if (active_txn_ != nullptr && active_txn_->IsActive()) {
                    active_txn_->SetLastUndoLSN(lsn);
                }
            }
            // 原位更新：RID 不变。
            if (out_new_rid != nullptr) *out_new_rid = rid;
            return true;
        }
        // Cannot grow in place — delete and reinsert
        // 关键：DeleteTuple 和 InsertTuple 各自会写自己的 WAL 记录；这里不再单独追加。
        // 先把本方法持有的页写锁归还（同帧二次 Grab 独占锁会自锁），write_mutex_ 是
        // 递归锁，DeleteTuple/InsertTuple 内部再次加锁放行。
        guard.Release();
        DeleteTuple(rid);
        // delete+insert 迁移：原 slot 被墓碑化，行落到新 slot，RID 改变。
        RID moved;
        bool ok = InsertTuple(new_tuple, &moved, column_types);
        if (ok && out_new_rid != nullptr) *out_new_rid = moved;
        return ok;
    }

    // ================= 快照更新：版本链（RID 稳定） =================
    // 把旧 head 迁到本页新分配的 slot（其 end_xid=me、保留其 prev），再在稳定
    // head 槽覆写新版本（begin_xid=me、end_xid=0、prev=新 slot）。条件：
    //   1) 新版本内容放得进 head 槽（new_total <= len）；
    //   2) 本页有空间容纳「新 slot 项 + 旧 head 内容」。
    const int32_t new_total = hdr + new_len;
    const int32_t old_cols = len - hdr;  // 旧 head 的列字节数（有头）
    // 迁出目标槽（t3 墓碑槽复用）：优先复用页内墓碑槽（真空链摘除保证不再被引用），
    // 无则追加新目录项；复用比追加少占 kSlotBytes。空间不足无法原位建链才退级 relocate。
    int32_t move_slot = slot_count;
    int32_t slot_dir_end = kHeaderBytes + (slot_count + 1) * kSlotBytes;
    const int32_t ts = FindTombstoneSlot(data, slot_count);
    if (ts >= 0) {
        move_slot = ts;
        slot_dir_end = kHeaderBytes + slot_count * kSlotBytes;
        ++tombstone_reuse_count_;
    }
    bool move_ok =
        (new_total <= len && old_cols >= 0 &&
         (hdr + old_cols + static_cast<int32_t>(kSlotBytes)) <= (free_off - slot_dir_end));
    if (move_ok) {
        // a) 把旧 head（含头）迁到目标槽。
        int32_t new_old_off = free_off - len;
        MvccRecordHeader old_v = head_hdr;
        old_v.end_xid = active_txn_->GetTxnId();
        old_v.end_csn = 0;  // 提交时回填
        WriteMvccHeader(data + new_old_off, old_v);
        std::memcpy(data + new_old_off + hdr, data + off + hdr, old_cols);
        WriteSlot(data, move_slot, new_old_off, len);
        // b) 覆写稳定 head 槽为新版本。
        MvccRecordHeader nh;
        std::memset(&nh, 0, sizeof(nh));
        nh.magic = kMvccMagic;
        nh.begin_xid = active_txn_->GetTxnId();
        nh.end_xid = 0;
        nh.prev_page_id = rid.page_id;
        nh.prev_slot_num = move_slot;  // 指向刚迁出的旧版本
        WriteMvccHeader(data + off, nh);
        std::memcpy(data + off + hdr, serialized.data(), new_len);
        WriteSlot(data, rid.slot_num, off, new_total);
        WritePageHeader(data, next_pid,
                        (move_slot == slot_count) ? slot_count + 1 : slot_count,
                        new_old_off);
        guard.MarkDirty();
        // Phase 2 内联轻量真空：本页旧版本（end_csn <= 低水位）在写路径上顺带回收，
        // 摊薄全表真空成本；WAL after-image 在下方捕获，墓碑随页重放一致。
        RunInlineVacuum(rid.page_id, data,
                        (move_slot == slot_count) ? slot_count + 1 : slot_count);

        // WAL：本操作一次性改动了页（迁旧 + 覆写）。
        if (log_manager_ != nullptr) {
            LogRecord rec;
            rec.type_ = LogRecordType::UPDATE;
            rec.txn_id_ = active_txn_->GetTxnId();
            rec.page_id_ = rid.page_id;
            rec.before_image_ = std::move(before_image);
            rec.after_image_.assign(data, data + PAGE_SIZE);
            lsn_t lsn = log_manager_->AppendRecord(std::move(rec));
            guard.SetPageLsn(lsn);
            active_txn_->SetLastUndoLSN(lsn);
        }
        // 版本槽位回填登记：新 head = begin_csn；旧版本 = end_csn。
        active_txn_->AddVersionSlot(rid.page_id, rid.slot_num, /*is_end=*/false);
        active_txn_->AddVersionSlot(rid.page_id, move_slot, /*is_end=*/true);
        // 写集基：取快照读登记的可见版本写者；无则回退 head（或 0）。
        int64_t bx = 0, bc = 0;
        if (!active_txn_->TakeSnapshotRead(rid, &bx, &bc)) {
            bx = head_hdr.begin_xid;
            bc = head_hdr.begin_csn;
        }
        active_txn_->AddToWriteSet(rid, bx, bc);
        // 版本链更新覆写的仍是原 head 槽：RID 不变。
        if (out_new_rid != nullptr) *out_new_rid = rid;
        return true;
    }

    // 空间不足无法原位建链：退级为 relocate（RID 变化由上层索引维护补齐）。
    guard.Release();
    DeleteTuple(rid);
    // relocate：行被搬到新 slot，RID 改变。
    RID moved;
    bool ok = InsertTuple(new_tuple, &moved, column_types);
    if (ok && out_new_rid != nullptr) *out_new_rid = moved;
    return ok;
}

void TableHeap::ClearAll() {
    // Walk the linked list of pages, free every overflow page beyond the
    // first, then reset the first page's slot directory to empty.
    std::lock_guard<std::recursive_mutex> lk(write_mutex_);
    page_id_t pid = first_page_id_;
    if (pid == INVALID_PAGE_ID) return;

    // Find first page header
    page_id_t next_pid = INVALID_PAGE_ID;
    {
        PageGuard guard = PageGuard::Fetch(buffer_pool_manager_, pid);
        if (!guard.Valid()) return;
        int32_t slot_count, free_off;
        ReadPageHeader(guard.Data(), next_pid, slot_count, free_off);
    }

    // Free overflow pages
    page_id_t cur = next_pid;
    while (cur != INVALID_PAGE_ID && cur >= 0) {
        page_id_t victim = cur;
        {
            PageGuard guard = PageGuard::Fetch(buffer_pool_manager_, cur);
            if (!guard.Valid()) return;
            int32_t nnext, ns, nf;
            ReadPageHeader(guard.Data(), nnext, ns, nf);
            cur = nnext;
        }  // guard 作用域结束：页已 unpin，pin==0，允许 DeletePage
        if (!buffer_pool_manager_->DeletePage(victim)) {
            // DeletePage failed (probably pinned); bail out — the table is
            // still partially cleared, but tombstones on overflow pages
            // are still skipped by the iterator, so the user-visible effect
            // is consistent.
            break;
        }
    }

    // Reset first page header to empty
    PageWriteGuard head = PageWriteGuard::Fetch(buffer_pool_manager_, first_page_id_);
    if (!head.Valid()) return;
    InitEmptyPageHeader(head.Data());
    head.MarkDirty();
    // 版本索引缓存随表内容清空而失效（自愈：即使漏清，标记不匹配也会重建）。
    {
        std::lock_guard<std::mutex> lk(version_index_mutex_);
        version_index_.clear();
    }
}

uint64_t TableHeap::GetApproxRowCount() const {
    uint64_t total = 0;
    page_id_t pid = first_page_id_;
    std::unordered_set<page_id_t> visited;
    while (pid != INVALID_PAGE_ID && pid >= 0 && visited.insert(pid).second) {
        PageReadGuard guard = PageReadGuard::Fetch(buffer_pool_manager_, pid);
        if (!guard.Valid()) return 0;
        int32_t next_pid, slot_count, free_off;
        ReadPageHeader(guard.Data(), next_pid, slot_count, free_off);
        if (slot_count > 0) total += static_cast<uint64_t>(slot_count);
        pid = next_pid;
    }
    return total;
}

bool TableHeap::FindNextRid(RID current, RID* next) {
    page_id_t pid = current.IsValid() ? current.page_id : first_page_id_;
    int slot_num = current.IsValid() ? current.slot_num + 1 : 0;
    // 防止 next_pid 形成循环（数据损坏时保护）
    std::unordered_set<page_id_t> visited;
    while (pid != INVALID_PAGE_ID && pid >= 0) {
        if (!visited.insert(pid).second) {
            // 已经访问过这个页面，next_pid 形成环
            return false;
        }
        // NormalizePageHeader 可能修复页头（写）；用写锁避免与并发写者撕裂。
        // 仅在此写锁作用域内完成「读页头 + 规整」，随即释放——因为后续的
        // IsReferencedAsPrev 会再对本页取读锁，若仍持有页写锁会造成自锁。
        int32_t this_next = INVALID_PAGE_ID, this_slot_count = 0;
        {
            PageWriteGuard guard = PageWriteGuard::Fetch(buffer_pool_manager_, pid);
            if (!guard.Valid()) return false;
            int32_t slot_count, free_off;
            ReadPageHeader(guard.Data(), this_next, slot_count, free_off);
            bool header_fixed =
                NormalizePageHeader(guard.Data(), this_next, slot_count, free_off);
            if (header_fixed) guard.MarkDirty();
            this_slot_count = slot_count;
        }
        while (slot_num < this_slot_count) {
            // 短读锁作用域内读取 slot 状态；随后释放再调 IsReferencedAsPrev，
            // 避免同线程对该页「先持写锁/读锁、再取读锁」的自锁。
            bool is_candidate = false;
            {
                PageReadGuard rg = PageReadGuard::Fetch(buffer_pool_manager_, pid);
                if (!rg.Valid()) return false;
                int32_t off, len;
                ReadSlot(rg.Data(), slot_num, off, len);
                if (IsTombstone(len)) { ++slot_num; continue; }
                is_candidate = true;
                // 快照模式：跳过「被某版本引为 prev」的旧版本 slot，只交付 head。
                // 逻辑行最终可见性由 Iterator::Next -> GetTuple 过滤。
                if (commit_tracker_ != nullptr) {
                    MvccRecordHeader h;
                    if (!ReadMvccHeader(rg.Data() + off, &h)) { is_candidate = false; }
                }
            }
            if (commit_tracker_ != nullptr && is_candidate &&
                IsReferencedAsPrev(pid, slot_num)) {
                ++slot_num;
                continue;
            }
            if (next) {
                next->page_id = pid;
                next->slot_num = slot_num;
            }
            return true;
        }
        // next_pid 越界保护
        if (this_next < 0) return false;
        pid = this_next;
        slot_num = 0;
    }
    return false;
}

// 判断 (pid, slot) 是否被任意一处的「非墓碑、有 MVCC 头」版本引为 prev
// （即它是某个更旧版本，而非稳定 head）。快照扫描据此跳过旧版本 slot。
bool TableHeap::IsReferencedAsPrev(page_id_t pid, int32_t slot) const {
    std::unordered_set<page_id_t> visited;
    page_id_t cur = first_page_id_;
    while (cur != INVALID_PAGE_ID && cur >= 0) {
        if (!visited.insert(cur).second) break;
        PageReadGuard g = PageReadGuard::Fetch(buffer_pool_manager_, cur);
        if (!g.Valid()) break;
        const char* d = g.Data();
        int32_t np, sc, fo;
        ReadPageHeader(d, np, sc, fo);
        for (int32_t s = 0; s < sc; ++s) {
            int32_t o, l;
            ReadSlot(d, s, o, l);
            if (IsTombstone(l)) continue;
            MvccRecordHeader h;
            if (!ReadMvccHeader(d + o, &h)) continue;
            if (h.prev_page_id == pid && h.prev_slot_num == slot) return true;
        }
        cur = np;
    }
    return false;
}

// Phase 2 内联轻量真空：在已持页写锁的页上回收「已被替代/删除且替代/删除已提交
// 不晚于低水位」的旧版本槽位（写墓碑）。判据与 Vacuum 完全一致（end_csn 而非
// begin_csn：任何活动快照都看不到「结束于它之前」的版本；begin_csn 判据会误删
// 「创建早、但删除/替代晚于活动快照」的版本）。至多回收 budget 个后提前终止，
// 单次成本有界（摊薄）。Phase 3（t3）：回收时先做链摘除（UnlinkVersionInPage），
// 把本页内引用被回收版本为 prev 的后继接到被回收版本自身的 prev，保证墓碑槽
// 不再被任何版本引用、可被 InsertIntoPage/UpdateTuple 安全复用。
// 调用方持页写锁，页已被写路径标脏；墓碑/摘除发生在 WAL after-image 捕获前。
int TableHeap::InlineVacuumPage(page_id_t page_id, char* data, int32_t slot_count,
                                int64_t low_water, int budget) {
    int reclaimed = 0;
    for (int32_t s = 0; s < slot_count && reclaimed < budget; ++s) {
        int32_t o, l;
        ReadSlot(data, s, o, l);
        if (IsTombstone(l)) continue;
        MvccRecordHeader h;
        if (!ReadMvccHeader(data + o, &h)) continue;
        if (h.end_xid != 0 && h.end_csn != 0 && h.end_csn <= low_water) {
            UnlinkVersionInPage(data, slot_count, page_id, s, h);
            WriteSlot(data, s, 0, static_cast<int32_t>(kTombstone));
            ++reclaimed;
        }
    }
    return reclaimed;
}

// 内联真空入口：取全局低水位（O(1) 缓存读）并回收本页旧版本，扣减语句级预算。
// 无 tracker（非快照库）/ 预算耗尽 / 无活动快照（低水位 0）时整体 no-op。
void TableHeap::RunInlineVacuum(page_id_t page_id, char* data, int32_t slot_count) {
    if (commit_tracker_ == nullptr || inline_vacuum_remaining_ <= 0) return;
    const int64_t low_water = commit_tracker_->OldestActiveSnapshot();
    if (low_water <= 0) return;
    const int n = InlineVacuumPage(page_id, data, slot_count, low_water,
                                   inline_vacuum_remaining_);
    inline_vacuum_remaining_ -= n;
}

// 惰性真空回收：回收 begin_csn < oldest_active_csn 的「已被替代/删除（end_xid!=0）」
// 非 head 旧版本槽位（写墓碑）。以最老活动快照为界，避免误删仍可能被读取的版本。
// Phase 2 起作为「低频兜底」通道：常规回收由写路径内联真空摊薄完成。
// Phase 3（t3）：回收时先做链摘除（同 InlineVacuumPage），保证墓碑槽不再被任何
// 版本引用、可被安全复用。
void TableHeap::Vacuum(int64_t oldest_active_csn) {
    if (oldest_active_csn <= 0) return;
    std::lock_guard<std::recursive_mutex> lk(write_mutex_);
    std::unordered_set<page_id_t> visited;
    page_id_t pid = first_page_id_;
    while (pid != INVALID_PAGE_ID && pid >= 0) {
        if (!visited.insert(pid).second) break;
        PageWriteGuard guard = PageWriteGuard::Fetch(buffer_pool_manager_, pid);
        if (!guard.Valid()) break;
        char* data = guard.Data();
        int32_t np, sc, fo;
        ReadPageHeader(data, np, sc, fo);
        bool changed = false;
        for (int32_t s = 0; s < sc; ++s) {
            int32_t o, l;
            ReadSlot(data, s, o, l);
            if (IsTombstone(l)) continue;
            MvccRecordHeader h;
            if (!ReadMvccHeader(data + o, &h)) continue;
            // end_xid != 0 = 已被替代/删除。可回收判据用 end_csn（替代/删除的
            // 提交水位）不晚于最老活动快照：任何活动快照都看不到「结束于它之前」
            // 的版本。不能用 begin_csn——「创建早、但删除/替代晚于某活动快照」的
            // 版本（如 begin=1、end=3，老读者 S=2 仍可见）会被 begin<2 误回收，
            // 导致老快照读者丢行（脏读/幻读）。
            if (h.end_xid != 0 && h.end_csn != 0 &&
                h.end_csn <= oldest_active_csn) {
                UnlinkVersionInPage(data, sc, pid, s, h);
                WriteSlot(data, s, 0, static_cast<int32_t>(kTombstone));
                changed = true;
            }
        }
        if (changed) guard.MarkDirty();
        pid = np;
    }
}

// Phase 3（t4）索引墓碑回收判定。仅页读闩 + 槽头检查 + 版本键比较，不依赖本堆
// SetSnapshot 状态（后台真空不得扰动共享堆的会话快照）。
//
// (i)  槽不可见 / 行已删除且失效水位对全部活动快照不可见 → kRemove。
// (ii) 头稳定且已提交、头键 != 条目键（旧键条目）：
//        * 头 begin_csn <= 最老活动快照 → kRemove（任何活动快照的可见版本都是该头）；
//        * 头 begin_csn > 最老活动快照 → 精确化：沿版本链逐版本检查「可见区间
//          [v.begin_csn, succ.begin_csn) 是否含任一活动快照且 v 键 == 条目键」——
//          有则 kKeep（仍有老快照需要该条目），无则 kRemove。判定期间新注册的快照
//          CSN 恒 >= 判定时全局 CSN >= head.begin_csn（head 已提交），其可见版本必为
//          head（键 != 条目键），故不会因「判定后再注册快照」而误删。
// legacy 无头 / 未提交写者 / 头键==条目键的活跃条目 / 链损坏（循环 / CSN 非递增 /
// 墓碑 / 越界 / 跨页取不到）→ kKeep。
TableHeap::IndexVacuumDecision TableHeap::DecideIndexEntry(
    const RID& rid, const IndexKey& entry_key,
    const std::vector<int64_t>& active_snapshots,
    const std::vector<int32_t>& key_col_indices,
    const std::vector<ValueType>& column_types) const {
    const int64_t oldest = active_snapshots.empty() ? 0 : active_snapshots.front();
    if (!rid.IsValid() || oldest <= 0) {
        return IndexVacuumDecision::kKeep;
    }
    PageReadGuard guard = PageReadGuard::Fetch(buffer_pool_manager_, rid.page_id);
    if (!guard.Valid()) return IndexVacuumDecision::kKeep;  // 页都取不到：保守保留
    const char* data = guard.Data();
    int32_t next_pid, slot_count, free_off;
    ReadPageHeader(data, next_pid, slot_count, free_off);
    if (rid.slot_num < 0 || rid.slot_num >= slot_count) {
        return IndexVacuumDecision::kRemove;  // 槽不存在：行已不在，读取方必读不到
    }
    int32_t off, len;
    ReadSlot(data, rid.slot_num, off, len);
    if (IsTombstone(len)) return IndexVacuumDecision::kRemove;
    if (off < kHeaderBytes || len <= 0 ||
        off > static_cast<int32_t>(PAGE_SIZE) - len) {
        return IndexVacuumDecision::kRemove;  // 记录越界/损坏：GetTuple 必返 false
    }
    MvccRecordHeader h;
    if (!ReadMvccHeader(data + off, &h)) return IndexVacuumDecision::kKeep;  // legacy

    // (i) 行已删除 / 头已被替代，且失效水位对全部活动快照不可见。
    if (h.end_xid != 0 && h.end_csn != 0 && h.end_csn <= oldest) {
        return IndexVacuumDecision::kRemove;
    }

    // (ii) 头稳定且已提交：提取头键。头键 == 条目键 → 活跃条目（真空后新读者仍需），
    // 一律 kKeep；头键 != 条目键 → 旧键条目，进入精确化判定。
    if (h.end_xid == 0 && h.begin_csn > 0) {
        const char* page_end = data + PAGE_SIZE;
        auto extract_key = [&](const char* rec, const char* end) -> std::pair<IndexKey, bool> {
            if (rec + sizeof(MvccRecordHeader) > end) return {{}, false};
            Tuple t = Tuple::Deserialize(rec + sizeof(MvccRecordHeader), column_types);
            IndexKey k;
            k.values.reserve(key_col_indices.size());
            for (int ci : key_col_indices) {
                if (ci < 0 || ci >= static_cast<int>(t.ColumnCount())) return {{}, false};
                k.values.push_back(t.GetValue(ci));
            }
            return {std::move(k), true};
        };
        auto head_key = extract_key(data + off, page_end);
        if (!head_key.second) return IndexVacuumDecision::kKeep;
        if (CompareKeyOnly(head_key.first, entry_key) == 0) {
            return IndexVacuumDecision::kKeep;  // 活跃条目
        }
        // fast path：头对全部活动快照可见 → 旧键版本无人可见。
        if (h.begin_csn <= oldest) {
            return IndexVacuumDecision::kRemove;
        }
        // 精确化：头改写提交晚于最老活动快照。沿链从 head.prev 走向更旧版本，
        // 检查每个版本是否被某活动快照可见（可见区间含该快照）且键 == 条目键。
        // 链上比「最老快照可见版本」更旧的版本，其可见区间上界 <= oldest <= 全部
        // 活动快照 → 不可能被任何活动快照可见，无需检查（到 begin_csn <= oldest
        // 的版本即可停止）。
        // 无步数上限：合法链沿 prev 必然终结（链尾 / 达最老快照可见版本），长链
        // （>64 版本）也能完整判定，不再保守 kKeep 漏回收。防损坏链循环靠双保险：
        //   * 访问过的 (page, slot) 集合——链循环必复访（同事务多版本共享同一
        //     CSN，故不能用 CSN 严格递减判环，必须用访问集；预先登记 head 槽，
        //     链直接回指 head 立即被否决）；
        //   * CSN 沿链非递增——vh.begin_csn > succ_begin 即损坏，快速否决。
        // 跨页步进「释放上一页读闩后再 Fetch 下一页」、同页步进复用 head 页读闩
        // （data/guard 全程有效），不违反「持页闩不请求 BPM」锁序。
        bool seen = false;
        int64_t succ_begin = h.begin_csn;  // 当前版本的后继（更近 head）的 begin_csn
        int32_t v_page = h.prev_page_id;
        int32_t v_slot = h.prev_slot_num;
        std::unordered_set<uint64_t> visited;
        visited.insert(((uint64_t)(uint32_t)rid.page_id << 32) | (uint32_t)rid.slot_num);
        while (!seen && v_page != INVALID_PAGE_ID) {
            const uint64_t vkey = ((uint64_t)(uint32_t)v_page << 32) | (uint32_t)v_slot;
            if (!visited.insert(vkey).second) { seen = true; break; }  // 复访：链循环
            const char* vd = data;  // 同页版本复用 head 页读闩（head 页仍被 guard 持有）
            PageReadGuard vg;       // 跨页版本：本步局部读闩（作用域延至本步结束）
            if (v_page != rid.page_id) {
                vg = PageReadGuard::Fetch(buffer_pool_manager_, v_page);
                if (!vg.Valid()) { seen = true; break; }  // 页取不到：保守 kKeep
                vd = vg.Data();
            }
            int32_t v_next_pid, v_slot_count, v_free_off;
            ReadPageHeader(vd, v_next_pid, v_slot_count, v_free_off);
            if (v_slot < 0 || v_slot >= v_slot_count) { seen = true; break; }  // 链损坏
            int32_t voff, vlen;
            ReadSlot(vd, v_slot, voff, vlen);
            if (IsTombstone(vlen)) { seen = true; break; }  // 版本已被回收：区间不可知
            if (voff < kHeaderBytes || vlen <= 0 ||
                voff > static_cast<int32_t>(PAGE_SIZE) - vlen) {
                seen = true; break;  // 损坏
            }
            MvccRecordHeader vh;
            if (!ReadMvccHeader(vd + voff, &vh)) { seen = true; break; }  // legacy：保守
            if (vh.begin_csn <= 0) { seen = true; break; }  // 未提交写者：保守
            if (vh.begin_csn > succ_begin) { seen = true; break; }  // CSN 沿链非递增被破坏
            auto vk = extract_key(vd + voff, vd + PAGE_SIZE);
            if (!vk.second) { seen = true; break; }
            // 该版本可见区间 [vh.begin_csn, succ_begin)：是否存在活动快照落入？
            if (CompareKeyOnly(vk.first, entry_key) == 0) {
                auto it = std::lower_bound(active_snapshots.begin(),
                                           active_snapshots.end(), vh.begin_csn);
                if (it != active_snapshots.end() && *it < succ_begin) {
                    seen = true;  // 有活动快照仍需要该条目
                    break;
                }
            }
            if (vh.begin_csn <= oldest) {
                // 已达最老快照的可见版本：更旧版本可见区间上界 <= oldest <= 全部
                // 活动快照，不可能被任何活动快照可见 → 正常终止（不继续走）。
                break;
            }
            succ_begin = vh.begin_csn;
            v_page = vh.prev_page_id;
            v_slot = vh.prev_slot_num;
        }
        if (seen) return IndexVacuumDecision::kKeep;
        // 自然终结（链尾 / 达最老快照可见版本）→ 无活动快照需要该条目 → kRemove。
        return IndexVacuumDecision::kRemove;
    }
    return IndexVacuumDecision::kKeep;
}

// ============ TableHeap::Iterator ============

TableHeap::Iterator::Iterator(TableHeap* table_heap, RID start_rid)
    : table_heap_(table_heap), current_rid_(start_rid) {
}

bool TableHeap::Iterator::HasNext() const {
    return current_rid_.IsValid();
}

Tuple TableHeap::Iterator::Next(const std::vector<ValueType>& column_types) {
    Tuple t;
    if (!current_rid_.IsValid()) return t;
    RID rid_to_read = current_rid_;
    RID next;
    table_heap_->FindNextRid(rid_to_read, &next);
    current_rid_ = next;
    table_heap_->GetTuple(rid_to_read, &t, column_types);
    return t;
}

TableHeap::Iterator TableHeap::Begin() {
    RID first;
    FindNextRid(RID(), &first);
    return Iterator(this, first);
}

}  // namespace sqlcompiler