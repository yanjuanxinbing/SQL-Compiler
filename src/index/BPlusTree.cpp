#include "index/BPlusTree.h"

#include "index/BPlusTreePage.h"
#include "storage/Page.h"
#include "txn/LogManager.h"
#include "txn/LogRecord.h"
#include "txn/Transaction.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace sqlcompiler {

namespace {

using namespace bptree;

// 写一条 UPDATE 日志记录，并把 page_lsn 设为该记录的 LSN。
// 仅在 log_manager_ 非空时调用。Phase C：返回 LSN 后顺便回填 txn 最近的
// undo log 条目的 LSN，让 Rollback 能产出正确的 undo_next_lsn。
lsn_t EmitPageImageRecord(LogManager* lm, page_id_t pid, const char* before_data,
                          const char* after_data, Transaction* txn) {
    LogRecord rec;
    rec.type_ = LogRecordType::UPDATE;
    rec.txn_id_ = (txn != nullptr) ? txn->GetTxnId() : 0;
    rec.page_id_ = pid;
    if (before_data != nullptr) {
        rec.before_image_.assign(before_data, before_data + PAGE_SIZE);
    }
    if (after_data != nullptr) {
        rec.after_image_.assign(after_data, after_data + PAGE_SIZE);
    }
    lsn_t lsn = lm->AppendRecord(std::move(rec));
    if (txn != nullptr && txn->IsActive()) {
        txn->SetLastUndoLSN(lsn);
    }
    return lsn;
}

// ============================================================================
// 页面读写辅助
//
// 实现策略：所有页内修改都走「物化 -> 修改 -> 整页重写」，而不是原地挪动字节。
// 页内至多 200 余条记录，一次重写不到 4KB 的 memcpy，代价可忽略；换来的是
// 插入、分裂、删除三条路径共用同一套读写函数，不存在「槽位目录与记录区各自
// 维护偏移」导致的错位类 bug。碎片回收也随之免费（重写即压缩）。
// ============================================================================

int32_t ReadI32(const char* d, size_t off) {
    int32_t v;
    std::memcpy(&v, d + off, sizeof(int32_t));
    return v;
}
void WriteI32(char* d, size_t off, int32_t v) {
    std::memcpy(d + off, &v, sizeof(int32_t));
}
uint16_t ReadU16(const char* d, size_t off) {
    uint16_t v;
    std::memcpy(&v, d + off, sizeof(uint16_t));
    return v;
}
void WriteU16(char* d, size_t off, uint16_t v) {
    std::memcpy(d + off, &v, sizeof(uint16_t));
}
uint32_t ReadU32(const char* d, size_t off) {
    uint32_t v;
    std::memcpy(&v, d + off, sizeof(uint32_t));
    return v;
}
void WriteU32(char* d, size_t off, uint32_t v) {
    std::memcpy(d + off, &v, sizeof(uint32_t));
}

PageType GetPageType(const char* d) {
    uint8_t t = static_cast<uint8_t>(d[0]);
    if (t == 1) return PageType::kInternal;
    if (t == 2) return PageType::kLeaf;
    return PageType::kUninitialized;
}
void SetPageType(char* d, PageType t) { d[0] = static_cast<char>(t); }

uint16_t GetKeyCount(const char* d) { return ReadU16(d, 4); }
void SetKeyCount(char* d, uint16_t n) { WriteU16(d, 4, n); }
uint16_t GetFreeOffset(const char* d) { return ReadU16(d, 6); }
void SetFreeOffset(char* d, uint16_t v) { WriteU16(d, 6, v); }

// 页头是否自洽。与 TableHeap 的 NormalizePageHeader 同源：全零页的 page_type
// 为 0、free_offset 为 0，都落在非法区间，因此不会被误认成合法节点。
bool IsValidHeader(const char* d, PageType expect) {
    if (GetPageType(d) != expect) return false;
    uint16_t free_off = GetFreeOffset(d);
    if (free_off == 0 || free_off > PAGE_SIZE) return false;
    size_t header = (expect == PageType::kLeaf) ? kLeafHeaderBytes : kInternalHeaderBytes;
    size_t slot_bytes = (expect == PageType::kLeaf) ? kLeafSlotBytes : kInternalSlotBytes;
    size_t dir_end = header + static_cast<size_t>(GetKeyCount(d)) * slot_bytes;
    return dir_end <= free_off;
}

void InitLeaf(char* d) {
    std::memset(d, 0, PAGE_SIZE);
    SetPageType(d, PageType::kLeaf);
    SetKeyCount(d, 0);
    SetFreeOffset(d, static_cast<uint16_t>(PAGE_SIZE));
    WriteI32(d, 8, INVALID_PAGE_ID);   // parent（保留，未使用）
    WriteI32(d, 16, INVALID_PAGE_ID);  // next_leaf
    WriteI32(d, 20, INVALID_PAGE_ID);  // prev_leaf
}

void InitInternal(char* d) {
    std::memset(d, 0, PAGE_SIZE);
    SetPageType(d, PageType::kInternal);
    SetKeyCount(d, 0);
    SetFreeOffset(d, static_cast<uint16_t>(PAGE_SIZE));
    WriteI32(d, 8, INVALID_PAGE_ID);
    WriteI32(d, 16, INVALID_PAGE_ID);  // first_child
}

page_id_t GetNextLeaf(const char* d) { return ReadI32(d, 16); }
void SetNextLeaf(char* d, page_id_t v) { WriteI32(d, 16, v); }
page_id_t GetPrevLeaf(const char* d) { return ReadI32(d, 20); }
void SetPrevLeaf(char* d, page_id_t v) { WriteI32(d, 20, v); }
page_id_t GetFirstChild(const char* d) { return ReadI32(d, 16); }
void SetFirstChild(char* d, page_id_t v) { WriteI32(d, 16, v); }

struct LeafEntry {
    std::vector<char> key_bytes;
    IndexKey key;
    RID rid;
};

struct InternalEntry {
    std::vector<char> key_bytes;
    IndexKey key;
    RID rid;                 // 分隔键的 RID 部分
    page_id_t child = INVALID_PAGE_ID;
};

bool ReadLeafEntries(const char* d, const std::vector<ValueType>& schema,
                     std::vector<LeafEntry>* out) {
    out->clear();
    if (!IsValidHeader(d, PageType::kLeaf)) return false;
    const int n = GetKeyCount(d);
    out->reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const size_t so = kLeafHeaderBytes + static_cast<size_t>(i) * kLeafSlotBytes;
        uint32_t key_off = ReadU32(d, so);
        uint16_t key_len = ReadU16(d, so + 4);
        uint16_t flags = ReadU16(d, so + 6);
        if (flags & kSlotDeleted) continue;  // 墓碑
        if (key_off < kLeafHeaderBytes ||
            static_cast<size_t>(key_off) + key_len > PAGE_SIZE) {
            return false;  // 槽位越界：页面损坏，宁可报错也不越界读
        }
        LeafEntry e;
        if (!DeserializeKey(d + key_off, key_len, schema, &e.key)) return false;
        e.key_bytes.assign(d + key_off, d + key_off + key_len);
        e.rid.page_id = ReadI32(d, so + 8);
        e.rid.slot_num = ReadI32(d, so + 12);
        out->push_back(std::move(e));
    }
    return true;
}

size_t LeafBytesNeeded(const std::vector<LeafEntry>& entries) {
    size_t total = kLeafHeaderBytes;
    for (const auto& e : entries) total += kLeafSlotBytes + e.key_bytes.size();
    return total;
}

// 整页重写。放不下返回 false（调用方据此触发分裂）。
bool WriteLeafEntries(char* d, const std::vector<LeafEntry>& entries,
                      page_id_t next_leaf, page_id_t prev_leaf) {
    if (entries.size() > static_cast<size_t>(kMaxLeafSlots)) return false;
    if (LeafBytesNeeded(entries) > PAGE_SIZE) return false;

    InitLeaf(d);
    SetNextLeaf(d, next_leaf);
    SetPrevLeaf(d, prev_leaf);
    size_t free_off = PAGE_SIZE;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        free_off -= e.key_bytes.size();
        if (!e.key_bytes.empty()) {
            std::memcpy(d + free_off, e.key_bytes.data(), e.key_bytes.size());
        }
        const size_t so = kLeafHeaderBytes + i * kLeafSlotBytes;
        WriteU32(d, so, static_cast<uint32_t>(free_off));
        WriteU16(d, so + 4, static_cast<uint16_t>(e.key_bytes.size()));
        WriteU16(d, so + 6, 0);
        WriteI32(d, so + 8, e.rid.page_id);
        WriteI32(d, so + 12, e.rid.slot_num);
    }
    SetKeyCount(d, static_cast<uint16_t>(entries.size()));
    SetFreeOffset(d, static_cast<uint16_t>(free_off));
    return true;
}

