#include "storage_engine/TableHeap.h"

#include <cstring>
#include <unordered_set>

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
    page_id_t pid = INVALID_PAGE_ID;
    Page* page = buffer_pool_manager->NewPage(&pid);
    if (!page) return nullptr;
    InitEmptyPageHeader(page->GetData());
    page->SetDirty(true);
    buffer_pool_manager->UnpinPage(pid, true);
    return new TableHeap(buffer_pool_manager, pid);
}

TableHeap* TableHeap::Open(BufferPoolManager* buffer_pool_manager, page_id_t first_page_id) {
    return new TableHeap(buffer_pool_manager, first_page_id);
}

page_id_t TableHeap::GetFirstPageId() const {
    return first_page_id_;
}

bool TableHeap::InsertIntoPage(page_id_t page_id, const Tuple& tuple, RID* rid) {
    Page* page = buffer_pool_manager_->GetPage(page_id);
    if (!page) return false;
    char* data = page->GetData();
    int32_t next_pid, slot_count, free_off;
    ReadPageHeader(data, next_pid, slot_count, free_off);

    std::vector<char> serialized = tuple.Serialize();
    int32_t len = static_cast<int32_t>(serialized.size());

    // Need room for: new slot entry (kSlotBytes) + record bytes
    int32_t slot_dir_end = kHeaderBytes + (slot_count + 1) * kSlotBytes;
    if (slot_dir_end > free_off || len > free_off - slot_dir_end) {
        buffer_pool_manager_->UnpinPage(page_id, false);
        return false;
    }

    int32_t new_off = free_off - len;
    if (len > 0) {
        std::memcpy(data + new_off, serialized.data(), len);
    }
    WriteSlot(data, slot_count, new_off, len);
    WritePageHeader(data, next_pid, slot_count + 1, new_off);
    page->SetDirty(true);
    if (rid) {
        rid->page_id = page_id;
        rid->slot_num = slot_count;
    }
    buffer_pool_manager_->UnpinPage(page_id, true);
    return true;
}

bool TableHeap::InsertTuple(const Tuple& tuple, RID* rid) {
    page_id_t pid = first_page_id_;
    page_id_t prev_pid = INVALID_PAGE_ID;
    while (pid != INVALID_PAGE_ID) {
        if (InsertIntoPage(pid, tuple, rid)) {
            return true;
        }
        // Walk to next page in chain
        Page* page = buffer_pool_manager_->GetPage(pid);
        if (!page) return false;
        int32_t next_pid, slot_count, free_off;
        ReadPageHeader(page->GetData(), next_pid, slot_count, free_off);
        buffer_pool_manager_->UnpinPage(pid, false);
        prev_pid = pid;
        pid = next_pid;
    }
    // Allocate a new page and link from prev page
    page_id_t new_pid = INVALID_PAGE_ID;
    Page* new_page = buffer_pool_manager_->NewPage(&new_pid);
    if (!new_page) return false;
    InitEmptyPageHeader(new_page->GetData());
    new_page->SetDirty(true);
    buffer_pool_manager_->UnpinPage(new_pid, true);
    if (prev_pid != INVALID_PAGE_ID) {
        Page* prev = buffer_pool_manager_->GetPage(prev_pid);
        if (prev) {
            int32_t next_pid, slot_count, free_off;
            ReadPageHeader(prev->GetData(), next_pid, slot_count, free_off);
            WritePageHeader(prev->GetData(), new_pid, slot_count, free_off);
            prev->SetDirty(true);
            buffer_pool_manager_->UnpinPage(prev_pid, true);
        }
    } else {
        first_page_id_ = new_pid;
    }
    return InsertIntoPage(new_pid, tuple, rid);
}

