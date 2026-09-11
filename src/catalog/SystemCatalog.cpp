#include "catalog/SystemCatalog.h"

#include <cstring>

namespace sqlcompiler {

namespace {

constexpr const char* kSysTablesKey = "__sys_tables__";
// 索引目录堆。它的首页 id 以一条特殊记录（表名为该常量、零列）存放在
// __sys_tables__ 里，这样无需为它约定固定页号，旧库缺这条记录也能正常打开。
constexpr const char* kSysIndexesKey = "__sys_indexes__";

// Encode TableInfo as a single VARCHAR blob.
//
// Layout (binary):
//   uint16 table_name_len
//   char[table_name_len] table_name
//   uint32 num_columns
//   for each column:
//     uint16 name_len
//     char[name_len] name
//     uint8  data_type_id  (0=INT, 1=FLOAT, 2=VARCHAR, 3=BIGINT,
//                           4=DOUBLE, 5=TEXT, 6=CHAR, 7=STRING)
//     uint8  flags         (bit0=PRIMARY KEY, bit1=NOT NULL)
//     uint16 char_length + 1  (0 表示未声明长度；即 VARCHAR(50) 存 51)
//   uint16 num_pk_groups
//   for each pk group:
//     uint16 num_cols_in_group
//     for each col: uint16 name_len + char[name_len]
//   uint32 first_page_id

uint8_t DataTypeId(const std::string& s) {
    // 归一化为大写再编码，避免CREATE TABLE中大小写写法不同导致编码失败
    std::string up;
    up.reserve(s.size());
    for (char c : s) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (up == "INT" || up == "INTEGER") return 0;
    if (up == "FLOAT") return 1;
    if (up == "VARCHAR") return 2;
    if (up == "BIGINT") return 3;
    if (up == "DOUBLE" || up == "DECIMAL") return 4;
    if (up == "TEXT") return 5;
    if (up == "CHAR") return 6;
    if (up == "STRING") return 7;
    return 8;
}

const char* DataTypeName(uint8_t id) {
    switch (id) {
        case 0: return "INT";
        case 1: return "FLOAT";
        case 2: return "VARCHAR";
        case 3: return "BIGINT";
        case 4: return "DOUBLE";
        case 5: return "TEXT";
        case 6: return "CHAR";
        case 7: return "STRING";
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
        // char_length 以「+1 偏移」编码，0 保留给「未声明」
        uint16_t enc_len = (c.char_length > 0 && c.char_length < 65535)
                               ? static_cast<uint16_t>(c.char_length + 1)
                               : 0;
        WriteU16(buf, enc_len);
    }
    WriteU16(buf, static_cast<uint16_t>(info.primary_keys.size()));
    for (const auto& group : info.primary_keys) {
        WriteU16(buf, static_cast<uint16_t>(group.size()));
        for (const auto& col : group) {
            WriteU16(buf, static_cast<uint16_t>(col.size()));
            buf.append(col);
        }
    }
    WriteU32(buf, static_cast<uint32_t>(first_page_id));
    return buf;
}

// 索引元数据 blob。
//
// 布局（与上面的表元数据编码同风格）：
//   uint16 index_name_len + bytes
//   uint16 table_name_len + bytes
//   uint16 num_key_cols;  每列: uint16 len + bytes
//   uint8  flags (bit0 = is_unique)
//   uint32 root_page_id
//
// key_types 不落盘：它由表定义现推，避免「改了列类型而索引元数据陈旧」。
std::string EncodeIndexInfo(const IndexInfo& info) {
    std::string buf;
    WriteU16(buf, static_cast<uint16_t>(info.index_name.size()));
    buf.append(info.index_name);
    WriteU16(buf, static_cast<uint16_t>(info.table_name.size()));
    buf.append(info.table_name);
    WriteU16(buf, static_cast<uint16_t>(info.key_columns.size()));
    for (const auto& c : info.key_columns) {
        WriteU16(buf, static_cast<uint16_t>(c.size()));
        buf.append(c);
    }
    WriteU8(buf, info.is_unique ? 0x1 : 0x0);
    WriteU32(buf, static_cast<uint32_t>(info.root_page_id));
    return buf;
}

// 解码。任何长度不足都返回空 index_name，调用方据此跳过这条损坏记录。
IndexInfo DecodeIndexInfo(const std::string& blob) {
    IndexInfo info;
    const char* p = blob.data();
    const char* end = blob.data() + blob.size();
    auto need = [&](size_t n) { return static_cast<size_t>(end - p) >= n; };

    if (!need(2)) return info;
    uint16_t n = ReadU16(p);
    if (!need(n)) return info;
    std::string index_name(p, n);
    p += n;

    if (!need(2)) return info;
    n = ReadU16(p);
    if (!need(n)) return info;
    info.table_name.assign(p, n);
    p += n;

    if (!need(2)) return info;
    uint16_t num_cols = ReadU16(p);
    for (uint16_t i = 0; i < num_cols; ++i) {
        if (!need(2)) return IndexInfo();
        uint16_t len = ReadU16(p);
        if (!need(len)) return IndexInfo();
        info.key_columns.emplace_back(p, len);
        p += len;
    }
    if (!need(1)) return IndexInfo();
    info.is_unique = (ReadU8(p) & 0x1) != 0;
    if (!need(4)) return IndexInfo();
    info.root_page_id = static_cast<page_id_t>(ReadU32(p));

    info.index_name = std::move(index_name);
    return info;
}

}  // namespace

SystemCatalog::SystemCatalog(BufferPoolManager* buffer_pool_manager)
    : buffer_pool_manager_(buffer_pool_manager),
      sys_tables_first_page_id_(INVALID_PAGE_ID),
      sys_indexes_first_page_id_(INVALID_PAGE_ID) {
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
        page_id_t user_pid = INVALID_PAGE_ID;
        const std::string& blob = t.GetValue(0).AsVarchar();
        if (blob.size() >= 4) {
            uint32_t pid_u32 = 0;
            std::memcpy(&pid_u32, blob.data() + blob.size() - 4, sizeof(uint32_t));
            user_pid = static_cast<page_id_t>(pid_u32);
        }
        if (info.table_name == kSysIndexesKey) {
            // 索引目录的定位记录，不是用户表
            sys_indexes_first_page_id_ = user_pid;
            continue;
        }
        symbol_table_.AddTable(info);
        if (user_pid >= 0) {
            table_heaps_[info.table_name].reset(
                TableHeap::Open(buffer_pool_manager_, user_pid));
        }
    }
    LoadIndexesFromDisk();
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
    // 先回收索引：表元数据一旦删掉就再也拿不到索引的归属信息，页面会永久泄漏
    DropIndexesOfTable(table_name);
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

bool SystemCatalog::TruncateTable(const std::string& table_name) {
    if (!symbol_table_.HasTable(table_name)) return false;
    auto it = table_heaps_.find(table_name);
    if (it == table_heaps_.end() || !it->second) return false;
    it->second->ClearAll();
    return true;
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
    return sys_heap->InsertTuple(t, &rid, {ValueType::VARCHAR});
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
        if (p + 2 > end) { info.table_name.clear(); return info; }
        uint16_t enc_len = ReadU16(p);
        ci.char_length = (enc_len == 0) ? -1 : static_cast<int32_t>(enc_len) - 1;
        info.columns.push_back(std::move(ci));
    }
    // 主键分组
    if (p + 2 > end) return info;
    uint16_t num_groups = ReadU16(p);
    for (uint16_t g = 0; g < num_groups; ++g) {
        if (p + 2 > end) return info;
        uint16_t group_size = ReadU16(p);
        std::vector<std::string> group;
        group.reserve(group_size);
        for (uint16_t i = 0; i < group_size; ++i) {
            if (p + 2 > end) return info;
            uint16_t len = ReadU16(p);
            if (p + len > end) return info;
            group.emplace_back(p, len);
            p += len;
        }
        if (!group.empty()) info.primary_keys.push_back(std::move(group));
    }
    return info;
}