bool ReadInternalEntries(const char* d, const std::vector<ValueType>& schema,
                         std::vector<InternalEntry>* out) {
    out->clear();
    if (!IsValidHeader(d, PageType::kInternal)) return false;
    const int n = GetKeyCount(d);
    out->reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const size_t so = kInternalHeaderBytes + static_cast<size_t>(i) * kInternalSlotBytes;
        uint32_t key_off = ReadU32(d, so);
        uint16_t key_len = ReadU16(d, so + 4);
        if (key_off < kInternalHeaderBytes ||
            static_cast<size_t>(key_off) + key_len > PAGE_SIZE) {
            return false;
        }
        InternalEntry e;
        if (!DeserializeKey(d + key_off, key_len, schema, &e.key)) return false;
        e.key_bytes.assign(d + key_off, d + key_off + key_len);
        e.child = ReadI32(d, so + 8);
        e.rid.page_id = ReadI32(d, so + 12);
        e.rid.slot_num = ReadI32(d, so + 16);
        out->push_back(std::move(e));
    }
    return true;
}

size_t InternalBytesNeeded(const std::vector<InternalEntry>& entries) {
    size_t total = kInternalHeaderBytes;
    for (const auto& e : entries) total += kInternalSlotBytes + e.key_bytes.size();
    return total;
}

bool WriteInternalEntries(char* d, page_id_t first_child,
                          const std::vector<InternalEntry>& entries) {
    if (entries.size() > static_cast<size_t>(kMaxInternalSlots)) return false;
    if (InternalBytesNeeded(entries) > PAGE_SIZE) return false;

    InitInternal(d);
    SetFirstChild(d, first_child);
    size_t free_off = PAGE_SIZE;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        free_off -= e.key_bytes.size();
        if (!e.key_bytes.empty()) {
            std::memcpy(d + free_off, e.key_bytes.data(), e.key_bytes.size());
        }
        const size_t so = kInternalHeaderBytes + i * kInternalSlotBytes;
        WriteU32(d, so, static_cast<uint32_t>(free_off));
        WriteU16(d, so + 4, static_cast<uint16_t>(e.key_bytes.size()));
        WriteU16(d, so + 6, 0);
        WriteI32(d, so + 8, e.child);
        WriteI32(d, so + 12, e.rid.page_id);
        WriteI32(d, so + 16, e.rid.slot_num);
    }
    SetKeyCount(d, static_cast<uint16_t>(entries.size()));
    SetFreeOffset(d, static_cast<uint16_t>(free_off));
    return true;
}

// 节点是否可能装不下再插一条键长为 reserve 的记录。
// 预分裂的判定基准：宁可早分裂，也不要在插入中途才发现放不下。
bool MayOverflowGeneric(const char* d, size_t reserve, PageType type) {
    const size_t header = (type == PageType::kLeaf) ? kLeafHeaderBytes
                                                    : kInternalHeaderBytes;
    const size_t slot_bytes = (type == PageType::kLeaf) ? kLeafSlotBytes
                                                        : kInternalSlotBytes;
    const int max_slots = (type == PageType::kLeaf) ? kMaxLeafSlots : kMaxInternalSlots;
    const int count = GetKeyCount(d);
    if (count + 1 > max_slots) return true;
    const size_t used = header + static_cast<size_t>(count) * slot_bytes +
                        (PAGE_SIZE - GetFreeOffset(d));
    return used + slot_bytes + reserve > PAGE_SIZE;
}

bool MayOverflow(const char* d, size_t reserve) {
    const PageType t = GetPageType(d);
    if (t == PageType::kUninitialized) return false;
    return MayOverflowGeneric(d, reserve, t);
}

// 在内部节点中选出 (key, rid) 应当进入的孩子。
// 分隔键语义 sep[i] <= 右子树所有项，因此取最后一个满足 (key,rid) >= sep[i] 的孩子。
page_id_t ChooseChild(const char* d, const std::vector<InternalEntry>& entries,
                      const IndexKey& key, const RID& rid) {
    page_id_t child = GetFirstChild(d);
    for (const auto& e : entries) {
        if (CompareKeyThenRid(key, rid, e.key, e.rid) >= 0) {
            child = e.child;
        } else {
            break;
        }
    }
    return child;
}

// ============================================================================
// 删除再平衡的占用阈值
//
// 用法：删除后某非根节点的 key_count 跌破阈值，则 RedistributeOrMerge。
// 「四分之一上限」是工程里常用的简单策略——比 B 树经典的「一半」宽松得多，
// 因为我们用的是预分裂策略，节点本来就偏满，下界放到 1/4 既能回收大多数空页
// 又避免一次删除触发长链合并。
// ============================================================================
constexpr int kLeafMinOccupancy = (kMaxLeafSlots + 1) / 4;       // ≈ 63
constexpr int kInternalMinOccupancy = (kMaxInternalSlots + 1) / 4;  // ≈ 51

}  // namespace

// ============================================================================
// 构造 / 生命周期
// ============================================================================

BPlusTree::BPlusTree(BufferPoolManager* bpm, std::vector<ValueType> key_schema,
                     bool is_unique, page_id_t root_page_id)
    : bpm_(bpm), key_schema_(std::move(key_schema)), is_unique_(is_unique),
      root_page_id_(root_page_id) {
}

std::unique_ptr<BPlusTree> BPlusTree::Create(BufferPoolManager* bpm,
                                             std::vector<ValueType> key_schema,
                                             bool is_unique) {
    PageGuard root = PageGuard::New(bpm);
    if (!root.Valid()) return nullptr;
    InitLeaf(root.Data());
    root.MarkDirty();
    page_id_t pid = root.PageId();
    root.Release();
    return std::make_unique<BPlusTree>(bpm, std::move(key_schema), is_unique, pid);
}

std::unique_ptr<BPlusTree> BPlusTree::Open(BufferPoolManager* bpm,
                                           std::vector<ValueType> key_schema,
                                           bool is_unique,
                                           page_id_t root_page_id) {
    if (root_page_id < 0) return nullptr;
    return std::make_unique<BPlusTree>(bpm, std::move(key_schema), is_unique,
                                       root_page_id);
}

void BPlusTree::Destroy(BufferPoolManager* bpm, page_id_t root_page_id) {
    if (bpm == nullptr || root_page_id < 0) return;
    // 广度优先回收。visited 防御损坏页形成的环，避免这里变成死循环——
    // 与 TableHeap 页链遍历同样的教训。
    std::vector<page_id_t> queue{root_page_id};
    std::unordered_set<page_id_t> visited;
    std::vector<page_id_t> to_free;
    while (!queue.empty()) {
        page_id_t pid = queue.back();
        queue.pop_back();
        if (pid < 0 || !visited.insert(pid).second) continue;
        to_free.push_back(pid);
        PageGuard g = PageGuard::Fetch(bpm, pid);
        if (!g.Valid()) continue;
        const char* d = g.Data();
        if (GetPageType(d) != PageType::kInternal) continue;
        if (!IsValidHeader(d, PageType::kInternal)) continue;
        queue.push_back(GetFirstChild(d));
        const int n = GetKeyCount(d);
        for (int i = 0; i < n; ++i) {
            const size_t so = kInternalHeaderBytes +
                              static_cast<size_t>(i) * kInternalSlotBytes;
            queue.push_back(ReadI32(d, so + 8));
        }
    }
    for (page_id_t pid : to_free) {
        bpm->DeletePage(pid);
    }
}

// ============================================================================
// 下降
// ============================================================================

page_id_t BPlusTree::FindLeafPage(const IndexKey& key, const RID& rid) const {
    page_id_t pid = root_page_id_;
    std::unordered_set<page_id_t> visited;
    while (pid >= 0) {
        if (!visited.insert(pid).second) return INVALID_PAGE_ID;  // 环路防御
        PageGuard g = PageGuard::Fetch(bpm_, pid);
        if (!g.Valid()) return INVALID_PAGE_ID;
        const char* d = g.Data();
        if (GetPageType(d) == PageType::kLeaf) return pid;
        std::vector<InternalEntry> entries;
        if (!ReadInternalEntries(d, key_schema_, &entries)) return INVALID_PAGE_ID;
        pid = ChooseChild(d, entries, key, rid);
    }
    return INVALID_PAGE_ID;
}

page_id_t BPlusTree::LeftmostLeafPage() const {
    page_id_t pid = root_page_id_;
    std::unordered_set<page_id_t> visited;
    while (pid >= 0) {
        if (!visited.insert(pid).second) return INVALID_PAGE_ID;
        PageGuard g = PageGuard::Fetch(bpm_, pid);
        if (!g.Valid()) return INVALID_PAGE_ID;
        const char* d = g.Data();
        if (GetPageType(d) == PageType::kLeaf) return pid;
        if (!IsValidHeader(d, PageType::kInternal)) return INVALID_PAGE_ID;
        pid = GetFirstChild(d);
    }
    return INVALID_PAGE_ID;
}

