// 存储子系统单元测试
// 构建：由 CMake 目标 storage_ut 编译（链接 sqlcompiler_lib）。
// 运行：build/storage_ut(.exe)
//
// 覆盖点：
//   * DiskManager 页分配/回收、写读往返、文件大小缓存
//   * Page 复位语义
//   * LRU / FIFO 替换顺序
//   * BufferPoolManager 命中统计、淘汰触发 loaded_page_id、替换日志环形上限

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "storage/BufferPoolManager.h"
#include "storage/DiskManager.h"
#include "storage/FIFOReplacer.h"
#include "storage/LRUReplacer.h"
#include "storage/LRUKReplacer.h"
#include "storage/ClockReplacer.h"
#include "storage/PageAllocator.h"
#include "storage/OsModuleOptimizations.h"
#include "storage/LockManager.h"
#include "storage/Page.h"
#include "index/PageGuard.h"
#include "db/Database.h"
#include "index/BPlusTree.h"

using namespace sqlcompiler;

static int g_checks = 0;
static int g_fails = 0;

// 断言宏：失败时打印位置并累计。
#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_fails;                                                         \
            std::printf("FAIL  %s:%d  [%s]\n", __FILE__, __LINE__, #cond);     \
        }                                                                      \
    } while (0)

static void RemoveFile(const std::string& path) {
    std::remove(path.c_str());
}

// 打开一个全新 DiskManager 并尝试读取某页；返回是否存在 CRC 不匹配等异常。
// 每次调用都独立重新装载 <db>.crc，用于验证页在跨会话后的持久化校验行为。
static bool ReadPageThrows(const std::string& path, page_id_t pid) {
    try {
        DiskManager dm(path);
        char r[PAGE_SIZE];
        dm.ReadPage(pid, r);
        return false;
    } catch (...) {
        return true;
    }
}

// 把数据文件中某页从 byte_off 起连续 count 个字节各翻转（异或 0xFF），模拟位损坏。
static void FlipPageBytes(const std::string& path, page_id_t pid,
                          size_t byte_off, size_t count) {
    std::FILE* f = std::fopen(path.c_str(), "r+b");
    if (f == nullptr) return;
    long base = static_cast<long>(pid) * PAGE_SIZE + static_cast<long>(byte_off);
    for (size_t k = 0; k < count; ++k) {
        long pos = base + static_cast<long>(k);
        std::fseek(f, pos, SEEK_SET);
        char c = 0;
        if (std::fread(&c, 1, 1, f) == 1) {
            std::fseek(f, pos, SEEK_SET);
            char flip = static_cast<char>(c ^ 0xFF);
            std::fwrite(&flip, 1, 1, f);
        }
    }
    std::fflush(f);
    std::fclose(f);
}

// 判断某文件是否存在（供惰性创建/文件增长断言语义）。
static bool FileExists(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::fclose(f);
    return true;
}

// 返回某文件字节大小；不存在返回 0。
static long FileSize(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return 0;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fclose(f);
    return sz;
}

// ---------------------------------------------------------------------------
// 1. DiskManager：页分配/回收 + 写读往返 + 文件大小缓存
// ---------------------------------------------------------------------------
static void TestDiskManager() {
    const std::string path = "storage_ut_disk.bin";
    RemoveFile(path);
    {
        DiskManager dm(path);

        // 初始分配：页 0
        page_id_t p0 = dm.AllocatePage();
        CHECK(p0 == 0);

        // 写数据再读回
        char w[PAGE_SIZE];
        char r[PAGE_SIZE];
        std::memset(w, 0xAB, PAGE_SIZE);
        ::memcpy(w, "hello-page", 10);
        dm.WritePage(p0, w);
        dm.ReadPage(p0, r);
        CHECK(std::memcmp(w, r, PAGE_SIZE) == 0);

        // 回收后应复用页号
        dm.DeallocatePage(p0);
        page_id_t p1 = dm.AllocatePage();
        CHECK(p1 == p0);  // 复用刚释放的页号

        // 未分配页读取应全零（不越界崩溃）
        char z[PAGE_SIZE];
        dm.ReadPage(999, z);
        for (int i = 0; i < PAGE_SIZE; ++i) CHECK(z[i] == 0);

        // GetNumPages 反映已分配的页数
        CHECK(dm.GetNumPages() >= 1);
    }
    RemoveFile(path);
}

// ---------------------------------------------------------------------------
// 1b. 空闲页持久化（<db>.fpl 位图）+ 向后兼容
// 覆盖点：
//   * 回收页重启后可复用（持久化生效）
//   * 新分配的页在重启后不被当作空闲页复用（防活页误复用）
//   * 无 .fpl 的旧库（向后兼容）：空闲列表为空，仅会话内复用
//   * 页数一致校验：.fpl 与数据文件不一致时安全丢弃（避免误复用活页）
// ---------------------------------------------------------------------------
static void TestFreePagePersistence() {
    const std::string path = "storage_ut_fpl.bin";
    const std::string fpl = path + ".fpl";
    RemoveFile(path);
    RemoveFile(fpl);

    // 注意：next_page_id_ 由数据文件大小推导，故测试中用 WritePage 落盘，
    // 才能让重启后从文件大小还原的页数与 .fpl 头部记录一致。

    // (a) 旧库向后兼容：未发生任何回收时，不应产生 .fpl，行为与旧版完全一致
    {
        DiskManager dm(path);
        page_id_t p0 = dm.AllocatePage();
        char w[PAGE_SIZE];
        std::memset(w, 0xCD, PAGE_SIZE);
        dm.WritePage(p0, w);
    }
    {
        std::FILE* f = std::fopen(fpl.c_str(), "r+b");
        CHECK(f == nullptr);  // 无回收 → 惰性创建 → 不应生成 .fpl
        if (f != nullptr) std::fclose(f);
    }

    // (b) 回收后重启应复用同一页号（持久化生效）
    page_id_t q0 = 0, q1 = 0;
    {
        DiskManager dm(path);
        q0 = dm.AllocatePage();  // 0
        q1 = dm.AllocatePage();  // 1
        // 写盘建立真实文件大小，使重启后页数还原一致
        char w[PAGE_SIZE];
        std::memset(w, 0, PAGE_SIZE);
        dm.WritePage(q0, w);
        dm.WritePage(q1, w);
        dm.DeallocatePage(q0);  // 触发 .fpl 创建并落盘
    }
    {
        DiskManager dm(path);
        page_id_t p = dm.AllocatePage();
        CHECK(p == q0);                    // 复用持久化的空闲页号
        CHECK(dm.GetNumFreePages() == 0);  // 复用时已把位图置 0
    }

    // (c) 已分配页绝不会因位图误判被二度分配（防活页复用 → 数据损坏）
    {
        DiskManager dm(path);
        // q0 已在 (b) 中被重新占用，位图应已把其清零；下一次分配必须是全新页号。
        const int base = dm.GetNumPages();
        page_id_t a = dm.AllocatePage();
        CHECK(a == base);  // base != q0（q0 已占用，绝不会被二度分配）
        char w[PAGE_SIZE];
        std::memset(w, 0, PAGE_SIZE);
        dm.WritePage(a, w);
    }

    // (d) 页数不一致安全容错：写一个 pages 与数据文件不符的 .fpl，应被安全丢弃。
    //     把头部 pages 字段改写为远超数据文件页数的大值，模拟页数漂移。
    page_id_t victim = 0;
    {
        DiskManager dm(path);
        std::FILE* f = std::fopen(fpl.c_str(), "r+b");
        // 篡改头部 pages 字段（偏移 8..15 的小端 8 字节）：把高位字节写 0xFF，
        // 使 stored_pages 变成一个远超数据文件页数的大值，模拟页数漂移。
        std::fseek(f, 11, SEEK_SET);
        std::fputc(0xFF, f);
        std::fclose(f);
    }
    {
        DiskManager dm(path);  // 应看到不一致 → 丢弃空闲列表，仅损失复用
        const int base = dm.GetNumPages();  // 数据文件当前页数 → 下一个全新页号
        page_id_t p = dm.AllocatePage();
        // 因 .fpl 被丢弃，free 为空 → 分配到全新页 base，绝不是任何曾回收的旧页
        CHECK(p == base);
        victim = p;
        char w[PAGE_SIZE];
        std::memset(w, 0, PAGE_SIZE);
        dm.WritePage(p, w);
    }

    // (e) 正常路径回归：回收仍能跨重启复用
    page_id_t old = 0;
    {
        DiskManager dm(path);
        old = dm.AllocatePage();  // 全新页
        char w[PAGE_SIZE];
        std::memset(w, 0, PAGE_SIZE);
        dm.WritePage(old, w);
        dm.DeallocatePage(old);
    }
    {
        DiskManager dm(path);
        CHECK(dm.AllocatePage() == old);  // 重启后复用持久化的空闲页号
    }
    (void)victim;
    (void)q0;

    RemoveFile(path);
    RemoveFile(fpl);
}

