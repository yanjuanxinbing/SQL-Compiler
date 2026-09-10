// 页式存储子系统测试：覆盖 Page、LRU/FIFO 替换策略、DiskManager
// 页级读写与页号回收复用、BufferPoolManager 命中统计与淘汰写回。
#include "test_framework.h"

#include <cstring>
#include <filesystem>
#include <fstream>

#include "storage/BufferPoolManager.h"
#include "storage/DiskManager.h"
#include "storage/FIFOReplacer.h"
#include "storage/LRUReplacer.h"
#include "storage/Page.h"
#include "storage/Replacer.h"

using namespace sqlcompiler;

namespace {

constexpr size_t kPageSize = PAGE_SIZE;

// 创建一个空白数据文件并返回路径（测试结束由调用方删除）
std::string MakeTempDbFile(const std::string& name) {
    std::filesystem::remove(name);
    std::ofstream f(name, std::ios::binary | std::ios::trunc);
    f.close();
    return name;
}

// ---- Page ----

void TestPageBasics() {
    Page p;
    CHECK_EQ(p.GetPageId(), INVALID_PAGE_ID);
    CHECK_EQ(p.GetPinCount(), 0);
    CHECK(!p.IsDirty());
    CHECK(p.GetData() != nullptr);

    p.SetPageId(7);
    CHECK_EQ(p.GetPageId(), 7);
    p.IncPinCount();
    CHECK_EQ(p.GetPinCount(), 1);
    p.IncPinCount();
    CHECK_EQ(p.GetPinCount(), 2);
    p.DecPinCount();
    CHECK_EQ(p.GetPinCount(), 1);
    p.SetDirty(true);
    CHECK(p.IsDirty());

    // ResetMemory 应清零数据并复位脏标记
    p.GetData()[0] = 'A';
    p.ResetMemory();
    CHECK_EQ(p.GetData()[0], '\0');
    CHECK(!p.IsDirty());
}

// ---- 替换策略 ----

void TestReplacerContract() {
    // LRU 与 FIFO 共同满足的基本契约
    std::unique_ptr<Replacer> replacers[2];
    replacers[0] = std::make_unique<LRUReplacer>(8);
    replacers[1] = std::make_unique<FIFOReplacer>(8);

    for (auto& r : replacers) {
        CHECK_EQ(r->Size(), static_cast<size_t>(0));
        int frame = -1;
        CHECK(!r->Victim(&frame));  // 空替换器无可淘汰帧

        r->Unpin(0);
        r->Unpin(1);
        CHECK_EQ(r->Size(), static_cast<size_t>(2));

        // 两个候选中，最旧的 0 应先被淘汰
        CHECK(r->Victim(&frame));
        CHECK_EQ(frame, 0);
        CHECK_EQ(r->Size(), static_cast<size_t>(1));
        CHECK(r->Victim(&frame));
        CHECK_EQ(frame, 1);
        CHECK(!r->Victim(&frame));
    }
}

void TestReplacerPinRemovesCandidate() {
    LRUReplacer lru(4);
    lru.Unpin(3);
    lru.Pin(3);  // 重新 pin 后不再是淘汰候选
    CHECK_EQ(lru.Size(), static_cast<size_t>(0));
    int frame = -1;
    CHECK(!lru.Victim(&frame));

    FIFOReplacer fifo(4);
    fifo.Unpin(5);
    fifo.Pin(5);
    CHECK_EQ(fifo.Size(), static_cast<size_t>(0));
    CHECK(!fifo.Victim(&frame));
}

void TestLRUOrder() {
    LRUReplacer lru(8);
    lru.Unpin(0);
    lru.Unpin(1);
    lru.Unpin(2);
    // 访问 0 使其变为最近使用，随后 1 成为最久未用
    lru.Pin(0);
    lru.Unpin(0);
    int frame = -1;
    CHECK(lru.Victim(&frame));
    CHECK_EQ(frame, 1);
}

// ---- DiskManager ----

void TestDiskManagerAllocateAndReuse() {
    std::string file = MakeTempDbFile("test_dm_alloc.db");
    {
        DiskManager dm(file);
        page_id_t a = dm.AllocatePage();
        page_id_t b = dm.AllocatePage();
        CHECK_EQ(b, a + 1);              // 全新页号递增
        CHECK(dm.GetNumPages() >= 2);

        dm.DeallocatePage(a);            // 回收后应被复用
        page_id_t c = dm.AllocatePage();
        CHECK_EQ(c, a);
    }
    std::filesystem::remove(file);
}

void TestDiskManagerReadWriteRoundtrip() {
    std::string file = MakeTempDbFile("test_dm_rw.db");
    {
        DiskManager dm(file);
        page_id_t pid = dm.AllocatePage();

        std::vector<char> wbuf(kPageSize);
        for (size_t i = 0; i < kPageSize; ++i) wbuf[i] = static_cast<char>(i % 251);
        dm.WritePage(pid, wbuf.data());

        std::vector<char> rbuf(kPageSize, '\0');
        dm.ReadPage(pid, rbuf.data());
        CHECK(std::memcmp(wbuf.data(), rbuf.data(), kPageSize) == 0);
    }
    std::filesystem::remove(file);
}

void TestDiskManagerPageIsolation() {
    std::string file = MakeTempDbFile("test_dm_iso.db");
    {
        DiskManager dm(file);
        page_id_t p1 = dm.AllocatePage();
        page_id_t p2 = dm.AllocatePage();

        std::vector<char> wbuf(kPageSize, 'X');
        dm.WritePage(p1, wbuf.data());

        std::vector<char> rbuf(kPageSize, '\0');
        dm.ReadPage(p2, rbuf.data());
        // 未写入过的页不应读到另一页的数据
        bool all_zero = true;
        for (char c : rbuf) if (c != '\0') { all_zero = false; break; }
        CHECK(all_zero);
    }
    std::filesystem::remove(file);
}

// ---- BufferPoolManager ----

void TestBufferPoolNewPageAndGetPage() {
    std::string file = MakeTempDbFile("test_bpm_basic.db");
    {
        DiskManager dm(file);
        BufferPoolManager bpm(4, &dm);

        page_id_t pid = INVALID_PAGE_ID;
        Page* p = bpm.NewPage(&pid);
        CHECK(p != nullptr);
        CHECK(pid >= 0);
        if (p) {
            p->GetData()[0] = 'Z';
            p->GetData()[1] = 'Q';
        }
        CHECK(bpm.UnpinPage(pid, true));

        // 仍在池内：GetPage 命中，数据保留
        Page* q = bpm.GetPage(pid);
        CHECK(q != nullptr);
        if (q) {
            CHECK_EQ(q->GetPageId(), pid);
            CHECK_EQ(q->GetData()[0], 'Z');
        }
        CHECK(bpm.UnpinPage(pid, false));

        const BufferPoolStats& stats = bpm.GetStats();
        CHECK(stats.hit_count >= 1);
        CHECK(stats.miss_count >= 1);
        CHECK(stats.HitRate() > 0.0 && stats.HitRate() <= 1.0);
    }
    std::filesystem::remove(file);
}

void TestBufferPoolFlushPageToDisk() {
    std::string file = MakeTempDbFile("test_bpm_flush.db");
    {
        DiskManager dm(file);
        BufferPoolManager bpm(4, &dm);

        page_id_t pid = INVALID_PAGE_ID;
        Page* p = bpm.NewPage(&pid);
        CHECK(p != nullptr);
        for (size_t i = 0; i < 16; ++i) p->GetData()[i] = static_cast<char>('A' + i);
        bpm.UnpinPage(pid, true);
        CHECK(bpm.FlushPage(pid));

        // 绕过缓冲池直接从磁盘读，验证已落盘
        std::vector<char> disk_buf(kPageSize, '\0');
        dm.ReadPage(pid, disk_buf.data());
        CHECK_EQ(disk_buf[0], 'A');
        CHECK_EQ(disk_buf[15], 'P');
    }
    std::filesystem::remove(file);
}

void TestBufferPoolEvictionWritesBackDirtyPage() {
    std::string file = MakeTempDbFile("test_bpm_evict.db");
    {
        DiskManager dm(file);
        // 池容量 2：放入 3 个页必然触发淘汰
        BufferPoolManager bpm(2, &dm, ReplacementPolicy::LRU);

        page_id_t pid1, pid2, pid3;
        Page* p1 = bpm.NewPage(&pid1);
        CHECK(p1 != nullptr);
        p1->GetData()[0] = '1';
        bpm.UnpinPage(pid1, true);  // 脏页

        Page* p2 = bpm.NewPage(&pid2);
        CHECK(p2 != nullptr);
        p2->GetData()[0] = '2';
        bpm.UnpinPage(pid2, true);

        Page* p3 = bpm.NewPage(&pid3);
        CHECK(p3 != nullptr);  // 此时应已发生淘汰
        bpm.UnpinPage(pid3, false);

        // 替换日志应记录淘汰事件
        CHECK(!bpm.GetReplacementLog().empty());
        CHECK(bpm.GetStats().replacement_count >= 1);

        // 重新加载被淘汰的脏页：数据应已写回磁盘
        Page* reloaded = bpm.GetPage(pid1);
        CHECK(reloaded != nullptr);
        if (reloaded) CHECK_EQ(reloaded->GetData()[0], '1');
    }
    std::filesystem::remove(file);
}

void TestBufferPoolAllPinnedNewPageFails() {
    std::string file = MakeTempDbFile("test_bpm_pinned.db");
    {
        DiskManager dm(file);
        BufferPoolManager bpm(2, &dm);

        page_id_t pid1, pid2;
        CHECK(bpm.NewPage(&pid1) != nullptr);
        CHECK(bpm.NewPage(&pid2) != nullptr);
        // 两页均保持 pin 状态（未 Unpin），新页分配应失败
        page_id_t pid3;
        CHECK(bpm.NewPage(&pid3) == nullptr);
    }
    std::filesystem::remove(file);
}

void TestBufferPoolDeletePage() {
    std::string file = MakeTempDbFile("test_bpm_delete.db");
    {
        DiskManager dm(file);
        BufferPoolManager bpm(4, &dm);

        page_id_t pid = INVALID_PAGE_ID;
        CHECK(bpm.NewPage(&pid) != nullptr);
        bpm.UnpinPage(pid, false);
        CHECK(bpm.DeletePage(pid));
        CHECK(!bpm.DeletePage(9999));  // 不存在的页
    }
    std::filesystem::remove(file);
}

void TestBufferPoolFlushAllPages() {
    std::string file = MakeTempDbFile("test_bpm_flushall.db");
    {
        DiskManager dm(file);
        {
            BufferPoolManager bpm(4, &dm);
            for (int i = 0; i < 3; ++i) {
                page_id_t pid;
                Page* p = bpm.NewPage(&pid);
                if (p) p->GetData()[0] = static_cast<char>('0' + i);
                bpm.UnpinPage(pid, true);
            }
            bpm.FlushAllPages();
        }
        // 重新打开缓冲池，从磁盘验证三页内容
        BufferPoolManager bpm2(4, &dm);
        for (char expect = '0'; expect < '3'; ++expect) {
            page_id_t pid = expect - '0';
            Page* p = bpm2.GetPage(pid);
            CHECK(p != nullptr);
            if (p) CHECK_EQ(p->GetData()[0], expect);
            bpm2.UnpinPage(pid, false);
        }
    }
    std::filesystem::remove(file);
}

}  // namespace