// ============================================================================
// 插入：下降途中预分裂
//
// 算法选择的理由：自底向上分裂需要回溯父节点，一旦父节点也满就要级联向上，
// 还要处理「根分裂」这个特例——三者纠缠在一起极易写错（第一版就写歪了）。
// 预分裂把它拉直成一条单向下降：进入某个节点之前先保证它装得下，于是叶子插入
// 永远不会失败，也就没有级联。代价是节点会略早分裂、填充率稍低，对教学规模
// 无影响。
// ============================================================================

bool BPlusTree::Insert(const IndexKey& key, const RID& rid) {
    std::vector<char> key_bytes = SerializeKey(key, key_schema_);
    if (key_bytes.size() > kMaxKeyBytes) return false;
    if (is_unique_ && FindFirst(key).IsValid()) return false;

    // 预留量按「本次要插入的键」计算，而不是按最大可能键长，
    // 这样定长小键（如 INT 主键）仍能接近填满页面。
    const size_t reserve = key_bytes.size();

    // ---- 1) 根：装不下就先分裂。根 id 不变，因此无需回写目录元数据 ----
    {
        PageGuard root = PageGuard::Fetch(bpm_, root_page_id_);
        if (!root.Valid()) return false;
        char* rd = root.Data();
        if (GetPageType(rd) == PageType::kUninitialized) {
            InitLeaf(rd);  // 自愈：全零页当作空叶子
            root.MarkDirty();
        }
        const bool overflow = MayOverflow(rd, reserve);
        root.Release();
        if (overflow && !SplitRoot()) return false;
    }

    // ---- 2) 下降，遇到装不下的孩子就地分裂 ----
    page_id_t pid = root_page_id_;
    int guard_steps = 0;
    while (true) {
        // 每层最多重试一次（分裂后重选孩子），步数上限兜底防御损坏页导致的死循环
        if (++guard_steps > 128) return false;

        PageGuard node = PageGuard::Fetch(bpm_, pid);
        if (!node.Valid()) return false;
        const char* d = node.Data();
        if (GetPageType(d) == PageType::kLeaf) break;

        std::vector<InternalEntry> entries;
        if (!ReadInternalEntries(d, key_schema_, &entries)) return false;
        const page_id_t child_pid = ChooseChild(d, entries, key, rid);
        node.Release();
        if (child_pid < 0) return false;

        PageGuard child = PageGuard::Fetch(bpm_, child_pid);
        if (!child.Valid()) return false;
        char* cd = child.Data();
        if (GetPageType(cd) == PageType::kUninitialized) {
            InitLeaf(cd);
            child.MarkDirty();
        }
        const bool overflow = MayOverflow(cd, reserve);
        child.Release();

        if (overflow) {
            // 父节点此刻一定装得下分隔键（进入本层前已保证），分裂后重选孩子
            if (!SplitChild(pid, child_pid, reserve)) return false;
            continue;
        }
        pid = child_pid;
    }

    // ---- 3) 叶子插入。由不变式保证一定装得下 ----
    PageGuard leaf = PageGuard::Fetch(bpm_, pid);
    if (!leaf.Valid()) return false;
    char* d = leaf.Data();
    if (!IsValidHeader(d, PageType::kLeaf)) {
        InitLeaf(d);
        leaf.MarkDirty();
    }
    // Phase A：写之前抓叶子整页 before-image。
    if (active_txn_ != nullptr && active_txn_->IsActive()) {
        active_txn_->AppendUndo(pid, d, PAGE_SIZE, "BPlusTree::Insert(leaf)");
    }
    // Phase B：抓叶子整页 before-image 给 WAL。
    std::vector<char> leaf_before;
    if (log_manager_ != nullptr) {
        leaf_before.assign(d, d + PAGE_SIZE);
    }
    std::vector<LeafEntry> entries;
    if (!ReadLeafEntries(d, key_schema_, &entries)) return false;

    LeafEntry ne;
    ne.key_bytes = std::move(key_bytes);
    ne.key = key;
    ne.rid = rid;
    auto pos = std::lower_bound(
        entries.begin(), entries.end(), ne,
        [](const LeafEntry& a, const LeafEntry& b) {
            return CompareKeyThenRid(a.key, a.rid, b.key, b.rid) < 0;
        });
    entries.insert(pos, std::move(ne));

    if (!WriteLeafEntries(d, entries, GetNextLeaf(d), GetPrevLeaf(d))) return false;
    leaf.MarkDirty();

    // Phase B：写 UPDATE 记录（leaf 整页 before/after）。
    if (log_manager_ != nullptr) {
        lsn_t lsn = EmitPageImageRecord(log_manager_, pid,
                                       leaf_before.data(), d, active_txn_);
        leaf.SetPageLsn(lsn);
    }
    return true;
}

bool BPlusTree::SplitChild(page_id_t parent_pid, page_id_t child_pid,
                           size_t reserve) {
    PageGuard parent = PageGuard::Fetch(bpm_, parent_pid);
    if (!parent.Valid()) return false;
    std::vector<InternalEntry> parent_entries;
    if (!ReadInternalEntries(parent.Data(), key_schema_, &parent_entries)) return false;
    const page_id_t parent_first_child = GetFirstChild(parent.Data());

    // Phase B：抓 parent 整页 before-image 给 WAL。
    std::vector<char> parent_before;
    if (log_manager_ != nullptr) {
        parent_before.assign(parent.Data(), parent.Data() + PAGE_SIZE);
    }

    PageGuard child = PageGuard::Fetch(bpm_, child_pid);
    if (!child.Valid()) return false;
    char* cd = child.Data();

    // Phase B：抓 child 整页 before-image。
    std::vector<char> child_before;
    if (log_manager_ != nullptr) {
        child_before.assign(cd, cd + PAGE_SIZE);
    }

    PageGuard sib = PageGuard::New(bpm_);
    if (!sib.Valid()) return false;  // 缓冲池耗尽
    const page_id_t sib_pid = sib.PageId();

    InternalEntry sep;
    sep.child = sib_pid;

    if (GetPageType(cd) == PageType::kLeaf) {
        std::vector<LeafEntry> entries;
        if (!ReadLeafEntries(cd, key_schema_, &entries)) return false;
        if (entries.size() < 2) return false;
        const size_t mid = entries.size() / 2;
        std::vector<LeafEntry> left(entries.begin(), entries.begin() + mid);
        std::vector<LeafEntry> right(entries.begin() + mid, entries.end());

        const page_id_t old_next = GetNextLeaf(cd);
        const page_id_t old_prev = GetPrevLeaf(cd);
        if (!WriteLeafEntries(sib.Data(), right, old_next, child_pid)) return false;
        sib.MarkDirty();
        if (!WriteLeafEntries(cd, left, sib_pid, old_prev)) return false;
        child.MarkDirty();
        if (old_next >= 0) {
            PageGuard nxt = PageGuard::Fetch(bpm_, old_next);
            if (nxt.Valid() && IsValidHeader(nxt.Data(), PageType::kLeaf)) {
                SetPrevLeaf(nxt.Data(), sib_pid);
                nxt.MarkDirty();
            }
        }
        // 分隔键取右页第一条的完整 (key, rid)
        sep.key = right.front().key;
        sep.rid = right.front().rid;
        sep.key_bytes = right.front().key_bytes;
    } else {
        std::vector<InternalEntry> entries;
        if (!ReadInternalEntries(cd, key_schema_, &entries)) return false;
        if (entries.size() < 2) return false;
        const page_id_t first_child = GetFirstChild(cd);
        const size_t mid = entries.size() / 2;
        // 内部节点分裂：中间键上推到父节点，不在两个孩子里保留
        InternalEntry up = entries[mid];
        std::vector<InternalEntry> left(entries.begin(), entries.begin() + mid);
        std::vector<InternalEntry> right(entries.begin() + mid + 1, entries.end());

        if (!WriteInternalEntries(sib.Data(), up.child, right)) return false;
        sib.MarkDirty();
        if (!WriteInternalEntries(cd, first_child, left)) return false;
        child.MarkDirty();

        sep.key = up.key;
        sep.rid = up.rid;
        sep.key_bytes = up.key_bytes;
    }

    // Phase B：先写 child 和 sib 的 UPDATE 记录（两者是新增或修改页面，
    // sibling 是全新页 before-image 视为全零）。
    if (log_manager_ != nullptr) {
        // child 整页 UPDATE
        lsn_t child_lsn = EmitPageImageRecord(log_manager_, child_pid,
                                              child_before.data(), cd, active_txn_);
        child.SetPageLsn(child_lsn);
        // sib 全新页面：before-image 全零，after 是当前内容。
        std::vector<char> zero_before(PAGE_SIZE, 0);
        lsn_t sib_lsn = EmitPageImageRecord(log_manager_, sib_pid,
                                            zero_before.data(), sib.Data(), active_txn_);
        sib.SetPageLsn(sib_lsn);
    }

    child.Release();
    sib.Release();

    auto pos = std::lower_bound(
        parent_entries.begin(), parent_entries.end(), sep,
        [](const InternalEntry& a, const InternalEntry& b) {
            return CompareKeyThenRid(a.key, a.rid, b.key, b.rid) < 0;
        });
    parent_entries.insert(pos, std::move(sep));

    // 前置条件保证这里必然写得下；写不下说明不变式被破坏，宁可失败也不静默截断
    if (!WriteInternalEntries(parent.Data(), parent_first_child, parent_entries)) {
        return false;
    }
    parent.MarkDirty();

    // Phase B：写 parent 的 UPDATE 记录。
    if (log_manager_ != nullptr) {
        lsn_t parent_lsn = EmitPageImageRecord(log_manager_, parent_pid,
                                               parent_before.data(),
                                               parent.Data(), active_txn_);
        parent.SetPageLsn(parent_lsn);
    }
    (void)reserve;
    return true;
}