// ============================================================================
// 索引管理
// ============================================================================

bool SystemCatalog::EnsureSysIndexesHeap() {
    if (index_heap_ != nullptr) return true;
    if (sys_indexes_first_page_id_ != INVALID_PAGE_ID) {
        index_heap_.reset(
            TableHeap::Open(buffer_pool_manager_, sys_indexes_first_page_id_));
        return index_heap_ != nullptr;
    }
    // 惰性创建：旧版本数据库里没有索引目录堆，首次用到时才建，
    // 这样旧库文件不需要迁移也能打开。
    TableHeap* heap = TableHeap::Create(buffer_pool_manager_);
    if (heap == nullptr) return false;
    index_heap_.reset(heap);
    sys_indexes_first_page_id_ = heap->GetFirstPageId();

    // 把首页 id 以一条特殊记录写进 __sys_tables__，供下次启动定位
    TableHeap* sys_heap = table_heaps_[kSysTablesKey].get();
    if (sys_heap == nullptr) return false;
    TableInfo marker;
    marker.table_name = kSysIndexesKey;
    std::string blob = EncodeTableInfo(marker, sys_indexes_first_page_id_);
    Tuple t({Value::MakeVarchar(blob)});
    RID rid;
    return sys_heap->InsertTuple(t, &rid, {ValueType::VARCHAR});
}

