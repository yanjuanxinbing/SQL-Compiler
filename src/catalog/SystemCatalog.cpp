#include "catalog/SystemCatalog.h"

#include <cstring>

namespace sqlcompiler {

namespace {

constexpr const char* kSysTablesKey = "__sys_tables__";

// Encode TableInfo as a single VARCHAR blob.
//
// Layout (binary):
//   uint16 table_name_len
//   char[table_name_len] table_name
//   uint32 num_columns
//   for each column:
//     uint16 name_len
//     char[name_len] name
//     uint8  data_type_id  (0=INT, 1=FLOAT, 2=VARCHAR)
//     uint8  flags         (bit0=PRIMARY KEY, bit1=NOT NULL)
//   uint32 first_page_id

uint8_t DataTypeId(const std::string& s) {
    if (s == "INT") return 0;
    if (s == "FLOAT") return 1;
    if (s == "VARCHAR") return 2;
    return 3;
}

const char* DataTypeName(uint8_t id) {
    switch (id) {
        case 0: return "INT";
        case 1: return "FLOAT";
        case 2: return "VARCHAR";
    }
    return "VARCHAR";
}

void WriteU32(std::string& buf, uint32_t v) {
    size_t pos = buf.size();
    buf.resize(pos + 4);
    std::memcpy(&buf[pos], &v, sizeof(uint32_t));
}

void WriteU16(std::string& buf, uint16_t v) {
    size_t pos = buf.size();
    buf.resize(pos + 2);
    std::memcpy(&buf[pos], &v, sizeof(uint16_t));
}

void WriteU8(std::string& buf, uint8_t v) {
    buf.push_back(static_cast<char>(v));
}

uint32_t ReadU32(const char*& p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(uint32_t));
    p += sizeof(uint32_t);
    return v;
}

uint16_t ReadU16(const char*& p) {
    uint16_t v;
    std::memcpy(&v, p, sizeof(uint16_t));
    p += sizeof(uint16_t);
    return v;
}

uint8_t ReadU8(const char*& p) {
    uint8_t v = static_cast<uint8_t>(*p);
    ++p;
    return v;
}

std::string EncodeTableInfo(const TableInfo& info, page_id_t first_page_id) {
    std::string buf;
    uint16_t tn_len = static_cast<uint16_t>(info.table_name.size());
    WriteU16(buf, tn_len);
    buf.append(info.table_name);
    WriteU32(buf, static_cast<uint32_t>(info.columns.size()));
    for (const auto& c : info.columns) {
        uint16_t name_len = static_cast<uint16_t>(c.name.size());
        WriteU16(buf, name_len);
        buf.append(c.name);
        WriteU8(buf, DataTypeId(c.data_type));
        uint8_t flags = 0;
        if (c.is_primary_key) flags |= 0x1;
        if (c.is_not_null)    flags |= 0x2;
        WriteU8(buf, flags);
    }
    WriteU32(buf, static_cast<uint32_t>(first_page_id));
    return buf;
}

}  // namespace

SystemCatalog::SystemCatalog(BufferPoolManager* buffer_pool_manager)
    : buffer_pool_manager_(buffer_pool_manager), sys_tables_first_page_id_(INVALID_PAGE_ID) {
}

SystemCatalog::~SystemCatalog() {
}

void SystemCatalog::Bootstrap() {
    if (sys_tables_first_page_id_ == INVALID_PAGE_ID) {
        page_id_t pid = INVALID_PAGE_ID;
        Page* p = buffer_pool_manager_->NewPage(&pid);
        if (p) {
            std::memset(p->GetData(), 0, PAGE_SIZE);
            // Set next_pid (offset 0) = INVALID and free_space_offset (offset 8) = PAGE_SIZE
            int32_t invalid_pid = INVALID_PAGE_ID;
            int32_t sentinel = PAGE_SIZE;
            std::memcpy(p->GetData(), &invalid_pid, sizeof(int32_t));
            std::memcpy(p->GetData() + 8, &sentinel, sizeof(int32_t));
            p->SetDirty(true);
            buffer_pool_manager_->UnpinPage(pid, true);
            sys_tables_first_page_id_ = pid;
        }
    }
    auto it = table_heaps_.find(kSysTablesKey);
    if (it == table_heaps_.end() && sys_tables_first_page_id_ != INVALID_PAGE_ID) {
        table_heaps_[kSysTablesKey].reset(
            TableHeap::Open(buffer_pool_manager_, sys_tables_first_page_id_));
    }
}