bool BPlusTree::SplitRoot() {
    PageGuard root = PageGuard::Fetch(bpm_, root_page_id_);
    if (!root.Valid()) return false;
    char* rd = root.Data();
    const PageType type = GetPageType(rd);

    // Phase B：抓 root 整页 before-image。SplitRoot 之后 root 变成新的内部节点，
    // before-image 用于回放（万一 redo 时 root 已不是当时状态也能重建）。
    std::vector<char> root_before;
    if (log_manager_ != nullptr) {
        root_before.assign(rd, rd + PAGE_SIZE);
    }

    // 把根的全部内容搬到一个新页，根页本身改写成新的内部节点
    PageGuard moved = PageGuard::New(bpm_);
    if (!moved.Valid()) return false;
    const page_id_t moved_pid = moved.PageId();
    std::memcpy(moved.Data(), rd, PAGE_SIZE);
    moved.MarkDirty();

    PageGuard sib = PageGuard::New(bpm_);
    if (!sib.Valid()) return false;
    const page_id_t sib_pid = sib.PageId();

    InternalEntry sep;
    sep.child = sib_pid;

    if (type == PageType::kLeaf) {
        std::vector<LeafEntry> entries;
        if (!ReadLeafEntries(moved.Data(), key_schema_, &entries)) return false;
        // 只有一条记录时无法一分为二。此时树高增加也换不来空间，直接放弃分裂：
        // 调用方会继续尝试插入，若确实放不下则返回失败而不是无限分裂。
        if (entries.size() < 2) return false;
        const size_t mid = entries.size() / 2;
        std::vector<LeafEntry> left(entries.begin(), entries.begin() + mid);
        std::vector<LeafEntry> right(entries.begin() + mid, entries.end());
        const page_id_t old_next = GetNextLeaf(moved.Data());

        if (!WriteLeafEntries(sib.Data(), right, old_next, moved_pid)) return false;
        sib.MarkDirty();
        if (!WriteLeafEntries(moved.Data(), left, sib_pid, INVALID_PAGE_ID)) return false;
        moved.MarkDirty();
        if (old_next >= 0) {
            PageGuard nxt = PageGuard::Fetch(bpm_, old_next);
            if (nxt.Valid() && IsValidHeader(nxt.Data(), PageType::kLeaf)) {
                SetPrevLeaf(nxt.Data(), sib_pid);
                nxt.MarkDirty();
            }
        }
        sep.key = right.front().key;
        sep.rid = right.front().rid;
        sep.key_bytes = right.front().key_bytes;
    } else {
        std::vector<InternalEntry> entries;
        if (!ReadInternalEntries(moved.Data(), key_schema_, &entries)) return false;
        if (entries.size() < 2) return false;
        const page_id_t first_child = GetFirstChild(moved.Data());
        const size_t mid = entries.size() / 2;
        InternalEntry up = entries[mid];
        std::vector<InternalEntry> left(entries.begin(), entries.begin() + mid);
        std::vector<InternalEntry> right(entries.begin() + mid + 1, entries.end());

        if (!WriteInternalEntries(sib.Data(), up.child, right)) return false;
        sib.MarkDirty();
        if (!WriteInternalEntries(moved.Data(), first_child, left)) return false;
        moved.MarkDirty();
        sep.key = up.key;
        sep.rid = up.rid;
        sep.key_bytes = up.key_bytes;
    }

    // Phase B：写 moved 与 sib 的 UPDATE 记录。
    if (log_manager_ != nullptr) {
        std::vector<char> zero_before(PAGE_SIZE, 0);
        // moved 是新分配的页面，但写入前的快照是 root 的拷贝（已被搬过来）；
        // 这里 before 视为「全零」即可：redo 时 moved 会被 after_image 覆盖。
        lsn_t moved_lsn = EmitPageImageRecord(log_manager_, moved_pid,
                                              zero_before.data(), moved.Data(),
                                              active_txn_);
        moved.SetPageLsn(moved_lsn);
        lsn_t sib_lsn = EmitPageImageRecord(log_manager_, sib_pid,
                                            zero_before.data(), sib.Data(),
                                            active_txn_);
        sib.SetPageLsn(sib_lsn);
    }

    std::vector<InternalEntry> root_entries{std::move(sep)};
    if (!WriteInternalEntries(rd, moved_pid, root_entries)) return false;
    root.MarkDirty();

    // Phase B：写 root 的 UPDATE 记录（root 变成新内部节点，before/after 都捕获）。
    if (log_manager_ != nullptr) {
        lsn_t root_lsn = EmitPageImageRecord(log_manager_, root_page_id_,
                                             root_before.data(), rd, active_txn_);
        root.SetPageLsn(root_lsn);
    }
    return true;
}

// ============================================================================
// 查找与删除
// ============================================================================

RID BPlusTree::FindFirst(const IndexKey& key) const {
    // RID{} 是 {-1, -1}，在 (key, rid) 全序里等价于 -inf，因此下降会落到
    // 该键的第一条记录所在的叶子。
    const RID min_rid;
    page_id_t leaf_pid = FindLeafPage(key, min_rid);
    if (leaf_pid < 0) return RID();

    PageGuard g = PageGuard::Fetch(bpm_, leaf_pid);
    if (!g.Valid()) return RID();
    std::vector<LeafEntry> entries;
    if (!ReadLeafEntries(g.Data(), key_schema_, &entries)) return RID();
    for (const auto& e : entries) {
        int c = CompareKeyOnly(e.key, key);
        if (c == 0) return e.rid;
        if (c > 0) break;
    }
    // 边界情形：目标键恰好全部落在后继叶子上
    const page_id_t next = GetNextLeaf(g.Data());
    if (next < 0) return RID();
    g.Release();
    PageGuard g2 = PageGuard::Fetch(bpm_, next);
    if (!g2.Valid()) return RID();
    std::vector<LeafEntry> next_entries;
    if (!ReadLeafEntries(g2.Data(), key_schema_, &next_entries)) return RID();
    for (const auto& e : next_entries) {
        int c = CompareKeyOnly(e.key, key);
        if (c == 0) return e.rid;
        if (c > 0) break;
    }
    return RID();
}