bool SystemCatalog::PersistIndexMetadata(const IndexInfo& index_info) {
    if (!EnsureSysIndexesHeap()) return false;
    std::string blob = EncodeIndexInfo(index_info);
    Tuple t({Value::MakeVarchar(blob)});
    RID rid;
    return index_heap_->InsertTuple(t, &rid, {ValueType::VARCHAR});
}

void SystemCatalog::RemoveIndexMetadata(const std::string& index_name) {
    if (index_heap_ == nullptr) return;
    const std::vector<ValueType> schema = {ValueType::VARCHAR};
    auto iter = index_heap_->Begin();
    while (iter.HasNext()) {
        Tuple t = iter.Next(schema);
        if (t.ColumnCount() == 0) continue;
        IndexInfo info = DecodeIndexInfo(t.GetValue(0).AsVarchar());
        if (info.index_name == index_name) {
            index_heap_->DeleteTuple(t.GetRid());
            return;
        }
    }
}

void SystemCatalog::LoadIndexesFromDisk() {
    if (sys_indexes_first_page_id_ == INVALID_PAGE_ID) return;  // 旧库：无索引
    index_heap_.reset(
        TableHeap::Open(buffer_pool_manager_, sys_indexes_first_page_id_));
    if (index_heap_ == nullptr) return;

    const std::vector<ValueType> schema = {ValueType::VARCHAR};
    auto iter = index_heap_->Begin();
    while (iter.HasNext()) {
        Tuple t = iter.Next(schema);
        if (t.ColumnCount() == 0) continue;
        IndexInfo info = DecodeIndexInfo(t.GetValue(0).AsVarchar());
        if (info.index_name.empty()) continue;
        const TableInfo* table = symbol_table_.GetTable(info.table_name);
        if (table == nullptr) continue;  // 表已被删，遗留元数据直接忽略
        if (!BuildIndexKeyTypes(*table, info.key_columns, &info.key_types)) {
            continue;  // 列已不存在：索引失效，当作没有这个索引
        }
        if (!OpenIndexTree(info)) continue;
        indexes_[info.index_name] = std::move(info);
    }
}

bool SystemCatalog::OpenIndexTree(const IndexInfo& index_info) {
    auto tree = BPlusTree::Open(buffer_pool_manager_, index_info.key_types,
                                index_info.is_unique, index_info.root_page_id);
    if (tree == nullptr) return false;
    index_trees_[index_info.index_name] = std::move(tree);
    return true;
}

bool SystemCatalog::CreateIndex(const IndexInfo& index_info, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error != nullptr) *error = msg;
        return false;
    };
    if (index_info.index_name.empty()) return fail("index name cannot be empty");
    if (indexes_.count(index_info.index_name) != 0) {
        return fail("index already exists: " + index_info.index_name);
    }
    const TableInfo* table = symbol_table_.GetTable(index_info.table_name);
    if (table == nullptr) return fail("table not found: " + index_info.table_name);

    std::string reason;
    if (!ValidateIndexableColumns(*table, index_info.key_columns, &reason)) {
        return fail(reason);
    }

    IndexInfo info = index_info;
    if (!BuildIndexKeyTypes(*table, info.key_columns, &info.key_types)) {
        return fail("cannot resolve index key types");
    }
    auto tree = BPlusTree::Create(buffer_pool_manager_, info.key_types,
                                  info.is_unique);
    if (tree == nullptr) return fail("failed to allocate index root page");
    info.root_page_id = tree->GetRootPageId();

    if (!PersistIndexMetadata(info)) {
        // 元数据落不了盘就把刚分配的树回收掉，否则页面永久泄漏
        BPlusTree::Destroy(buffer_pool_manager_, info.root_page_id);
        return fail("failed to persist index metadata");
    }
    index_trees_[info.index_name] = std::move(tree);
    indexes_[info.index_name] = std::move(info);
    return true;
}

bool SystemCatalog::DropIndex(const std::string& index_name) {
    auto it = indexes_.find(index_name);
    if (it == indexes_.end()) return false;
    const page_id_t root = it->second.root_page_id;
    index_trees_.erase(index_name);
    indexes_.erase(it);
    RemoveIndexMetadata(index_name);
    BPlusTree::Destroy(buffer_pool_manager_, root);
    return true;
}