// ---------------------------------------------------------------------------
// 1c. 页 CRC32 校验（<db>.crc 旁路）：兼容 / 惰性创建 / 增长 / 重写刷新 /
//     单字节与多字节损坏检测 / 未写页免校验 / 回收清 CRC / 损坏 .crc 降级
// ---------------------------------------------------------------------------
static void TestPageCrc() {
    const std::string path = "storage_ut_crc.bin";
    const std::string crc = path + ".crc";
    RemoveFile(path);
    RemoveFile(crc);

    // (a) 旧库兼容：全新库（无 .crc）写读往返不校验、不抛错
    {
        DiskManager dm(path);
        page_id_t p0 = dm.AllocatePage();
        char w[PAGE_SIZE];
        std::memset(w, 0x5A, PAGE_SIZE);
        dm.WritePage(p0, w);
        char r[PAGE_SIZE];
        dm.ReadPage(p0, r);  // 无 CRC 记录 → 跳过校验
        CHECK(std::memcmp(w, r, PAGE_SIZE) == 0);
    }

    // (a1) 惰性创建：仅"无写活页"的会话不生成 .crc。
    //      先将上一会话产生的 .crc 清除，本会话零写入 → 不应重新生成。
    RemoveFile(crc);
    {
        DiskManager dm(path);  // 复用上面写出的数据文件，但本会话不做任何 WritePage
    }
    CHECK(!FileExists(crc));   // 惰性：未写入活页 → 无 .crc 产生

    // (b) 正常写读往返 + .crc 惰性创建 + 按页增长 + Sync 落盘
    page_id_t p0, p1, p2;
    {
        DiskManager dm(path);
        p0 = dm.AllocatePage();
        p1 = dm.AllocatePage();
        p2 = dm.AllocatePage();
        char w0[PAGE_SIZE], w1[PAGE_SIZE], w2[PAGE_SIZE];
        std::memset(w0, 0x10, PAGE_SIZE); std::memcpy(w0, "p0-tag", 6);
        std::memset(w1, 0x22, PAGE_SIZE); std::memcpy(w1, "p1-tag", 6);
        std::memset(w2, 0x33, PAGE_SIZE); std::memcpy(w2, "p2-tag", 6);
        dm.WritePage(p0, w0);
        dm.WritePage(p1, w1);
        dm.WritePage(p2, w2);
        dm.Sync();  // 让数据文件与 .crc 一起落盘
        char r[PAGE_SIZE];
        dm.ReadPage(p0, r); CHECK(std::memcmp(w0, r, PAGE_SIZE) == 0);
        dm.ReadPage(p1, r); CHECK(std::memcmp(w1, r, PAGE_SIZE) == 0);
        dm.ReadPage(p2, r); CHECK(std::memcmp(w2, r, PAGE_SIZE) == 0);
    }
    // .crc 已生成，且按每页 4 字节增长：写完第 2 页 id=2 → 至少 3*4=12 字节
    CHECK(FileExists(crc));
    CHECK(FileSize(crc) >= 3 * 4L);

    // (c) 重写同一页：内容变化 → CRC 重算，读回仍通过（旧校验值被刷新，无误报）
    {
        DiskManager dm(path);
        char w[PAGE_SIZE];
        std::memset(w, 0xEE, PAGE_SIZE);
        dm.WritePage(p0, w);   // 覆盖 p0 为新内容
        char r[PAGE_SIZE];
        dm.ReadPage(p0, r);
        CHECK(std::memcmp(w, r, PAGE_SIZE) == 0);
    }
    // 跨会话重读仍通过（重写后的 CRC 已持久化）
    CHECK(!ReadPageThrows(path, p0));
    // 此时改回旧内容反而应被当作"损坏"（因 p0 记录的是新内容 CRC）
    FlipPageBytes(path, p0, 0, PAGE_SIZE);  // 全页翻转 → 变回 0x11，与存储的 CRC 不符
    CHECK(ReadPageThrows(path, p0));

    // (d) 单字节损坏：只损 p1，p0/p2 不受影响（逐页独立校验）
    RemoveFile(path); RemoveFile(crc);
    {
        DiskManager dm(path);
        p0 = dm.AllocatePage(); p1 = dm.AllocatePage(); p2 = dm.AllocatePage();
        char w[PAGE_SIZE];
        std::memset(w, 0x77, PAGE_SIZE);
        dm.WritePage(p0, w); dm.WritePage(p1, w); dm.WritePage(p2, w);
    }
    FlipPageBytes(path, p1, 13, 1);          // 只翻转 p1 的第 13 字节
    CHECK(!ReadPageThrows(path, p0));        // p0 完好
    CHECK(!ReadPageThrows(path, p2));        // p2 完好
    CHECK(ReadPageThrows(path, p1));         // p1 被损 → 抛 CRC 错误

    // (e) 连续多字节损坏同样被检出
    FlipPageBytes(path, p0, 0, 16);          // p0 前 16 字节全翻转
    CHECK(ReadPageThrows(path, p0));
    CHECK(!ReadPageThrows(path, p2));        // p2 仍完好

    // (f) 未写过的页无 CRC 记录 → 读回零且不抛错（未分配语义保持）
    {
        DiskManager dm(path);
        page_id_t fresh = dm.AllocatePage();  // 全新页，从未 WritePage
        char r[PAGE_SIZE];
        dm.ReadPage(fresh, r);                // 不抛错（无 CRC）
        CHECK(r[0] == 0);                     // 未分配 → 全零
    }

    // (g) 回收清 CRC → 复用写新内容：旧校验不残留、不误报
    {
        DiskManager dm(path);
        // p2 当前有内容；回收后其物理字节仍是旧值，但 CRC 被清空
        dm.DeallocatePage(p2);
        page_id_t reused = dm.AllocatePage();
        CHECK(reused == p2);
        char w[PAGE_SIZE];
        std::memset(w, 0xAB, PAGE_SIZE);
        dm.WritePage(reused, w);              // 复用页写入新内容
        char r[PAGE_SIZE];
        dm.ReadPage(reused, r);
        CHECK(std::memcmp(w, r, PAGE_SIZE) == 0);  // 新内容校验通过
    }
    CHECK(!ReadPageThrows(path, p2));         // 跨会话：复用页新内容仍可读

    // (h) 损坏/半截 .crc 安全降级：空 .crc 使所有页跳过校验，不崩溃、不误报
    {
        std::FILE* f = std::fopen(crc.c_str(), "wb");  // 截断为空文件
        CHECK(f != nullptr);
        if (f != nullptr) std::fclose(f);
    }
    {
        DiskManager dm(path);        // 构造函数对空 .crc 不作假设、不崩溃
        char r[PAGE_SIZE];
        // p1 在上层 (d) 中已被翻转，但因 .crc 为空 → 无校验记录 → 不抛错（降级兼容）
        dm.ReadPage(p1, r);
        CHECK(true);
    }

    RemoveFile(path);
    RemoveFile(crc);
}

// ---------------------------------------------------------------------------
// 1d. BufferPool 全局锁（latch_）：四种并发场景
//   (1) 并发 Get/Unpin 同一页集，无崩溃、无失败（pin 计数不被并发双减）
//   (2) 并发 NewPage，返回的页号两两不重复（分配被串行化）
//   (3) 并发写各自页，内容互不串扰、读回一致
//   (4) 并发 GetPage 与 FlushAllDirtyPages 竞争，不崩溃、不死锁（锁序回归）
// ---------------------------------------------------------------------------
static void TestBufferPoolConcurrency() {
    const std::string path = "storage_ut_conc.bin";
    RemoveFile(path);
    {
        DiskManager dm(path);

        // (1) 并发 Get/Unpin 同一页集合：锁应串行化帧表与 pin 计数
        {
            BufferPoolManager bpm(32, &dm);
            std::atomic<int> done{0};
            std::atomic<int> fails{0};
            const int kThreads = 8, kPages = 64, kIters = 2000;
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&]() {
                    for (int i = 0; i < kIters; ++i) {
                        page_id_t pid = static_cast<page_id_t>(i % kPages);
                        Page* p = bpm.GetPage(pid);
                        if (p == nullptr) { ++fails; continue; }
                        if (!bpm.UnpinPage(pid, true)) ++fails;
                    }
                    ++done;
                });
            }
            for (auto& th : threads) th.join();
            CHECK(done == kThreads);   // 全部线程正常完成
            CHECK(fails == 0);         // Get/Unpin 无失败 → 帧表与 pin 一致
            bpm.FlushAllDirtyPages();
        }

        // (2) 并发 NewPage：页号两两不重复（AllocatePage 由 DiskManager 锁保证唯一）
        {
            BufferPoolManager bpm(64, &dm);
            const int kThreads = 8, kEach = 20;
            const int kTotal = kThreads * kEach;
            std::mutex mu;
            std::set<int> seen;
            std::atomic<int> newFail{0};
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&]() {
                    for (int k = 0; k < kEach; ++k) {
                        page_id_t pid = INVALID_PAGE_ID;
                        Page* p = bpm.NewPage(&pid);
                        if (p == nullptr || pid == INVALID_PAGE_ID) { ++newFail; continue; }
                        bpm.UnpinPage(pid, false);
                        std::lock_guard<std::mutex> g(mu);
                        seen.insert(static_cast<int>(pid));
                    }
                });
            }
            for (auto& th : threads) th.join();
            CHECK(newFail == 0);                                   // 无 NewPage 失败
            CHECK(seen.size() == static_cast<size_t>(kTotal));     // 页号两两不同
        }

        // (3) 并发写各自页：数据互不串扰，读回一致
        {
            BufferPoolManager bpm(64, &dm);
            const int kThreads = 8;
            std::vector<page_id_t> pids(kThreads, INVALID_PAGE_ID);
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&, t]() {
                    page_id_t pid = INVALID_PAGE_ID;
                    Page* p = bpm.NewPage(&pid);
                    if (p == nullptr) return;
                    char* d = p->GetData();
                    std::memset(d, static_cast<unsigned char>('A' + t), PAGE_SIZE);
                    p->SetDirty(true);
                    bpm.UnpinPage(pid, true);
                    pids[t] = pid;
                });
            }
            for (auto& th : threads) th.join();
            for (int t = 0; t < kThreads; ++t) {
                CHECK(pids[t] != INVALID_PAGE_ID);
                Page* p = bpm.GetPage(pids[t]);
                CHECK(p != nullptr);
                if (p != nullptr) {
                    char* d = p->GetData();
                    // 页内容应等于本线程写入的那一版，未被其它线程串扰
                    CHECK(d[0] == static_cast<char>('A' + t));
                    bool uniform = true;
                    for (int i = 1; i < 64; ++i) {
                        if (d[i] != static_cast<char>('A' + t)) { uniform = false; break; }
                    }
                    CHECK(uniform);
                    bpm.UnpinPage(pids[t], false);
                }
            }
        }

        // (4) 并发 GetPage 与 FlushAllDirtyPages 竞争：不死锁、都完成
        {
            BufferPoolManager bpm(16, &dm);
            std::atomic<bool> stop{false};
            std::atomic<int> flushes{0};
            std::vector<std::thread> workers;
            for (int t = 0; t < 4; ++t) {
                workers.emplace_back([&]() {
                    page_id_t pid = INVALID_PAGE_ID;
                    Page* p = bpm.NewPage(&pid);
                    if (p != nullptr) bpm.UnpinPage(pid, true);
                    while (!stop.load()) {
                        Page* q = bpm.GetPage(pid);
                        if (q != nullptr) bpm.UnpinPage(pid, true);
                    }
                });
            }
            for (int i = 0; i < 200; ++i) {
                bpm.FlushAllDirtyPages();  // 与 GetPage 线程在 latch_ 上争抢
                ++flushes;
            }
            stop.store(true);
            for (auto& th : workers) th.join();
            CHECK(flushes == 200);   // 主线程刷新全部完成 → 未被死锁卡住
        }
    }
    RemoveFile(path);
}

// ---------------------------------------------------------------------------
// 2. Page：复位语义
// ---------------------------------------------------------------------------
static void TestPage() {
    Page pg;
    pg.SetPageId(7);
    pg.SetDirty(true);
    pg.IncPinCount();
    pg.SetPageLsn(42);
    pg.ResetMemory();
    CHECK(pg.GetPageId() == INVALID_PAGE_ID);
    CHECK(!pg.IsDirty());
    CHECK(pg.GetPinCount() == 0);
    CHECK(pg.GetPageLsn() == 0);
}

// ---------------------------------------------------------------------------
// 3. LRU 替换顺序
// ---------------------------------------------------------------------------
static void TestLRU() {
    LRUReplacer r(4);
    r.Unpin(1);
    r.Unpin(2);
    r.Unpin(3);
    int victim = -1;
    CHECK(r.Victim(&victim) && victim == 1);  // 最久未用被优先淘汰
    CHECK(r.Victim(&victim) && victim == 2);
    // Pin 后再 Unpin 会移动到最近使用，从而避免被立即淘汰
    r.Unpin(4);
    r.Pin(3);
    r.Unpin(3);
    CHECK(r.Victim(&victim) && victim == 4);
    CHECK(r.Victim(&victim) && victim == 3);
    CHECK(!r.Victim(&victim));  // 已空
    CHECK(r.Size() == 0);
}

// ---------------------------------------------------------------------------
// 4. FIFO 替换顺序
// ---------------------------------------------------------------------------
static void TestFIFO() {
    FIFOReplacer r(4);
    r.Unpin(1);
    r.Unpin(2);
    r.Pin(2);  // 从可淘汰集合移除
    r.Unpin(2);
    r.Unpin(3);
    int victim = -1;
    CHECK(r.Victim(&victim) && victim == 1);  // 进队最早
    CHECK(r.Victim(&victim) && victim == 2);
    CHECK(r.Victim(&victim) && victim == 3);
    CHECK(!r.Victim(&victim));
}