int main() {
    testfw::Run("存储: Page基础操作", TestPageBasics);
    testfw::Run("存储: 替换策略基本契约", TestReplacerContract);
    testfw::Run("存储: Pin移出淘汰候选", TestReplacerPinRemovesCandidate);
    testfw::Run("存储: LRU淘汰顺序", TestLRUOrder);
    testfw::Run("存储: 页号分配与回收复用", TestDiskManagerAllocateAndReuse);
    testfw::Run("存储: 磁盘页读写往返", TestDiskManagerReadWriteRoundtrip);
    testfw::Run("存储: 页间数据隔离", TestDiskManagerPageIsolation);
    testfw::Run("存储: 缓冲池取页与命中", TestBufferPoolNewPageAndGetPage);
    testfw::Run("存储: 单页强制落盘", TestBufferPoolFlushPageToDisk);
    testfw::Run("存储: 淘汰脏页写回+重载", TestBufferPoolEvictionWritesBackDirtyPage);
    testfw::Run("存储: 全Pin时新页分配失败", TestBufferPoolAllPinnedNewPageFails);
    testfw::Run("存储: 删除页", TestBufferPoolDeletePage);
    testfw::Run("存储: 全量落盘后重载", TestBufferPoolFlushAllPages);
    return testfw::Summary("storage_test");
}