bool BPlusTree::Delete(const IndexKey& key, const RID& rid) {
    // =====================================================================
    // 1) Path-stack descent：边下降边记下 (parent_pid, separator_index)，
    //    到达叶子后即可定位叶子的「父亲-我-索引」。
    //    - 根没有父亲，用 INVALID_PAGE_ID 标记，sep_index 取 0 仅占位。
    //    - 走 first_child 时该层 separator_index = 0（first_child 视为
    //      child[0]，其「左分隔键」是 sentinel）；走 entries[i].child 时
    //      separator_index = i + 1，因为 entries[i] 是该孩子与左侧兄弟的
    //      分隔键。
    // =====================================================================
    struct Frame {
        page_id_t parent_pid;
        size_t separator_index;
    };
    std::vector<Frame> path;
    path.reserve(8);

    page_id_t pid = root_page_id_;
    while (true) {
        PageGuard node = PageGuard::Fetch(bpm_, pid);
        if (!node.Valid()) return false;
        const char* d = node.Data();
        const PageType type = GetPageType(d);
        if (type == PageType::kLeaf) {
            node.Release();
            break;
        }
        if (type != PageType::kInternal) return false;
        std::vector<InternalEntry> entries;
        if (!ReadInternalEntries(d, key_schema_, &entries)) return false;
        // 找到要去的 child 索引：0 = first_child，i+1 = entries[i].child
        size_t idx = 0;
        page_id_t child_pid = GetFirstChild(d);
        for (size_t i = 0; i < entries.size(); ++i) {
            if (CompareKeyThenRid(key, rid, entries[i].key, entries[i].rid) >= 0) {
                ++idx;
                child_pid = entries[i].child;
            } else {
                break;
            }
        }
        path.push_back(Frame{pid, idx});
        node.Release();
        pid = child_pid;
    }

    // =====================================================================
    // 2) 在叶子层做实际删除。重复键跨页时最多往后看一页，沿用旧 Delete 的语义。
    // =====================================================================
    bool deleted = false;
    size_t after_count = 0;
    page_id_t erased_leaf = INVALID_PAGE_ID;
    {
        page_id_t leaf_pid = pid;
        for (int attempt = 0; attempt < 2 && leaf_pid >= 0; ++attempt) {
            PageGuard g = PageGuard::Fetch(bpm_, leaf_pid);
            if (!g.Valid()) return false;
            char* d = g.Data();
            std::vector<LeafEntry> entries;
            if (!ReadLeafEntries(d, key_schema_, &entries)) return false;
            for (size_t i = 0; i < entries.size(); ++i) {
                if (CompareKeyOnly(entries[i].key, key) == 0 &&
                    entries[i].rid == rid) {
                    // Phase A：写之前抓叶子整页 before-image。
                    if (active_txn_ != nullptr && active_txn_->IsActive()) {
                        active_txn_->AppendUndo(leaf_pid, d, PAGE_SIZE,
                                                "BPlusTree::Delete(leaf)");
                    }
                    // Phase B：抓叶子 before-image 给 WAL。
                    std::vector<char> leaf_before;
                    if (log_manager_ != nullptr) {
                        leaf_before.assign(d, d + PAGE_SIZE);
                    }
                    entries.erase(entries.begin() + static_cast<long>(i));
                    if (!WriteLeafEntries(d, entries, GetNextLeaf(d), GetPrevLeaf(d))) {
                        return false;
                    }
                    g.MarkDirty();
                    after_count = entries.size();
                    erased_leaf = leaf_pid;
                    // Phase B：写 UPDATE 记录。
                    if (log_manager_ != nullptr) {
                        lsn_t lsn = EmitPageImageRecord(log_manager_, leaf_pid,
                                                       leaf_before.data(), d,
                                                       active_txn_);
                        g.SetPageLsn(lsn);
                    }
                    deleted = true;
                    break;
                }
            }
            if (deleted) break;
            leaf_pid = GetNextLeaf(d);
        }
    }
    if (!deleted) return false;

    // =====================================================================
    // 3) 自底向上修 underflow。仅当叶子的 key_count 跌破阈值才需要处理，
    //    否则说明删得还不够狠，无需无谓改动兄弟。
    // =====================================================================
    if (after_count >= static_cast<size_t>(kLeafMinOccupancy)) {
        // 路径上叶子仍是健康的，无需 redistribute/merge，直接返回
        return true;
    }

    // 叶子是根（path 为空）时无需处理——根无最小占用
    if (path.empty()) {
        // 即使叶子空了也允许：root_page_id 保持有效，下次插入会重新激活
        return true;
    }

    // 从 path 末尾往上修。栈顶就是叶子的直接父亲。
    // 每处理一层，要检查该层的父亲（也就是上一层 frame 的 parent）是否也
    // 因为合并/借出而 underflow；若是，则再向上走一级。
    size_t level = path.size() - 1;
    while (true) {
        const Frame& frame = path[level];
        if (!RedistributeOrMerge(frame.parent_pid, frame.separator_index)) {
            // 修复失败：保守地返回 false，调用方按未删除处理（数据一致性不会破坏）
            return false;
        }
        // 处理到根或顶层：尝试塌缩
        if (frame.parent_pid == root_page_id_ || level == 0) {
            (void)CollapseRoot();
            break;
        }
        // 检查父（上一层 frame.parent_pid）是否因我们刚才的合并而 underflow。
        // 若 underflow，则 frame.parent_pid 仍为 deficient，让上一层处理它。
        const page_id_t upper_parent = path[level - 1].parent_pid;
        {
            PageGuard pg = PageGuard::Fetch(bpm_, upper_parent);
            if (!pg.Valid()) {
                // 拿不到父：保守返回 false
                return false;
            }
            const char* ud = pg.Data();
            if (GetPageType(ud) == PageType::kInternal) {
                const int cnt = GetKeyCount(ud);
                if (cnt < kInternalMinOccupancy) {
                    --level;
                    continue;
                }
            }
            // 父未 underflow，但它的孩子减少了，仍可能在 root 一层需要塌缩
        }
        (void)CollapseRoot();
        break;
    }
    return true;
}

// ============================================================================
// 删除再平衡辅助
// ============================================================================

bool BPlusTree::RedistributeOrMerge(page_id_t parent_pid, size_t separator_index) {
    if (parent_pid < 0 || parent_pid == root_page_id_) {
        // parent 是根：无需做，但若根塌缩条件满足则 CollapseRoot 会处理
        return true;
    }

    PageGuard parent = PageGuard::Fetch(bpm_, parent_pid);
    if (!parent.Valid()) return false;
    char* pd = parent.Data();
    if (GetPageType(pd) != PageType::kInternal) {
        // 父亲不是内部节点（根是叶子）—— 不需要 underflow 修复
        return true;
    }
    std::vector<InternalEntry> p_entries;
    if (!ReadInternalEntries(pd, key_schema_, &p_entries)) return false;
    const page_id_t p_first_child = GetFirstChild(pd);

    // 定位 deficient 与其兄弟。
    // separator_index = 0 表示 deficient = first_child；separator_index > 0
    // 表示 deficient = entries[separator_index - 1].child。
    page_id_t deficient_pid;
    page_id_t left_pid = INVALID_PAGE_ID;
    page_id_t right_pid = INVALID_PAGE_ID;
    if (separator_index == 0) {
        deficient_pid = p_first_child;
        if (p_entries.empty()) {
            // 内部节点只有一个孩子：所有 underflow 都该走 CollapseRoot
            return true;
        }
        right_pid = p_entries[0].child;
    } else {
        deficient_pid = p_entries[separator_index - 1].child;
        if (separator_index - 1 > 0) {
            left_pid = p_entries[separator_index - 2].child;
        } else {
            left_pid = p_first_child;
        }
        if (separator_index < p_entries.size()) {
            right_pid = p_entries[separator_index].child;
        }
    }

    // 选择兄弟：优先有富余的那个；都没有则合并
    PageType deficient_type = PageType::kUninitialized;
    {
        PageGuard dg = PageGuard::Fetch(bpm_, deficient_pid);
        if (!dg.Valid()) return false;
        deficient_type = GetPageType(dg.Data());
    }

    // 计算 deficient 当前 key_count，决定是否真的需要修
    int deficient_count = 0;
    int min_occ = (deficient_type == PageType::kLeaf) ? kLeafMinOccupancy
                                                      : kInternalMinOccupancy;
    {
        PageGuard dg = PageGuard::Fetch(bpm_, deficient_pid);
        if (!dg.Valid()) return false;
        deficient_count = GetKeyCount(dg.Data());
    }
    if (deficient_count >= min_occ) {
        // 不知为何走到这一步（上层估计失误），啥都不做
        return true;
    }

    // 先看右兄弟能否借出
    auto can_borrow = [&](page_id_t sib_pid) -> bool {
        if (sib_pid < 0) return false;
        PageGuard sg = PageGuard::Fetch(bpm_, sib_pid);
        if (!sg.Valid()) return false;
        if (GetPageType(sg.Data()) != deficient_type) return false;
        const int cnt = GetKeyCount(sg.Data());
        return cnt > min_occ;
    };

    bool tried_left = false;
    bool tried_right = false;
    if (can_borrow(right_pid)) {
        if (deficient_type == PageType::kLeaf) {
            return RedistributeLeaf(deficient_pid, right_pid,
                                    /*sibling_is_left=*/false,
                                    parent_pid, separator_index);
        } else {
            return RedistributeInternal(deficient_pid, right_pid,
                                        /*sibling_is_left=*/false,
                                        parent_pid, separator_index);
        }
    }
    tried_right = true;
    (void)tried_right;
    if (can_borrow(left_pid)) {
        tried_left = true;
        if (deficient_type == PageType::kLeaf) {
            return RedistributeLeaf(deficient_pid, left_pid,
                                    /*sibling_is_left=*/true,
                                    parent_pid, separator_index);
        } else {
            return RedistributeInternal(deficient_pid, left_pid,
                                        /*sibling_is_left=*/true,
                                        parent_pid, separator_index);
        }
    }
    (void)tried_left;

    // 兄弟都没有富余：合并。优先 deficient + right；缺右就 left + deficient。
    page_id_t merge_left, merge_right;
    bool merging_into_left = true;  // true 表示最终结果写到 left_pid
    if (right_pid >= 0) {
        merge_left = deficient_pid;
        merge_right = right_pid;
        merging_into_left = true;
    } else if (left_pid >= 0) {
        merge_left = left_pid;
        merge_right = deficient_pid;
        merging_into_left = false;
    } else {
        // 不该发生：deficient 既没有左兄弟也没有右兄弟
        return false;
    }

    // 先把两条 sibling 链上涉及的页抓牢，避免合并后还有页要写
    const IndexKey* sep_key_ptr = nullptr;
    const RID* sep_rid_ptr = nullptr;
    const std::vector<char>* sep_kb_ptr = nullptr;
    IndexKey sep_key;
    RID sep_rid;
    std::vector<char> sep_kb;
    if (deficient_type == PageType::kInternal) {
        // 计算 parent 中分隔 merge_left 与 merge_right 的条目，作为合并分隔键。
        size_t sep_idx;
        if (merging_into_left) {
            sep_idx = separator_index;
        } else {
            sep_idx = separator_index - 1;
        }
        if (sep_idx >= p_entries.size()) return false;
        sep_key = p_entries[sep_idx].key;
        sep_rid = p_entries[sep_idx].rid;
        sep_kb = p_entries[sep_idx].key_bytes;
        sep_key_ptr = &sep_key;
        sep_rid_ptr = &sep_rid;
        sep_kb_ptr = &sep_kb;
    }
    if (!MergeNodes(merge_left, merge_right, deficient_type,
                    sep_key_ptr, sep_rid_ptr, sep_kb_ptr)) {
        return false;
    }

    // 对叶子还要修 next/prev 链：merge_left 已在 MergeNodes 内把 next 设为
    // merge_right 的 next；还需要把 merge_right.next 的 prev 指回 merge_left。
    if (deficient_type == PageType::kLeaf) {
        page_id_t right_next = INVALID_PAGE_ID;
        {
            // 此刻 merge_right 仍在缓冲池（DeletePage 还没调），可以读
            PageGuard rg = PageGuard::Fetch(bpm_, merge_right);
            if (rg.Valid()) right_next = GetNextLeaf(rg.Data());
        }
        if (right_next >= 0) {
            PageGuard ng = PageGuard::Fetch(bpm_, right_next);
            if (ng.Valid()) {
                char* nd = ng.Data();
                if (active_txn_ != nullptr && active_txn_->IsActive()) {
                    active_txn_->AppendUndo(right_next, nd, PAGE_SIZE,
                                            "BPlusTree::MergeNodes(leaf-next.prev)");
                }
                std::vector<char> before;
                if (log_manager_ != nullptr) before.assign(nd, nd + PAGE_SIZE);
                SetPrevLeaf(nd, merge_left);
                ng.MarkDirty();
                if (log_manager_ != nullptr) {
                    lsn_t lsn = EmitPageImageRecord(log_manager_, right_next,
                                                   before.data(), nd, active_txn_);
                    ng.SetPageLsn(lsn);
                }
            }
        }
    }

    // 从 parent 中删除合并掉的那个分隔键
    std::vector<char> parent_before;
    if (log_manager_ != nullptr) {
        parent_before.assign(pd, pd + PAGE_SIZE);
    }
    if (active_txn_ != nullptr && active_txn_->IsActive()) {
        active_txn_->AppendUndo(parent_pid, pd, PAGE_SIZE,
                                "BPlusTree::MergeNodes(parent-erase-sep)");
    }

    size_t erase_idx = static_cast<size_t>(-1);
    if (merging_into_left) {
        // 把 right 并入 left：删除 separator_index（分隔 left 与 right 的键）
        erase_idx = separator_index;
    } else {
        // 把 right=deficient 并入 left=sibling：删除 separator_index - 1
        erase_idx = separator_index - 1;
    }
    if (erase_idx >= p_entries.size()) return false;
    p_entries.erase(p_entries.begin() + static_cast<long>(erase_idx));
    if (!WriteInternalEntries(pd, p_first_child, p_entries)) return false;
    parent.MarkDirty();
    if (log_manager_ != nullptr) {
        lsn_t lsn = EmitPageImageRecord(log_manager_, parent_pid,
                                       parent_before.data(), pd, active_txn_);
        parent.SetPageLsn(lsn);
    }
    parent.Release();

    // 释放被合并掉的页
    bpm_->DeletePage(merge_right);

    // 现在 parent 可能也 underflow 了；调用方（Delete 的循环）会继续向上处理
    return true;
}