bool TableHeap::GetTuple(const RID& rid, Tuple* tuple,
                          const std::vector<ValueType>& column_types) {
    if (!rid.IsValid()) return false;
    Page* page = buffer_pool_manager_->GetPage(rid.page_id);
    if (!page) return false;
    char* data = page->GetData();
    int32_t next_pid, slot_count, free_off;
    ReadPageHeader(data, next_pid, slot_count, free_off);
    if (rid.slot_num < 0 || rid.slot_num >= slot_count) {
        buffer_pool_manager_->UnpinPage(rid.page_id, false);
        return false;
    }
    int32_t off, len;
    ReadSlot(data, rid.slot_num, off, len);
    if (IsTombstone(len)) {
        buffer_pool_manager_->UnpinPage(rid.page_id, false);
        return false;
    }
    if (tuple) {
        *tuple = Tuple::Deserialize(data + off, column_types);
        tuple->SetRid(rid);
    }
    buffer_pool_manager_->UnpinPage(rid.page_id, false);
    return true;
}

bool TableHeap::DeleteTuple(const RID& rid) {
    if (!rid.IsValid()) return false;
    Page* page = buffer_pool_manager_->GetPage(rid.page_id);
    if (!page) return false;
    char* data = page->GetData();
    int32_t next_pid, slot_count, free_off;
    ReadPageHeader(data, next_pid, slot_count, free_off);
    if (rid.slot_num < 0 || rid.slot_num >= slot_count) {
        buffer_pool_manager_->UnpinPage(rid.page_id, false);
        return false;
    }
    WriteSlot(data, rid.slot_num, 0, static_cast<int32_t>(kTombstone));
    page->SetDirty(true);
    buffer_pool_manager_->UnpinPage(rid.page_id, true);
    return true;
}

bool TableHeap::UpdateTuple(const RID& rid, const Tuple& new_tuple) {
    if (!rid.IsValid()) return false;
    Page* page = buffer_pool_manager_->GetPage(rid.page_id);
    if (!page) return false;
    char* data = page->GetData();
    int32_t next_pid, slot_count, free_off;
    ReadPageHeader(data, next_pid, slot_count, free_off);
    if (rid.slot_num < 0 || rid.slot_num >= slot_count) {
        buffer_pool_manager_->UnpinPage(rid.page_id, false);
        return false;
    }
    int32_t off, len;
    ReadSlot(data, rid.slot_num, off, len);
    if (IsTombstone(len) || off < 0 || len < 0 ||
        off + len > static_cast<int32_t>(PAGE_SIZE)) {
        buffer_pool_manager_->UnpinPage(rid.page_id, false);
        return false;
    }
    std::vector<char> serialized = new_tuple.Serialize();
    int32_t new_len = static_cast<int32_t>(serialized.size());
    if (new_len == 0) {
        buffer_pool_manager_->UnpinPage(rid.page_id, false);
        return false;
    }
    if (new_len <= len) {
        if (new_len < len) {
            std::memset(data + off + new_len, 0, len - new_len);
        }
        std::memcpy(data + off, serialized.data(), new_len);
        WriteSlot(data, rid.slot_num, off, new_len);
        page->SetDirty(true);
        buffer_pool_manager_->UnpinPage(rid.page_id, true);
        return true;
    }
    buffer_pool_manager_->UnpinPage(rid.page_id, false);
    // Cannot grow in place — delete and reinsert
    DeleteTuple(rid);
    return InsertTuple(new_tuple, nullptr);
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
        Page* page = buffer_pool_manager_->GetPage(pid);
        if (!page) return false;
        char* data = page->GetData();
        int32_t next_pid, slot_count, free_off;
        ReadPageHeader(data, next_pid, slot_count, free_off);
        while (slot_num < slot_count) {
            int32_t off, len;
            ReadSlot(data, slot_num, off, len);
            if (!IsTombstone(len)) {
                if (next) {
                    next->page_id = pid;
                    next->slot_num = slot_num;
                }
                buffer_pool_manager_->UnpinPage(pid, false);
                return true;
            }
            ++slot_num;
        }
        buffer_pool_manager_->UnpinPage(pid, false);
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