void SystemCatalog::LoadFromDisk() {
    if (sys_tables_first_page_id_ == INVALID_PAGE_ID) {
        sys_tables_first_page_id_ = 0;
    }
    table_heaps_[kSysTablesKey].reset(
        TableHeap::Open(buffer_pool_manager_, sys_tables_first_page_id_));
    TableHeap* sys_heap = table_heaps_[kSysTablesKey].get();
    if (!sys_heap) return;

    const std::vector<ValueType> schema = {ValueType::VARCHAR};
    auto iter = sys_heap->Begin();
    while (iter.HasNext()) {
        Tuple t = iter.Next(schema);
        if (t.ColumnCount() == 0) continue;
        TableInfo info = DecodeTableMetadata(t);
        if (info.table_name.empty()) continue;
        symbol_table_.AddTable(info);
        page_id_t user_pid = INVALID_PAGE_ID;
        const std::string& blob = t.GetValue(0).AsVarchar();
        if (blob.size() >= 4) {
            uint32_t pid_u32 = 0;
            std::memcpy(&pid_u32, blob.data() + blob.size() - 4, sizeof(uint32_t));
            user_pid = static_cast<page_id_t>(pid_u32);
        }
        if (user_pid >= 0) {
            table_heaps_[info.table_name].reset(
                TableHeap::Open(buffer_pool_manager_, user_pid));
        }
    }
}

bool SystemCatalog::CreateTable(const TableInfo& table_info) {
    if (symbol_table_.HasTable(table_info.table_name)) return false;
    if (!symbol_table_.AddTable(table_info)) return false;
    TableHeap* heap = TableHeap::Create(buffer_pool_manager_);
    if (!heap) return false;
    page_id_t user_pid = heap->GetFirstPageId();
    table_heaps_[table_info.table_name].reset(heap);
    return PersistTableMetadata(table_info);
}

bool SystemCatalog::DropTable(const std::string& table_name) {
    bool removed = symbol_table_.RemoveTable(table_name);
    table_heaps_.erase(table_name);
    TableHeap* sys_heap = table_heaps_[kSysTablesKey].get();
    if (sys_heap) {
        const std::vector<ValueType> schema = {ValueType::VARCHAR};
        auto iter = sys_heap->Begin();
        while (iter.HasNext()) {
            Tuple t = iter.Next(schema);
            if (t.ColumnCount() == 0) continue;
            TableInfo info = DecodeTableMetadata(t);
            if (info.table_name == table_name) {
                sys_heap->DeleteTuple(t.GetRid());
                break;
            }
        }
    }
    return removed;
}

bool SystemCatalog::HasTable(const std::string& table_name) const {
    return symbol_table_.HasTable(table_name);
}

const TableInfo* SystemCatalog::GetTable(const std::string& table_name) const {
    return symbol_table_.GetTable(table_name);
}

TableHeap* SystemCatalog::GetTableHeap(const std::string& table_name) {
    auto it = table_heaps_.find(table_name);
    if (it == table_heaps_.end()) return nullptr;
    return it->second.get();
}

SymbolTable& SystemCatalog::GetSymbolTable() {
    return symbol_table_;
}

bool SystemCatalog::PersistTableMetadata(const TableInfo& table_info) {
    TableHeap* sys_heap = table_heaps_[kSysTablesKey].get();
    if (!sys_heap) {
        if (sys_tables_first_page_id_ == INVALID_PAGE_ID) return false;
        table_heaps_[kSysTablesKey].reset(
            TableHeap::Open(buffer_pool_manager_, sys_tables_first_page_id_));
        sys_heap = table_heaps_[kSysTablesKey].get();
        if (!sys_heap) return false;
    }
    page_id_t user_pid = INVALID_PAGE_ID;
    auto it = table_heaps_.find(table_info.table_name);
    if (it != table_heaps_.end()) {
        user_pid = it->second->GetFirstPageId();
    }
    std::string blob = EncodeTableInfo(table_info, user_pid);
    Tuple t({Value::MakeVarchar(blob)});
    RID rid;
    return sys_heap->InsertTuple(t, &rid);
}

TableInfo SystemCatalog::DecodeTableMetadata(const Tuple& tuple) const {
    TableInfo info;
    if (tuple.ColumnCount() == 0) return info;
    const std::string& blob = tuple.GetValue(0).AsVarchar();
    if (blob.size() < 2) return info;
    const char* p = blob.data();
    const char* end = blob.data() + blob.size();
    uint16_t tn_len = ReadU16(p);
    if (p + tn_len > end) return info;
    info.table_name.assign(p, tn_len);
    p += tn_len;
    if (p + 4 > end) return info;
    uint32_t num_cols = ReadU32(p);
    for (uint32_t i = 0; i < num_cols; ++i) {
        if (p + 2 > end) { info.table_name.clear(); return info; }
        uint16_t name_len = ReadU16(p);
        if (p + name_len + 2 > end) { info.table_name.clear(); return info; }
        ColumnInfo ci;
        ci.name.assign(p, name_len);
        p += name_len;
        uint8_t dt = ReadU8(p);
        uint8_t flags = ReadU8(p);
        ci.data_type = DataTypeName(dt);
        ci.is_primary_key = (flags & 0x1) != 0;
        ci.is_not_null = (flags & 0x2) != 0;
        info.columns.push_back(std::move(ci));
    }
    return info;
}

}  // namespace sqlcompiler