bool BPlusTree::MergeNodes(page_id_t left_pid, page_id_t right_pid,
                          PageType /*type*/, const IndexKey* parent_sep_key,
                          const RID* parent_sep_rid,
                          const std::vector<char>* parent_sep_key_bytes) {
    // Phase B：抓 before-image
    // 合并的写入只动 left 这一页（结果在 left）。若 left 与 right 都是叶子，
    // 我们需要分别抓两页的 before-image（因为 WAL 是整页更新）。
    PageGuard left = PageGuard::Fetch(bpm_, left_pid);
    if (!left.Valid()) return false;
    PageGuard right = PageGuard::Fetch(bpm_, right_pid);
    if (!right.Valid()) return false;
    char* ld = left.Data();
    char* rd = right.Data();

    // Phase A：写之前抓 before-image
    if (active_txn_ != nullptr && active_txn_->IsActive()) {
        active_txn_->AppendUndo(left_pid, ld, PAGE_SIZE, "BPlusTree::MergeNodes(left)");
        active_txn_->AppendUndo(right_pid, rd, PAGE_SIZE, "BPlusTree::MergeNodes(right)");
    }

    std::vector<char> left_before;
    std::vector<char> right_before;
    if (log_manager_ != nullptr) {
        left_before.assign(ld, ld + PAGE_SIZE);
        right_before.assign(rd, rd + PAGE_SIZE);
    }

    bool ok = false;
    if (GetPageType(ld) == PageType::kLeaf && GetPageType(rd) == PageType::kLeaf) {
        std::vector<LeafEntry> le, re;
        if (!ReadLeafEntries(ld, key_schema_, &le)) return false;
        if (!ReadLeafEntries(rd, key_schema_, &re)) return false;
        le.insert(le.end(), re.begin(), re.end());
        if (le.size() > static_cast<size_t>(kMaxLeafSlots)) return false;
        // 合并后 left 的 next = right.next，prev = left.prev（left 占据原位）
        if (!WriteLeafEntries(ld, le, GetNextLeaf(rd), GetPrevLeaf(ld))) return false;
        ok = true;
    } else if (GetPageType(ld) == PageType::kInternal &&
               GetPageType(rd) == PageType::kInternal) {
        // 内部节点合并：parent_sep（parent 中分隔 left 与 right 的那条 entry）
        // 必须出现在合并结果中，其 child = right.first_child，正好充当
        // left 最后一项与 right 第一项之间的分隔键。
        if (parent_sep_key == nullptr || parent_sep_rid == nullptr) return false;
        std::vector<InternalEntry> le, re;
        if (!ReadInternalEntries(ld, key_schema_, &le)) return false;
        if (!ReadInternalEntries(rd, key_schema_, &re)) return false;
        const page_id_t left_first = GetFirstChild(ld);
        if (le.size() + 1 + re.size() > static_cast<size_t>(kMaxInternalSlots)) {
            return false;
        }
        InternalEntry sep;
        sep.key = *parent_sep_key;
        sep.rid = *parent_sep_rid;
        sep.child = GetFirstChild(rd);
        if (parent_sep_key_bytes != nullptr && !parent_sep_key_bytes->empty()) {
            sep.key_bytes = *parent_sep_key_bytes;
        } else {
            // parent_sep_key_bytes 缺失：调用方大概率忘了传；这里用父分隔键的
            // 序列化补救，避免写入时缺 key_bytes 导致读取解析失败。
            sep.key_bytes = SerializeKey(sep.key, key_schema_);
        }
        le.push_back(std::move(sep));
        for (auto& r : re) {
            le.push_back(std::move(r));
        }
        if (!WriteInternalEntries(ld, left_first, le)) return false;
        ok = true;
    } else {
        return false;  // 类型不一致：损坏
    }
    if (!ok) return false;

    left.MarkDirty();
    if (log_manager_ != nullptr) {
        lsn_t lsn_l = EmitPageImageRecord(log_manager_, left_pid,
                                         left_before.data(), ld, active_txn_);
        left.SetPageLsn(lsn_l);
        // right 也写一条 UPDATE（之后 DeletePage，但 redo 时若走 right 也无害）
        lsn_t lsn_r = EmitPageImageRecord(log_manager_, right_pid,
                                         right_before.data(), rd, active_txn_);
        right.SetPageLsn(lsn_r);
    }
    return true;
}

