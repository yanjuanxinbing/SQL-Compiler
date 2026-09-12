#include "storage_engine/TableHeap.h"

#include <cstring>
#include <unordered_set>

#include "index/PageGuard.h"
#include "storage/Page.h"
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

// MVCC：版本可见性谓词——版本 v 对快照 S 是否可见。
//   * creation：v.begin_xid 非自身且未提交 -> 不可见（排斥脏读）；已提交但 CSN>S -> 不可见。
//   * supersession：v.end_xid==0 仍为最新 -> 可见；end_xid 为自身 -> 被自己删除/替代，不可见；
//                   end 写者未提交 -> 可见（删除尚未提交）；end CSN>S -> 删除发生在快照后，可见。
bool VisibleForSnapshot(const MvccRecordHeader& v, int64_t S, int64_t self_xid,
                        CommitTracker* tracker) {
    if (v.begin_xid != self_xid && v.begin_xid != 0) {
        int64_t bcsn = 0;
        if (!tracker->LookupCommitted(v.begin_xid, &bcsn)) return false;
        if (bcsn > S) return false;
    }
    if (v.end_xid == 0) return true;
    if (v.end_xid == self_xid) return false;
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

    // Need room for: new slot entry (kSlotBytes) + record bytes
    int32_t slot_dir_end = kHeaderBytes + (slot_count + 1) * kSlotBytes;
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
            // 提交时回填 begin_csn（新版本）：记录 head 槽位。
            active_txn_->AddVersionSlot(page_id, slot_count, /*is_end=*/false);
            // 写集基（纯 INSERT 无旧基）：FCW 提交时看到 head==自身即跳过。
            active_txn_->AddToWriteSet(RID{page_id, slot_count}, 0, 0);
        }
        std::memcpy(data + new_off + hdr, serialized.data(), serialized.size());
    }
    WriteSlot(data, slot_count, new_off, len);
    WritePageHeader(data, next_pid, slot_count + 1, new_off);
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
        rid->slot_num = slot_count;
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
            WritePageHeader(prev.Data(), new_pid, slot_count, free_off);
            prev.MarkDirty();
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

    // 最终要反序列化的版本内容位置（默认 = head 自身）。快照模式下可能沿链
    // 回到更旧的版本页，data 会被换成该版本页指针。
    int32_t v_off = off;
    bool adopt_and_record_base = false;
    int64_t base_xid = 0, base_csn = 0;

    if (has_head_hdr && commit_tracker_ != nullptr) {
        // ---- 快照读：沿链（头 -> 越来越旧）找首个可见版本 ----
        const int64_t S = snapshot_csn_;
        Transaction* self = (active_txn_ != nullptr && active_txn_->IsActive())
                                ? active_txn_
                                : nullptr;
        const int64_t self_xid = self ? self->GetTxnId() : 0;
        MvccRecordHeader cur = head;
        page_id_t cpid = rid.page_id;
        int32_t cslot = rid.slot_num;
        int32_t coff = off;
        bool found = false;
        while (true) {
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
                             const std::vector<ValueType>& column_types) {
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
            return true;
        }
        // Cannot grow in place — delete and reinsert
        // 关键：DeleteTuple 和 InsertTuple 各自会写自己的 WAL 记录；这里不再单独追加。
        // 先把本方法持有的页写锁归还（同帧二次 Grab 独占锁会自锁），write_mutex_ 是
        // 递归锁，DeleteTuple/InsertTuple 内部再次加锁放行。
        guard.Release();
        DeleteTuple(rid);
        return InsertTuple(new_tuple, nullptr, column_types);
    }

    // ================= 快照更新：版本链（RID 稳定） =================
    // 把旧 head 迁到本页新分配的 slot（其 end_xid=me、保留其 prev），再在稳定
    // head 槽覆写新版本（begin_xid=me、end_xid=0、prev=新 slot）。条件：
    //   1) 新版本内容放得进 head 槽（new_total <= len）；
    //   2) 本页有空间容纳「新 slot 项 + 旧 head 内容」。
    const int32_t new_total = hdr + new_len;
    const int32_t old_cols = len - hdr;  // 旧 head 的列字节数（有头）
    int32_t slot_dir_end = kHeaderBytes + (slot_count + 1) * kSlotBytes;
    if (new_total <= len && old_cols >= 0 &&
        (hdr + old_cols + static_cast<int32_t>(kSlotBytes)) <= (free_off - slot_dir_end)) {
        // a) 把旧 head（含头）迁到新槽。
        int32_t new_old_off = free_off - len;
        MvccRecordHeader old_v = head_hdr;
        old_v.end_xid = active_txn_->GetTxnId();
        old_v.end_csn = 0;  // 提交时回填
        WriteMvccHeader(data + new_old_off, old_v);
        std::memcpy(data + new_old_off + hdr, data + off + hdr, old_cols);
        WriteSlot(data, slot_count, new_old_off, len);
        // b) 覆写稳定 head 槽为新版本。
        MvccRecordHeader nh;
        std::memset(&nh, 0, sizeof(nh));
        nh.magic = kMvccMagic;
        nh.begin_xid = active_txn_->GetTxnId();
        nh.end_xid = 0;
        nh.prev_page_id = rid.page_id;
        nh.prev_slot_num = slot_count;  // 指向刚迁出的旧版本
        WriteMvccHeader(data + off, nh);
        std::memcpy(data + off + hdr, serialized.data(), new_len);
        WriteSlot(data, rid.slot_num, off, new_total);
        WritePageHeader(data, next_pid, slot_count + 1, new_old_off);
        guard.MarkDirty();

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
        active_txn_->AddVersionSlot(rid.page_id, slot_count, /*is_end=*/true);
        // 写集基：取快照读登记的可见版本写者；无则回退 head（或 0）。
        int64_t bx = 0, bc = 0;
        if (!active_txn_->TakeSnapshotRead(rid, &bx, &bc)) {
            bx = head_hdr.begin_xid;
            bc = head_hdr.begin_csn;
        }
        active_txn_->AddToWriteSet(rid, bx, bc);
        return true;
    }

    // 空间不足无法原位建链：退级为 relocate（RID 变化由上层索引维护补齐）。
    guard.Release();
    DeleteTuple(rid);
    return InsertTuple(new_tuple, nullptr, column_types);
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

// 惰性真空回收：回收 begin_csn < oldest_active_csn 的「已被替代/删除（end_xid!=0）」
// 非 head 旧版本槽位（写墓碑）。以最老活动快照为界，避免误删仍可能被读取的版本。
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
            // end_xid!=0 = 已被替代/删除；begin_csn 已提交且早于最老活动快照 → 可回收。
            if (h.end_xid != 0 && h.begin_csn != 0 && h.begin_csn < oldest_active_csn) {
                WriteSlot(data, s, 0, static_cast<int32_t>(kTombstone));
                changed = true;
            }
        }
        if (changed) guard.MarkDirty();
        pid = np;
    }
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