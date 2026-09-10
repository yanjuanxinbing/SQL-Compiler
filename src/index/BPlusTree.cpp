#include "index/BPlusTree.h"

#include "index/BPlusTreePage.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace sqlcompiler {

namespace {

using namespace bptree;

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
    return true;
}

bool BPlusTree::SplitChild(page_id_t parent_pid, page_id_t child_pid,
                           size_t reserve) {
    PageGuard parent = PageGuard::Fetch(bpm_, parent_pid);
    if (!parent.Valid()) return false;
    std::vector<InternalEntry> parent_entries;
    if (!ReadInternalEntries(parent.Data(), key_schema_, &parent_entries)) return false;
    const page_id_t parent_first_child = GetFirstChild(parent.Data());

    PageGuard child = PageGuard::Fetch(bpm_, child_pid);
    if (!child.Valid()) return false;
    char* cd = child.Data();

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
    (void)reserve;
    return true;
}

bool BPlusTree::SplitRoot() {
    PageGuard root = PageGuard::Fetch(bpm_, root_page_id_);
    if (!root.Valid()) return false;
    char* rd = root.Data();
    const PageType type = GetPageType(rd);

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

    std::vector<InternalEntry> root_entries{std::move(sep)};
    if (!WriteInternalEntries(rd, moved_pid, root_entries)) return false;
    root.MarkDirty();
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
    page_id_t leaf_pid = FindLeafPage(key, rid);
    if (leaf_pid < 0) return false;

    // 目标可能落在相邻叶子（重复键跨页），最多向后看一页
    for (int attempt = 0; attempt < 2 && leaf_pid >= 0; ++attempt) {
        PageGuard g = PageGuard::Fetch(bpm_, leaf_pid);
        if (!g.Valid()) return false;
        char* d = g.Data();
        std::vector<LeafEntry> entries;
        if (!ReadLeafEntries(d, key_schema_, &entries)) return false;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (CompareKeyOnly(entries[i].key, key) == 0 &&
                entries[i].rid == rid) {
                entries.erase(entries.begin() + static_cast<long>(i));
                if (!WriteLeafEntries(d, entries, GetNextLeaf(d), GetPrevLeaf(d))) {
                    return false;
                }
                g.MarkDirty();
                return true;
            }
        }
        leaf_pid = GetNextLeaf(d);
    }
    return false;
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