bool BPlusTree::RedistributeLeaf(page_id_t deficient_leaf, page_id_t sibling,
                                bool sibling_is_left, page_id_t parent_pid,
                                size_t separator_index) {
    PageGuard dg = PageGuard::Fetch(bpm_, deficient_leaf);
    if (!dg.Valid()) return false;
    PageGuard sg = PageGuard::Fetch(bpm_, sibling);
    if (!sg.Valid()) return false;
    PageGuard pg = PageGuard::Fetch(bpm_, parent_pid);
    if (!pg.Valid()) return false;
    char* dd = dg.Data();
    char* sd = sg.Data();
    char* pd = pg.Data();

    if (active_txn_ != nullptr && active_txn_->IsActive()) {
        active_txn_->AppendUndo(deficient_leaf, dd, PAGE_SIZE,
                                "BPlusTree::RedistributeLeaf(deficient)");
        active_txn_->AppendUndo(sibling, sd, PAGE_SIZE,
                                "BPlusTree::RedistributeLeaf(sibling)");
        active_txn_->AppendUndo(parent_pid, pd, PAGE_SIZE,
                                "BPlusTree::RedistributeLeaf(parent)");
    }
    std::vector<char> dbefore, sbefore, pbefore;
    if (log_manager_ != nullptr) {
        dbefore.assign(dd, dd + PAGE_SIZE);
        sbefore.assign(sd, sd + PAGE_SIZE);
        pbefore.assign(pd, pd + PAGE_SIZE);
    }

    std::vector<LeafEntry> de, se;
    if (!ReadLeafEntries(dd, key_schema_, &de)) return false;
    if (!ReadLeafEntries(sd, key_schema_, &se)) return false;
    if (se.empty()) return false;
    std::vector<InternalEntry> pe;
    if (!ReadInternalEntries(pd, key_schema_, &pe)) return false;
    const page_id_t p_first = GetFirstChild(pd);

    // 找 parent 中分隔 deficient 与 sibling 的 entry
    size_t sep_in_parent;
    if (sibling_is_left) {
        if (separator_index == 0) return false;
        sep_in_parent = separator_index - 1;
    } else {
        sep_in_parent = separator_index;
    }
    if (sep_in_parent >= pe.size()) return false;

    if (sibling_is_left) {
        // 把 sibling 最后一条搬到 deficient 的最前
        LeafEntry moved = std::move(se.back());
        se.pop_back();
        de.insert(de.begin(), std::move(moved));
        // parent sep 现在指向 deficient；deficient 头部新增了 moved（即 se 的旧
        // 最后一条，键小于原 parent sep？不一定）。
        // ——正确规则：parent sep 应是 sibling.first[0]，因为 sibling 仍然在
        // 左侧，sibling 的第一条 < parent sep <= deficient 的第一条。
        // sibling 仍然拥有 first_child 之外的孩子吗？sibling 失去的是 se.back()
        // 这条 entry 与其 child；新 sibling 是 [first_child, ..., 旧 entries[0..N-2]]
        // 即 children = first_child, entries[0..N-2].child. 损失了最后一个 child。
        // sibling 的新第一条 entry 不变（即 entries[0]），但 parent sep 应改为
        // sibling 新第一条 entry 的 (key, rid)。sibling 新第一条 entry 实际上是
        // 原 entries[0]（即 se.front() 现在的内容）。
        if (se.empty()) return false;
        pe[sep_in_parent].key = se.front().key;
        pe[sep_in_parent].rid = se.front().rid;
        pe[sep_in_parent].key_bytes = se.front().key_bytes;
    } else {
        // 把 sibling 第一条搬到 deficient 的最后
        LeafEntry moved = std::move(se.front());
        se.erase(se.begin());
        de.push_back(std::move(moved));
        // parent sep 现在指向 sibling 的第一条 = 原 sibling 第二条；
        // ——但 sibling 是右兄弟，parent sep 指向 deficient 还是 sibling？
        // 答：separator 是分隔 deficient（左）与 sibling（右）的键，应该让
        //     deficient 的第一条成为新 sep——deficient 获得 moved（来自
        //     sibling），但 moved 比 sibling 的旧第一条小，所以 deficient 的
        //     新第一条就是 moved。
        if (de.empty()) return false;
        pe[sep_in_parent].key = de.front().key;
        pe[sep_in_parent].rid = de.front().rid;
        pe[sep_in_parent].key_bytes = de.front().key_bytes;
    }

    // 保留 deficient 与 sibling 当前的 next/prev 链不变。
    if (!WriteLeafEntries(dd, de, GetNextLeaf(dd), GetPrevLeaf(dd))) return false;
    if (!WriteLeafEntries(sd, se, GetNextLeaf(sd), GetPrevLeaf(sd))) return false;
    if (!WriteInternalEntries(pd, p_first, pe)) return false;
    dg.MarkDirty();
    sg.MarkDirty();
    pg.MarkDirty();

    if (log_manager_ != nullptr) {
        lsn_t dl = EmitPageImageRecord(log_manager_, deficient_leaf,
                                      dbefore.data(), dd, active_txn_);
        dg.SetPageLsn(dl);
        lsn_t sl = EmitPageImageRecord(log_manager_, sibling,
                                      sbefore.data(), sd, active_txn_);
        sg.SetPageLsn(sl);
        lsn_t pl = EmitPageImageRecord(log_manager_, parent_pid,
                                      pbefore.data(), pd, active_txn_);
        pg.SetPageLsn(pl);
    }
    return true;
}

bool BPlusTree::RedistributeInternal(page_id_t deficient_internal, page_id_t sibling,
                                    bool sibling_is_left, page_id_t parent_pid,
                                    size_t separator_index) {
    PageGuard dg = PageGuard::Fetch(bpm_, deficient_internal);
    if (!dg.Valid()) return false;
    PageGuard sg = PageGuard::Fetch(bpm_, sibling);
    if (!sg.Valid()) return false;
    PageGuard pg = PageGuard::Fetch(bpm_, parent_pid);
    if (!pg.Valid()) return false;
    char* dd = dg.Data();
    char* sd = sg.Data();
    char* pd = pg.Data();

    if (active_txn_ != nullptr && active_txn_->IsActive()) {
        active_txn_->AppendUndo(deficient_internal, dd, PAGE_SIZE,
                                "BPlusTree::RedistributeInternal(deficient)");
        active_txn_->AppendUndo(sibling, sd, PAGE_SIZE,
                                "BPlusTree::RedistributeInternal(sibling)");
        active_txn_->AppendUndo(parent_pid, pd, PAGE_SIZE,
                                "BPlusTree::RedistributeInternal(parent)");
    }
    std::vector<char> dbefore, sbefore, pbefore;
    if (log_manager_ != nullptr) {
        dbefore.assign(dd, dd + PAGE_SIZE);
        sbefore.assign(sd, sd + PAGE_SIZE);
        pbefore.assign(pd, pd + PAGE_SIZE);
    }

    std::vector<InternalEntry> de, se;
    if (!ReadInternalEntries(dd, key_schema_, &de)) return false;
    if (!ReadInternalEntries(sd, key_schema_, &se)) return false;
    if (se.empty()) return false;
    std::vector<InternalEntry> pe;
    if (!ReadInternalEntries(pd, key_schema_, &pe)) return false;
    const page_id_t d_first_old = GetFirstChild(dd);
    const page_id_t s_first = GetFirstChild(sd);
    const page_id_t p_first = GetFirstChild(pd);

    // 找到 parent 中分隔 deficient 与 sibling 的那条 entry。
    //   separator_index = 0 表示 deficient = p_first_child；
    //   separator_index > 0 表示 deficient = pe[separator_index - 1].child。
    //   分隔 deficient 与 sibling 的条目：
    //     sibling 是右 → pe[separator_index]
    //     sibling 是左 → pe[separator_index - 1]
    size_t sep_in_parent;
    if (sibling_is_left) {
        if (separator_index == 0) return false;  // 左兄弟不存在
        sep_in_parent = separator_index - 1;
    } else {
        sep_in_parent = separator_index;
    }
    if (sep_in_parent >= pe.size()) return false;

    // ----- 重组 -----
    //
    // sibling 是左：
    //   取 sibling.entries.last（包含一个 child = promoted_child）。
    //   sibling 失去该 entry 与 promoted_child 这个子节点。
    //   deficient 接纳 promoted_child 作为新的 first_child，并新增 entries[0]
    //     其 (key, rid) 沿用旧 parent separator，child = d_first_old
    //     （即旧 deficient.first_child，现在是新的 deficient.entries[0].child）。
    //   parent.sep_in_parent 升级为 sibling.last 的 (key, rid)，child = promoted_child。
    //
    // sibling 是右：
    //   取 sibling.entries.first（其 child = promoted_child，但 child 仍归
    //     sibling 所有——sibling 只失 entries.first 与 promoted_child 这个子节点？
    //     不对：sibling.entries.first 是分隔 sibling.first_child 与 promoted_child
    //     的键。拿走该 entry 后 sibling 仍拥有 first_child 与 promoted_child
    //     以及它们之间的「空白」（没有 entry）。再 promote promoted_child
    //     移到 deficient 末尾。
    //   deficient.entries 末尾追加新条目：
    //     (key, rid) = 旧 parent sep，child = d_first_old
    //     （即原 deficient.first_child，现在变成新条目.deficient 末尾的 child）。
    //   deficient.first_child 保持不变。
    //   parent.sep_in_parent 升级为 sibling.first 的 (key, rid)，child = d_first_old
    //     （deficient 的 first_child，作为新 parent sep 右侧的孩子）。
    //
    // ——写时按「先记旧值再覆盖」的顺序避免丢数据。
    if (sibling_is_left) {
        const InternalEntry old_parent_sep = pe[sep_in_parent];
        InternalEntry moved = std::move(se.back());
        se.pop_back();
        const page_id_t promoted_child = moved.child;

        // 新 parent sep
        pe[sep_in_parent].key = moved.key;
        pe[sep_in_parent].rid = moved.rid;
        pe[sep_in_parent].key_bytes = std::move(moved.key_bytes);
        pe[sep_in_parent].child = promoted_child;

        // 新 deficient.head = (parent_sep.key/rid, child = d_first_old)
        InternalEntry de_head;
        de_head.key = old_parent_sep.key;
        de_head.rid = old_parent_sep.rid;
        de_head.key_bytes = old_parent_sep.key_bytes;
        de_head.child = d_first_old;
        de.insert(de.begin(), std::move(de_head));

        // 写回：deficient 用 promoted_child 作为新 first_child
        if (!WriteInternalEntries(dd, promoted_child, de)) return false;
        if (!WriteInternalEntries(sd, s_first, se)) return false;
        if (!WriteInternalEntries(pd, p_first, pe)) return false;
    } else {
        const InternalEntry old_parent_sep = pe[sep_in_parent];
        InternalEntry moved = std::move(se.front());
        se.erase(se.begin());
        // moved.child 是 sibling 失去的子节点，但 promoted_child 仍归 sibling 所有
        // （sibling 仍持有 first_child 与 promoted_child，只是它们之间没有 entry）。
        const page_id_t promoted_child = moved.child;

        // 新 parent sep: child = d_first_old（deficient 的 first_child，是新 sep 右侧孩子）
        pe[sep_in_parent].key = moved.key;
        pe[sep_in_parent].rid = moved.rid;
        pe[sep_in_parent].key_bytes = std::move(moved.key_bytes);
        pe[sep_in_parent].child = d_first_old;

        // 新 deficient.tail = (parent_sep.key/rid, child = d_first_old)
        // ——等等，这里 child = d_first_old 就和 parent sep.child 重复。
        // 正确语义：新 deficient.tail 的 child = promoted_child
        // （即从 sibling 搬过来的那个 child，它在 deficient 末尾）。deficient
        // 的 first_child 保持 d_first_old 不变。
        InternalEntry de_tail;
        de_tail.key = old_parent_sep.key;
        de_tail.rid = old_parent_sep.rid;
        de_tail.key_bytes = old_parent_sep.key_bytes;
        de_tail.child = promoted_child;
        de.push_back(std::move(de_tail));

        // 写回：deficient 用 d_first_old（不变）作为 first_child
        if (!WriteInternalEntries(dd, d_first_old, de)) return false;
        if (!WriteInternalEntries(sd, s_first, se)) return false;
        if (!WriteInternalEntries(pd, p_first, pe)) return false;
    }

    dg.MarkDirty();
    sg.MarkDirty();
    pg.MarkDirty();

    if (log_manager_ != nullptr) {
        lsn_t dl = EmitPageImageRecord(log_manager_, deficient_internal,
                                      dbefore.data(), dd, active_txn_);
        dg.SetPageLsn(dl);
        lsn_t sl = EmitPageImageRecord(log_manager_, sibling,
                                      sbefore.data(), sd, active_txn_);
        sg.SetPageLsn(sl);
        lsn_t pl = EmitPageImageRecord(log_manager_, parent_pid,
                                      pbefore.data(), pd, active_txn_);
        pg.SetPageLsn(pl);
    }
    return true;
}