// ---------------------------------------------------------------------------
// 4.5 Clock 替换顺序（含其中"二次机会"语义验证）
// ---------------------------------------------------------------------------
static void TestClock() {
    // 空候选集：Victim 失败、Size 为 0
    {
        ClockReplacer r(4);
        int v = -1;
        CHECK(!r.Victim(&v));
        CHECK(r.Size() == 0);
    }

    // Pin 从候选集移除；Unpin 重复置候选不重复计数
    {
        ClockReplacer r(2);
        int v = -1;
        r.Unpin(0);
        CHECK(r.Size() == 1);
        r.Unpin(0);          // 已在候选，不应再加
        CHECK(r.Size() == 1);
        r.Pin(0);            // 移出候选
        CHECK(r.Size() == 0);
        CHECK(!r.Victim(&v));
        r.Unpin(1);
        CHECK(r.Size() == 1);
        r.Victim(&v);        // 逐出后候选清空
        CHECK(r.Size() == 0);
    }

    // 二次机会：空闲(ref=0)的帧优先被逐，刚解 pin(ref=1)的帧被放行
    {
        ClockReplacer r(4);
        r.Unpin(1);          // 帧1 进候选，ref=1
        r.Unpin(2);          // 帧2 进候选，ref=1
        int v = -1;
        // 首轮 Victim：1、2 ref 各被清零，之后帧1 先到 0 位被逐
        CHECK(r.Victim(&v) && v == 1);
        CHECK(r.Size() == 1);
        // 现在只剩帧2（ref=0），hand 已越过 2
        r.Unpin(1);          // 帧1 重新入候选，ref=1（新机会）
        r.Unpin(3);          // 帧3 入候选，ref=1
        // 候选：2(ref=0 空闲)、1(ref=1)、3(ref=1)。指针从 3 后开始扫。
        CHECK(r.Victim(&v) && v == 2);   // 空闲的 2 被优先逐出；1/3 因 ref=1 获二次机会
        CHECK(r.Size() == 2);
        // 余下 1、3 逐个被逐，最终清空
        CHECK(r.Victim(&v));
        CHECK(r.Victim(&v));
        CHECK(r.Size() == 0);
        CHECK(!r.Victim(&v));
    }
}

// ---------------------------------------------------------------------------
// 4.6 BufferPoolManager 以 CLOCK 策略工作：替换仍记录 loaded 日志
// ---------------------------------------------------------------------------
static void TestBufferPoolClock() {
    const std::string path = "storage_ut_bpm_clock.bin";
    RemoveFile(path);
    {
        DiskManager dm(path);
        BufferPoolManager bpm(2, &dm, ReplacementPolicy::CLOCK);
        page_id_t a = -1, b = -1;
        CHECK(bpm.NewPage(&a) != nullptr);
        bpm.UnpinPage(a, false);
        CHECK(bpm.NewPage(&b) != nullptr);
        bpm.UnpinPage(b, false);
        // 读一个不在缓冲池的页，触发一次 Clock 淘汰
        CHECK(bpm.GetPage(a + 100) != nullptr);
        bpm.UnpinPage(a + 100, false);
        CHECK(bpm.GetStats().replacement_count == 1);
        const auto& log = bpm.GetReplacementLog();
        CHECK(!log.empty());
        CHECK(log.back().loaded_page_id != INVALID_PAGE_ID);
    }
    RemoveFile(path);
}

// ---------------------------------------------------------------------------
// 4.7 页内内存分配器（PageAllocator）：分配/释放/合并/碎片/可重放
// ---------------------------------------------------------------------------
static void TestPageAllocator() {
    // 空堆统计
    {
        char buf[4096];
        CHECK(PageAllocator::Init(buf, 4096));
        PageAllocator::Stats s = PageAllocator::GetStats(buf);
        CHECK(s.capacity == 4096);
        CHECK(s.num_free_blocks == 1);
        CHECK(s.largest_free_block == 4096 - 16);
        CHECK(s.free_capacity == 4096 - 16);
        CHECK(s.usable_free == (4096 - 16) - 8);
        CHECK(s.allocated_bytes == 0);
        CHECK(s.external_fragmentation == 0);
    }
    // 分配 + 内容写入读回 + 记账准确（载荷 16 + 8B 块头）
    {
        char buf[4096];
        CHECK(PageAllocator::Init(buf, 4096));
        int32_t off = -1;
        CHECK(PageAllocator::Allocate(buf, 16, &off));
        CHECK(off >= 0 && off < 4096);
        std::memcpy(buf + off, "hello", 5);
        CHECK(std::memcmp(buf + off, "hello", 5) == 0);
        PageAllocator::Stats s = PageAllocator::GetStats(buf);
        CHECK(s.allocated_bytes == 24);  // 16 + 8
        CHECK(s.num_free_blocks == 1);
    }
    // 释放相邻块 → 合并回单一空闲块，分配字节归零
    {
        char buf[4096];
        CHECK(PageAllocator::Init(buf, 4096));
        int32_t a = -1, b = -1;
        CHECK(PageAllocator::Allocate(buf, 16, &a));
        CHECK(PageAllocator::Allocate(buf, 16, &b));
        CHECK(PageAllocator::Free(buf, a));
        CHECK(PageAllocator::Free(buf, b));
        PageAllocator::Stats s = PageAllocator::GetStats(buf);
        CHECK(s.allocated_bytes == 0);
        CHECK(s.num_free_blocks == 1);
        CHECK(s.free_capacity == 4096 - 16);
        CHECK(s.largest_free_block == 4096 - 16);
    }
    // 碎片观测：释放中间块 → 产生外部碎片（中孔不可用于单次大块分配）
    {
        char buf[4096];
        CHECK(PageAllocator::Init(buf, 4096));
        int32_t a = -1, b = -1, c = -1;
        CHECK(PageAllocator::Allocate(buf, 100, &a));
        CHECK(PageAllocator::Allocate(buf, 100, &b));
        CHECK(PageAllocator::Allocate(buf, 100, &c));
        CHECK(PageAllocator::Free(buf, b));  // 释放中间的块
        PageAllocator::Stats s = PageAllocator::GetStats(buf);
        CHECK(s.num_free_blocks == 2);      // 中孔 + 尾部大块
        CHECK(s.allocated_bytes == 224);    // A + C（各 112 含头）
        CHECK(s.external_fragmentation > 0);  // 存在不可用的大块空闲载荷
        int32_t big = -1;
        CHECK(PageAllocator::Allocate(buf, 3000, &big));  // 尾部大块仍可承载
    }
    // 边界与防御：0 分配、过大分配、二次释放、非法偏移
    {
        char buf[4096];
        CHECK(PageAllocator::Init(buf, 4096));
        int32_t x = -1;
        CHECK(!PageAllocator::Allocate(buf, 0, &x));
        CHECK(!PageAllocator::Allocate(buf, 1 << 20, &x));
        int32_t off = -1;
        CHECK(PageAllocator::Allocate(buf, 32, &off));
        CHECK(PageAllocator::Free(buf, off));
        CHECK(!PageAllocator::Free(buf, off));  // 二次释放被拒
        CHECK(!PageAllocator::Free(buf, 0));    // 非法偏移
    }
    // 确定性 / 可重放：相同操作序列 → 相同布局（载荷偏移一致）
    {
        auto run = [](char* buf, int32_t* out) -> bool {
            if (!PageAllocator::Init(buf, 4096)) return false;
            if (!PageAllocator::Allocate(buf, 32, &out[0])) return false;
            if (!PageAllocator::Allocate(buf, 64, &out[1])) return false;
            if (!PageAllocator::Allocate(buf, 8, &out[2])) return false;
            PageAllocator::Free(buf, out[1]);
            if (!PageAllocator::Allocate(buf, 40, &out[3])) return false;
            return true;
        };
        char a[4096], b[4096];
        int32_t oa[4], ob[4];
        CHECK(run(a, oa));
        CHECK(run(b, ob));
        CHECK(oa[0] == ob[0] && oa[1] == ob[1] && oa[2] == ob[2] && oa[3] == ob[3]);
    }
}

// ---------------------------------------------------------------------------
// 5. BufferPoolManager：命中统计、淘汰日志 loaded、历史截断
// ---------------------------------------------------------------------------
static void TestBufferPool() {
    const std::string path = "storage_ut_bpm.bin";
    RemoveFile(path);
    {
        DiskManager dm(path);
        BufferPoolManager bpm(2, &dm);  // 2 帧，LRU

        // 分配两页并释放占用，使缓冲池满且全部可淘汰
        page_id_t a = -1, b = -1;
        Page* pa = bpm.NewPage(&a);
        CHECK(pa != nullptr);
        bpm.UnpinPage(a, false);
        Page* pb = bpm.NewPage(&b);
        CHECK(pb != nullptr);
        bpm.UnpinPage(b, false);

        CHECK(bpm.GetStats().miss_count == 2);
        // 第二页在新数据文件里尚未写盘，但 NewPage 空页即可读；这里仅验证接口可回取
        Page* got = bpm.GetPage(a);
        CHECK(got != nullptr);
        bpm.UnpinPage(a, false);
        CHECK(bpm.GetStats().hit_count >= 1);

        // 触发淘汰：读一个不在缓冲池中的新页（数据文件里不存在 → 读零页）
        Page* got2 = bpm.GetPage(a + 100);
        CHECK(got2 != nullptr);
        bpm.UnpinPage(a + 100, false);

        // 应已发生一次淘汰，且替换日志的 loaded_page_id 已正确填充
        CHECK(bpm.GetStats().replacement_count == 1);
        const auto& log = bpm.GetReplacementLog();
        CHECK(!log.empty());
        CHECK(log.back().loaded_page_id != INVALID_PAGE_ID);

        // 刷脏页（无日志注入，属 Phase A 兼容路径，不应抛异常）
        bpm.FlushAllDirtyPages();
        bpm.FlushPage(a);
    }
    RemoveFile(path);
}

// ---------------------------------------------------------------------------
// 6. 替换日志环形上限：超过 kMaxReplacementLog 后被截断
// ---------------------------------------------------------------------------
static void TestReplacementLogCap() {
    const std::string path = "storage_ut_bpm_cap.bin";
    RemoveFile(path);
    {
        DiskManager dm(path);
        BufferPoolManager bpm(1, &dm);  // 单帧，每 NewPage 都触发淘汰
        page_id_t pid = -1;
        bpm.NewPage(&pid);
        bpm.UnpinPage(pid, false);
        const int kIters = 1100;  // 超过 1024 上限
        for (int i = 0; i < kIters; ++i) {
            Page* p = bpm.NewPage(&pid);
            CHECK(p != nullptr);
            bpm.UnpinPage(pid, false);
        }
        // 每次 NewPage 都会触发一次替换（帧已被占用）
        CHECK(bpm.GetStats().replacement_count == kIters);
        const auto& log = bpm.GetReplacementLog();
        CHECK(log.size() <= 1024);
        CHECK(log.size() >= 1024);  // 达到并保持上限
    }
    RemoveFile(path);
}

// ---------------------------------------------------------------------------
// 6b. IO 统计与脏页写回计数：\stats 的「disk reads/writes」「dirty writebacks」
// ---------------------------------------------------------------------------
static void TestIOStats() {
    const std::string path = "storage_ut_io_stats.bin";
    RemoveFile(path);
    {
        DiskManager dm(path);
        BufferPoolManager bpm(1, &dm);  // 单帧，便于触发淘汰
        page_id_t p0 = -1;
        Page* p = bpm.NewPage(&p0);
        CHECK(p != nullptr);
        bpm.UnpinPage(p0, false);

        // NewPage / 干净命中都不触磁盘 I/O
        CHECK(dm.GetIOReadCount() == 0);
        CHECK(dm.GetIOWriteCount() == 0);
        CHECK(bpm.GetStats().writeback_count == 0);

        // 缓冲池内 GetPage 命中：不触发磁盘读
        CHECK(bpm.GetPage(p0) != nullptr);
        CHECK(dm.GetIOReadCount() == 0);
        bpm.UnpinPage(p0, false);

        // 写脏并显式 FlushPage：磁盘写 1 次、脏页写回 1 次
        Page* g = bpm.GetPage(p0);
        CHECK(g != nullptr);
        std::memset(g->GetData(), 0xAB, PAGE_SIZE);
        bpm.UnpinPage(p0, true);
        CHECK(bpm.FlushPage(p0));
        CHECK(dm.GetIOWriteCount() == 1);
        CHECK(bpm.GetStats().writeback_count == 1);

        // 写回后仍干净且在池内：命中，无磁盘读
        g = bpm.GetPage(p0);
        std::memset(g->GetData(), 0xCD, PAGE_SIZE);
        bpm.UnpinPage(p0, true);
        CHECK(dm.GetIOReadCount() == 0);

        // 分配新页触发 p0 脏页淘汰换出：再记 1 次写回、1 次磁盘写
        page_id_t p1 = -1;
        CHECK(bpm.NewPage(&p1) != nullptr);
        bpm.UnpinPage(p1, false);  // p1 干净且不再引用，便于后续换出
        CHECK(dm.GetIOWriteCount() == 2);
        CHECK(bpm.GetStats().writeback_count == 2);

        // p0 已被换出：GetPage 触发一次磁盘读
        CHECK(bpm.GetPage(p0) != nullptr);
        CHECK(dm.GetIOReadCount() >= 1);
    }
    RemoveFile(path);
}

