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

// ---- Phase 1：乐观并发版本号（页头 offset 12 保留字段） ----
// 每个页内容被修改（整页重写或邻居指针改写）时版本号自增一次。读路径记录
// 遍历路径上各页版本号，结束后校验未变则认为遍历期间结构未被扰动。
// 版本号只用于进程内并发控制，单调递增杜绝 ABA；不改页格式，落盘/恢复不受影响。
uint32_t ReadVersion(const char* d) {
    return static_cast<uint32_t>(ReadI32(d, 12));
}
void BumpVersion(char* d) {
    WriteI32(d, 12, ReadI32(d, 12) + 1);
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
// 版本号保留并自增：InitLeaf 会把页清零，先取出版本号再写回（乐观并发用）。
bool WriteLeafEntries(char* d, const std::vector<LeafEntry>& entries,
                      page_id_t next_leaf, page_id_t prev_leaf) {
    if (entries.size() > static_cast<size_t>(kMaxLeafSlots)) return false;
    if (LeafBytesNeeded(entries) > PAGE_SIZE) return false;

    const uint32_t version = ReadVersion(d);
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
    WriteI32(d, 12, static_cast<int32_t>(version + 1));
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

    const uint32_t version = ReadVersion(d);
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
    WriteI32(d, 12, static_cast<int32_t>(version + 1));
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

// 叶子页中是否已存在相同 key 的条目（唯一性检查）。墓碑条目已从
// ReadLeafEntries 过滤，因此不参与唯一性判定（与旧实现 FindFirst 语义一致）。
bool KeyExistsInLeaf(const char* d, const std::vector<ValueType>& schema,
                     const IndexKey& key) {
    std::vector<LeafEntry> entries;
    if (!ReadLeafEntries(d, schema, &entries)) return false;
    for (const auto& e : entries) {
        int c = CompareKeyOnly(e.key, key);
        if (c == 0) return true;
        if (c > 0) break;
    }
    return false;
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
// 乐观下降（Phase 1）
//
// 读路径统一采用「共享闩逐页下降 + 版本号校验 + 乐观重启」：
//   - 每个节点只在读取的瞬间持共享闩（作用域结束即释放），记录其版本号后下降；
//   - 遍历结束后 ValidatePath 复查路径上每页版本号，任一变化说明遍历期间有
//     结构改动（分裂/改写），本次结果作废、重来；
//   - 因此读不阻塞写（读闩与写闩互斥但都极短），写不阻塞读（冲突靠重启消化），
//     无等待环，与事务死锁检测正交。
// ============================================================================

page_id_t BPlusTree::OptimisticFindLeafPage(const IndexKey& key, const RID& rid,
                                            std::vector<PathEntry>* path) const {
    page_id_t pid = root_page_id_;
    std::unordered_set<page_id_t> visited;
    for (int steps = 0; steps < 128; ++steps) {
        if (!visited.insert(pid).second) return INVALID_PAGE_ID;  // 环路防御
        PageReadGuard g = PageReadGuard::Fetch(bpm_, pid);
        if (!g.Valid()) return INVALID_PAGE_ID;
        const char* d = g.Data();
        path->push_back(PathEntry{pid, ReadVersion(d)});
        if (GetPageType(d) == PageType::kLeaf) return pid;
        std::vector<InternalEntry> entries;
        if (!ReadInternalEntries(d, key_schema_, &entries)) return INVALID_PAGE_ID;
        pid = ChooseChild(d, entries, key, rid);
        // g 在此作用域结束（下一轮 Fetch 之前）释放页锁并 Unpin——遵守锁序。
    }
    return INVALID_PAGE_ID;
}

page_id_t BPlusTree::OptimisticLeftmostLeafPage(std::vector<PathEntry>* path) const {
    page_id_t pid = root_page_id_;
    std::unordered_set<page_id_t> visited;
    for (int steps = 0; steps < 128; ++steps) {
        if (!visited.insert(pid).second) return INVALID_PAGE_ID;
        PageReadGuard g = PageReadGuard::Fetch(bpm_, pid);
        if (!g.Valid()) return INVALID_PAGE_ID;
        const char* d = g.Data();
        path->push_back(PathEntry{pid, ReadVersion(d)});
        if (GetPageType(d) == PageType::kLeaf) return pid;
        if (!IsValidHeader(d, PageType::kInternal)) return INVALID_PAGE_ID;
        pid = GetFirstChild(d);
    }
    return INVALID_PAGE_ID;
}

bool BPlusTree::ValidatePath(const std::vector<PathEntry>& path) const {
    for (const auto& e : path) {
        PageReadGuard g = PageReadGuard::Fetch(bpm_, e.pid);
        if (!g.Valid()) return false;
        if (ReadVersion(g.Data()) != e.version) return false;
    }
    return true;
}

// ============================================================================
// 插入：乐观快路径 + 预分裂慢路径（Phase 1）
//
// 快路径（常见情况，叶子放得下）：
//   乐观下降记录路径版本 → 校验路径 → 只对目标叶子取独占写闩 → 复核叶版本 →
//   唯一性检查 → 插入。不同键空间落在不同叶子，可完全并行。
// 慢路径（叶子放不下）：
//   沿用「下降途中预分裂」算法：自底向上分裂需要回溯父节点，一旦父节点也满就
//   要级联向上，还要处理「根分裂」特例——三者纠缠极易写错；预分裂拉直成单向
//   下降，进入某个节点前先保证它装得下，叶子插入永不失败、无级联。代价是节点
//   略早分裂、填充率稍低。慢路径带写闩下降、分裂点以「先 pin 后加闩」原子发布。
// ============================================================================

bool BPlusTree::Insert(const IndexKey& key, const RID& rid) {
    std::vector<char> key_bytes = SerializeKey(key, key_schema_);
    if (key_bytes.size() > kMaxKeyBytes) return false;

    // 预留量按「本次要插入的键」计算，而不是按最大可能键长，
    // 这样定长小键（如 INT 主键）仍能接近填满页面。
    const size_t reserve = key_bytes.size();

    // ---- 快路径：乐观下降 + 校验后只锁目标叶子 ----
    const int kMaxAttempts = 256;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        std::vector<PathEntry> path;
        page_id_t leaf_pid = OptimisticFindLeafPage(key, rid, &path);
        if (leaf_pid < 0) return false;
        // 校验路径版本：下降期间任何结构改动都会使某页版本变化 → 重启
        if (!ValidatePath(path)) continue;

        PageWriteGuard leaf = PageWriteGuard::Fetch(bpm_, leaf_pid);
        if (!leaf.Valid()) continue;
        // 取到独占闩后再核一次叶版本：校验与加闩之间叶子可能已被并发改写
        if (ReadVersion(leaf.Data()) != path.back().version) continue;

        if (MayOverflow(leaf.Data(), reserve)) {
            // 叶子放不下 → 慢路径（带写闩下降、沿途预分裂）
            leaf.Release();
            return InsertSlowPath(key, rid, key_bytes, reserve);
        }
        if (is_unique_ && KeyExistsInLeaf(leaf.Data(), key_schema_, key)) {
            leaf.Release();
            return false;
        }
        if (!InsertIntoLeaf(&leaf, key, rid, std::vector<char>(key_bytes))) {
            leaf.Release();
            return false;
        }
        leaf.MarkDirty();
        return true;
    }
    // 极端重启风暴（不应发生）：放弃本次插入，与旧实现的步数兜底语义一致。
    return false;
}

bool BPlusTree::InsertSlowPath(const IndexKey& key, const RID& rid,
                               const std::vector<char>& key_bytes, size_t reserve) {
    // 慢路径整体可重试：分裂途中若发现结构已被并发改写（SplitChild 返回 2），
    // 从根重新下降。重试上限兜底，正常情况一次即中。
    const int kMaxRestarts = 32;
    for (int restart = 0; restart < kMaxRestarts; ++restart) {
        // ---- 1) 根：装不下就先分裂。根 id 不变，因此无需回写目录元数据 ----
        {
            PageWriteGuard root = PageWriteGuard::Fetch(bpm_, root_page_id_);
            if (!root.Valid()) return false;
            char* rd = root.Data();
            if (GetPageType(rd) == PageType::kUninitialized) {
                InitLeaf(rd);  // 自愈：全零页当作空叶子
                BumpVersion(rd);
                root.MarkDirty();
            }
            const bool overflow = MayOverflow(rd, reserve);
            root.Release();  // SplitRoot 内部自行加闩（先 pin 后加闩）
            if (overflow && !SplitRoot(reserve)) return false;
        }

        // ---- 2) 下降，遇到装不下的孩子就地分裂 ----
        page_id_t pid = root_page_id_;
        // 慢路径「父闩 → 子闩」切换间的并发防护（两层，缺一不可）：
        //   a) 父节点回读复核：下降决策基于父节点旧状态（写闩下 ChooseChild）。
        //      释放父写闩后，父节点可能被并发分裂，分隔键更新后键的归属叶子右移。
        //      若只靠子页版本复核，当子页恰在窗口期被分裂、读到的已是「分裂后的
        //      左半页」版本时无法察觉——键被插进错误的叶子，破坏「分隔键 ↔ 叶子
        //      内容」一致性（键落错叶子、导航找不到）。因此读子页后要回读父节点
        //      版本，变化即整体重启下降。
        //   b) 子页版本复核：取到子写闩后核对读闩时记录的版本，覆盖「父回读之后
        //      到子加闩之前」子页再被分裂的窗口。
        uint32_t expected_child_ver = 0;
        bool have_child_ver = false;
        int guard_steps = 0;
        while (true) {
            // 每层最多重试一次（分裂后重选孩子），步数上限兜底防御损坏页导致的死循环
            if (++guard_steps > 128) return false;

            PageWriteGuard node = PageWriteGuard::Fetch(bpm_, pid);
            if (!node.Valid()) return false;
            if (have_child_ver && ReadVersion(node.Data()) != expected_child_ver) {
                node.Release();  // 子页在下降窗口期被并发改写 → 整体重启
                break;
            }
            char* d = node.Data();
            if (GetPageType(d) == PageType::kLeaf) {
                // ---- 3) 叶子插入。由预分裂不变式保证一定装得下 ----
                if (is_unique_ && KeyExistsInLeaf(d, key_schema_, key)) {
                    node.Release();
                    return false;
                }
                if (!InsertIntoLeaf(&node, key, rid, std::vector<char>(key_bytes))) {
                    node.Release();
                    return false;
                }
                node.MarkDirty();
                return true;
            }

            std::vector<InternalEntry> entries;
            if (!ReadInternalEntries(d, key_schema_, &entries)) return false;
            const page_id_t child_pid = ChooseChild(d, entries, key, rid);
            const uint32_t parent_ver = ReadVersion(d);  // 记录父版本，供回读复核
            const page_id_t parent_pid = pid;
            node.Release();  // 释放父闩后才能取子页（持页闩期间禁止请求 BPM）
            if (child_pid < 0) return false;

            bool overflow;
            uint32_t child_ver = 0;
            {
                PageReadGuard child = PageReadGuard::Fetch(bpm_, child_pid);
                if (!child.Valid()) return false;
                overflow = MayOverflow(child.Data(), reserve);
                child_ver = ReadVersion(child.Data());
            }

            // 父节点回读复核（见上 a）：下降窗口期内父分隔键若被并发分裂更新，
            // child_pid 可能已不再是 key 的归属叶子 → 整体重启下降。
            {
                PageReadGuard parent_check = PageReadGuard::Fetch(bpm_, parent_pid);
                if (!parent_check.Valid()) return false;
                if (ReadVersion(parent_check.Data()) != parent_ver) break;
            }

            if (overflow) {
                const int r = SplitChild(pid, child_pid, reserve);
                if (r == 0) return false;   // 硬失败
                if (r == 2) break;          // 结构已变 → 整体重启
                continue;                   // 分裂成功 → 重选孩子
            }
            pid = child_pid;
            expected_child_ver = child_ver;  // 下一轮取子写闩后复核
            have_child_ver = true;
        }
    }
    // 极端重启风暴（不应发生）：放弃本次插入，与旧实现的步数兜底语义一致。
    return false;
}

bool BPlusTree::InsertIntoLeaf(PageWriteGuard* leaf, const IndexKey& key,
                               const RID& rid, std::vector<char>&& key_bytes) {
    char* d = leaf->Data();
    const page_id_t pid = leaf->PageId();
    if (!IsValidHeader(d, PageType::kLeaf)) {
        InitLeaf(d);  // 自愈：全零页当作空叶子
        BumpVersion(d);
        leaf->MarkDirty();
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
    // WriteLeafEntries 已保留版本号并自增。

    // Phase B：写 UPDATE 记录（leaf 整页 before/after）。
    if (log_manager_ != nullptr) {
        lsn_t lsn = EmitPageImageRecord(log_manager_, pid,
                                        leaf_before.data(), d, active_txn_);
        leaf->SetPageLsn(lsn);
    }
    return true;
}

// 返回值语义见头文件：1=成功，0=硬失败，2=结构已变（调用方重启慢路径）。
int BPlusTree::SplitChild(page_id_t parent_pid, page_id_t child_pid,
                          size_t reserve) {
    // ---- 1) 先 pin（不持闩）。持闩阶段严禁调用 BPM，因此所有可能需要访问的页
    //         （parent、child、可能的 next 叶）都在此一次性 pin 住。 ----
    PageGuard parent_pin = PageGuard::Fetch(bpm_, parent_pid);
    if (!parent_pin.Valid()) return 0;
    PageGuard child_pin = PageGuard::Fetch(bpm_, child_pid);
    if (!child_pin.Valid()) return 0;
    PageGuard sib = PageGuard::New(bpm_);
    if (!sib.Valid()) return 0;  // 缓冲池耗尽
    const page_id_t sib_pid = sib.PageId();

    // 探测 child 的类型与 next 指针，决定是否需要 pin 后继叶（prev 回链用）。
    // 这只是「要 pin 哪些页」的提示；真正的内容一律在写闩下复读。
    bool child_is_leaf = false;
    page_id_t next_pid = INVALID_PAGE_ID;
    {
        PageReadGuard probe = PageReadGuard::LatchPinned(child_pin.GetPagePtr(),
                                                         child_pid);
        if (!probe.Valid()) return 0;
        child_is_leaf = (GetPageType(probe.Data()) == PageType::kLeaf);
        if (child_is_leaf) next_pid = GetNextLeaf(probe.Data());
    }  // probe 析构只释放页锁，不 Unpin（pin 归 child_pin）

    PageGuard next_pin;
    if (child_is_leaf && next_pid >= 0) {
        next_pin = PageGuard::Fetch(bpm_, next_pid);
        if (!next_pin.Valid()) return 0;
    }

    // ---- 2) 再按固定顺序加写闩：parent → child → next。该顺序全树唯一，
    //          其余路径单页持闩，不可能与这里形成等待环。 ----
    PageWriteGuard parent = PageWriteGuard::LatchPinned(parent_pin.GetPagePtr(),
                                                        parent_pid);
    if (!parent.Valid()) return 0;
    PageWriteGuard child = PageWriteGuard::LatchPinned(child_pin.GetPagePtr(),
                                                       child_pid);
    if (!child.Valid()) return 0;
    PageWriteGuard nxt;  // 可选：prev 回链用
    if (next_pin.Valid()) {
        nxt = PageWriteGuard::LatchPinned(next_pin.GetPagePtr(), next_pid);
        if (!nxt.Valid()) return 0;
    }

    char* cd = child.Data();

    // Phase B：抓 child 整页 before-image（写闩下捕获，保证与 after 严格对应）。
    std::vector<char> child_before;
    if (log_manager_ != nullptr) {
        child_before.assign(cd, cd + PAGE_SIZE);
    }

    // 复读 parent 内容（pin 与加闩之间可能被并发改写），并校验 child 仍是
    // parent 的直接孩子——并发分裂可能把 child 挪到 parent 的新兄弟下，
    // 此时不得在此分裂，返回 2 让调用方整体重启下降。
    std::vector<InternalEntry> parent_entries;
    if (!ReadInternalEntries(parent.Data(), key_schema_, &parent_entries)) return 0;
    const page_id_t parent_first_child = GetFirstChild(parent.Data());
    bool child_under_parent = (parent_first_child == child_pid);
    for (const auto& e : parent_entries) {
        if (e.child == child_pid) {
            child_under_parent = true;
            break;
        }
    }
    if (!child_under_parent) {
        // 结构已被并发改写：先释放所有页闩与多余 pin，回收误分配的新页，
        // 再返回 2 让调用方整体重启下降。
        parent.Release();
        child.Release();
        if (nxt.Valid()) nxt.Release();
        sib.Release();
        if (next_pin.Valid()) next_pin.Release();
        bpm_->DeletePage(sib_pid);
        return 2;
    }

    // Phase B：抓 parent 整页 before-image。
    std::vector<char> parent_before;
    if (log_manager_ != nullptr) {
        parent_before.assign(parent.Data(), parent.Data() + PAGE_SIZE);
    }

    InternalEntry sep;
    sep.child = sib_pid;

    const bool is_leaf = (GetPageType(cd) == PageType::kLeaf);
    if (is_leaf) {
        std::vector<LeafEntry> entries;
        if (!ReadLeafEntries(cd, key_schema_, &entries)) return 0;
        if (entries.size() < 2) return 0;
        const size_t mid = entries.size() / 2;
        std::vector<LeafEntry> left(entries.begin(), entries.begin() + mid);
        std::vector<LeafEntry> right(entries.begin() + mid, entries.end());

        const page_id_t old_next = GetNextLeaf(cd);
        const page_id_t old_prev = GetPrevLeaf(cd);
        if (!WriteLeafEntries(sib.Data(), right, old_next, child_pid)) return 0;
        sib.MarkDirty();
        if (!WriteLeafEntries(cd, left, sib_pid, old_prev)) return 0;
        child.MarkDirty();
        // 后继叶 prev 回链：仅当探测到的 next 仍是当前 next 时才更新（并发分裂
        // 可能已改写 child 的 next，探测值过期）。prev 不用于导航，过期即跳过，
        // 绝不影响正确性；写它必须持锁并自增版本号（与整页重写一致的发布规则）。
        if (old_next >= 0 && old_next == next_pid && nxt.Valid() &&
            IsValidHeader(nxt.Data(), PageType::kLeaf)) {
            SetPrevLeaf(nxt.Data(), sib_pid);
            BumpVersion(nxt.Data());
            nxt.MarkDirty();
        }
        // 分隔键取右页第一条的完整 (key, rid)
        sep.key = right.front().key;
        sep.rid = right.front().rid;
        sep.key_bytes = right.front().key_bytes;
    } else {
        std::vector<InternalEntry> entries;
        if (!ReadInternalEntries(cd, key_schema_, &entries)) return 0;
        if (entries.size() < 2) return 0;
        const page_id_t first_child = GetFirstChild(cd);
        const size_t mid = entries.size() / 2;
        // 内部节点分裂：中间键上推到父节点，不在两个孩子里保留
        InternalEntry up = entries[mid];
        std::vector<InternalEntry> left(entries.begin(), entries.begin() + mid);
        std::vector<InternalEntry> right(entries.begin() + mid + 1, entries.end());

        if (!WriteInternalEntries(sib.Data(), up.child, right)) return 0;
        sib.MarkDirty();
        if (!WriteInternalEntries(cd, first_child, left)) return 0;
        child.MarkDirty();

        sep.key = up.key;
        sep.rid = up.rid;
        sep.key_bytes = up.key_bytes;
    }

    // Phase B：先写 child 和 sib 的 UPDATE 记录（sibling 是全新页，before 视为全零）。
    if (log_manager_ != nullptr) {
        lsn_t child_lsn = EmitPageImageRecord(log_manager_, child_pid,
                                              child_before.data(), cd, active_txn_);
        child.SetPageLsn(child_lsn);
        std::vector<char> zero_before(PAGE_SIZE, 0);
        lsn_t sib_lsn = EmitPageImageRecord(log_manager_, sib_pid,
                                            zero_before.data(), sib.Data(),
                                            active_txn_);
        sib.SetPageLsn(sib_lsn);
    }

    // 父、子写闩在整个分裂期间同时持有：读者要么看到旧结构（无新分隔键），
    // 要么看到新结构（分隔键已就位），绝不会看到「子已分裂、父还没有分隔键」
    // 的中间态导致丢数据。
    auto pos = std::lower_bound(
        parent_entries.begin(), parent_entries.end(), sep,
        [](const InternalEntry& a, const InternalEntry& b) {
            return CompareKeyThenRid(a.key, a.rid, b.key, b.rid) < 0;
        });
    parent_entries.insert(pos, std::move(sep));

    // 前置条件保证这里必然写得下；写不下说明不变式被破坏，宁可失败也不静默截断
    if (!WriteInternalEntries(parent.Data(), parent_first_child, parent_entries)) {
        return 0;
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
    return 1;
}

bool BPlusTree::SplitRoot(size_t reserve) {
    // 先 pin 根（不持闩）。持闩期间不得调用 BPM，因此新页与可能的后继叶
    // 都先 pin 好。
    PageGuard root_pin = PageGuard::Fetch(bpm_, root_page_id_);
    if (!root_pin.Valid()) return false;

    // 探测根的类型与 next 指针，决定是否需要 pin 后继叶（根为叶子时的 prev 回链）。
    bool root_is_leaf = false;
    page_id_t next_pid = INVALID_PAGE_ID;
    {
        PageReadGuard probe = PageReadGuard::LatchPinned(root_pin.GetPagePtr(),
                                                         root_page_id_);
        if (!probe.Valid()) return false;
        root_is_leaf = (GetPageType(probe.Data()) == PageType::kLeaf);
        if (root_is_leaf) next_pid = GetNextLeaf(probe.Data());
    }
    PageGuard next_pin;
    if (root_is_leaf && next_pid >= 0) {
        next_pin = PageGuard::Fetch(bpm_, next_pid);
        if (!next_pin.Valid()) return false;
    }

    // 加根写闩，复读内容
    PageWriteGuard root = PageWriteGuard::LatchPinned(root_pin.GetPagePtr(),
                                                       root_page_id_);
    if (!root.Valid()) return false;
    char* rd = root.Data();
    if (GetPageType(rd) == PageType::kUninitialized) {
        InitLeaf(rd);  // 自愈：全零页当作空叶子
        BumpVersion(rd);
        root.MarkDirty();
    }
    // 并发已把根分裂/改写，根不再溢出 → 无需分裂，直接放行（调用方继续下降）
    if (!MayOverflow(rd, reserve)) {
        root.Release();
        return true;
    }

    // 需要分裂：先把闩还给根，再分配两个新页（持闩期间不得请求 BPM）。
    root.Release();
    PageGuard moved = PageGuard::New(bpm_);
    if (!moved.Valid()) return false;
    const page_id_t moved_pid = moved.PageId();
    PageGuard sib = PageGuard::New(bpm_);
    if (!sib.Valid()) return false;
    const page_id_t sib_pid = sib.PageId();

    // 重新加闩并重读内容（分配期间根可能又被并发分裂）
    PageWriteGuard root2 = PageWriteGuard::LatchPinned(root_pin.GetPagePtr(),
                                                       root_page_id_);
    if (!root2.Valid()) return false;
    rd = root2.Data();
    if (GetPageType(rd) != PageType::kUninitialized && !MayOverflow(rd, reserve)) {
        // 根已被并发分裂（现在是内部节点且放得下）：归还误分配的两个新页。
        root2.Release();  // 此刻无闩，可安全调用 BPM
        moved.Release();
        sib.Release();
        bpm_->DeletePage(moved_pid);
        bpm_->DeletePage(sib_pid);
        return true;
    }
    const PageType type = GetPageType(rd);

    // Phase B：抓 root 整页 before-image。SplitRoot 之后 root 变成新的内部节点，
    // before-image 用于回放（万一 redo 时 root 已不是当时状态也能重建）。
    std::vector<char> root_before;
    if (log_manager_ != nullptr) {
        root_before.assign(rd, rd + PAGE_SIZE);
    }

    // 把根的全部内容搬到一个新页，根页本身改写成新的内部节点
    std::memcpy(moved.Data(), rd, PAGE_SIZE);
    moved.MarkDirty();

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
        // 后继叶 prev 回链：仅在探测到的 next 仍是当前 next 时更新（与 SplitChild
        // 同一规则：prev 不用于导航，过期即跳过）。
        if (old_next >= 0 && old_next == next_pid && next_pin.Valid()) {
            PageWriteGuard nxt = PageWriteGuard::LatchPinned(next_pin.GetPagePtr(),
                                                             next_pid);
            if (nxt.Valid() && IsValidHeader(nxt.Data(), PageType::kLeaf)) {
                SetPrevLeaf(nxt.Data(), sib_pid);
                BumpVersion(nxt.Data());
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
    root2.MarkDirty();

    // Phase B：写 root 的 UPDATE 记录（root 变成新内部节点，before/after 都捕获）。
    if (log_manager_ != nullptr) {
        lsn_t root_lsn = EmitPageImageRecord(log_manager_, root_page_id_,
                                             root_before.data(), rd, active_txn_);
        root2.SetPageLsn(root_lsn);
    }
    return true;
}

// ============================================================================
// 查找与删除
// ============================================================================

RID BPlusTree::FindFirst(const IndexKey& key) const {
    // 乐观点查：乐观下降记录路径版本 → 校验 → 读闩复核叶版本 → 在叶内定位。
    // 任一版本变化即整体重启；读不阻塞写、写不阻塞读（冲突靠重启消化）。
    const int kMaxAttempts = 256;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        // RID{} 是 {-1, -1}，在 (key, rid) 全序里等价于 -inf，因此下降会落到
        // 该键的第一条记录所在的叶子。
        std::vector<PathEntry> path;
        const RID min_rid;
        page_id_t leaf_pid = OptimisticFindLeafPage(key, min_rid, &path);
        if (leaf_pid < 0) return RID();
        if (!ValidatePath(path)) continue;

        PageReadGuard g = PageReadGuard::Fetch(bpm_, leaf_pid);
        if (!g.Valid()) continue;
        if (ReadVersion(g.Data()) != path.back().version) continue;
        std::vector<LeafEntry> entries;
        if (!ReadLeafEntries(g.Data(), key_schema_, &entries)) return RID();
        for (const auto& e : entries) {
            int c = CompareKeyOnly(e.key, key);
            if (c == 0) return e.rid;
            if (c > 0) break;
        }
        // 边界情形：目标键恰好全部落在后继叶子上
        const page_id_t next = GetNextLeaf(g.Data());
        g.Release();
        if (next < 0) return RID();
        PageReadGuard g2 = PageReadGuard::Fetch(bpm_, next);
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
    return RID();
}

bool BPlusTree::Delete(const IndexKey& key, const RID& rid) {
    // 乐观删除：下降路径版本校验通过后，只对目标叶子取独占写闩（写闩下复核
    // 叶版本），重复键可能跨页则最多向后看一页。任一版本变化即整体重启。
    const int kMaxAttempts = 256;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        std::vector<PathEntry> path;
        page_id_t leaf_pid = OptimisticFindLeafPage(key, rid, &path);
        if (leaf_pid < 0) return false;
        if (!ValidatePath(path)) continue;

        // 目标可能落在相邻叶子（重复键跨页），最多向后看一页
        for (int step = 0; step < 2 && leaf_pid >= 0; ++step) {
            PageWriteGuard g = PageWriteGuard::Fetch(bpm_, leaf_pid);
            if (!g.Valid()) continue;
            // 首叶须与下降路径版本一致；后继叶持写闩下读取即一致，无需版本复核
            if (step == 0 && ReadVersion(g.Data()) != path.back().version) continue;
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
                    // Phase B：写 UPDATE 记录。
                    if (log_manager_ != nullptr) {
                        lsn_t lsn = EmitPageImageRecord(log_manager_, leaf_pid,
                                                       leaf_before.data(), d,
                                                       active_txn_);
                        g.SetPageLsn(lsn);
                    }
                    return true;
                }
            }
            const page_id_t next = GetNextLeaf(d);
            g.Release();
            leaf_pid = next;
        }
        return false;
    }
    return false;
}

// Phase 3（t4）：低频索引真空。遍历所有叶子，用 is_dead 谓词逐条判定索引项是否
// 可回收，可回收则从叶子里物理删除（整页重写 + 版本号自增）。
//
// 并发/一致性设计：
//   - 单叶处理 = 持叶写闩下完成「判定 + 重写」。判定期间 is_dead 会顺带回表取行页
//     短读闩，不会与写路径死锁：执行器写堆（持行页写闩）与写索引（持叶写闩）是
//     顺序调用、从不同时持两种闩；本函数持叶写闩 + 行页读闩与既有锁序无环。
//   - 判据单调：is_dead 的 kRemove 判据（TableHeap::DecideIndexEntry）只依赖
//     end_csn / 头键等一经提交即不可变的信息，判定为死即恒死，无需二次复核。
//   - best-effort 遍历：沿 next_leaf 推进前读指针、放锁再取下一页（同 Delete）；
//     并发分裂导致跳页/重读时靠 visited 防环，遗漏留给下一趟。删条目不合并节点。
uint64_t BPlusTree::Vacuum(const std::function<bool(const RID&, const IndexKey&)>& is_dead) {
    if (!is_dead) return 0;
    uint64_t removed = 0;
    // 定位最左叶子（乐观下降，版本校验失败重试有限次后放弃本趟）。
    page_id_t leaf_pid = INVALID_PAGE_ID;
    for (int attempt = 0; attempt < 16; ++attempt) {
        std::vector<PathEntry> path;
        leaf_pid = OptimisticLeftmostLeafPage(&path);
        if (leaf_pid < 0) return removed;
        if (ValidatePath(path)) break;
    }
    if (leaf_pid < 0) return removed;

    std::unordered_set<page_id_t> visited;
    while (leaf_pid >= 0) {
        if (!visited.insert(leaf_pid).second) break;  // 环保护
        PageWriteGuard g = PageWriteGuard::Fetch(bpm_, leaf_pid);
        if (!g.Valid()) break;
        char* d = g.Data();
        std::vector<LeafEntry> entries;
        if (!ReadLeafEntries(d, key_schema_, &entries)) break;
        std::vector<LeafEntry> keep;
        keep.reserve(entries.size());
        bool changed = false;
        for (const auto& e : entries) {
            if (is_dead(e.rid, e.key)) {
                ++removed;
                changed = true;
            } else {
                keep.push_back(e);
            }
        }
        if (changed) {
            // keep 是 entries 的子集，空间必够（放不下说明页损坏，放弃本页）。
            if (!WriteLeafEntries(d, keep, GetNextLeaf(d), GetPrevLeaf(d))) break;
            g.MarkDirty();
        }
        leaf_pid = GetNextLeaf(d);
    }
    return removed;
}

// ============================================================================
// 游标
// ============================================================================

BPlusTree::Cursor::Cursor(const BPlusTree* tree, page_id_t leaf_pid,
                          const IndexKey* lower_key)
    : tree_(tree), leaf_pid_(leaf_pid) {
    if (leaf_pid_ < 0 || !LoadLeaf(leaf_pid_)) {
        entries_.clear();
        next_leaf_ = INVALID_PAGE_ID;
        leaf_version_ = 0;
        return;
    }
    has_last_ = false;
    index_ = 0;
    // lower_key == nullptr 表示从页首开始（Begin）；否则定位到第一个 >= key 的条目
    if (lower_key != nullptr) {
        while (index_ < entries_.size() &&
               CompareKeyOnly(entries_[index_].first, *lower_key) < 0) {
            ++index_;
        }
    }
}

bool BPlusTree::Cursor::LoadLeaf(page_id_t pid) {
    entries_.clear();
    index_ = 0;
    next_leaf_ = INVALID_PAGE_ID;
    leaf_pid_ = pid;
    leaf_version_ = 0;
    if (tree_ == nullptr || pid < 0) return false;
    // 读闩下装载：内容与版本号、next 指针同一次读取，天然一致
    PageReadGuard g = PageReadGuard::Fetch(tree_->bpm_, pid);
    if (!g.Valid()) return false;
    std::vector<LeafEntry> raw;
    if (!ReadLeafEntries(g.Data(), tree_->key_schema_, &raw)) return false;
    leaf_version_ = ReadVersion(g.Data());
    next_leaf_ = GetNextLeaf(g.Data());
    entries_.reserve(raw.size());
    for (auto& e : raw) {
        entries_.emplace_back(std::move(e.key), e.rid);
    }
    return true;
}

bool BPlusTree::Cursor::CurrentLeafUnchanged() const {
    if (tree_ == nullptr || leaf_pid_ < 0) return false;
    PageReadGuard g = PageReadGuard::Fetch(tree_->bpm_, leaf_pid_);
    if (!g.Valid()) return false;
    return ReadVersion(g.Data()) == leaf_version_;
}

bool BPlusTree::Cursor::ReloadCurrentLeaf() {
    if (!LoadLeaf(leaf_pid_)) return false;
    // 重新定位到最后一个已发出条目之后（严格大于），避免重复/遗漏
    index_ = entries_.size();
    if (has_last_) {
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (CompareKeyThenRid(entries_[i].first, entries_[i].second,
                                  last_key_, last_rid_) > 0) {
                index_ = i;
                break;
            }
        }
    }
    return true;
}

bool BPlusTree::Cursor::Next(IndexKey* key, RID* rid) {
    while (true) {
        if (index_ < entries_.size()) {
            if (key != nullptr) *key = entries_[index_].first;
            if (rid != nullptr) *rid = entries_[index_].second;
            ++index_;
            // 记住最近发出的条目：叶子被并发改写后据此重新定位
            has_last_ = true;
            last_key_ = entries_[index_ - 1].first;
            last_rid_ = entries_[index_ - 1].second;
            return true;
        }
        // 当前叶条目发完：跨叶前进前先校验版本。并发分裂会改写叶子的 next 指针，
        // 必须先据此刷新扫描位置，避免跳过新分裂出的右半页或重复发出条目。
        if (!CurrentLeafUnchanged()) {
            if (!ReloadCurrentLeaf()) return false;
            if (index_ < entries_.size()) continue;
        }
        if (next_leaf_ < 0) return false;
        if (!LoadLeaf(next_leaf_)) return false;
    }
}

std::unique_ptr<BPlusTree::Cursor> BPlusTree::LowerBound(const IndexKey& key) const {
    // 乐观定位：乐观下降 + 路径版本校验，构造游标时读闩装载叶子并复核版本。
    const int kMaxAttempts = 256;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        std::vector<PathEntry> path;
        const RID min_rid;
        page_id_t leaf_pid = OptimisticFindLeafPage(key, min_rid, &path);
        if (leaf_pid < 0) return nullptr;
        if (!ValidatePath(path)) continue;

        auto c = std::make_unique<Cursor>(this, leaf_pid, &key);
        // 游标装载的叶版本必须与下降路径末端一致，否则下降结果已过期 → 重启
        if (c->GetLeafVersion() != path.back().version) continue;
        return c;
    }
    return nullptr;
}

std::unique_ptr<BPlusTree::Cursor> BPlusTree::Begin() const {
    const int kMaxAttempts = 256;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        std::vector<PathEntry> path;
        page_id_t pid = OptimisticLeftmostLeafPage(&path);
        if (pid < 0) return nullptr;
        if (!ValidatePath(path)) continue;

        auto c = std::make_unique<Cursor>(this, pid, nullptr);
        if (c->GetLeafVersion() != path.back().version) continue;
        return c;
    }
    return nullptr;
}

}  // namespace sqlcompiler