bool BPlusTree::CollapseRoot() {
    PageGuard root = PageGuard::Fetch(bpm_, root_page_id_);
    if (!root.Valid()) return false;
    char* rd = root.Data();
    if (GetPageType(rd) != PageType::kInternal) return false;
    std::vector<InternalEntry> entries;
    if (!ReadInternalEntries(rd, key_schema_, &entries)) return false;
    const page_id_t first_child = GetFirstChild(rd);

    // root 是空内部节点（树被删空）→ 让 root 退化成空叶子，下一次插入能自愈
    if (entries.empty() || first_child < 0) {
        std::vector<char> rbefore;
        if (log_manager_ != nullptr) rbefore.assign(rd, rd + PAGE_SIZE);
        if (active_txn_ != nullptr && active_txn_->IsActive()) {
            active_txn_->AppendUndo(root_page_id_, rd, PAGE_SIZE,
                                    "BPlusTree::CollapseRoot(empty)");
        }
        InitLeaf(rd);
        root.MarkDirty();
        if (log_manager_ != nullptr) {
            lsn_t lsn = EmitPageImageRecord(log_manager_, root_page_id_,
                                           rbefore.data(), rd, active_txn_);
            root.SetPageLsn(lsn);
        }
        return true;
    }

    // 只有一个孩子：把孩子搬进 root
    if (entries.size() == 1 && entries[0].child == first_child) {
        // 上面这个条件其实意味着 root 有「first_child == entries[0].child」
        // 即 first_child 之外还有一个相同的 child 指针。这不应发生。
        return false;
    }
    if (entries.size() != 1) return false;  // 还需两层以上，不塌缩

    // 抓唯一孩子
    PageGuard child = PageGuard::Fetch(bpm_, first_child);
    if (!child.Valid()) return false;
    char* cd = child.Data();
    if (GetPageType(cd) != PageType::kInternal) return false;  // 根是叶子就不塌缩

    // 把 child 的内容复制到 root
    std::vector<char> rbefore, cbefore;
    if (log_manager_ != nullptr) {
        rbefore.assign(rd, rd + PAGE_SIZE);
        cbefore.assign(cd, cd + PAGE_SIZE);
    }
    if (active_txn_ != nullptr && active_txn_->IsActive()) {
        active_txn_->AppendUndo(root_page_id_, rd, PAGE_SIZE,
                                "BPlusTree::CollapseRoot(root)");
        active_txn_->AppendUndo(first_child, cd, PAGE_SIZE,
                                "BPlusTree::CollapseRoot(child)");
    }
    std::memcpy(rd, cd, PAGE_SIZE);
    root.MarkDirty();
    if (log_manager_ != nullptr) {
        lsn_t lsn_r = EmitPageImageRecord(log_manager_, root_page_id_,
                                         rbefore.data(), rd, active_txn_);
        root.SetPageLsn(lsn_r);
        lsn_t lsn_c = EmitPageImageRecord(log_manager_, first_child,
                                         cbefore.data(), cd, active_txn_);
        child.SetPageLsn(lsn_c);
    }
    root.Release();
    child.Release();

    // 释放被搬走的子页
    bpm_->DeletePage(first_child);
    return true;
}

// ============================================================================
// 游标
// ============================================================================

BPlusTree::Cursor::Cursor(const BPlusTree* tree, page_id_t leaf_pid, size_t index)
    : tree_(tree), leaf_pid_(leaf_pid), index_(index) {
    if (leaf_pid_ >= 0) {
        size_t want = index;
        if (!LoadLeaf(leaf_pid_)) {
            entries_.clear();
        }
        index_ = want;
    }
}

bool BPlusTree::Cursor::LoadLeaf(page_id_t pid) {
    entries_.clear();
    index_ = 0;
    next_leaf_ = INVALID_PAGE_ID;
    leaf_pid_ = pid;
    if (tree_ == nullptr || pid < 0) return false;
    PageGuard g = PageGuard::Fetch(tree_->bpm_, pid);
    if (!g.Valid()) return false;
    std::vector<LeafEntry> raw;
    if (!ReadLeafEntries(g.Data(), tree_->key_schema_, &raw)) return false;
    next_leaf_ = GetNextLeaf(g.Data());
    entries_.reserve(raw.size());
    for (auto& e : raw) {
        entries_.emplace_back(std::move(e.key), e.rid);
    }
    return true;
}

bool BPlusTree::Cursor::Next(IndexKey* key, RID* rid) {
    while (true) {
        if (index_ < entries_.size()) {
            if (key != nullptr) *key = entries_[index_].first;
            if (rid != nullptr) *rid = entries_[index_].second;
            ++index_;
            return true;
        }
        if (next_leaf_ < 0) return false;
        if (!LoadLeaf(next_leaf_)) return false;
    }
}

std::unique_ptr<BPlusTree::Cursor> BPlusTree::LowerBound(const IndexKey& key) const {
    const RID min_rid;
    page_id_t leaf_pid = FindLeafPage(key, min_rid);
    if (leaf_pid < 0) return nullptr;

    PageGuard g = PageGuard::Fetch(bpm_, leaf_pid);
    if (!g.Valid()) return nullptr;
    std::vector<LeafEntry> entries;
    if (!ReadLeafEntries(g.Data(), key_schema_, &entries)) return nullptr;
    size_t idx = 0;
    while (idx < entries.size() && CompareKeyOnly(entries[idx].key, key) < 0) {
        ++idx;
    }
    // 若本页所有键都小于目标，游标停在页尾，Next() 会自动跨到下一页
    return std::make_unique<Cursor>(this, leaf_pid, idx);
}

std::unique_ptr<BPlusTree::Cursor> BPlusTree::Begin() const {
    page_id_t pid = LeftmostLeafPage();
    if (pid < 0) return nullptr;
    return std::make_unique<Cursor>(this, pid, 0);
}

}  // namespace sqlcompiler