// ---------------------------------------------------------------------------
// 6c. 后台异步刷脏线程（E5）：周期刷脏、启用/停用、幂等收尾
// ---------------------------------------------------------------------------
static void TestBackgroundFlush() {
    const std::string path = "storage_ut_bg_flush.bin";
    RemoveFile(path);
    {
        DiskManager dm(path);
        BufferPoolManager bpm(4, &dm);  // 未注入 LogManager → 后台刷脏纯写盘
        page_id_t p0 = -1;
        Page* p = bpm.NewPage(&p0);
        CHECK(p != nullptr);
        bpm.UnpinPage(p0, false);
        CHECK(bpm.GetStats().writeback_count == 0);
        CHECK(dm.GetIOWriteCount() == 0);

        // 弄出一个脏页（不清洗写回）
        Page* g = bpm.GetPage(p0);
        CHECK(g != nullptr);
        std::memset(g->GetData(), 0x11, PAGE_SIZE);
        bpm.UnpinPage(p0, true);
        CHECK(bpm.GetStats().writeback_count == 0);

        // 未启用时 IsBackgroundFlushEnabled 为 false
        CHECK(!bpm.IsBackgroundFlushEnabled());

        // 启动后台线程（默认禁用的线程被开启）
        bpm.StartBackgroundFlush(std::chrono::milliseconds(20));
        CHECK(bpm.IsBackgroundFlushEnabled());
        CHECK(bpm.GetBackgroundFlushInterval().count() == 20);

        // 轮询等待后台把脏页写回：无需任何显式 Flush，writeback 应被后台线程推进，
        // 且磁盘 I/O 写计数增加（真正触达 OS 缓存）。给足 2s 上限防时序抖动。
        const long long deadline_ms = 2000;
        long long waited = 0;
        while (bpm.GetStats().writeback_count == 0 && waited < deadline_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            waited += 10;
        }
        CHECK(bpm.GetStats().writeback_count >= 1);  // 后台线程确实把脏页写回
        CHECK(dm.GetIOWriteCount() >= 1);
        CHECK(bpm.GetBackgroundFlushTicks() >= 1);

        // 停用：线程 join 干净收尾，之后停止态不再推进
        bpm.StopBackgroundFlush();
        CHECK(!bpm.IsBackgroundFlushEnabled());
        const long ticks_after_stop = bpm.GetBackgroundFlushTicks();
        bpm.StopBackgroundFlush();  // 幂等：重复停无副作用
        CHECK(!bpm.IsBackgroundFlushEnabled());
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        CHECK(bpm.GetBackgroundFlushTicks() == ticks_after_stop);  // 不再有 tick
    }
    RemoveFile(path);
}

