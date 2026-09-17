#include "storage_engine/TableHeap.h"

#include <cstring>
#include <unordered_set>

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

TableHeap::TableHeap(StorageAccess* storage, page_id_t first_page_id)
    : storage_(storage), first_page_id_(first_page_id) {
}

TableHeap* TableHeap::Create(StorageAccess* storage) {
    page_id_t pid = INVALID_PAGE_ID;
    Page* page = storage->NewPage(&pid);
    if (!page) return nullptr;
    InitEmptyPageHeader(page->GetData());
    page->SetDirty(true);
    storage->UnpinPage(pid, true);
    return new TableHeap(storage, pid);
}

TableHeap* TableHeap::Open(StorageAccess* storage, page_id_t first_page_id) {
    return new TableHeap(storage, first_page_id);
}

page_id_t TableHeap::GetFirstPageId() const {
    return first_page_id_;
}

bool TableHeap::InsertIntoPage(page_id_t page_id, const Tuple& tuple, RID* rid,
                                 const std::vector<ValueType>& column_types,
                                 page_id_t* out_next_pid) {
    Page* page = storage_->GetPage(page_id);
    if (!page) return false;
    char* data = page->GetData();
    int32_t next_pid, slot_count, free_off;
    ReadPageHeader(data, next_pid, slot_count, free_off);
    bool header_fixed = NormalizePageHeader(data, next_pid, slot_count, free_off);
    if (header_fixed) page->SetDirty(true);

    std::vector<char> serialized = tuple.Serialize(column_types);
    int32_t len = static_cast<int32_t>(serialized.size());

    // Tombstone reuse (item #2): 先扫一遍现有 slot，找第一个墓碑位复用——
    // 避免每次 DELETE 后都让 slot_count 单调递增、最终把 slot 目录撑爆。
    // 扫描代价 O(M)（M = 当前 slot_count），远比 O(M*kMaxSlots) 安全。
    int32_t reuse_slot = -1;
    for (int32_t s = 0; s < slot_count; ++s) {
        int32_t o, l;
        ReadSlot(data, s, o, l);
        if (IsTombstone(l)) { reuse_slot = s; break; }
    }

    // 算"插入后"是否还有空间。新 slot 总是占 1 个槽（要么复用、要么追加）；
    // 复用路径下 slot_count 不变，所以 slot_dir_end 不变；追加路径下 +1。
    int32_t new_slot_count = (reuse_slot >= 0) ? slot_count : slot_count + 1;
    int32_t slot_dir_end = kHeaderBytes + new_slot_count * kSlotBytes;
    if (slot_dir_end > free_off || len > free_off - slot_dir_end) {
        if (out_next_pid) *out_next_pid = next_pid;
        storage_->UnpinPage(page_id, header_fixed);
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
    int32_t target_slot = (reuse_slot >= 0) ? reuse_slot : slot_count;
    WriteSlot(data, target_slot, new_off, len);
    WritePageHeader(data, next_pid, new_slot_count, new_off);
    page->SetDirty(true);

    // Phase B：写 WAL（UPDATE 记录；before/after 都是 PAGE_SIZE 字节）。
    if (log_manager_ != nullptr) {
        LogRecord rec;
        rec.type_ = LogRecordType::UPDATE;
        rec.txn_id_ = (active_txn_ != nullptr) ? active_txn_->GetTxnId() : 0;
        rec.page_id_ = page_id;
        rec.before_image_ = std::move(before_image);
        rec.after_image_.assign(data, data + PAGE_SIZE);
        lsn_t lsn = log_manager_->AppendRecord(std::move(rec));
        page->SetPageLsn(lsn);
        // Phase C：回填 in-memory undo 记录的 LSN。
        if (active_txn_ != nullptr && active_txn_->IsActive()) {
            active_txn_->SetLastUndoLSN(lsn);
        }
    }
    if (rid) {
        rid->page_id = page_id;
        rid->slot_num = target_slot;
    }
    if (out_next_pid) *out_next_pid = next_pid;
    storage_->UnpinPage(page_id, true);
    return true;
}

bool TableHeap::InsertTuple(const Tuple& tuple, RID* rid,
                            const std::vector<ValueType>& column_types) {
    page_id_t pid = first_page_with_space_;
    if (pid == INVALID_PAGE_ID) {
        // 游标未初始化（重新打开堆 / 首次写入），从首页起步。
        pid = first_page_id_;
    }
    page_id_t prev_pid = INVALID_PAGE_ID;
    // 防止损坏的 next_pid 形成环导致死循环（例如全零页自指 page 0）
    std::unordered_set<page_id_t> visited;
    while (pid != INVALID_PAGE_ID && pid >= 0) {
        if (!visited.insert(pid).second) break;
        // out_next_pid 让我们一次 page fetch 完成"尝试插入 + 取链表下一节点"，
        // 避免失败后再 GetPage+UnpinPage 一次。
        page_id_t next_pid_unused = INVALID_PAGE_ID;
        if (InsertIntoPage(pid, tuple, rid, column_types, &next_pid_unused)) {
            // 命中一页：检查"是否变满"。若变满（slot 目录到顶 / 数据满），
            // 把游标推进到下一页；否则留在本页（O(1) 摊销）。
            Page* page = storage_->GetPage(pid);
            if (page) {
                char* data = page->GetData();
                int32_t nxt, sc, fo;
                ReadPageHeader(data, nxt, sc, fo);
                int32_t slot_dir_end = kHeaderBytes + sc * kSlotBytes;
                if (slot_dir_end >= fo) {
                    // 已满：游标前进到下一页（保证下次插入不会再次撞同一页）
                    first_page_with_space_ = nxt;
                } else {
                    first_page_with_space_ = pid;
                }
                storage_->UnpinPage(pid, false);
            }
            return true;
        }
        // 插入失败（页已满）：游标前进到 next_pid_unused——InsertIntoPage 已
        // 把 next_pid 写进 out_next_pid，省掉一次额外的 GetPage+UnpinPage。
        first_page_with_space_ = next_pid_unused;
        prev_pid = pid;
        if (next_pid_unused == pid) break;
        pid = next_pid_unused;
    }
    // Allocate a new page and link from prev page
    page_id_t new_pid = INVALID_PAGE_ID;
    Page* new_page = storage_->NewPage(&new_pid);
    if (!new_page) return false;
    InitEmptyPageHeader(new_page->GetData());
    new_page->SetDirty(true);
    storage_->UnpinPage(new_pid, true);
    if (prev_pid != INVALID_PAGE_ID) {
        Page* prev = storage_->GetPage(prev_pid);
        if (prev) {
            int32_t next_pid, slot_count, free_off;
            ReadPageHeader(prev->GetData(), next_pid, slot_count, free_off);
            WritePageHeader(prev->GetData(), new_pid, slot_count, free_off);
            prev->SetDirty(true);
            storage_->UnpinPage(prev_pid, true);
        }
    } else {
        first_page_id_ = new_pid;
    }
    // 新页是空的；插入并把游标设回它（之后若变满会再前进）。
    if (InsertIntoPage(new_pid, tuple, rid, column_types)) {
        first_page_with_space_ = new_pid;
        return true;
    }
    return false;
}

bool TableHeap::GetTuple(const RID& rid, Tuple* tuple,
                          const std::vector<ValueType>& column_types) {
    if (!rid.IsValid()) return false;
    Page* page = storage_->GetPage(rid.page_id);
    if (!page) return false;
    char* data = page->GetData();
    int32_t next_pid, slot_count, free_off;
    ReadPageHeader(data, next_pid, slot_count, free_off);
    if (rid.slot_num < 0 || rid.slot_num >= slot_count) {
        storage_->UnpinPage(rid.page_id, false);
        return false;
    }
    int32_t off, len;
    ReadSlot(data, rid.slot_num, off, len);
    if (IsTombstone(len)) {
        storage_->UnpinPage(rid.page_id, false);
        return false;
    }
    // 防御性边界校验：slot 中的 (off, len) 必须完整落在数据区内。
    // 若页面被误用（例如目录页与用户表页冲突）或文件损坏，这里挡住越界读取，
    // 避免 Tuple::Deserialize 读到页外内存而崩溃。
    if (off < kHeaderBytes || len <= 0 ||
        off > static_cast<int32_t>(PAGE_SIZE) - len) {
        storage_->UnpinPage(rid.page_id, false);
        return false;
    }
    if (tuple) {
        *tuple = Tuple::Deserialize(data + off, column_types);
        tuple->SetRid(rid);
    }
    storage_->UnpinPage(rid.page_id, false);
    return true;
}

bool TableHeap::DeleteTuple(const RID& rid) {
    if (!rid.IsValid()) return false;
    Page* page = storage_->GetPage(rid.page_id);
    if (!page) return false;
    char* data = page->GetData();
    int32_t next_pid, slot_count, free_off;
    ReadPageHeader(data, next_pid, slot_count, free_off);
    if (rid.slot_num < 0 || rid.slot_num >= slot_count) {
        storage_->UnpinPage(rid.page_id, false);
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
    page->SetDirty(true);

    // Phase B：写 UPDATE 记录（before = 原 page，after = 带墓碑的 page）。
    if (log_manager_ != nullptr) {
        LogRecord rec;
        rec.type_ = LogRecordType::UPDATE;
        rec.txn_id_ = (active_txn_ != nullptr) ? active_txn_->GetTxnId() : 0;
        rec.page_id_ = rid.page_id;
        rec.before_image_ = std::move(before_image);
        rec.after_image_.assign(data, data + PAGE_SIZE);
        lsn_t lsn = log_manager_->AppendRecord(std::move(rec));
        page->SetPageLsn(lsn);
        // Phase C：回填 in-memory undo 记录的 LSN。
        if (active_txn_ != nullptr && active_txn_->IsActive()) {
            active_txn_->SetLastUndoLSN(lsn);
        }
    }
    storage_->UnpinPage(rid.page_id, true);
    // Item #3 (optional part): 如果本页之前已经"满了"且游标已越过它（指向它
    // 之后某页），那这条 DELETE 释放出的 slot 让本页重新可写——回退游标。
    // 判定方式：slot_dir_end 算出来比 free_off 至少小 1 字节即"有空间"。
    // 只有当 cursor 严格晚于本页时才回退，避免把游标"重置"到一个其实已满的
    // 页上。
    if (first_page_with_space_ != INVALID_PAGE_ID &&
        rid.page_id < first_page_with_space_) {
        Page* probe = storage_->GetPage(rid.page_id);
        if (probe) {
            char* pd = probe->GetData();
            int32_t n2, s2, f2;
            ReadPageHeader(pd, n2, s2, f2);
            int32_t slot_dir_end2 = kHeaderBytes + s2 * kSlotBytes;
            if (slot_dir_end2 < f2) {
                first_page_with_space_ = rid.page_id;
            }
            storage_->UnpinPage(rid.page_id, false);
        }
    }
    return true;
}

bool TableHeap::UpdateTuple(const RID& rid, const Tuple& new_tuple,
                             const std::vector<ValueType>& column_types,
                             RID* out_new_rid) {
    if (!rid.IsValid()) return false;
    Page* page = storage_->GetPage(rid.page_id);
    if (!page) return false;
    char* data = page->GetData();
    int32_t next_pid, slot_count, free_off;
    ReadPageHeader(data, next_pid, slot_count, free_off);
    if (rid.slot_num < 0 || rid.slot_num >= slot_count) {
        storage_->UnpinPage(rid.page_id, false);
        return false;
    }
    int32_t off, len;
    ReadSlot(data, rid.slot_num, off, len);
    if (IsTombstone(len) || off < 0 || len < 0 ||
        off + len > static_cast<int32_t>(PAGE_SIZE)) {
        storage_->UnpinPage(rid.page_id, false);
        return false;
    }
    std::vector<char> serialized = new_tuple.Serialize(column_types);
    int32_t new_len = static_cast<int32_t>(serialized.size());
    if (new_len == 0) {
        storage_->UnpinPage(rid.page_id, false);
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
        page->SetDirty(true);

        // 原位更新：RID 不变。
        if (out_new_rid != nullptr) *out_new_rid = rid;

        // Phase B：写 UPDATE 记录（in-place，before/after 都在同一页）。
        if (log_manager_ != nullptr) {
            LogRecord rec;
            rec.type_ = LogRecordType::UPDATE;
            rec.txn_id_ = (active_txn_ != nullptr) ? active_txn_->GetTxnId() : 0;
            rec.page_id_ = rid.page_id;
            rec.before_image_ = std::move(before_image);
            rec.after_image_.assign(data, data + PAGE_SIZE);
            lsn_t lsn = log_manager_->AppendRecord(std::move(rec));
            page->SetPageLsn(lsn);
            // Phase C：回填 in-memory undo 记录的 LSN。
            if (active_txn_ != nullptr && active_txn_->IsActive()) {
                active_txn_->SetLastUndoLSN(lsn);
            }
        }
        storage_->UnpinPage(rid.page_id, true);
        return true;
    }
    storage_->UnpinPage(rid.page_id, false);
    // Cannot grow in place — delete and reinsert
    // 关键：DeleteTuple 和 InsertTuple 各自会写自己的 WAL 记录；这里不再单独追加。
    // 与 in-place 路径不同：原 slot 被墓碑化，行被搬到新 slot，RID 改变。
    // 调用方必须用 out_new_rid 拿到正确位置；否则 InsertIntoIndexes 等
    // 基于旧 RID 的索引项会指向已墓碑化的 slot，导致后续 UPDATE 撞 PK。
    DeleteTuple(rid);
    RID new_rid;
    bool ok = InsertTuple(new_tuple, &new_rid, column_types);
    if (ok && out_new_rid != nullptr) *out_new_rid = new_rid;
    return ok;
}

void TableHeap::ClearAll() {
    // Walk the linked list of pages, free every overflow page beyond the
    // first, then reset the first page's slot directory to empty.
    page_id_t pid = first_page_id_;
    if (pid == INVALID_PAGE_ID) return;

    // Find first page header
    Page* first = storage_->GetPage(pid);
    if (!first) return;
    char* first_data = first->GetData();
    int32_t next_pid, slot_count, free_off;
    ReadPageHeader(first_data, next_pid, slot_count, free_off);
    storage_->UnpinPage(pid, false);

    // Free overflow pages
    page_id_t cur = next_pid;
    while (cur != INVALID_PAGE_ID && cur >= 0) {
        Page* p = storage_->GetPage(cur);
        if (!p) return;
        int32_t nnext, ns, nf;
        ReadPageHeader(p->GetData(), nnext, ns, nf);
        storage_->UnpinPage(cur, false);
        page_id_t victim = cur;
        cur = nnext;
        if (!storage_->DeletePage(victim)) {
            // DeletePage failed (probably pinned); bail out — the table is
            // still partially cleared, but tombstones on overflow pages
            // are still skipped by the iterator, so the user-visible effect
            // is consistent.
            break;
        }
    }

    // Reset first page header to empty
    Page* head = storage_->GetPage(first_page_id_);
    if (!head) return;
    InitEmptyPageHeader(head->GetData());
    head->SetDirty(true);
    storage_->UnpinPage(first_page_id_, true);
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
        Page* page = storage_->GetPage(pid);
        if (!page) return false;
        char* data = page->GetData();
        int32_t next_pid, slot_count, free_off;
        ReadPageHeader(data, next_pid, slot_count, free_off);
        bool header_fixed =
            NormalizePageHeader(data, next_pid, slot_count, free_off);
        if (header_fixed) page->SetDirty(true);
        while (slot_num < slot_count) {
            int32_t off, len;
            ReadSlot(data, slot_num, off, len);
            if (!IsTombstone(len)) {
                if (next) {
                    next->page_id = pid;
                    next->slot_num = slot_num;
                }
                storage_->UnpinPage(pid, header_fixed);
                return true;
            }
            ++slot_num;
        }
        storage_->UnpinPage(pid, header_fixed);
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

TableHeap::Iterator::~Iterator() {
    // 跨 Next 调用复用的 pinned page 必须在这里释放，否则 frame pin count
    // 永不归零、永远不能被 Replacer 选为淘汰候选。
    if (current_page_guard_ != INVALID_PAGE_ID) {
        table_heap_->storage_->UnpinPage(current_page_guard_, false);
        current_page_guard_ = INVALID_PAGE_ID;
        current_page_ptr_ = nullptr;
    }
}

TableHeap::Iterator::Iterator(Iterator&& other) noexcept
    : table_heap_(other.table_heap_),
      current_rid_(other.current_rid_),
      current_page_guard_(other.current_page_guard_),
      current_page_ptr_(other.current_page_ptr_),
      exhausted_(other.exhausted_) {
    other.current_rid_ = RID();
    other.current_page_guard_ = INVALID_PAGE_ID;
    other.current_page_ptr_ = nullptr;
    other.exhausted_ = false;
}

TableHeap::Iterator& TableHeap::Iterator::operator=(Iterator&& other) noexcept {
    if (this != &other) {
        // 释放当前持有的 pin
        if (current_page_guard_ != INVALID_PAGE_ID) {
            table_heap_->storage_->UnpinPage(current_page_guard_, false);
        }
        table_heap_ = other.table_heap_;
        current_rid_ = other.current_rid_;
        current_page_guard_ = other.current_page_guard_;
        current_page_ptr_ = other.current_page_ptr_;
        exhausted_ = other.exhausted_;
        other.current_rid_ = RID();
        other.current_page_guard_ = INVALID_PAGE_ID;
        other.current_page_ptr_ = nullptr;
        other.exhausted_ = false;
    }
    return *this;
}

bool TableHeap::Iterator::EnsurePagePinned(page_id_t page_id) {
    if (current_page_guard_ == page_id && current_page_ptr_ != nullptr) return true;
    if (current_page_guard_ != INVALID_PAGE_ID) {
        table_heap_->storage_->UnpinPage(current_page_guard_, false);
        current_page_guard_ = INVALID_PAGE_ID;
        current_page_ptr_ = nullptr;
    }
    Page* p = table_heap_->storage_->GetPage(page_id);
    if (p == nullptr) return false;
    current_page_guard_ = page_id;
    current_page_ptr_ = p;
    return true;
}

bool TableHeap::Iterator::AdvanceToNextValidSlot(int start_slot, RID* next_rid) {
    // 已完成或从未开始：从首页起步。
    page_id_t pid = current_page_guard_;
    int slot = start_slot;
    if (pid == INVALID_PAGE_ID) {
        pid = table_heap_->first_page_id_;
        slot = 0;
    }
    // 防止损坏的 next_pid 形成环。
    std::unordered_set<page_id_t> visited;
    while (pid != INVALID_PAGE_ID && pid >= 0) {
        if (!visited.insert(pid).second) {
            current_rid_ = RID();
            if (current_page_guard_ != INVALID_PAGE_ID) {
                table_heap_->storage_->UnpinPage(current_page_guard_, false);
                current_page_guard_ = INVALID_PAGE_ID;
                current_page_ptr_ = nullptr;
            }
            return false;
        }
        if (!EnsurePagePinned(pid)) {
            current_rid_ = RID();
            current_page_guard_ = INVALID_PAGE_ID;
            current_page_ptr_ = nullptr;
            return false;
        }
        // 用 current_page_ptr_ 直接访问 pinned 帧的数据，避免再 GetPage 一次
        // （每次 GetPage 都 +1 pin count，会造成 pin count 漂移）。
        Page* page = current_page_ptr_;
        char* data = page->GetData();
        int32_t next_pid, slot_count, free_off;
        ReadPageHeader(data, next_pid, slot_count, free_off);
        bool header_fixed =
            NormalizePageHeader(data, next_pid, slot_count, free_off);
        if (header_fixed) page->SetDirty(true);
        while (slot < slot_count) {
            int32_t off, len;
            ReadSlot(data, slot, off, len);
            if (!IsTombstone(len)) {
                next_rid->page_id = pid;
                next_rid->slot_num = slot;
                current_rid_ = *next_rid;
                return true;
            }
            ++slot;
        }
        // 当前页没找到 → 跳到下一页
        if (next_pid == pid) {
            // 自指环：保护
            current_rid_ = RID();
            table_heap_->storage_->UnpinPage(pid, header_fixed);
            current_page_guard_ = INVALID_PAGE_ID;
            current_page_ptr_ = nullptr;
            return false;
        }
        if (next_pid < 0) {
            current_rid_ = RID();
            table_heap_->storage_->UnpinPage(pid, header_fixed);
            current_page_guard_ = INVALID_PAGE_ID;
            current_page_ptr_ = nullptr;
            return false;
        }
        table_heap_->storage_->UnpinPage(pid, header_fixed);
        current_page_guard_ = INVALID_PAGE_ID;
        current_page_ptr_ = nullptr;
        pid = next_pid;
        slot = 0;
    }
    current_rid_ = RID();
    if (current_page_guard_ != INVALID_PAGE_ID) {
        table_heap_->storage_->UnpinPage(current_page_guard_, false);
        current_page_guard_ = INVALID_PAGE_ID;
        current_page_ptr_ = nullptr;
    }
    return false;
}

bool TableHeap::Iterator::HasNext() {
    if (exhausted_) return false;
    if (current_rid_.IsValid()) return true;
    // 第一次调用：从堆首扫到第一个非墓碑 slot。复用 AdvanceToNextValidSlot。
    RID nxt;
    if (!AdvanceToNextValidSlot(0, &nxt)) {
        exhausted_ = true;
        return false;
    }
    return true;
}

Tuple TableHeap::Iterator::Next(const std::vector<ValueType>& column_types) {
    Tuple t;
    if (exhausted_) return t;
    if (!current_rid_.IsValid()) {
        // 与 HasNext 一致：第一次 Next（未先 HasNext）从堆首起步。
        RID nxt;
        if (!AdvanceToNextValidSlot(0, &nxt)) {
            exhausted_ = true;
            return t;
        }
    }
    RID rid_to_read = current_rid_;
    // current_rid_ 现在对应的页已经被 pin 在 current_page_guard_ 上。
    if (current_page_guard_ != rid_to_read.page_id || current_page_ptr_ == nullptr) {
        // 防御性：游标与 pin 不一致，重新 pin。
        if (!EnsurePagePinned(rid_to_read.page_id)) {
            current_rid_ = RID();
            exhausted_ = true;
            return t;
        }
    }
    // 用 current_page_ptr_ 直接访问 pinned 帧的数据，不再 GetPage（避免
    // pin count 漂移）。
    Page* page = current_page_ptr_;
    char* data = page->GetData();
    int32_t next_pid, slot_count, free_off;
    ReadPageHeader(data, next_pid, slot_count, free_off);
    int32_t off, len;
    ReadSlot(data, rid_to_read.slot_num, off, len);
    if (!IsTombstone(len) && len > 0 &&
        off >= kHeaderBytes &&
        off <= static_cast<int32_t>(PAGE_SIZE) - len) {
        t = Tuple::Deserialize(data + off, column_types);
        t.SetRid(rid_to_read);
    }
    // 推进 current_rid_ 到下一个非墓碑 slot；保持 current_page_guard_ pin 着。
    RID nxt;
    if (!AdvanceToNextValidSlot(rid_to_read.slot_num + 1, &nxt)) {
        current_rid_ = RID();
        exhausted_ = true;
    }
    return t;
}

TableHeap::Iterator TableHeap::Begin() {
    // 新的"lazy" 策略：Begin 只构造 Iterator，current_rid_ = invalid；第一次
    // HasNext / Next 才触发 AdvanceToNextValidSlot。这样 Begin 的成本从
    // O(P)（scan first page）降到 O(1)。
    return Iterator(this, RID());
}

}  // namespace sqlcompiler