#include "catalog/SystemCatalog.h"

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "txn/LogManager.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace sqlcompiler {

namespace {

// 把落盘的 CHECK / DEFAULT 文本重新解析为 AST。失败时返回 nullptr 并让
// 调用方静默继续 —— CHECK/DEFAULT 是"约束增强"，缺失不应阻塞表被打开。
// 抛异常的代价是下次启动后所有用户都拿不到这张表，这与"约束可选"的语义
// 不符。
ExprPtr ReParseExprOrNull(const std::string& text) {
    if (text.empty()) return nullptr;
    try {
        Lexer lexer(text);
        std::vector<Token> tokens = lexer.Tokenize();
        // 兜底追加一个 END_OF_FILE 防止某些路径上 Tokenize 未自动收尾
        if (tokens.empty() || tokens.back().type != TokenType::END_OF_FILE) {
            tokens.emplace_back(TokenType::END_OF_FILE, "", 0, 0);
        }
        Parser parser(std::move(tokens));
        StatementPtr stmt = parser.Parse();
        if (!stmt) return nullptr;
        // 我们落盘的内容来自 Expr::ToString()，不属于完整语句；但 Parser 入口
        // 是 ParseStatement()。这里退而求其次：尝试解析为 SELECT 表达式别名，
        // 拿到 AST 中的第一个 Expr。
        //
        // 实际策略：再起一次"只解析表达式"路径 —— 借助 Parser 的内部入口
        // ParseExpression()。但 ParseExpression 是 private，因此这里临时走
        // 一条 hack：把表达式包成 "SELECT <expr>" 走 ParseSelectStatement，
        // 然后从 SelectStatement 的 select_list 取第一个元素。
        std::string wrapped = "SELECT " + text;
        Lexer l2(wrapped);
        std::vector<Token> t2 = l2.Tokenize();
        if (t2.empty() || t2.back().type != TokenType::END_OF_FILE) {
            t2.emplace_back(TokenType::END_OF_FILE, "", 0, 0);
        }
        Parser p2(std::move(t2));
        StatementPtr s2 = p2.Parse();
        if (!s2) return nullptr;
        if (s2->GetType() != NodeType::SELECT_STMT) return nullptr;
        auto* sel = static_cast<SelectStatement*>(s2.get());
        if (sel->select_list.empty()) return nullptr;
        return sel->select_list[0];
    } catch (...) {
        return nullptr;
    }
}

constexpr const char* kSysTablesKey = "__sys_tables__";
// 索引目录堆。它的首页 id 以一条特殊记录（表名为该常量、零列）存放在
// __sys_tables__ 里，这样无需为它约定固定页号，旧库缺这条记录也能正常打开。
constexpr const char* kSysIndexesKey = "__sys_indexes__";
// 60_view_trigger (Category 9): 触发器目录堆，与索引目录堆相同的定位方式。
constexpr const char* kSysTriggersKey = "__sys_triggers__";

// Encode TableInfo as a single VARCHAR blob.
//
// Layout (binary):
//   uint16 table_name_len
//   char[table_name_len] table_name
//   uint32 num_columns
//   for each column:
//     uint16 name_len
//     char[name_len] name
//     uint8  data_type_id  (see DataTypeId table below)
//     uint8  flags         (bit0=PRIMARY KEY, bit1=NOT NULL,
//                           bit2=UNIQUE, bit3=AUTO_INCREMENT)
//     uint16 char_length + 1  (0 表示未声明长度；即 VARCHAR(50) 存 51)
//     uint16 check_expr_text_len (0 表示无 CHECK)
//     char[check_expr_text_len]  check_expr_text   （AST ToString 的可重新解析文本）
//     uint16 default_expr_text_len (0 表示无 DEFAULT)
//     char[default_expr_text_len]  default_expr_text
//   uint16 num_pk_groups
//   for each pk group:
//     uint16 num_cols_in_group
//     for each col: uint16 name_len + char[name_len]
//   uint16 num_unique_groups  (52_data_types: 表级 UNIQUE(col, ...) 约束)
//   for each unique group:
//     uint16 num_cols_in_group
//     for each col: uint16 name_len + char[name_len]
//   uint32 first_page_id
//
// 备注：CHECK / DEFAULT 这里只存「可重新解析的文本」，落盘只多 ~ 几十字节，
// 但保留了完整的语义信息；启动时 DecodeTableMetadata 用 Parser 把文本解析
// 回 AST 接到 ColumnInfo.check_expr / default_expr。
//
// ---- DataTypeId 表（column_data_type 编码）----
//   0=INT  1=FLOAT  2=VARCHAR  3=BIGINT  4=DOUBLE(legacy)  5=TEXT
//   6=CHAR  7=STRING  8=DATE  9=TIMESTAMP
//   ---- 52_data_types: 新增（10..20） ----
//   10=BOOLEAN  12=TEXT  13=DECIMAL  14=DOUBLE  15=REAL
//   16=SMALLINT  17=TINYINT  18=TIME  19=JSON  20=UUID
// 旧库 blob 的 flag 仅 bit0/PRIMARY+bit1/NOT NULL；探测剩余位长度不足时
// 视为未设置，向前兼容不破坏现有元数据。

uint8_t DataTypeId(const std::string& s) {
    // 归一化为大写再编码，避免CREATE TABLE中大小写写法不同导致编码失败
    std::string up;
    up.reserve(s.size());
    for (char c : s) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (up == "INT" || up == "INTEGER") return 0;
    if (up == "FLOAT") return 1;
    if (up == "VARCHAR") return 2;
    if (up == "BIGINT") return 3;
    // 52_data_types: DOUBLE 与 DECIMAL 拆分为独立 ID，不再共用 4。
    if (up == "DOUBLE") return 14;
    if (up == "DECIMAL" || up == "NUMERIC") return 13;
    if (up == "TEXT") return 12;
    if (up == "CHAR") return 6;
    if (up == "STRING") return 7;
    // 52_data_types: 继续保留 4=DOUBLE 旧编码以便向后兼容（已被 14 取代）。
    // 历史 blob 中 DOUBLE/DECIMAL 都编码为 4，DataTypeName(4) 返回 "DOUBLE"。
    // 实际值类型由 ValueTypeFromString 决定，这里只影响落盘格式。
    if (up == "REAL") return 15;
    if (up == "SMALLINT") return 16;
    if (up == "TINYINT") return 17;
    if (up == "BOOLEAN" || up == "BOOL") return 10;
    if (up == "TIME") return 18;
    if (up == "JSON") return 19;
    if (up == "UUID") return 20;
    // 45_datetime: DATE / TIMESTAMP 持久化为 VARCHAR（按 YYYY-MM-DD 或
    // YYYY-MM-DD HH:MM:SS 文本格式），但保留独立 DataTypeId 以便未来切到
    // 原生二进制布局时无需再次迁移 schema；wire 格式上额外 ID 不影响旧库。
    if (up == "DATE") return 8;
    if (up == "TIMESTAMP") return 9;
    return 8;
}

const char* DataTypeName(uint8_t id) {
    switch (id) {
        case 0: return "INT";
        case 1: return "FLOAT";
        case 2: return "VARCHAR";
        case 3: return "BIGINT";
        case 4: return "DOUBLE";          // 历史编码（DOUBLE/DECIMAL 共用）
        case 5: return "TEXT";
        case 6: return "CHAR";
        case 7: return "STRING";
        case 8: return "DATE";
        case 9: return "TIMESTAMP";
        // 52_data_types: 新增 ID
        case 10: return "BOOLEAN";
        case 12: return "TEXT";
        case 13: return "DECIMAL";
        case 14: return "DOUBLE";
        case 15: return "REAL";
        case 16: return "SMALLINT";
        case 17: return "TINYINT";
        case 18: return "TIME";
        case 19: return "JSON";
        case 20: return "UUID";
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
        // 52_data_types: 扩展 flag。UNIQUE 与 AUTO_INCREMENT 各占 1 位。
        // 旧库 blob 没有这些位（只用了 bit0/1），向后兼容。
        if (c.is_unique)         flags |= 0x4;
        if (c.is_auto_increment) flags |= 0x8;
        WriteU8(buf, flags);
        // char_length 以「+1 偏移」编码，0 保留给「未声明」
        uint16_t enc_len = (c.char_length > 0 && c.char_length < 65535)
                               ? static_cast<uint16_t>(c.char_length + 1)
                               : 0;
        WriteU16(buf, enc_len);
        // CHECK / DEFAULT 表达式：以 AST->ToString() 的可重新解析文本落盘。
        // 空文本表示未声明；旧版数据库写出的 blob 没有这两段，ReadU16 会读到 0。
        std::string check_text;
        if (c.check_expr) check_text = c.check_expr->ToString();
        WriteU16(buf, static_cast<uint16_t>(check_text.size()));
        if (!check_text.empty()) buf.append(check_text);
        std::string default_text;
        if (c.default_expr) default_text = c.default_expr->ToString();
        WriteU16(buf, static_cast<uint16_t>(default_text.size()));
        if (!default_text.empty()) buf.append(default_text);
    }
    WriteU16(buf, static_cast<uint16_t>(info.primary_keys.size()));
    for (const auto& group : info.primary_keys) {
        WriteU16(buf, static_cast<uint16_t>(group.size()));
        for (const auto& col : group) {
            WriteU16(buf, static_cast<uint16_t>(col.size()));
            buf.append(col);
        }
    }
    // 52_data_types: 表级 UNIQUE(col1, col2, ...) 约束分组。
    // 旧库 blob 没有这段；decoder 会探测剩余字节长度，不足视为空分组。
    // 这里把 column-level UNIQUE 列也收集成单列 UNIQUE 分组，保证唯一性
    // 校验在 catalog 一侧完整——便于运行时统一走索引路径。
    std::vector<std::vector<std::string>> uniq_groups = info.unique_constraints;
    for (const auto& c : info.columns) {
        if (c.is_unique) {
            bool dup = false;
            for (const auto& g : uniq_groups) {
                if (g.size() == 1 && g[0] == c.name) { dup = true; break; }
            }
            if (!dup) uniq_groups.push_back({c.name});
        }
    }
    WriteU16(buf, static_cast<uint16_t>(uniq_groups.size()));
    for (const auto& group : uniq_groups) {
        WriteU16(buf, static_cast<uint16_t>(group.size()));
        for (const auto& col : group) {
            WriteU16(buf, static_cast<uint16_t>(col.size()));
            buf.append(col);
        }
    }
    // 58_constraints: 表级 CHECK 约束序列。每条记录（按出现顺序）：
    //   uint16 constraint_name_len
    //   char[constraint_name_len] constraint_name（0 表示匿名）
    //   uint16 expr_text_len
    //   char[expr_text_len] expr_text（AST->ToString 形式，落盘后可重解析）
    // 旧库 blob 不含此段；decoder 通过剩余字节探测读取，新库关闭时统一回写。
    WriteU16(buf, static_cast<uint16_t>(info.table_checks.size()));
    for (const auto& tc : info.table_checks) {
        WriteU16(buf, static_cast<uint16_t>(tc.constraint_name.size()));
        if (!tc.constraint_name.empty()) buf.append(tc.constraint_name);
        std::string expr_text;
        if (tc.expr) expr_text = tc.expr->ToString();
        WriteU16(buf, static_cast<uint16_t>(expr_text.size()));
        if (!expr_text.empty()) buf.append(expr_text);
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
      sys_indexes_first_page_id_(INVALID_PAGE_ID),
      sys_triggers_first_page_id_(INVALID_PAGE_ID) {
}

void SystemCatalog::SetLogManager(LogManager* lm) {
    log_manager_ = lm;
    // 把 LogManager 注入到 catalog 已经持有的所有 TableHeap / BPlusTree。
    // 注：BPlusTree 的 log_manager 是写在 BPlusTree::SetLogManager 上的——
    // 这里不直接访问私有字段，但通过 public 接口完成。
    for (auto& kv : table_heaps_) {
        if (kv.second) kv.second->SetLogManager(lm);
    }
    for (auto& kv : index_trees_) {
        if (kv.second) kv.second->SetLogManager(lm);
    }
    if (index_heap_) index_heap_->SetLogManager(lm);
}

// ---- Phase B helper：把 catalog 持有的所有 heap/tree 在创建/打开之后立即
// 绑定 log_manager（针对 Bootstrap / LoadFromDisk 之后又新建 heap 的场景）。
// 直接调用 SetLogManager(lm) 即可（它已经会遍历全部已存在的成员）。
// 该函数是 SetLogManager 的同义别名，让调用点的语义更明确。
namespace { void BindWalToAllCatalogMembers(SystemCatalog*, LogManager*) {} }

void SystemCatalog::SetActiveTransaction(Transaction* txn) {
    // 推到所有 catalog 自有的 TableHeap 与 BPlusTree，让 sys_tables 与
    // sys_indexes 的写路径正确记录 WAL。
    for (auto& kv : table_heaps_) {
        if (kv.second) kv.second->SetActiveTransaction(txn);
    }
    for (auto& kv : index_trees_) {
        if (kv.second) kv.second->SetActiveTransaction(txn);
    }
    if (index_heap_) index_heap_->SetActiveTransaction(txn);
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
        if (info.table_name == kSysTriggersKey) {
            // 60_view_trigger: 触发器目录的定位记录，不是用户表
            sys_triggers_first_page_id_ = user_pid;
            continue;
        }
        symbol_table_.AddTable(info);
        if (user_pid >= 0) {
            table_heaps_[info.table_name].reset(
                TableHeap::Open(buffer_pool_manager_, user_pid));
        }
    }
    LoadIndexesFromDisk();
    LoadTriggersFromDisk();
}

bool SystemCatalog::CreateTable(const TableInfo& table_info) {
    if (symbol_table_.HasTable(table_info.table_name)) return false;
    if (!symbol_table_.AddTable(table_info)) return false;
    TableHeap* heap = TableHeap::Create(buffer_pool_manager_);
    if (!heap) return false;
    if (log_manager_ != nullptr) heap->SetLogManager(log_manager_);
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

std::vector<std::string> SystemCatalog::ListAllTables() const {
    // SymbolTable::GetAllTableNames() 已返回所有表名；这里过滤掉 __sys_tables__
    // 与 __sys_indexes__ 两条系统目录条目，避免被 SHOW TABLES 当作用户表展示。
    std::vector<std::string> names = symbol_table_.GetAllTableNames();
    std::vector<std::string> out;
    out.reserve(names.size());
    for (auto& n : names) {
        if (n == kSysTablesKey || n == kSysIndexesKey) continue;
        out.push_back(std::move(n));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<ColumnInfo> SystemCatalog::GetColumnInfos(
    const std::string& table_name) const {
    std::vector<ColumnInfo> out;
    const TableInfo* info = symbol_table_.GetTable(table_name);
    if (!info) return out;
    out.reserve(info->columns.size());
    for (const auto& c : info->columns) {
        out.push_back(c);  // 浅拷贝：包含 check_expr/default_expr 的 shared_ptr
    }
    return out;
}

std::string SystemCatalog::BuildCreateTableSQL(
    const std::string& table_name) const {
    const TableInfo* info = symbol_table_.GetTable(table_name);
    if (!info) return "";
    std::ostringstream oss;
    oss << "CREATE TABLE " << info->table_name << " (";
    bool first = true;
    for (const auto& c : info->columns) {
        if (!first) oss << ", ";
        first = false;
        oss << c.name << " " << c.data_type;
        if (c.data_type == "VARCHAR" || c.data_type == "CHAR") {
            if (c.char_length > 0) oss << "(" << c.char_length << ")";
        }
        // 52_data_types: SERIAL 在元数据里被记录为 INT + is_auto_increment +
        // is_primary_key；为了展示时贴近原始 SQL，这里把 INT + AUTO_INCREMENT
        // 合并输出成 SERIAL。
        if (c.data_type == "INT" && c.is_auto_increment &&
            c.is_primary_key && !c.is_unique) {
            oss << " SERIAL";
            if (c.is_not_null) {/* SERIAL 隐含 NOT NULL */}
        } else {
            if (c.is_primary_key) oss << " PRIMARY KEY";
            if (c.is_not_null) oss << " NOT NULL";
            if (c.is_unique) oss << " UNIQUE";
            if (c.is_auto_increment) oss << " AUTO_INCREMENT";
        }
        if (c.default_expr) oss << " DEFAULT " << c.default_expr->ToString();
        // 58_constraints: 命名列级 CHECK 约束在展示时还原 CONSTRAINT name 前缀。
        if (c.check_expr) {
            if (!c.constraint_name.empty()) {
                oss << " CONSTRAINT " << c.constraint_name;
            }
            oss << " CHECK (" << c.check_expr->ToString() << ")";
        }
    }
    // 复合主键：以表级 PRIMARY KEY(...) 形式追加（与 parser 输出一致）。
    for (const auto& group : info->primary_keys) {
        if (group.empty()) continue;
        // 跳过"已被列内 is_primary_key 标记过的单列主键"。
        if (group.size() == 1) {
            bool found = false;
            for (const auto& c : info->columns) {
                if (c.name == group[0] && c.is_primary_key) { found = true; break; }
            }
            if (found) continue;
        }
        if (!first) oss << ", ";
        first = false;
        oss << "PRIMARY KEY(";
        for (size_t i = 0; i < group.size(); ++i) {
            if (i) oss << ", ";
            oss << group[i];
        }
        oss << ")";
    }
    // 52_data_types: 表级 UNIQUE(col, ...) 约束。列级 UNIQUE 已在列定义中输出。
    for (const auto& group : info->unique_constraints) {
        if (group.empty()) continue;
        // 单列 UNIQUE 已被列内 is_unique 输出过；跳过避免重复。
        if (group.size() == 1) {
            bool found = false;
            for (const auto& c : info->columns) {
                if (c.name == group[0] && c.is_unique) { found = true; break; }
            }
            if (found) continue;
        }
        if (!first) oss << ", ";
        first = false;
        oss << "UNIQUE(";
        for (size_t i = 0; i < group.size(); ++i) {
            if (i) oss << ", ";
            oss << group[i];
        }
        oss << ")";
    }
    // 58_constraints: 表级 CHECK(expr) / CONSTRAINT name CHECK(expr) 约束。
    // 列级 CHECK 已在列定义中输出（同样按 ToString 序列化）。表级 CHECK 顺序
    // 与 CREATE TABLE 中出现的次序一致，便于 SHOW CREATE TABLE 还原原语句。
    for (const auto& tc : info->table_checks) {
        if (!tc.expr) continue;
        if (!first) oss << ", ";
        first = false;
        if (!tc.constraint_name.empty()) {
            oss << "CONSTRAINT " << tc.constraint_name << " ";
        }
        oss << "CHECK (" << tc.expr->ToString() << ")";
    }
    oss << ")";
    return oss.str();
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
        // 52_data_types: bit2=UNIQUE / bit3=AUTO_INCREMENT。旧库 blob 中这两位
        // 始终为 0，向后兼容。
        ci.is_unique = (flags & 0x4) != 0;
        ci.is_auto_increment = (flags & 0x8) != 0;
        if (p + 2 > end) { info.table_name.clear(); return info; }
        uint16_t enc_len = ReadU16(p);
        ci.char_length = (enc_len == 0) ? -1 : static_cast<int32_t>(enc_len) - 1;
        // ---- CHECK / DEFAULT 表达式文本（旧库可能没有这两段，做长度探测）----
        // 探测方法：剩余字节至少要能容纳两个 uint16 长度字段；否则视为
        // 旧版编码、CHECK/DEFAULT 留空。
        if (p + 4 > end) {
            // 旧版 blob：本列无 CHECK/DEFAULT 段
            ci.check_expr = nullptr;
            ci.default_expr = nullptr;
        } else {
            uint16_t check_len = ReadU16(p);
            std::string check_text;
            if (check_len > 0) {
                if (p + check_len > end) {
                    info.table_name.clear();
                    return info;
                }
                check_text.assign(p, check_len);
                p += check_len;
            }
            if (p + 2 > end) {
                // 编码被截断：保守回退
                info.table_name.clear();
                return info;
            }
            uint16_t default_len = ReadU16(p);
            std::string default_text;
            if (default_len > 0) {
                if (p + default_len > end) {
                    info.table_name.clear();
                    return info;
                }
                default_text.assign(p, default_len);
                p += default_len;
            }
            // 把文本重新解析为 AST；解析失败时静默退化为 nullptr（CHECK/DEFAULT
            // 是约束增强，不阻塞表加载）
            ci.check_expr = ReParseExprOrNull(check_text);
            ci.default_expr = ReParseExprOrNull(default_text);
        }
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
    // 52_data_types: 表级 UNIQUE 约束分组。旧库 blob 没有这段 (剩余字节不足
    // 容纳一个 uint16 字段)，视为空。剩余字节耗尽即停止扫描。
    if (p + 2 > end) return info;
    uint16_t num_uniq = ReadU16(p);
    for (uint16_t g = 0; g < num_uniq; ++g) {
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
        if (!group.empty()) info.unique_constraints.push_back(std::move(group));
    }
    // 58_constraints: 表级 CHECK 约束序列。旧库 blob 没有这段（剩余字节不足
    // 容纳一个 uint16）；旧库反序列化时 table_checks 留空，写入路径跳过。
    if (p + 2 > end) return info;
    uint16_t num_tc = ReadU16(p);
    for (uint16_t i = 0; i < num_tc; ++i) {
        if (p + 2 > end) return info;
        uint16_t name_len = ReadU16(p);
        std::string name;
        if (name_len > 0) {
            if (p + name_len > end) return info;
            name.assign(p, name_len);
            p += name_len;
        }
        if (p + 2 > end) return info;
        uint16_t expr_len = ReadU16(p);
        std::string expr_text;
        if (expr_len > 0) {
            if (p + expr_len > end) return info;
            expr_text.assign(p, expr_len);
            p += expr_len;
        }
        TableInfo::TableCheck tc;
        tc.constraint_name = std::move(name);
        // 同列级 CHECK / DEFAULT：解析失败时静默退化为 nullptr，约束语义降级
        // 但不阻塞表加载。语法写错的 CHECK 在新库下第一次写入时报错。
        tc.expr = ReParseExprOrNull(expr_text);
        info.table_checks.push_back(std::move(tc));
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
        if (index_heap_ != nullptr && log_manager_ != nullptr) {
            index_heap_->SetLogManager(log_manager_);
        }
        return index_heap_ != nullptr;
    }
    // 惰性创建：旧版本数据库里没有索引目录堆，首次用到时才建，
    // 这样旧库文件不需要迁移也能打开。
    TableHeap* heap = TableHeap::Create(buffer_pool_manager_);
    if (heap == nullptr) return false;
    if (log_manager_ != nullptr) heap->SetLogManager(log_manager_);
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
    if (log_manager_ != nullptr) index_heap_->SetLogManager(log_manager_);

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
    if (log_manager_ != nullptr) tree->SetLogManager(log_manager_);
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
    if (log_manager_ != nullptr) tree->SetLogManager(log_manager_);
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
        if (log_manager_ != nullptr) tree->SetLogManager(log_manager_);
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

void SystemCatalog::SetViewCheckOption(const std::string& view_name,
                                       ExprPtr where_expr,
                                       bool cascaded) {
    auto it = views_.find(view_name);
    if (it == views_.end()) return;
    it->second.has_check_option = true;
    it->second.check_option_cascaded = cascaded;
    it->second.check_option_where = where_expr;
}

bool SystemCatalog::DropView(const std::string& view_name) {
    auto it = views_.find(view_name);
    if (it == views_.end()) return false;
    views_.erase(it);
    return true;
}

// 60_view_trigger (Category 9)：物化视图注册接口。
bool SystemCatalog::CreateMaterializedView(const MaterializedViewInfo& info) {
    if (info.view_name.empty()) return false;
    materialized_views_[info.view_name] = info;
    return true;
}

bool SystemCatalog::DropMaterializedView(const std::string& view_name) {
    auto it = materialized_views_.find(view_name);
    if (it == materialized_views_.end()) return false;
    materialized_views_.erase(it);
    return true;
}

bool SystemCatalog::HasMaterializedView(const std::string& view_name) const {
    return materialized_views_.find(view_name) != materialized_views_.end();
}

const SystemCatalog::MaterializedViewInfo*
SystemCatalog::GetMaterializedView(const std::string& view_name) const {
    auto it = materialized_views_.find(view_name);
    return it == materialized_views_.end() ? nullptr : &it->second;
}

const SystemCatalog::MaterializedViewInfo*
SystemCatalog::LookupMaterializedView(const std::string& view_name) const {
    // 与 LookupView 一致：大小写不敏感的回退路径
    auto it = materialized_views_.find(view_name);
    if (it != materialized_views_.end()) return &it->second;
    std::string upper;
    upper.reserve(view_name.size());
    for (char c : view_name)
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    for (const auto& kv : materialized_views_) {
        std::string k = kv.first;
        for (char& c : k)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (k == upper) return &kv.second;
    }
    return nullptr;
}

std::string SystemCatalog::MaterializedViewBackingTable(const std::string& view_name) {
    return "__mv_" + view_name;
}

bool SystemCatalog::HasView(const std::string& view_name) const {
    return views_.find(view_name) != views_.end();
}

const SystemCatalog::ViewDefinition* SystemCatalog::GetView(
    const std::string& view_name) const {
    auto it = views_.find(view_name);
    return it == views_.end() ? nullptr : &it->second;
}

const SystemCatalog::ViewDefinition* SystemCatalog::LookupView(
    const std::string& view_name) const {
    // 大小写不敏感的回退：UDF / view 调用方可能使用与 CREATE 时不同的大小写。
    auto it = views_.find(view_name);
    if (it != views_.end()) return &it->second;
    std::string upper;
    upper.reserve(view_name.size());
    for (char c : view_name) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    for (const auto& kv : views_) {
        std::string k = kv.first;
        for (char& c : k) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (k == upper) return &kv.second;
    }
    return nullptr;
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

const SystemCatalog::FunctionDefinition* SystemCatalog::LookupFunction(
    const std::string& name) const {
    auto it = functions_.find(name);
    if (it != functions_.end()) return &it->second;
    std::string upper;
    upper.reserve(name.size());
    for (char c : name) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    for (const auto& kv : functions_) {
        std::string k = kv.first;
        for (char& c : k) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (k == upper) return &kv.second;
    }
    return nullptr;
}

// ============ 59_procs (Category 8)：Procedure 注册 ============
//
// 实现与 Function 几乎平行；只是 DropProcedure 接受 if_exists 以保持
// 与 DROP PROCEDURE IF EXISTS 语法的兼容（同 DROP TABLE IF EXISTS）。
bool SystemCatalog::CreateProcedure(const ProcedureDefinition& def) {
    if (def.procedure_name.empty()) return false;
    if (procedures_.count(def.procedure_name) != 0) return false;
    procedures_[def.procedure_name] = def;
    return true;
}

bool SystemCatalog::DropProcedure(const std::string& procedure_name, bool if_exists) {
    auto it = procedures_.find(procedure_name);
    if (it == procedures_.end()) {
        return if_exists;  // 不存在时返回 if_exists（true 表示静默成功）
    }
    procedures_.erase(it);
    return true;
}

bool SystemCatalog::HasProcedure(const std::string& procedure_name) const {
    return procedures_.find(procedure_name) != procedures_.end();
}

const SystemCatalog::ProcedureDefinition* SystemCatalog::GetProcedure(
    const std::string& procedure_name) const {
    auto it = procedures_.find(procedure_name);
    return it == procedures_.end() ? nullptr : &it->second;
}

const SystemCatalog::ProcedureDefinition* SystemCatalog::LookupProcedure(
    const std::string& name) const {
    auto it = procedures_.find(name);
    if (it != procedures_.end()) return &it->second;
    std::string upper;
    upper.reserve(name.size());
    for (char c : name) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    for (const auto& kv : procedures_) {
        std::string k = kv.first;
        for (char& c : k) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (k == upper) return &kv.second;
    }
    return nullptr;
}

bool SystemCatalog::CreateTrigger(const TriggerDefinition& def) {
    if (def.trigger_name.empty()) return false;
    if (triggers_.count(def.trigger_name) != 0) return false;
    triggers_[def.trigger_name] = def;
    // 60_view_trigger: 落盘到 __sys_triggers__。失败不回滚内存态
    // （避免上层必须处理"已注册但未持久化"的复杂语义），仅返回 false。
    if (!PersistTriggerMetadata(def)) {
        return false;
    }
    return true;
}

bool SystemCatalog::DropTrigger(const std::string& trigger_name) {
    auto it = triggers_.find(trigger_name);
    if (it == triggers_.end()) return false;
    triggers_.erase(it);
    // 60_view_trigger: 从 __sys_triggers__ 删除对应行。
    RemoveTriggerMetadata(trigger_name);
    return true;
}

bool SystemCatalog::HasTrigger(const std::string& trigger_name) const {
    return triggers_.find(trigger_name) != triggers_.end();
}

std::vector<const SystemCatalog::TriggerDefinition*>
SystemCatalog::LookupTriggers(const std::string& table_name,
                              TriggerTiming timing,
                              TriggerEvent event) const {
    std::vector<const TriggerDefinition*> out;
    for (const auto& kv : triggers_) {
        const TriggerDefinition& d = kv.second;
        if (d.table_name != table_name) continue;
        if (d.timing != timing) continue;
        if (d.event != event) continue;
        out.push_back(&d);
    }
    return out;
}

// ============================================================================
// ALTER TABLE 支撑
// ============================================================================

bool SystemCatalog::DropPersistedTableMetadata(const std::string& table_name) {
    TableHeap* sys_heap = table_heaps_[kSysTablesKey].get();
    if (!sys_heap) return false;
    const std::vector<ValueType> schema = {ValueType::VARCHAR};
    auto iter = sys_heap->Begin();
    while (iter.HasNext()) {
        Tuple t = iter.Next(schema);
        if (t.ColumnCount() == 0) continue;
        TableInfo info = DecodeTableMetadata(t);
        if (info.table_name == table_name) {
            sys_heap->DeleteTuple(t.GetRid());
            return true;
        }
    }
    return false;
}

bool SystemCatalog::PersistTableInfo(const TableInfo& info) {
    return PersistTableMetadata(info);
}

bool SystemCatalog::RenameTableHeapKey(const std::string& old_name,
                                       const std::string& new_name) {
    if (old_name == new_name) return true;
    auto it = table_heaps_.find(old_name);
    if (it == table_heaps_.end()) return false;
    if (table_heaps_.find(new_name) != table_heaps_.end()) return false;
    // 系统目录自身与索引目录堆用特殊键名，绝不能被重命名覆盖。
    if (old_name == kSysTablesKey || new_name == kSysTablesKey ||
        old_name == kSysIndexesKey || new_name == kSysIndexesKey) {
        return false;
    }
    std::unique_ptr<TableHeap> heap = std::move(it->second);
    table_heaps_.erase(it);
    table_heaps_[new_name] = std::move(heap);
    return true;
}

void SystemCatalog::DropIndexesForTable(const std::string& table_name) {
    DropIndexesOfTable(table_name);
}

bool SystemCatalog::UpdateTableSchema(const std::string& old_name,
                                      const TableInfo& new_info) {
    if (!symbol_table_.HasTable(old_name)) return false;
    // 先失效索引：schema/列名变化后旧索引可能引用不存在的列。
    DropIndexesForTable(old_name);
    // 元数据：先删旧记录，再写新记录，避免遗留。
    DropPersistedTableMetadata(old_name);
    if (old_name != new_info.table_name) {
        // RENAME: 移动 TableHeap 句柄。
        if (!RenameTableHeapKey(old_name, new_info.table_name)) return false;
    }
    // 内存态：用新 schema 覆盖。SymbolTable 是基于 lowercase key 的 hash map，
    // 不能原地改值（const TableInfo* 暴露），只能 Remove + Add。
    symbol_table_.RemoveTable(old_name);
    if (!symbol_table_.AddTable(new_info)) {
        // 极端情况：symbol_table_ 内部冲突——继续执行会让状态不一致，
        // 因此直接报错并返回 false，让上层决定回滚策略。
        return false;
    }
    // 落盘新元数据。注意：必须在 RenameTableHeapKey 之后调用，因为 PersistTableMetadata
    // 通过 table_heaps_[info.table_name] 查找首页 id。
    return PersistTableInfo(new_info);
}

// ============================================================================
// 53_ddl: SCHEMA / SEQUENCE / FK
// ============================================================================

std::pair<std::string, std::string> SystemCatalog::SplitQualifiedName(
    const std::string& maybe_qualified) {
    auto pos = maybe_qualified.find('.');
    if (pos == std::string::npos) {
        return std::make_pair(std::string(), maybe_qualified);
    }
    return std::make_pair(maybe_qualified.substr(0, pos),
                          maybe_qualified.substr(pos + 1));
}

bool SystemCatalog::CreateSchema(const std::string& schema_name, bool if_not_exists) {
    if (schema_name.empty()) return false;
    if (schemas_.count(schema_name) != 0) {
        if (if_not_exists) return true;
        return false;
    }
    schemas_.insert(schema_name);
    return true;
}

bool SystemCatalog::DropSchema(const std::string& schema_name, bool if_exists) {
    if (schema_name.empty()) return false;
    if (schemas_.count(schema_name) == 0) {
        if (if_exists) return true;
        return false;
    }
    // 仅当 schema 内已无表时可成功。
    std::string prefix = schema_name + ".";
    for (const auto& kv : table_heaps_) {
        if (kv.first.compare(0, prefix.size(), prefix) == 0) {
            return false;
        }
    }
    // 同样检查 symbol_table_。
    for (const auto& kv : symbol_table_.GetAllTableNames()) {
        if (kv.compare(0, prefix.size(), prefix) == 0) {
            return false;
        }
    }
    schemas_.erase(schema_name);
    return true;
}

bool SystemCatalog::HasSchema(const std::string& schema_name) const {
    if (schema_name.empty()) return true;  // 默认 schema 总是存在
    return schemas_.count(schema_name) != 0;
}

bool SystemCatalog::CreateSequence(const std::string& name, int64_t start_value,
                                   int64_t step, bool if_not_exists) {
    if (name.empty()) return false;
    if (sequences_.count(name) != 0) {
        if (if_not_exists) return true;
        return false;
    }
    SequenceState st;
    st.current_value = start_value - step;  // 下次 NextSequence 返回 start_value
    st.step = (step == 0) ? 1 : step;
    st.start_value = start_value;
    sequences_[name] = st;
    return true;
}

bool SystemCatalog::DropSequence(const std::string& name, bool if_exists) {
    if (sequences_.count(name) == 0) {
        if (if_exists) return true;
        return false;
    }
    sequences_.erase(name);
    return true;
}

bool SystemCatalog::HasSequence(const std::string& name) const {
    return sequences_.count(name) != 0;
}

bool SystemCatalog::NextSequence(const std::string& name, int64_t* out_value) {
    auto it = sequences_.find(name);
    if (it == sequences_.end()) return false;
    it->second.current_value += it->second.step;
    if (out_value) *out_value = it->second.current_value;
    return true;
}

bool SystemCatalog::AddForeignKey(const std::string& child_table,
                                  const std::vector<std::string>& child_cols,
                                  const std::string& parent_table,
                                  const std::vector<std::string>& parent_cols,
                                  int on_delete_action, int on_update_action) {
    if (child_table.empty() || parent_table.empty()) return false;
    if (child_cols.empty() || parent_cols.empty()) return false;
    if (child_cols.size() != parent_cols.size()) return false;
    ForeignKeyDef fk;
    fk.child_cols = child_cols;
    fk.parent_table = parent_table;
    fk.parent_cols = parent_cols;
    fk.on_delete_action = on_delete_action;
    fk.on_update_action = on_update_action;
    foreign_keys_[child_table].push_back(std::move(fk));
    return true;
}

std::vector<ForeignKeyDef> SystemCatalog::GetForeignKeysForChild(
    const std::string& child_table) const {
    auto it = foreign_keys_.find(child_table);
    if (it == foreign_keys_.end()) return {};
    return it->second;
}

std::vector<std::pair<std::string, ForeignKeyDef>>
SystemCatalog::GetForeignKeysReferencing(const std::string& parent_table) const {
    std::vector<std::pair<std::string, ForeignKeyDef>> out;
    for (const auto& kv : foreign_keys_) {
        for (const auto& fk : kv.second) {
            if (fk.parent_table == parent_table) {
                out.emplace_back(kv.first, fk);
            }
        }
    }
    return out;
}

// ============================================================================
// 60_view_trigger (Category 9): 触发器持久化（__sys_triggers__）
// ============================================================================
//
// 每条 trigger 在 __sys_triggers__ 中以一条 VARCHAR blob 表示，
// 格式（人类可读文本，按 '|' 分隔）：
//
//   trigger_name|timing|event|table_name|for_each_row|assignments
//
// 其中 timing = "BEFORE"/"AFTER"，
//       event = "INSERT"/"UPDATE"/"DELETE"，
//       for_each_row = "1"/"0"，
//       assignments = "lhs1 = expr1; lhs2 = expr2; ..."，
//                       expr 部分直接来自 Expr::ToString()，可用 Parser 重新解析。
//
// 持久化策略：
//   - CreateTrigger 时追加写入；
//   - DropTrigger 时按 trigger_name 找到对应 RID 后删除；
//   - LoadFromDisk 阶段读全部行重建内存态；
//   - 旧库文件没有这张堆时（sys_triggers_first_page_id_ == INVALID_PAGE_ID），
//     视为"无 trigger 持久化"，首次 CREATE TRIGGER 时惰性创建。

bool SystemCatalog::EnsureSysTriggersHeap() {
    if (sys_triggers_first_page_id_ != INVALID_PAGE_ID && trigger_heap_) {
        return true;
    }
    TableHeap* heap = TableHeap::Create(buffer_pool_manager_);
    if (!heap) return false;
    if (log_manager_ != nullptr) heap->SetLogManager(log_manager_);
    trigger_heap_.reset(heap);
    sys_triggers_first_page_id_ = heap->GetFirstPageId();
    // 在 sys_tables 里登记一条"定位记录"：表名 = kSysTriggersKey,
    // 列为空，末尾 4 字节是该堆的首页 id。与 __sys_indexes__ 完全相同。
    TableInfo marker;
    marker.table_name = kSysTriggersKey;
    marker.columns.clear();
    std::string blob = EncodeTableInfo(marker, sys_triggers_first_page_id_);
    TableHeap* sys_heap = table_heaps_[kSysTablesKey].get();
    if (!sys_heap) return false;
    Tuple t;
    std::vector<Value> row;
    row.emplace_back(Value::MakeVarchar(blob));
    Tuple row_t(std::move(row));
    RID rid;
    std::vector<ValueType> schema = {ValueType::VARCHAR};
    return sys_heap->InsertTuple(row_t, &rid, schema);
}

bool SystemCatalog::PersistTriggerMetadata(const TriggerDefinition& def) {
    if (!EnsureSysTriggersHeap()) return false;
    if (!trigger_heap_) return false;
    std::ostringstream oss;
    oss << def.trigger_name << '|';
    oss << (def.timing == TriggerTiming::BEFORE ? "BEFORE" : "AFTER") << '|';
    switch (def.event) {
        case TriggerEvent::INSERT: oss << "INSERT"; break;
        case TriggerEvent::UPDATE: oss << "UPDATE"; break;
        case TriggerEvent::DELETE: oss << "DELETE"; break;
    }
    oss << '|' << def.table_name << '|'
        << (def.for_each_row ? "1" : "0") << '|'
        << SerializeTriggerAssignments(def.assignments);
    Tuple t;
    std::vector<Value> row;
    row.emplace_back(Value::MakeVarchar(oss.str()));
    Tuple row_t(std::move(row));
    RID rid;
    std::vector<ValueType> schema = {ValueType::VARCHAR};
    return trigger_heap_->InsertTuple(row_t, &rid, schema);
}

void SystemCatalog::RemoveTriggerMetadata(const std::string& trigger_name) {
    if (sys_triggers_first_page_id_ == INVALID_PAGE_ID) return;
    if (!trigger_heap_) {
        trigger_heap_.reset(TableHeap::Open(buffer_pool_manager_,
                                             sys_triggers_first_page_id_));
    }
    if (!trigger_heap_) return;
    const std::vector<ValueType> schema = {ValueType::VARCHAR};
    auto iter = trigger_heap_->Begin();
    while (iter.HasNext()) {
        Tuple t = iter.Next(schema);
        if (t.ColumnCount() == 0) continue;
        const std::string& blob = t.GetValue(0).AsVarchar();
        // 解析 trigger_name 段（首个 '|' 之前）。
        auto pipe = blob.find('|');
        std::string name = (pipe == std::string::npos) ? blob : blob.substr(0, pipe);
        if (name == trigger_name) {
            trigger_heap_->DeleteTuple(t.GetRid());
            return;
        }
    }
}

void SystemCatalog::LoadTriggersFromDisk() {
    if (sys_triggers_first_page_id_ == INVALID_PAGE_ID) return;
    if (!trigger_heap_) {
        trigger_heap_.reset(TableHeap::Open(buffer_pool_manager_,
                                             sys_triggers_first_page_id_));
    }
    if (!trigger_heap_) return;
    const std::vector<ValueType> schema = {ValueType::VARCHAR};
    auto iter = trigger_heap_->Begin();
    while (iter.HasNext()) {
        Tuple t = iter.Next(schema);
        if (t.ColumnCount() == 0) continue;
        const std::string& blob = t.GetValue(0).AsVarchar();
        // 拆分 6 段。
        std::vector<std::string> parts;
        std::string cur;
        for (char c : blob) {
            if (c == '|') { parts.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        parts.push_back(cur);
        if (parts.size() < 6) continue;
        TriggerDefinition def;
        def.trigger_name = parts[0];
        if (parts[1] == "AFTER") def.timing = TriggerTiming::AFTER;
        else def.timing = TriggerTiming::BEFORE;
        if (parts[2] == "UPDATE") def.event = TriggerEvent::UPDATE;
        else if (parts[2] == "DELETE") def.event = TriggerEvent::DELETE;
        else def.event = TriggerEvent::INSERT;
        def.table_name = parts[3];
        def.for_each_row = (parts[4] == "1");
        def.assignments = DeserializeTriggerAssignments(parts[5]);
        if (def.trigger_name.empty()) continue;
        // 已存在同名 trigger 时（异常路径）不覆盖。
        if (triggers_.count(def.trigger_name) != 0) continue;
        triggers_[def.trigger_name] = std::move(def);
    }
}

std::string SystemCatalog::SerializeTriggerAssignments(
    const std::vector<std::pair<std::string, ExprPtr>>& assignments) {
    std::ostringstream oss;
    for (size_t i = 0; i < assignments.size(); ++i) {
        if (i) oss << "; ";
        oss << assignments[i].first << " = "
            << (assignments[i].second ? assignments[i].second->ToString() : "NULL");
    }
    return oss.str();
}

std::vector<std::pair<std::string, ExprPtr>>
SystemCatalog::DeserializeTriggerAssignments(const std::string& text) {
    std::vector<std::pair<std::string, ExprPtr>> out;
    if (text.empty()) return out;
    // 按 "; " 分隔 assignment；每个 chunk 用 "lhs = expr" 形式。
    std::string cur;
    auto flush_chunk = [&]() {
        if (cur.empty()) return;
        auto eq = cur.find('=');
        if (eq == std::string::npos) {
            cur.clear();
            return;
        }
        std::string lhs = cur.substr(0, eq);
        std::string rhs = cur.substr(eq + 1);
        // 去前导空格
        size_t s = rhs.find_first_not_of(' ');
        if (s != std::string::npos) rhs = rhs.substr(s);
        // 用 ReParseExprOrNull 重新解析右侧（落盘的 DEFAULT/CHECK 复用同一工具）。
        ExprPtr e = ReParseExprOrNull(rhs);
        out.emplace_back(lhs, e);
        cur.clear();
    };
    for (size_t i = 0; i < text.size(); ++i) {
        if (i + 1 < text.size() && text[i] == ';' && text[i + 1] == ' ') {
            flush_chunk();
            ++i;  // 跳过 ' '
        } else {
            cur.push_back(text[i]);
        }
    }
    flush_chunk();
    return out;
}

}  // namespace sqlcompiler