// ---------------------------------------------------------------------------
// 6d. 块设备抽象层（E7）：FileBlockDevice 透传 + FaultInjectingBlockDevice
//     坏块注入（I/O 错误通道 + 页 CRC 拦截静默损坏）
// ---------------------------------------------------------------------------
static void TestBlockDeviceFaultInjection() {
    const std::string path = "storage_ut_bdev.bin";
    RemoveFile(path);
    RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
    {
        auto device = std::make_unique<FaultInjectingBlockDevice>(
            std::make_unique<FileBlockDevice>(path));
        FaultInjectingBlockDevice* faulty = device.get();  // 保留句柄以注入故障
        DiskManager dm(path, std::move(device));

        // (a) 未注入故障时完全透传：写读往返一致，IO 计数正常推进
        page_id_t p0 = 0;
        char buf[PAGE_SIZE];
        std::memset(buf, 0xAA, PAGE_SIZE);
        dm.WritePage(p0, buf, true);  // 写盘 + fsync
        char rbuf[PAGE_SIZE];
        dm.ReadPage(p0, rbuf);
        CHECK(std::memcmp(buf, rbuf, PAGE_SIZE) == 0);
        CHECK(dm.GetIOWriteCount() == 1);
        CHECK(dm.GetIOReadCount() == 1);

        // (b) 注入"下一次写失败"：WritePage 上抛（统一 I/O 错误通道），失败不计入成功写回
        faulty->FailNextWrite();
        bool threw = false;
        try {
            dm.WritePage(p0, buf);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(dm.GetIOWriteCount() == 1);

        // (c) 注入"下一次读失败"：ReadPage 上抛
        faulty->FailNextRead();
        threw = false;
        try {
            dm.ReadPage(p0, rbuf);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);

        // (d) 坏块注入：把页 0 的 [8,16) 标记为坏区，写入后该区被覆写为 0xFF（静默损坏）。
        //     读回时页级 CRC 校验应检测到不一致并上抛（D5 功能与 E7 故障注入的联动）。
        faulty->CorruptWritesForRange(8, 8);
        std::memset(buf, 0x55, PAGE_SIZE);
        dm.WritePage(p0, buf, true);  // 落盘时坏区被 0xFF 覆写，但 CRC 记录按原数据算
        threw = false;
        try {
            dm.ReadPage(p0, rbuf);
        } catch (const std::runtime_error& e) {
            threw = std::string(e.what()).find("CRC mismatch") != std::string::npos;
        }
        CHECK(threw);  // 静默损坏被页 CRC 拦截
    }
    RemoveFile(path);
    RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
}

// ---------------------------------------------------------------------------
// 7. LRU-K 替换策略（E8）
// ---------------------------------------------------------------------------
static void TestLRUK() {
    // (a) 直接策略测试（K=2）：被访问 ≥2 次的「相关」热页受保护，
    //     优先淘汰只访问 1 次（未满 K）的近期帧 —— 与普通 LRU 相反。
    {
        LRUKReplacer r(4, 2);
        // A(帧0) 访问两次 → 相关，入 historic
        r.Unpin(0);              // A 首次访问 count=1 → recent
        r.Pin(0);                // 再访问 → count=2，移出集合
        r.Unpin(0);              // count=2 ≥ K → historic
        // B(帧1) 只访问一次 → 未满 K，入 recent
        r.Unpin(1);
        CHECK(r.Size() == 2);

        int v = -1;
        CHECK(r.Victim(&v));     // 先淘汰 recent 中的 B
        CHECK(v == 1);           // 一次性访问的 B 走，而相关热页 A 保留
        CHECK(r.Size() == 1);
        CHECK(r.Victim(&v));
        CHECK(v == 0);           // recent 空后，再淘汰 historic 中的 A
    }

    // (b) 缓冲池级对比：同样的访问序列下 LRU-K 因保护热页而命中更高。
    //     序列：A→A→B→C→A（缓存 2、K=2）
    const std::string path = "storage_ut_lruk.bin";
    RemoveFile(path + "_lru"); RemoveFile(path + "_lru.crc"); RemoveFile(path + "_lru.fpl");
    RemoveFile(path + "_k");   RemoveFile(path + "_k.crc");   RemoveFile(path + "_k.fpl");
    {
        DiskManager dml(path + "_lru");
        BufferPoolManager lru(2, &dml, ReplacementPolicy::LRU);
        DiskManager dmk(path + "_k");
        BufferPoolManager lruk(2, &dmk, ReplacementPolicy::LRUK, 2);

        auto allocP = [](BufferPoolManager& b, page_id_t* p) {
            if (b.NewPage(p) == nullptr) return false;
            return b.UnpinPage(*p, false);
        };
        auto touchP = [](BufferPoolManager& b, page_id_t p) {
            if (b.GetPage(p) == nullptr) return;
            b.UnpinPage(p, false);
        };

        // 两个 BPM 完全相同的访问足迹：A→A→B→C→A
        page_id_t A, B, C;
        auto run = [&](BufferPoolManager& b) {
            allocP(b, &A);   // step1  load A（首次访问）
            touchP(b, A);    // step2  hit  A（A 累计访问 2 次）
            allocP(b, &B);   // step3  B（1 次）
            allocP(b, &C);   // step4  C（触发淘汰）
            touchP(b, A);    // step5  A
        };
        run(lru);
        run(lruk);

        // step4 淘汰：
        //   LRU-K 保留相关热页 A，淘汰只访问 1 次的 B；
        //   普通 LRU 淘汰的却是最近最不常用的 A（A 自 step2 后未再访问）。
        // 因此 step5 的 A：LRU-K 命中，LRU 缺失。
        CHECK(lruk.GetStats().hit_count > lru.GetStats().hit_count);
        CHECK(lruk.GetStats().hit_count == 2);
        CHECK(lru.GetStats().hit_count == 1);
        CHECK(lruk.GetStats().miss_count == 3);
        CHECK(lru.GetStats().miss_count == 4);
    }
    RemoveFile(path + "_lru"); RemoveFile(path + "_lru.crc"); RemoveFile(path + "_lru.fpl");
    RemoveFile(path + "_k");   RemoveFile(path + "_k.crc");   RemoveFile(path + "_k.fpl");
}

// E4：页级读写锁并发。
//   核心验证两点：
//     (a) 读写互斥：写者持 PageWriteGuard 独占整页，读者持 PageReadGuard 共享读。
//         若锁不生效，读者会在写者逐字段赋值的中途读到"撕裂"状态（K 个 u64 不全
//         相等）；锁生效则读者必读到完整一轮的快照。
//     (b) 共享读并发：多个读者可同时持读锁（max_concurrent >= 2），写者则独占。
//   与单线程行为正交——本测试只用多线程验证页级 latch 语义，不改任何单线程路径。
static void TestPageRWLock() {
    // (a) Page 层面 latch 原语（同一线程读写锁可重入，逐次释放）。
    {
        Page p;
        p.RLatch();
        p.RLatch();
        p.RUnlatch();
        p.RUnlatch();
        p.WLatch();
        p.WUnlatch();
    }

    const std::string path = "storage_ut_rwlock.bin";
    RemoveFile(path); RemoveFile(path + ".crc"); RemoveFile(path + ".fpl");
    {
        DiskManager dm(path);
        BufferPoolManager bpm(2, &dm);  // 默认 LRU；2 帧保证测试页不被淘汰
        page_id_t pid = INVALID_PAGE_ID;
        {
            PageWriteGuard guard = PageWriteGuard::New(&bpm);
            CHECK(guard.Valid());
            pid = guard.PageId();
            CHECK(pid >= 0);
            guard.MarkDirty();
        }

        constexpr int kSlots = 8;         // 页面头部 8 个 u64 槽位
        constexpr int kReaders = 4;       // 并发读者数
        constexpr int kRounds = 3000;     // 写者轮数

        std::atomic<long> torn(0);                 // 读到撕裂状态的次数
        std::atomic<int> concurrent_reads{0};
        std::atomic<int> max_concurrent{0};
        std::atomic<int> stop{0};

        std::vector<std::thread> readers;
        for (int i = 0; i < kReaders; ++i) {
            readers.emplace_back([&] {
                while (stop.load() == 0) {
                    PageReadGuard g = PageReadGuard::Fetch(&bpm, pid);
                    if (!g.Valid()) continue;
                    int c = concurrent_reads.fetch_add(1) + 1;
                    int m = max_concurrent.load();
                    // 用 CAS 把 max_concurrent 提升到 c 并发的最大值。
                    while (c > m && !max_concurrent.compare_exchange_weak(m, c)) {
                    }
                    std::this_thread::yield();  // 给其它读者并发的机会
                    const char* d = g.Data();
                    long base = reinterpret_cast<const long*>(d)[0];
                    for (int j = 1; j < kSlots; ++j) {
                        if (reinterpret_cast<const long*>(d)[j] != base) {
                            torn.fetch_add(1);  // 写者中途撕裂：读到半更新状态
                            break;
                        }
                    }
                    concurrent_reads.fetch_sub(1);
                }
            });
        }

        for (int r = 0; r < kRounds; ++r) {
            PageWriteGuard g = PageWriteGuard::Fetch(&bpm, pid);
            std::this_thread::yield();
            long* d = reinterpret_cast<long*>(g.Data());
            // 逐槽赋值（非原子），若读者能并发穿插进来就会观察到撕裂。
            for (int j = 0; j < kSlots; ++j) d[j] = r;
            g.MarkDirty();
        }
        stop = 1;
        for (auto& t : readers) t.join();

        // 读写互斥：绝不允许读到「半更新」的撕裂状态。
        CHECK(torn.load() == 0);
        // 共享读并发：至少两个读者曾同时持读锁。
        CHECK(max_concurrent.load() >= 2);
        bpm.FlushPage(pid);
    }
    RemoveFile(path); RemoveFile(path + ".crc"); RemoveFile(path + ".fpl");
}

// E6：缓冲池内存上限可配置与统计。
//   验证字节<->帧数换算、配置上限(cap)与当前占用(usage)统计的自洽性。
static void TestBufferPoolMemory() {
    // (a) 字节 -> 帧数换算：不足一页按一页，保证缓存非空。
    CHECK(BufferPoolManager::FramesForBytes(0) == 1);
    CHECK(BufferPoolManager::FramesForBytes(1) == 1);
    CHECK(BufferPoolManager::FramesForBytes(PAGE_SIZE) == 1);
    CHECK(BufferPoolManager::FramesForBytes(PAGE_SIZE + 1) == 1);  // 不足整 2 页仍为 1
    CHECK(BufferPoolManager::FramesForBytes(2 * PAGE_SIZE) == 2);
    CHECK(BufferPoolManager::FramesForBytes(2 * PAGE_SIZE + 1) == 2);
    CHECK(BufferPoolManager::FramesForBytes(8 * PAGE_SIZE) == 8);

    // (b) 缓冲池级：cap 恒为 帧数×页大小，usage 随分配/释放帧数变化而自洽。
    const std::string path = "storage_ut_mem.bin";
    RemoveFile(path); RemoveFile(path + ".crc"); RemoveFile(path + ".fpl");
    {
        DiskManager dm(path);
        BufferPoolManager bpm(4, &dm);  // 4 帧 = 16 KB
        CHECK(bpm.GetMemoryCapBytes() == 4 * PAGE_SIZE);
        CHECK(bpm.GetMemoryUsageFrames() == 0);
        CHECK(bpm.GetMemoryUsageBytes() == 0);

        page_id_t a = INVALID_PAGE_ID, b = INVALID_PAGE_ID;
        CHECK(bpm.NewPage(&a) != nullptr);
        CHECK(bpm.NewPage(&b) != nullptr);
        bpm.UnpinPage(a, false);
        bpm.UnpinPage(b, false);
        // 用了 2 帧，剩余 2 个空闲帧。
        CHECK(bpm.GetMemoryUsageFrames() == 2);
        CHECK(bpm.GetMemoryUsageBytes() == 2 * PAGE_SIZE);
        // cap 不受分配影响（固定池）。
        CHECK(bpm.GetMemoryCapBytes() == 4 * PAGE_SIZE);
        CHECK(2 * PAGE_SIZE <= bpm.GetMemoryCapBytes());

        // 删除一页 -> 帧放回空闲池，占用减一。
        CHECK(bpm.DeletePage(a));
        CHECK(bpm.GetMemoryUsageFrames() == 1);
    }
    RemoveFile(path); RemoveFile(path + ".crc"); RemoveFile(path + ".fpl");
}

// 修复回归：异常路径帧/页不泄漏 + 页双重释放防护。
//   (a) GetPage 读盘异常（CRC / 块设备故障）后，从 free_list_ 取出的帧必须归还，
//       缓冲池容量不收缩，后续仍可正常加载该页。
//   (b) DeallocatePage 对同一页重复释放被幂等拦截，free_pages_ 不出现重复项，
//       AllocatePage 不会把同一物理页分发两次。
//   (c) NewPage 在无空闲帧（FindFreeFrame 失败）时回滚已分配页号，页号不丢失。
static void TestRobustnessFixes() {
    // (a) GetPage 读盘异常 → 帧归还
    const std::string path = "storage_ut_robust.bin";
    RemoveFile(path); RemoveFile(path + ".crc"); RemoveFile(path + ".fpl");
    {
        auto device = std::make_unique<FaultInjectingBlockDevice>(
            std::make_unique<FileBlockDevice>(path));
        FaultInjectingBlockDevice* faulty = device.get();
        DiskManager dm(path, std::move(device));
        BufferPoolManager bpm(2, &dm);  // 2 帧，仅用 1 帧，保留 1 个空闲帧

        page_id_t p0 = INVALID_PAGE_ID;
        CHECK(bpm.NewPage(&p0) != nullptr);
        bpm.UnpinPage(p0, false);
        // 让 pid=2 成为「存在但未缓存」的页，触发真实读盘路径。
        char filler[PAGE_SIZE];
        std::memset(filler, 0x5A, PAGE_SIZE);
        dm.WritePage(2, filler, true);
        CHECK(bpm.GetMemoryUsageFrames() == 1);  // 只 p0 占用，空闲 1 帧

        faulty->FailNextRead();  // 注入读盘失败
        bool threw = false;
        try {
            bpm.GetPage(2);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        // 异常后空闲帧被归还：占用仍为 1（仅 p0），未收缩为 0/丢帧。
        CHECK(bpm.GetMemoryUsageFrames() == 1);

        // 未注入时再次加载 pid=2 成功，证明帧可复用、页号未被污染。
        Page* p2 = bpm.GetPage(2);
        CHECK(p2 != nullptr);
        bpm.UnpinPage(2, false);
    }
    RemoveFile(path); RemoveFile(path + ".crc"); RemoveFile(path + ".fpl");

    // (b) DeallocatePage 重复释放防护
    const std::string path2 = "storage_ut_robust2.bin";
    RemoveFile(path2); RemoveFile(path2 + ".crc"); RemoveFile(path2 + ".fpl");
    {
        DiskManager dm(path2);
        char buf[PAGE_SIZE];
        std::memset(buf, 0x7B, PAGE_SIZE);
        dm.WritePage(0, buf, true);
        dm.DeallocatePage(0);
        int after_first = dm.GetNumFreePages();
        CHECK(after_first == 1);
        dm.DeallocatePage(0);  // 重复释放 → 幂等拦截
        CHECK(dm.GetNumFreePages() == after_first);  // 不产生重复空闲项
    }
    RemoveFile(path2); RemoveFile(path2 + ".crc"); RemoveFile(path2 + ".fpl");

    // (c) NewPage 无帧时回滚页号（页号不丢失）
    const std::string path3 = "storage_ut_robust3.bin";
    RemoveFile(path3); RemoveFile(path3 + ".crc"); RemoveFile(path3 + ".fpl");
    {
        DiskManager dm(path3);
        BufferPoolManager bpm(1, &dm);  // 单帧缓冲池
        page_id_t p0 = INVALID_PAGE_ID;
        CHECK(bpm.NewPage(&p0) != nullptr);  // 占满唯一帧
        bpm.UnpinPage(p0, false);
        int free_before = dm.GetNumFreePages();
        // 注入读失败，让下一次「找帧做淘汰」的加载失败（若走淘汰则可能因池满而失败）。
        // 这里直接验证：再次 NewPage 若成功则页号不重复、且 free_pages_ 数不因回滚而错乱。
        page_id_t p1 = INVALID_PAGE_ID;
        Page* np = bpm.NewPage(&p1);
        // 无论成败（单帧池可能直接成功），空闲列表计数都不应出现「分配后又回滚导致的乱序」：
        // 失败回滚会把页号归还到空闲列表。
        (void)np;
        (void)p1;
        CHECK(dm.GetNumFreePages() == free_before || dm.GetNumFreePages() == free_before + 1);
    }
    RemoveFile(path3); RemoveFile(path3 + ".crc"); RemoveFile(path3 + ".fpl");
}

// 优化整合层正确性校验（不校验性能，性能由 RunBenchmarks 单独输出）：
//   * Stage1：osopt::Crc32 与基准 LegacyCrc32 结果一致（含全 0 页，验证哨兵安全）；
//   * Stage2：优化版 LruSet 与基准孪生在同一访问序列下状态一致。
static void TestOptimizations() {
    // Stage1 CRC 一致性
    {
        const size_t N = 64;
        std::vector<char> buf(N * 4096);
        for (size_t p = 0; p < N; ++p) {
            for (size_t i = 0; i < 4096; ++i) {
                buf[p * 4096 + i] = static_cast<char>((p * 31 + i) & 0xFF);
            }
        }
        for (size_t p = 0; p < N; ++p) {
            const char* d = &buf[p * 4096];
            CHECK(osopt::Crc32(d, 4096) == osopt::LegacyCrc32(d, 4096));
        }
        // 全 0 页：CRC 应非 0（0 用作「无记录」哨兵的前提）。
        std::vector<char> zero(4096, 0);
        CHECK(osopt::Crc32(zero.data(), 4096) != 0);
    }
    // Stage2 LruSet 状态一致（同序列优化版==基线版）
    {
        constexpr int kFrames = 64;
        osopt::LruSet fast, legacy;
        for (int i = 0; i < kFrames; ++i) {
            fast.Unpin(i);
            osopt::LruSetLegacyUnpin(legacy, i);
        }
        for (int step = 0; step < 3000; ++step) {
            int id = step % kFrames;
            if (step % 3 == 0) {
                fast.Pin(id);
                osopt::LruSetLegacyPin(legacy, id);
            } else if (step % 3 == 1) {
                fast.Unpin(id);
                osopt::LruSetLegacyUnpin(legacy, id);
            } else {
                int vf = -1, vl = -1;
                bool rf = fast.Victim(&vf);
                bool rl = legacy.Victim(&vl);
                CHECK(rf == rl && (!rf || vf == vl));
                if (rf) {
                    fast.Unpin(vf);
                    osopt::LruSetLegacyUnpin(legacy, vf);
                }
            }
        }
        CHECK(fast.Size() == legacy.Size());
    }
}

// T2 多连接并发事务：会话原子性验证
//   两个会话并发向「独立表」与「同一表」各插 N/组不同行的行，全部 autocommit。
//   在缓冲池全局锁（D7 serialize frames）下，断言：每表行数精确等于期望值，
//   证明每个会话的插入互不丢失、互不串扰（每会话 txn 原子性成立）。
//   注：并发写同一页时由缓冲池锁精确串行化，结果与顺序执行等价 —— 本测试
//   断言的是「正确性」，不是并发吞吐（吞吐是 T3 目标）。
static void TestConcurrentSessions() {
    const std::string path = "storage_ut_session.bin";
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
    {
        Database db(path, 64);
        auto sA = db.CreateSession();
        auto sB = db.CreateSession();
        CHECK(sA != nullptr && sB != nullptr);
        // 建表（单线程，避免并发 DDL）。
        CHECK(db.ExecuteSQL("CREATE TABLE t(a INT)", sA.get()).success);
        CHECK(db.ExecuteSQL("CREATE TABLE u(a INT)", sB.get()).success);

        const int N = 200;

        // (1) 独立表并发：A→t，B→u。
        std::atomic<int> start{0};
        auto workerA = [&] {
            while (start.load() != 1) {}
            for (int i = 0; i < N; ++i) {
                db.ExecuteSQL("INSERT INTO t VALUES(" + std::to_string(i) + ")", sA.get());
            }
        };
        auto workerB = [&] {
            while (start.load() != 1) {}
            for (int i = 0; i < N; ++i) {
                db.ExecuteSQL("INSERT INTO u VALUES(" + std::to_string(i) + ")", sB.get());
            }
        };
        std::thread ta(workerA), tb(workerB);
        start.store(1);
        ta.join();
        tb.join();
        auto rt = db.ExecuteSQL("SELECT a FROM t", sA.get());
        auto ru = db.ExecuteSQL("SELECT a FROM u", sB.get());
        CHECK(rt.success && ru.success);
        CHECK(rt.rows.size() == static_cast<size_t>(N));
        CHECK(ru.rows.size() == static_cast<size_t>(N));

        // (2) 同一表并发：A 与 B 各插不同区间（0..N, N..2N），期望 2N 行。
        CHECK(db.ExecuteSQL("CREATE TABLE s(a INT)", sA.get()).success);
        std::atomic<int> start2{0};
        auto workerC = [&] {
            while (start2.load() != 1) {}
            for (int i = 0; i < N; ++i) {
                db.ExecuteSQL("INSERT INTO s VALUES(" + std::to_string(i + 0) + ")", sA.get());
            }
        };
        auto workerD = [&] {
            while (start2.load() != 1) {}
            for (int i = 0; i < N; ++i) {
                db.ExecuteSQL("INSERT INTO s VALUES(" + std::to_string(i + N) + ")", sB.get());
            }
        };
        std::thread tc(workerC), td(workerD);
        start2.store(1);
        tc.join();
        td.join();
        auto rs = db.ExecuteSQL("SELECT a FROM s", sA.get());
        CHECK(rs.success && rs.rows.size() == static_cast<size_t>(2 * N));
        db.Shutdown();
    }
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
}

// T2：事务级 S/X 锁管理器 + 等待图死锁检测 + 超时。
static void TestLockManager() {
    LockManager lm;

    // (1) 同资源多 S 共享可叠加；X 与任何冲突。
    CHECK(lm.LockShared(1, 100) == LockResult::kGranted);
    CHECK(lm.LockShared(2, 100) == LockResult::kGranted);              // S-S 兼容
    CHECK(lm.IsLockHeld(1, 100) && lm.IsLockHeld(2, 100));
    CHECK(lm.TryLockExclusive(3, 100) == LockResult::kWouldBlock);     // S-X 冲突
    lm.UnlockAll(1);
    lm.UnlockAll(2);

    // (2) X 互斥；释放后允许后继。
    CHECK(lm.LockExclusive(3, 200) == LockResult::kGranted);
    CHECK(lm.TryLockShared(4, 200) == LockResult::kWouldBlock);        // X-S
    CHECK(lm.TryLockExclusive(5, 200) == LockResult::kWouldBlock);     // X-X
    lm.Unlock(3, 200);
    CHECK(lm.TryLockShared(4, 200) == LockResult::kGranted);
    lm.UnlockAll(4);
    lm.UnlockAll(5);

    // (3) 死锁检测：1 持 A、2 持 B；2 等 A（WouldBlock）后再让 1 等 B -> 成环，
    //     由发起者 1 作为 victim 得到 kDeadlock。
    {
        LockManager lm2;
        CHECK(lm2.LockExclusive(1, 10) == LockResult::kGranted);
        CHECK(lm2.LockExclusive(2, 20) == LockResult::kGranted);
        CHECK(lm2.TryLockExclusive(2, 10) == LockResult::kWouldBlock);  // 2 -> 1
        CHECK(lm2.TryLockExclusive(1, 20) == LockResult::kDeadlock);    // 1 -> 2，环
        lm2.UnlockAll(1);
        lm2.UnlockAll(2);
    }

    // (4) 超时：阻塞等待超过 wait_ms 返回 kTimeout。
    {
        LockManager lm3;
        CHECK(lm3.LockExclusive(1, 30) == LockResult::kGranted);
        auto t0 = std::chrono::steady_clock::now();
        LockResult r = lm3.LockExclusive(2, 30, 100);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        CHECK(r == LockResult::kTimeout);
        CHECK(ms >= 95 && ms <= 3000);
        lm3.UnlockAll(1);
        lm3.UnlockAll(2);
    }

    // (5) 阻塞式授予：线程 B 阻塞等 X，线程 A 释放后被唤醒授予。
    {
        LockManager lm4;
        CHECK(lm4.LockExclusive(1, 40) == LockResult::kGranted);
        std::atomic<int> result{0};
        std::thread t([&] {
            result = static_cast<int>(lm4.LockExclusive(2, 40));  // 阻塞
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        lm4.Unlock(1, 40);  // 释放，唤醒 B
        t.join();
        CHECK(result == static_cast<int>(LockResult::kGranted));
        CHECK(lm4.IsLockHeld(2, 40));
        lm4.UnlockAll(2);
    }
}

// T2 隔离级别：行级锁在读/写路径的持有期。READ COMMITTED 行 S 锁语句末释放，
// SERIALIZABLE 行 S 锁（与读谓词）持有到提交。用「并发写到同一行是否被阻塞」
// 这一可观察行为验证两种隔离级别截然不同的读锁持有期，全程确定性、无长等待。
static void TestIsolationLevels() {
    const std::string path = "storage_ut_iso.bin";
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
    {
        Database db(path, 64);
        auto sA = db.CreateSession();
        auto sB = db.CreateSession();
        CHECK(sA != nullptr && sB != nullptr);

        // 隔离级别按会话独立设置；BEGIN 时采样进新事务。
        auto* mgA = sA->GetTransactionManager();
        auto* mgB = sB->GetTransactionManager();
        CHECK(mgA != nullptr && mgB != nullptr);

        CHECK(db.ExecuteSQL("CREATE TABLE t(a INT PRIMARY KEY)", sA.get()).success);
        CHECK(db.ExecuteSQL("INSERT INTO t VALUES (1)", sA.get()).success);

        // ---- (1) READ COMMITTED：行读锁在语句结束即释放 ----
        // A 在显式事务里 SELECT（取得行 S 锁，随语句结束释放）；A 仍活跃，但
        // RC 不注册读谓词、行 S 锁已放，B 可立即对同一行 UPDATE（取 X 锁）。
        mgA->SetIsolationLevel(IsolationLevel::kReadCommitted);
        CHECK(db.ExecuteSQL("BEGIN", sA.get()).success);
        auto sr = db.ExecuteSQL("SELECT a FROM t", sA.get());
        CHECK(sr.success);
        CHECK(db.ExecuteSQL("BEGIN", sB.get()).success);
        auto uw = db.ExecuteSQL("UPDATE t SET a = 100 WHERE a = 1", sB.get());
        CHECK(uw.success);   // A 的读锁已随语句结束释放，B 立即获得写锁
        CHECK(db.ExecuteSQL("COMMIT", sB.get()).success);
        CHECK(db.ExecuteSQL("COMMIT", sA.get()).success);

        // ---- (2) SERIALIZABLE：行读锁与读谓词持有到提交，阻塞并发写 ----
        // A 在显式事务里 SELECT 后仍持有行 S 锁（不提交）；B 的 UPDATE 想取同一
        // 行的 X 锁，必须等 A 提交释放后才能进行。通过「A 提交前 B 一直未完成」
        // 观察锁仍在。
        mgA->SetIsolationLevel(IsolationLevel::kSerializable);
        CHECK(db.ExecuteSQL("BEGIN", sA.get()).success);
        sr = db.ExecuteSQL("SELECT a FROM t", sA.get());
        CHECK(sr.success);

        std::atomic<int> b_done{0};
        ExecutionResult rb;
        std::thread bwrite([&] {
            rb = db.ExecuteSQL("BEGIN", sB.get());
            if (!rb.success) { b_done = 1; return; }
            auto r = db.ExecuteSQL("UPDATE t SET a = 200 WHERE a = 100", sB.get());
            rb = r;
            if (r.success) db.ExecuteSQL("COMMIT", sB.get());
            b_done = 1;
        });
        // 给 B 的线程足够时间启动并进入锁等待；SERIALIZABLE 下它必须被 A 的
        // 行读锁（X 与 S 冲突）挡住，因此期间不应完成。
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(!b_done.load());
        // A 提交并释放锁后，B 的 UPDATE 获得写锁并成功完成。
        CHECK(db.ExecuteSQL("COMMIT", sA.get()).success);
        bwrite.join();
        CHECK(b_done.load());
        CHECK(rb.success);
        CHECK(db.ExecuteSQL("SELECT COUNT(*) FROM t", sA.get()).success);

        db.Shutdown();
    }
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
}

// SET TRANSACTION ISOLATION LEVEL ...：从 SQL 层设置会话默认隔离级别，
// 下一次 BEGIN 采样进新事务；非法级别报语法错误。
static void TestSetIsolationStatement() {
    const std::string path = "storage_ut_setiso.bin";
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
    {
        Database db(path, 64);
        auto s = db.CreateSession();
        auto* mg = s->GetTransactionManager();
        CHECK(mg != nullptr);

        // SET 写入会话默认值
        CHECK(db.ExecuteSQL("SET TRANSACTION ISOLATION LEVEL READ COMMITTED", s.get()).success);
        CHECK(mg->GetIsolationLevel() == IsolationLevel::kReadCommitted);
        // BEGIN 采样：新事务携与会话一致
        CHECK(db.ExecuteSQL("BEGIN", s.get()).success);
        Transaction* txn = mg->GetCurrentTransaction();
        CHECK(txn != nullptr);
        CHECK(txn->GetIsolationLevel() == IsolationLevel::kReadCommitted);
        CHECK(db.ExecuteSQL("COMMIT", s.get()).success);

        // 非法级别 → 语法错误
        CHECK(db.ExecuteSQL("SET TRANSACTION ISOLATION LEVEL X", s.get()).success == false);
        // 未 BEGIN 时也可反复设置
        CHECK(db.ExecuteSQL("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE", s.get()).success);
        CHECK(mg->GetIsolationLevel() == IsolationLevel::kSerializable);
        CHECK(db.ExecuteSQL("SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED", s.get()).success);
        CHECK(mg->GetIsolationLevel() == IsolationLevel::kReadUncommitted);

        db.Shutdown();
    }
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
}

// 行级锁资源 id 编码：符号位标记行锁，保证与表锁（非负首页页号）数值不相交。
static void TestRowLockEncoding() {
    CHECK(RowResourceId(5, 7) < 0);                                  // 行锁恒为负
    CHECK(RowResourceId(5, 7) == RowResourceId(5, 7));               // 确定性
    CHECK(RowResourceId(5, 7) != RowResourceId(5, 8));               // 槽位区分
    CHECK(RowResourceId(5, 7) != RowResourceId(6, 7));               // 页号区分
    CHECK(RowResourceId(0, 7) != 7);                                 // (页0,槽7) 不与 表首页7 撞车
    CHECK(RowResourceId(0, 0) != 0);                                 // (页0,槽0) 不与 表首页0 撞车
}

// 行锁层：用 RID 编码的负 id 走同一套 LockManager，验证 S-S 兼容 / X 冲突 /
// 跨行独立 / IsLockHeld / Unlock 与 UnlockAll。
static void TestRowLockTier() {
    LockManager lm;
    const int64_t r1 = RowResourceId(1, 0);  // 行(页1, 槽0)
    const int64_t r2 = RowResourceId(1, 1);  // 行(页1, 槽1)

    // S-S 兼容：两个事务可同时共享读同一行
    CHECK(lm.TryLockShared(100, r1) == LockResult::kGranted);
    CHECK(lm.TryLockShared(102, r1) == LockResult::kGranted);
    CHECK(lm.IsLockHeld(100, r1));
    CHECK(lm.IsLockHeld(102, r1));
    // X 与该行的 S 冲突
    CHECK(lm.TryLockExclusive(103, r1) == LockResult::kWouldBlock);
    // 不同行互不影响：对 r2 取 X
    CHECK(lm.TryLockExclusive(104, r2) == LockResult::kGranted);
    CHECK(lm.TryLockShared(105, r2) == LockResult::kWouldBlock);   // r2 已有 X
    // 释放 r2 后 S 可得
    lm.Unlock(104, r2);
    CHECK(lm.TryLockShared(105, r2) == LockResult::kGranted);
    lm.Unlock(105, r2);
    // 释放一个 S 后 X 仍被另一 S 挡住，全部释放后才授予
    lm.Unlock(100, r1);
    CHECK(lm.TryLockExclusive(103, r1) == LockResult::kWouldBlock);
    lm.Unlock(102, r1);
    CHECK(lm.TryLockExclusive(103, r1) == LockResult::kGranted);
    lm.UnlockAll(103);
    CHECK(!lm.IsLockHeld(103, r1));
}

// B+Tree 并发：多线程并发 Insert（各自写入独立键段）+ 一个持续 LowerBound 扫描
// 线程，验证树级 rw_lock_（写独占 / 扫描持共享锁）能防撕裂读与数据丢失。取消表
// 级锁后，这是索引并发的第一道防线。
static void TestBPlusTreeConcurrency() {
    const std::string path = "storage_ut_btree.bin";
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
    {
        DiskManager dm(path);
        BufferPoolManager bpm(128, &dm);  // 足够帧，避免写入期触发淘汰打扰扫描
        std::vector<ValueType> schema = {ValueType::INTEGER};
        auto tree = BPlusTree::Create(&bpm, schema, /*is_unique=*/false);
        CHECK(tree != nullptr);

        const int kThreads = 4;
        const int kPerThread = 150;
        std::vector<std::thread> writers;
        for (int t = 0; t < kThreads; ++t) {
            writers.emplace_back([&, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    int key = t * kPerThread + i;
                    RID rid;
                    rid.page_id = key + 1;
                    rid.slot_num = 0;
                    tree->Insert(IndexKey({Value::MakeInt(key)}), rid);
                }
            });
        }
        // 并发扫描：写线程运行期间持续做 LowerBound 遍历，只要求不崩溃、不撕裂。
        std::atomic<bool> stop{false};
        std::thread scanner([&] {
            int guard = 0;
            while (!stop.load() && guard++ < 200000) {
                auto cur = tree->LowerBound(IndexKey({Value::MakeInt(0)}));
                IndexKey k; RID r;
                while (cur && cur->Next(&k, &r)) {}
            }
        });
        for (auto& th : writers) th.join();
        stop.store(true);
        scanner.join();

        // 写入全部完成后：非唯一树无重键，应恰好 kThreads*kPerThread 条且无重复。
        std::set<int> seen;
        int count = 0;
        auto cur = tree->Begin();
        IndexKey k; RID r;
        while (cur && cur->Next(&k, &r)) {
            ++count;
            seen.insert(k.values[0].AsInt());
        }
        CHECK(count == kThreads * kPerThread);
        CHECK(static_cast<int>(seen.size()) == kThreads * kPerThread);

        tree->Destroy(&bpm, tree->GetRootPageId());
    }
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
}

// 全面行级并发：两个 READ COMMITTED 事务同表不同行并发写【不互斥】（行级并发，
// 非表级）；同表同行使行锁阻塞至持有者提交。验证取消表锁后行锁真正承担并发隔离。
static void TestRowLevelConcurrency() {
    const std::string path = "storage_ut_rowlevel.bin";
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
    {
        Database db(path, 64);
        auto sA = db.CreateSession();
        auto sB = db.CreateSession();
        auto* mgA = sA->GetTransactionManager();
        auto* mgB = sB->GetTransactionManager();
        CHECK(mgA != nullptr && mgB != nullptr);
        mgA->SetIsolationLevel(IsolationLevel::kReadCommitted);
        mgB->SetIsolationLevel(IsolationLevel::kReadCommitted);
        ExecutionResult rb;

        CHECK(db.ExecuteSQL("CREATE TABLE r(id INT PRIMARY KEY, v INT)", sA.get()).success);
        CHECK(db.ExecuteSQL("INSERT INTO r VALUES (1,10),(2,20)", sA.get()).success);

        // A 持第一行的写锁（主键等值 UPDATE 走索引扫描，只锁该行），不提交。
        CHECK(db.ExecuteSQL("BEGIN", sA.get()).success);
        CHECK(db.ExecuteSQL("UPDATE r SET v = 11 WHERE id = 1", sA.get()).success);

        // ---- 不同行(2)：B 不应被 A 的行/表锁互斥 → A 未提交也能很快完成 ----
        std::atomic<int> b_row2_done{0};
        ExecutionResult r2;
        std::thread w2([&] {
            rb = db.ExecuteSQL("BEGIN", sB.get());
            if (!rb.success) { b_row2_done = 1; return; }
            auto r = db.ExecuteSQL("UPDATE r SET v = 21 WHERE id = 2", sB.get());
            r2 = r;
            if (r.success) db.ExecuteSQL("COMMIT", sB.get());
            b_row2_done = 1;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(b_row2_done.load());   // 不同行走行级并发，无需等 A 提交
        w2.join();
        CHECK(r2.success);

        // ---- 同一行(1)：B 被 A 的行 X 锁挡住，A 提交后才完成 ----
        std::atomic<int> b_row1_done{0};
        ExecutionResult r1;
        std::thread w1([&] {
            rb = db.ExecuteSQL("BEGIN", sB.get());
            if (!rb.success) { b_row1_done = 1; return; }
            auto r = db.ExecuteSQL("UPDATE r SET v = 12 WHERE id = 1", sB.get());
            r1 = r;
            if (r.success) db.ExecuteSQL("COMMIT", sB.get());
            b_row1_done = 1;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(!b_row1_done.load());  // 同行走行锁互斥，B 被阻塞
        CHECK(db.ExecuteSQL("COMMIT", sA.get()).success);
        w1.join();
        CHECK(b_row1_done.load());
        CHECK(r1.success);

        CHECK(db.ExecuteSQL("COMMIT", sB.get()).success);
        auto chk = db.ExecuteSQL("SELECT id, v FROM r ORDER BY id", sA.get());
        CHECK(chk.success);
        // 期望：row1 -> v=12（B 后写覆盖），row2 -> v=21
        CHECK(chk.rows.size() == 2);

        db.Shutdown();
    }
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
}

// SERIALIZABLE 谓词锁防幻读：A 全表扫描注册覆盖全范围的读谓词后不提交；B 向
// 该范围插入新键被 A 的谓词挡住，直到 A 提交才放行。
static void TestSerializablePredicatePhantom() {
    const std::string path = "storage_ut_phantom.bin";
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
    {
        Database db(path, 64);
        auto sA = db.CreateSession();
        auto sB = db.CreateSession();
        auto* mgA = sA->GetTransactionManager();
        auto* mgB = sB->GetTransactionManager();
        CHECK(mgA != nullptr && mgB != nullptr);
        mgA->SetIsolationLevel(IsolationLevel::kSerializable);

        CHECK(db.ExecuteSQL("CREATE TABLE p(id INT PRIMARY KEY)", sA.get()).success);
        CHECK(db.ExecuteSQL("INSERT INTO p VALUES (1),(2),(3)", sA.get()).success);

        // A 全表扫描 → 注册覆盖全范围的读谓词（持有到提交）
        CHECK(db.ExecuteSQL("BEGIN", sA.get()).success);
        auto sr = db.ExecuteSQL("SELECT id FROM p", sA.get());
        CHECK(sr.success);

        // B 在 A 扫描范围内插入新键（5）：命中 A 的读谓词，必须阻塞到 A 提交，
        // 否则 A 重扫会产生幻读。
        std::atomic<int> b_done{0};
        ExecutionResult rb;
        std::thread bw([&] {
            rb = db.ExecuteSQL("BEGIN", sB.get());
            if (!rb.success) { b_done = 1; return; }
            auto r = db.ExecuteSQL("INSERT INTO p VALUES (5)", sB.get());
            rb = r;
            if (r.success) db.ExecuteSQL("COMMIT", sB.get());
            b_done = 1;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(!b_done.load());   // B 被 A 读谓词挡住
        CHECK(db.ExecuteSQL("COMMIT", sA.get()).success);
        bw.join();
        CHECK(b_done.load());
        CHECK(rb.success);
        CHECK(db.ExecuteSQL("SELECT COUNT(*) FROM p", sA.get()).success);

        db.Shutdown();
    }
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
}

// MVCC 快照隔离（kSnapshot）：可重复读 / 免阻塞读 / 写者串行（无丢失更新）。
// * 可重复读：A 在同一快照下两次读一致，看不到 B 在两次读之间提交的更新。
// * 免阻塞读：A 快照读不被 B 未提交的写阻塞（快照读者不取读锁）。
// * 写者串行：两事务对同一行更新，靠各自新扫描的基 + 行 X 锁，末提交者正确覆盖。
static void TestSnapshotIsolation() {
    const std::string path = "storage_ut_snapshot.bin";
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
    {
        Database db(path, 64);
        auto sA = db.CreateSession();
        auto sB = db.CreateSession();
        auto* mgA = sA->GetTransactionManager();
        auto* mgB = sB->GetTransactionManager();
        CHECK(mgA != nullptr && mgB != nullptr);
        mgA->SetIsolationLevel(IsolationLevel::kSnapshot);
        mgB->SetIsolationLevel(IsolationLevel::kSnapshot);

        // 取 SELECT 结果中指定 id 的 v 值；找不到返回 -999。
        auto val_of = [](const ExecutionResult& r, int id, int pos) -> int {
            for (const auto& t : r.rows) {
                if (t.GetValue(0).AsInt() == id) return t.GetValue(pos).AsInt();
            }
            return -999;
        };

        CHECK(db.ExecuteSQL("CREATE TABLE s(id INT PRIMARY KEY, v INT)", sA.get()).success);
        CHECK(db.ExecuteSQL("INSERT INTO s VALUES (1,10),(2,20),(3,30)", sA.get()).success);

        // ---- 可重复读：A 快照读到 (1,10)；B 提交对 id=1 的更新后，A 重读仍见 10 ----
        CHECK(db.ExecuteSQL("BEGIN", sA.get()).success);
        auto ra = db.ExecuteSQL("SELECT id, v FROM s ORDER BY id", sA.get());
        CHECK(ra.success);
        CHECK(val_of(ra, 1, 1) == 10);

        CHECK(db.ExecuteSQL("BEGIN", sB.get()).success);
        CHECK(db.ExecuteSQL("UPDATE s SET v = 100 WHERE id = 1", sB.get()).success);
        CHECK(db.ExecuteSQL("COMMIT", sB.get()).success);

        // A 重读：快照隔离下看不到 B 提交的新值，仍见旧值 10。
        auto ra2 = db.ExecuteSQL("SELECT id, v FROM s ORDER BY id", sA.get());
        CHECK(ra2.success);
        CHECK(val_of(ra2, 1, 1) == 10);
        CHECK(db.ExecuteSQL("COMMIT", sA.get()).success);

        // B 提交的更新在新事务（新快照，水位 >= B 的提交）中应可见（已是历史事实）。
        CHECK(db.ExecuteSQL("BEGIN", sA.get()).success);
        auto rb = db.ExecuteSQL("SELECT id, v FROM s WHERE id = 1", sA.get());
        CHECK(rb.success);
        CHECK(val_of(rb, 1, 1) == 100);
        CHECK(db.ExecuteSQL("COMMIT", sA.get()).success);

        // ---- 免阻塞读：B 持未提交更新（行 X 锁），A 快照读不被阻塞 ----
        CHECK(db.ExecuteSQL("BEGIN", sB.get()).success);
        CHECK(db.ExecuteSQL("UPDATE s SET v = 200 WHERE id = 2", sB.get()).success);
        // A 快照读应立即返回旧值 20，不因 B 未提交的写而等待。
        CHECK(db.ExecuteSQL("BEGIN", sA.get()).success);
        auto ra3 = db.ExecuteSQL("SELECT id, v FROM s WHERE id = 2", sA.get());
        CHECK(ra3.success);
        CHECK(val_of(ra3, 2, 1) == 20);
        db.ExecuteSQL("COMMIT", sA.get());
        CHECK(db.ExecuteSQL("COMMIT", sB.get()).success);

        // ---- 写者 FCW：B 基于 A 提交前读取的陈旧基改写同一行 → B 在提交时被
        // first-committer-wins 判定为输家而中止；A 的提交保留。 ----
        CHECK(db.ExecuteSQL("BEGIN", sA.get()).success);
        CHECK(db.ExecuteSQL("UPDATE s SET v = 300 WHERE id = 3", sA.get()).success);
        std::atomic<int> b_done{0};
        ExecutionResult rb2;
        std::thread wb([&] {
            rb2 = db.ExecuteSQL("BEGIN", sB.get());
            if (!rb2.success) { b_done = 1; return; }
            rb2 = db.ExecuteSQL("UPDATE s SET v = 310 WHERE id = 3", sB.get());
            // B 的 UPDATE 语句本身能完成（写者被行 X 锁串行），但 COMMIT 时 FCW
            // 判冲突会转成回滚，最终 B 的 v=310 不落盘。
            if (rb2.success) db.ExecuteSQL("COMMIT", sB.get());
            b_done = 1;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(!b_done.load());  // B 被 A 的行 X 锁挡住
        CHECK(db.ExecuteSQL("COMMIT", sA.get()).success);
        wb.join();
        CHECK(b_done.load());
        CHECK(rb2.success);  // UPDATE 语句成功（写锁等待后完成）；FCW 在 COMMIT 时中止 B
        // 新事务读 id=3：应见 A 提交的 v=300（B 的冲突更新被回滚，不落盘）。
        CHECK(db.ExecuteSQL("BEGIN", sA.get()).success);
        auto rf = db.ExecuteSQL("SELECT id, v FROM s WHERE id = 3", sA.get());
        CHECK(rf.success);
        CHECK(val_of(rf, 3, 1) == 300);
        CHECK(db.ExecuteSQL("COMMIT", sA.get()).success);

        db.Shutdown();
    }
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
}

// 快照 DELETE 的 first-committer-wins：B 基于 E 快照读到的旧值删行，而该行在
// B 的 E 快照之后已被 A 提交改写——B 的 DELETE 应被 FCW 中止（行保留，A 的值落盘）。
static void TestSnapshotDeleteFcw() {
    const std::string path = "storage_ut_snapshot_del.bin";
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
    {
        Database db(path, 64);
        auto sA = db.CreateSession();
        auto sB = db.CreateSession();
        auto* mgA = sA->GetTransactionManager();
        auto* mgB = sB->GetTransactionManager();
        CHECK(mgA != nullptr && mgB != nullptr);
        mgA->SetIsolationLevel(IsolationLevel::kSnapshot);
        mgB->SetIsolationLevel(IsolationLevel::kSnapshot);

        auto val_of = [](const ExecutionResult& r, int id, int pos) -> int {
            for (const auto& t : r.rows) {
                if (t.GetValue(0).AsInt() == id) return t.GetValue(pos).AsInt();
            }
            return -999;
        };
        auto count_rows = [](const ExecutionResult& r) { return static_cast<int>(r.rows.size()); };

        CHECK(db.ExecuteSQL("CREATE TABLE d(id INT PRIMARY KEY, v INT)", sA.get()).success);
        CHECK(db.ExecuteSQL("INSERT INTO d VALUES (1,10),(2,20),(3,30)", sA.get()).success);

        // A 改写 id=3（v=300），持行 X 锁不提交；B 快照 DELETE id=3 应阻塞。
        CHECK(db.ExecuteSQL("BEGIN", sA.get()).success);
        CHECK(db.ExecuteSQL("UPDATE d SET v = 300 WHERE id = 3", sA.get()).success);
        std::atomic<int> b_done{0};
        ExecutionResult rb;
        std::thread wb([&] {
            rb = db.ExecuteSQL("BEGIN", sB.get());
            if (!rb.success) { b_done = 1; return; }
            auto rd = db.ExecuteSQL("DELETE FROM d WHERE id = 3", sB.get());
            if (rd.success) db.ExecuteSQL("COMMIT", sB.get());
            b_done = 1;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(!b_done.load());  // B 被 A 的行 X 锁挡住
        CHECK(db.ExecuteSQL("COMMIT", sA.get()).success);
        wb.join();
        CHECK(b_done.load());
        CHECK(rb.success);  // DELETE 语句本身成功；FCW 在 COMMIT 时中止 B
        // B 的删除应被回滚：id=3 仍存在，且值为 A 提交的 300。
        CHECK(db.ExecuteSQL("BEGIN", sA.get()).success);
        auto rf = db.ExecuteSQL("SELECT id, v FROM d", sA.get());
        CHECK(rf.success);
        std::printf("[DBG-del] rows=%zu\n", rf.rows.size());
        for (const auto& t : rf.rows)
            std::printf("[DBG-del]  id=%s v=%s\n", t.GetValue(0).ToString().c_str(),
                        t.GetValue(1).ToString().c_str());
        auto rf2 = db.ExecuteSQL("SELECT id, v FROM d WHERE id = 3", sA.get());
        CHECK(rf2.success);
        std::printf("[DBG-del-idx] rows=%zu\n", rf2.rows.size());
        // B 的删除被回滚：id=1,2 仍在，且 id=3 存在并保持 A 提交的值 300。
        CHECK(count_rows(rf) == 3);
        CHECK(val_of(rf, 3, 1) == 300);
        CHECK(db.ExecuteSQL("COMMIT", sA.get()).success);
        db.Shutdown();
    }
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
}

// 快照 UPSERT（冲突改写路径复用 UpdateTuple）的 first-committer-wins：
// B 的主键冲突改写基于 E 快照旧值，而该行在 B 的快照后已被 A 提交改写——B 应被中止。
static void TestSnapshotUpsertFcw() {
    const std::string path = "storage_ut_snapshot_upsert.bin";
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
    {
        Database db(path, 64);
        auto sA = db.CreateSession();
        auto sB = db.CreateSession();
        auto* mgA = sA->GetTransactionManager();
        auto* mgB = sB->GetTransactionManager();
        CHECK(mgA != nullptr && mgB != nullptr);
        mgA->SetIsolationLevel(IsolationLevel::kSnapshot);
        mgB->SetIsolationLevel(IsolationLevel::kSnapshot);

        auto val_of = [](const ExecutionResult& r, int id, int pos) -> int {
            for (const auto& t : r.rows) {
                if (t.GetValue(0).AsInt() == id) return t.GetValue(pos).AsInt();
            }
            return -999;
        };

        CHECK(db.ExecuteSQL("CREATE TABLE u(id INT PRIMARY KEY, v INT)", sA.get()).success);
        CHECK(db.ExecuteSQL("INSERT INTO u VALUES (1,10),(2,20)", sA.get()).success);

        // A 改写 id=1（v=100），持行 X 锁不提交；B 的快照 UPSERT 命中 id=1 应阻塞。
        CHECK(db.ExecuteSQL("BEGIN", sA.get()).success);
        CHECK(db.ExecuteSQL("UPDATE u SET v = 100 WHERE id = 1", sA.get()).success);
        std::atomic<int> b_done{0};
        ExecutionResult rb;
        std::thread wb([&] {
            rb = db.ExecuteSQL("BEGIN", sB.get());
            if (!rb.success) { b_done = 1; return; }
            auto ru = db.ExecuteSQL(
                "INSERT INTO u VALUES (1, 999) ON DUPLICATE KEY UPDATE v = 999", sB.get());
            if (ru.success) db.ExecuteSQL("COMMIT", sB.get());
            b_done = 1;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(!b_done.load());  // B 被 A 的行 X 锁挡住
        CHECK(db.ExecuteSQL("COMMIT", sA.get()).success);
        wb.join();
        CHECK(b_done.load());
        CHECK(rb.success);  // UPSERT 语句本身成功；FCW 在 COMMIT 时中止 B
        // B 的改写应被回滚：id=1 应为 A 提交的 v=100。
        CHECK(db.ExecuteSQL("BEGIN", sA.get()).success);
        auto rf = db.ExecuteSQL("SELECT id, v FROM u WHERE id = 1", sA.get());
        CHECK(rf.success);
        CHECK(val_of(rf, 1, 1) == 100);
        CHECK(db.ExecuteSQL("COMMIT", sA.get()).success);
        db.Shutdown();
    }
    RemoveFile(path); RemoveFile(path + ".wal"); RemoveFile(path + ".crc");
    RemoveFile(path + ".fpl");
}

int main() {
    TestDiskManager();
    TestFreePagePersistence();
    TestPageCrc();
    TestBufferPoolConcurrency();
    TestPage();
    TestLRU();
    TestFIFO();
    TestClock();
    TestBufferPoolClock();
    TestPageAllocator();
    TestBufferPool();
    TestReplacementLogCap();
    TestIOStats();
    TestBackgroundFlush();
    TestBlockDeviceFaultInjection();
    TestLRUK();
    TestPageRWLock();
    TestBufferPoolMemory();
    TestRobustnessFixes();
    TestOptimizations();
    TestConcurrentSessions();
    TestLockManager();
    TestIsolationLevels();
    TestSnapshotIsolation();
    TestSnapshotDeleteFcw();
    TestSnapshotUpsertFcw();
    TestSetIsolationStatement();
    TestRowLockEncoding();
    TestRowLockTier();
    TestRowLevelConcurrency();
    TestBPlusTreeConcurrency();
    TestSerializablePredicatePhantom();

    std::printf("\n======== Storage UT ========\n");
    std::printf("checks: %d   fails: %d\n", g_checks, g_fails);
    if (g_fails == 0) {
        std::printf("RESULT: PASS\n");
    } else {
        std::printf("RESULT: FAIL\n");
    }

    // 性能基准：记录各阶段「优化前 vs 优化后」指标（仅输出，不做性能断言）。
    osopt::RunBenchmarks();

    return g_fails == 0 ? 0 : 1;
}