void SystemCatalog::ResetIndexesOfTable(const std::string& table_name) {
    for (auto& kv : indexes_) {
        IndexInfo& info = kv.second;
        if (info.table_name != table_name) continue;
        // 旧树整棵回收，换一棵空树。这里必须回收而不是简单地重指向新根，
        // 否则每次 TRUNCATE 都会永久泄漏一批页面。
        BPlusTree::Destroy(buffer_pool_manager_, info.root_page_id);
        auto tree = BPlusTree::Create(buffer_pool_manager_, info.key_types,
                                      info.is_unique);
        if (tree == nullptr) continue;
        info.root_page_id = tree->GetRootPageId();
        index_trees_[info.index_name] = std::move(tree);
        // 根页变了，元数据要跟着落盘
        RemoveIndexMetadata(info.index_name);
        PersistIndexMetadata(info);
    }
}

void SystemCatalog::DropIndexesOfTable(const std::string& table_name) {
    std::vector<std::string> names;
    for (const auto& kv : indexes_) {
        if (kv.second.table_name == table_name) names.push_back(kv.first);
    }
    for (const auto& name : names) DropIndex(name);
}

const IndexInfo* SystemCatalog::GetIndex(const std::string& index_name) const {
    auto it = indexes_.find(index_name);
    return it == indexes_.end() ? nullptr : &it->second;
}

BPlusTree* SystemCatalog::GetIndexTree(const std::string& index_name) {
    auto it = index_trees_.find(index_name);
    return it == index_trees_.end() ? nullptr : it->second.get();
}

std::vector<const IndexInfo*> SystemCatalog::GetIndexesForTable(
    const std::string& table_name) const {
    std::vector<const IndexInfo*> out;
    for (const auto& kv : indexes_) {
        if (kv.second.table_name == table_name) out.push_back(&kv.second);
    }
    return out;
}

BPlusTree* SystemCatalog::GetPrimaryKeyIndexTree(
    const std::string& table_name, const std::vector<std::string>& pk_columns) {
    for (const auto& kv : indexes_) {
        const IndexInfo& info = kv.second;
        if (info.table_name != table_name) continue;
        if (!info.is_unique) continue;
        if (info.key_columns != pk_columns) continue;
        return GetIndexTree(info.index_name);
    }
    return nullptr;
}

// ============================================================================
// 40_txn_view_udf：视图 / UDF / 触发器（内存态，最小可用实现）
// ============================================================================

bool SystemCatalog::CreateView(const ViewDefinition& def) {
    if (def.view_name.empty()) return false;
    if (views_.count(def.view_name) != 0) return false;
    views_[def.view_name] = def;
    return true;
}

bool SystemCatalog::DropView(const std::string& view_name) {
    auto it = views_.find(view_name);
    if (it == views_.end()) return false;
    views_.erase(it);
    return true;
}

bool SystemCatalog::HasView(const std::string& view_name) const {
    return views_.find(view_name) != views_.end();
}

const SystemCatalog::ViewDefinition* SystemCatalog::GetView(
    const std::string& view_name) const {
    auto it = views_.find(view_name);
    return it == views_.end() ? nullptr : &it->second;
}

bool SystemCatalog::CreateFunction(const FunctionDefinition& def) {
    if (def.function_name.empty()) return false;
    if (functions_.count(def.function_name) != 0) return false;
    functions_[def.function_name] = def;
    return true;
}

bool SystemCatalog::DropFunction(const std::string& function_name) {
    auto it = functions_.find(function_name);
    if (it == functions_.end()) return false;
    functions_.erase(it);
    return true;
}

bool SystemCatalog::HasFunction(const std::string& function_name) const {
    return functions_.find(function_name) != functions_.end();
}

const SystemCatalog::FunctionDefinition* SystemCatalog::GetFunction(
    const std::string& function_name) const {
    auto it = functions_.find(function_name);
    return it == functions_.end() ? nullptr : &it->second;
}

bool SystemCatalog::CreateTrigger(const TriggerDefinition& def) {
    if (def.trigger_name.empty()) return false;
    if (triggers_.count(def.trigger_name) != 0) return false;
    triggers_[def.trigger_name] = def;
    return true;
}

bool SystemCatalog::DropTrigger(const std::string& trigger_name) {
    auto it = triggers_.find(trigger_name);
    if (it == triggers_.end()) return false;
    triggers_.erase(it);
    return true;
}

bool SystemCatalog::HasTrigger(const std::string& trigger_name) const {
    return triggers_.find(trigger_name) != triggers_.end();
}

}  // namespace sqlcompiler