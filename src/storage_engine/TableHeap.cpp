#include "storage_engine/TableHeap.h"

#include <cstring>
#include <unordered_set>

#include "index/PageGuard.h"
#include "storage/Page.h"
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
    PageGuard guard = PageGuard::Fetch(buffer_pool_manager_, page_id);
    if (!guard.Valid()) return false;
    char* data = guard.Data();
    int32_t next_pid, slot_count, free_off;
    ReadPageHeader(data, next_pid, slot_count, free_off);
    bool header_fixed = NormalizePageHeader(data, next_pid, slot_count, free_off);
    if (header_fixed) guard.MarkDirty();

    std::vector<char> serialized = tuple.Serialize(column_types);
    int32_t len = static_cast<int32_t>(serialized.size());

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
        std::memcpy(data + new_off, serialized.data(), len);
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
            PageGuard guard = PageGuard::Fetch(buffer_pool_manager_, pid);
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
        PageGuard prev = PageGuard::Fetch(buffer_pool_manager_, prev_pid);
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
    PageGuard guard = PageGuard::Fetch(buffer_pool_manager_, rid.page_id);
    if (!guard.Valid()) return false;
    char* data = guard.Data();
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
    // 若页面被误用（例如目录页与用户表页冲突）或文件损坏，这里挡住越界读取，
    // 避免 Tuple::Deserialize 读到页外内存而崩溃。
    if (off < kHeaderBytes || len <= 0 ||
        off > static_cast<int32_t>(PAGE_SIZE) - len) {
        return false;
    }
    if (tuple) {
        *tuple = Tuple::Deserialize(data + off, column_types);
        tuple->SetRid(rid);
    }
    return true;
}

bool TableHeap::DeleteTuple(const RID& rid) {
    if (!rid.IsValid()) return false;
    PageGuard guard = PageGuard::Fetch(buffer_pool_manager_, rid.page_id);
    if (!guard.Valid()) return false;
    char* data = guard.Data();
    int32_t next_pid, slot_count, free_off;
    ReadPageHeader(data, next_pid, slot_count, free_off);
    if (rid.slot_num < 0 || rid.slot_num >= slot_count) {
        return false;
    }
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
    WriteSlot(data, rid.slot_num, 0, static_cast<int32_t>(kTombstone));
    guard.MarkDirty();

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
    PageGuard guard = PageGuard::Fetch(buffer_pool_manager_, rid.page_id);
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
    if (new_len <= len) {
        if (new_len < len) {
            std::memset(data + off + new_len, 0, len - new_len);
        }
        std::memcpy(data + off, serialized.data(), new_len);
        WriteSlot(data, rid.slot_num, off, new_len);
        guard.MarkDirty();

        // Phase B：写 UPDATE 记录（in-place，before/after 都在同一页）。
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
    // Cannot grow in place — delete and reinsert
    // 关键：DeleteTuple 和 InsertTuple 各自会写自己的 WAL 记录；这里不再单独追加。
    DeleteTuple(rid);
    return InsertTuple(new_tuple, nullptr, column_types);
}

void TableHeap::ClearAll() {
    // Walk the linked list of pages, free every overflow page beyond the
    // first, then reset the first page's slot directory to empty.
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
    PageGuard head = PageGuard::Fetch(buffer_pool_manager_, first_page_id_);
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
        PageGuard guard = PageGuard::Fetch(buffer_pool_manager_, pid);
        if (!guard.Valid()) return false;
        char* data = guard.Data();
        int32_t next_pid, slot_count, free_off;
        ReadPageHeader(data, next_pid, slot_count, free_off);
        bool header_fixed =
            NormalizePageHeader(data, next_pid, slot_count, free_off);
        if (header_fixed) guard.MarkDirty();
        while (slot_num < slot_count) {
            int32_t off, len;
            ReadSlot(data, slot_num, off, len);
            if (!IsTombstone(len)) {
                if (next) {
                    next->page_id = pid;
                    next->slot_num = slot_num;
                }
                if (header_fixed) guard.MarkDirty();
                return true;
            }
            ++slot_num;
        }
        if (header_fixed) guard.MarkDirty();
        // next_pid 越界保护
        if (next_pid < 0) return false;
        pid = next_pid;
        slot_num = 0;
    }
    return false;
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