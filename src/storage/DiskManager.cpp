#include "storage/DiskManager.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <io.h>  // _fileno, _commit
#else
#include <unistd.h>  // fileno, fsync
#endif

namespace sqlcompiler {

namespace {

// 将 FILE* 的内容真正落盘（durable），而不仅是刷到 OS 缓冲。
//   Windows: _commit(_fileno(f))  ~= FlushFileBuffers
//   POSIX :  fsync(fileno(f))
void DurableSync(std::FILE* f) {
    if (f == nullptr) return;
    std::fflush(f);
#ifdef _WIN32
    ::_commit(::_fileno(f));
#else
    ::fsync(::fileno(f));
#endif
}

// 64 位文件定位：Windows 用 MS 专用 _fseeki64/_ftelli64；POSIX 用 fseeko/ftello。
// 数据文件偏移 = page_id * PAGE_SIZE，DB 超 2GB 时若用 32 位 long 会被截断回绕，
// 导致读写指向错误扇区，故必须 64 位定位。
int FSeek64(std::FILE* f, long long off, int whence) {
#ifdef _WIN32
    return ::_fseeki64(f, off, whence);
#else
    return ::fseeko(f, static_cast<off_t>(off), whence);
#endif
}
long long FTell64(std::FILE* f) {
#ifdef _WIN32
    return ::_ftelli64(f);
#else
    return static_cast<long long>(::ftello(f));
#endif
}

// 返回当前文件字节大小；失败返回 0
long long TellFileSize(std::FILE* f) {
    if (f == nullptr) return 0;
    long long cur = FTell64(f);
    if (FSeek64(f, 0, SEEK_END) != 0) return 0;
    long long end = FTell64(f);
    if (cur >= 0) FSeek64(f, cur, SEEK_SET);
    return end < 0 ? 0 : end;
}

inline void PutU32(char* p, uint32_t v) {
    std::memcpy(p, &v, 4);
}
inline uint32_t GetU32(const char* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
inline void PutU64(char* p, uint64_t v) {
    std::memcpy(p, &v, 8);
}
inline uint32_t GetU64(const char* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

// 标准 CRC-32/ISO-HDLC（IEEE 802.3，多项式 0xEDB88320），表驱动。
// 注意：对全 0 的 PAGE_SIZE 缓冲其值非 0，因此 0 可安全用作「无记录」哨兵。
uint32_t Crc32(const char* data, size_t len) {
    static uint32_t s_table[256] = {0};
    static bool s_init = false;
    if (!s_init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            s_table[i] = c;
        }
        s_init = true;
    }
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = s_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace

DiskManager::DiskManager(const std::string& db_file)
    : DiskManager(db_file, nullptr) {}

DiskManager::DiskManager(const std::string& db_file,
                         std::unique_ptr<BlockDevice> device)
    : db_file_name_(db_file),
      next_page_id_(0),
      file_size_(0),
      // E7：默认用文件设备打开数据文件；测试可注入自定义设备（如坏块注入器）。
      device_(device ? std::move(device)
                     : std::make_unique<FileBlockDevice>(db_file)) {
    device_ready_ = device_ && device_->IsReady();
    file_size_ = device_ready_ ? device_->Size() : 0;
    next_page_id_ = static_cast<page_id_t>(file_size_ / PAGE_SIZE);
    fpl_path_ = db_file_name_ + ".fpl";
    LoadFreePageBitmap();
    crc_path_ = db_file_name_ + ".crc";
    LoadPageCrcs();
}

DiskManager::~DiskManager() {
    std::lock_guard<std::mutex> lock(db_io_latch_);
    // 先收尾空闲页位图与页 CRC（分别持久化），再关闭数据文件（FileBlockDevice 析构
    // 负责 fflush + fclose）。
    FinalizeFreePageBitmap();
    FinalizePageCrcs();
    device_.reset();
}

page_id_t DiskManager::AllocatePage() {
    std::lock_guard<std::mutex> lock(db_io_latch_);
    if (!free_pages_.empty()) {
        page_id_t pid = free_pages_.back();
        free_pages_.pop_back();
        // 复用空闲页前必须先把位图置 0 并持久化，否则崩溃重启后该页会被当作
        // 空闲页再次分配，导致两个表共享同一物理页（数据损坏）。
        SetPageFree(pid, false);
        return pid;
    }
    return next_page_id_++;
}

void DiskManager::DeallocatePage(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(db_io_latch_);
    if (page_id < 0) return;
    // 重复释放防护：若该页已标记为空闲（位图既定位置 1），直接幂等返回。
    // 否则 free_pages_ 会出现重复项，后续 AllocatePage 可能把同一物理页分发两次，
    // 导致两个表/索引共享一个页 → 数据损坏。注意：位图未覆盖的页位视为「使用中」，
    // 故对从未分配/从未释放的页首次 Deallocate 不会被误拦截。
    size_t byte_index = static_cast<size_t>(page_id) / 8;
    if (byte_index < fbit_.size() &&
        (fbit_[byte_index] & static_cast<uint8_t>(1u << (page_id % 8)))) {
        return;
    }
    free_pages_.push_back(page_id);
    SetPageFree(page_id, true);
    // 回收页：清掉其 CRC，避免该页被复用后残留上一任内容的旧 CRC，造成误报。
    ClearPageCrc(page_id);
}

void DiskManager::ReadPage(page_id_t page_id, char* data) {
    std::lock_guard<std::mutex> lock(db_io_latch_);
    std::memset(data, 0, PAGE_SIZE);
    if (page_id < 0) return;
    long long offset = static_cast<long long>(page_id) * PAGE_SIZE;
    if (offset >= file_size_) return;  // 未分配页：读取全零，属合法语义
    if (!device_ready_) return;        // 介质不可用：保持全零（open 失败降级）
    ++io_read_count_;  // 真正触达磁盘的读页
    // 读取本页可用的字节数，不足 PAGE_SIZE 的部分已由 memset 补零。
    long long avail = file_size_ - offset;
    size_t to_read = (avail >= PAGE_SIZE) ? PAGE_SIZE
                                          : static_cast<size_t>(avail);
    size_t got = device_->Read(offset, data, to_read);
    if (got < to_read) {
        // 读到不足（非错误状态，如介质末尾）时保持补零结果，容错处理。
    }
    // 页 CRC 校验：仅当该页有持久化 CRC 记录时核对；无记录（未分配 / 旧库页）跳过。
    // 匹配失败说明磁盘内容已损坏（位翻转/部分写），上抛以走统一 I/O 错误通道。
    uint32_t stored = 0;
    if (GetPageCrc(page_id, &stored)) {
        if (Crc32(data, PAGE_SIZE) != stored) {
            throw std::runtime_error("page CRC mismatch: page " +
                                     std::to_string(page_id));
        }
    }
}

void DiskManager::WritePage(page_id_t page_id, const char* data, bool force) {
    std::lock_guard<std::mutex> lock(db_io_latch_);
    if (!device_ready_) {
        throw std::runtime_error(
            "disk write error: database file handle is not open");
    }
    EnsureFileCapacity(page_id);
    long long offset = static_cast<long long>(page_id) * PAGE_SIZE;
    size_t written = device_->Write(offset, data, PAGE_SIZE);
    if (written != PAGE_SIZE) {
        throw std::runtime_error("disk write error: page " +
                                 std::to_string(page_id) + " (short write)");
    }
    ++io_write_count_;  // 真正成功写回的页
    // 记录该页 CRC（写回 .crc 仅 fflush 到 OS 缓存，不逐页 fsync；真正落盘由
    // Sync() 统一完成，与组提交路径的 fsync 次数保持一致）。
    SetPageCrc(page_id, data);
    // Phase B：COMMIT 路径调用 force=true，把 dirty page 强制刷到磁盘；
    // 默认 false 保留 Phase A 行为以减少同步开销。
    if (force) {
        device_->Sync();
    }
}

void DiskManager::Sync() {
    std::lock_guard<std::mutex> lock(db_io_latch_);
    // .crc 页面缓冲先刷到 OS 缓存，随后把校验**先于数据**持久化（见下方顺序说明）。
    FlushCrcFile();
    // 顺序关键（修复崩溃窗口的假损坏误报）：
    //   必须先 fsync 校验（.crc），后 fsync 数据页。若顺序颠倒（数据先、校验后），
    //   在两次 fsync 之间断电会产生「数据==新值、校验==旧值」的磁盘状态：重启读回时
    //   用新数据比旧 CRC 必然 mismatch，且因数据页已是最新（page.page_lsn == record.lsn）
    //   恢复期 redo 不会重写该页，校验无法纠正 → 一次已正确提交的写入被误报为损坏。
    //   反之「校验先、数据后」：断电在中间时数据页仍是旧值（未 fsync），恢复期 redo
    //   会判定该页需要重放、重写数据页并连带刷新其 CRC，最终两态自洽。
    if (crc_ != nullptr) {
        DurableSync(crc_);  // 先落校验
    }
    if (device_ready_ && device_) {
        device_->Sync();    // 后落数据页
    }
}

int DiskManager::GetNumPages() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(db_io_latch_));
    return next_page_id_;
}

int DiskManager::GetNumFreePages() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(db_io_latch_));
    return static_cast<int>(free_pages_.size());
}

long long DiskManager::GetIOReadCount() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(db_io_latch_));
    return io_read_count_;
}

long long DiskManager::GetIOWriteCount() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(db_io_latch_));
    return io_write_count_;
}

void DiskManager::EnsureFileCapacity(page_id_t page_id) {
    if (page_id < 0) return;
    long long need = static_cast<long long>(page_id + 1) * PAGE_SIZE;
    if (file_size_ >= need) return;
    if (!device_ready_ || !device_) return;
    device_->EnsureCapacity(need);
    file_size_ = need;  // 更新缓存，供后续 ReadPage / EnsureFileCapacity 复用
}

// ---- 空闲页持久化（<db>.fpl 位图）----

// 惰性创建 .fpl：仅在首次 DeallocatePage 时落盘，避免无页回收场景产生多余文件；
// 对既有的旧库（从未生成 .fpl）完全无影响，因此无需改动数据文件格式，向后兼容。
void DiskManager::EnsureFplFile() {
    if (fpl_ != nullptr) return;
    fpl_ = std::fopen(fpl_path_.c_str(), "w+b");
    if (fpl_ == nullptr) {
        // 创建失败：静默降级为"不持久化空闲列表"，等同旧行为（仅会话内复用）。
        return;
    }
    // 空位图：全部 0（= 已分配）。首个 DeallocatePage 会通过 EnsureBitCapacity 扩展。
    fbit_.clear();
    fbit_bytes_ = 0;
}

void DiskManager::EnsureBitCapacity(size_t need_bytes) {
    if (fpl_ == nullptr) return;
    if (fbit_bytes_ >= need_bytes) return;
    // 扩展文件到位图区域结束位置，并用可靠的 0 覆盖（WC 未保证，故逐个扩展一次写最低字节）。
    std::fseek(fpl_, static_cast<long>(kFplHeader + need_bytes - 1), SEEK_SET);
    char zero = 0;
    std::fwrite(&zero, 1, 1, fpl_);
    std::fflush(fpl_);
    if (fbit_.size() < need_bytes) {
        fbit_.resize(need_bytes, 0);
    }
    fbit_bytes_ = need_bytes;
}

void DiskManager::FlushBitByte(size_t byte_index) {
    if (fpl_ == nullptr || byte_index >= fbit_bytes_) return;
    std::fseek(fpl_, static_cast<long>(kFplHeader + byte_index), SEEK_SET);
    std::fwrite(&fbit_[byte_index], 1, 1, fpl_);
    std::fflush(fpl_);
    DurableSync(fpl_);
}

void DiskManager::FlushFplHeader() {
    if (fpl_ == nullptr) return;
    char hdr[kFplHeader];
    PutU32(hdr, kFplMagic);
    PutU32(hdr + 4, kFplVersion);
    PutU64(hdr + 8, static_cast<uint64_t>(next_page_id_));   // 该位图对应的页数
    PutU64(hdr + 16, static_cast<uint64_t>(fbit_bytes_));    // 位图字节数
    std::fseek(fpl_, 0, SEEK_SET);
    std::fwrite(hdr, 1, kFplHeader, fpl_);
    std::fflush(fpl_);
    DurableSync(fpl_);
}

void DiskManager::SetPageFree(page_id_t page_id, bool free) {
    if (page_id < 0) return;
    EnsureFplFile();
    size_t byte_index = static_cast<size_t>(page_id) / 8;
    EnsureBitCapacity(byte_index + 1);
    if (free) {
        fbit_[byte_index] |= static_cast<uint8_t>(1u << (page_id % 8));
    } else {
        fbit_[byte_index] &= static_cast<uint8_t>(~(1u << (page_id % 8)));
    }
    FlushBitByte(byte_index);
    FlushFplHeader();
}

void DiskManager::LoadFreePageBitmap() {
    std::FILE* f = std::fopen(fpl_path_.c_str(), "r+b");
    if (f == nullptr) {
        return;  // 无 .fpl：空闲列表为空，向后兼容旧库
    }
    long long len = TellFileSize(f);
    if (len < static_cast<long long>(kFplHeader)) {
        std::fclose(f);
        return;  // 文件损坏/过短：视为无空闲列表
    }
    char hdr[kFplHeader];
    std::fseek(f, 0, SEEK_SET);
    std::fread(hdr, 1, kFplHeader, f);
    const uint32_t magic = GetU32(hdr);
    const uint32_t ver = GetU32(hdr + 4);
    const uint64_t stored_pages = GetU64(hdr + 8);
    const uint64_t bbytes64 = GetU64(hdr + 16);
    // 兼容校验：魔数/版本正确，且位图对应的页数必须与当前数据文件一致。
    // 不一致意味着数据文件在本 .fpl 之后被替换/截断过（或异常退出后页数漂移），
    // 此时为安全起见丢弃空闲列表（仅损失空间复用，绝不误复用活页）。
    const bool ok = (magic == kFplMagic) && (ver == kFplVersion) &&
                    (stored_pages == static_cast<uint64_t>(next_page_id_)) &&
                    (bbytes64 <= kFplMaxLoadBytes) &&
                    (static_cast<uint64_t>(kFplHeader) + bbytes64 <=
                     static_cast<uint64_t>(len));
    if (!ok) {
        std::fclose(f);
        return;
    }
    const size_t bbytes = static_cast<size_t>(bbytes64);
    fbit_.assign(bbytes, 0);
    std::fseek(f, static_cast<long>(kFplHeader), SEEK_SET);
    std::fread(fbit_.data(), 1, bbytes, f);
    fbit_bytes_ = bbytes;
    // 重建空闲列表：位 i=1 且 i < next_page_id_ 的页为空闲。
    for (uint32_t pid = 0; pid < static_cast<uint32_t>(next_page_id_); ++pid) {
        size_t bi = static_cast<size_t>(pid) / 8;
        if (bi >= fbit_bytes_) break;
        if ((fbit_[bi] >> (pid % 8)) & 1u) {
            free_pages_.push_back(static_cast<page_id_t>(pid));
        }
    }
    // 复用已打开的文件句柄，供本次会话内的 SetPageFree 直接更新。
    fpl_ = f;
}

void DiskManager::FinalizeFreePageBitmap() {
    if (fpl_ == nullptr) return;
    // 把位图补齐覆盖到 next_page_id_（新页默认已分配=0），并回写头部（pages =
    // next_page_id_），使优雅退出后重启能够装载。此处写盘失败不抛出。
    size_t need = (static_cast<size_t>(next_page_id_) + 7) / 8;
    EnsureBitCapacity(need);
    FlushFplHeader();
    std::fflush(fpl_);
    DurableSync(fpl_);
    std::fclose(fpl_);
    fpl_ = nullptr;
}

// ---- 页 CRC32 校验（<db>.crc 旁路）----

// 启动时装载既有 .crc。旧库没有该文件（或文件过短/损坏）时按「无记录」处理：
// 所有页跳过校验，行为与旧版完全一致（向后兼容）。
void DiskManager::LoadPageCrcs() {
    std::FILE* f = std::fopen(crc_path_.c_str(), "r+b");
    if (f == nullptr) return;
    long long len = TellFileSize(f);
    size_t count = static_cast<size_t>(len / 4);  // 每页 4 字节
    count = std::min<size_t>(count, 1u << 20);    // 上限 1M 页，防异常超大文件 OOM
    if (count > 0) {
        pcrc_.resize(count, 0);
        std::fseek(f, 0, SEEK_SET);
        size_t got = std::fread(pcrc_.data(), 4, count, f);
        // 仅接受完整读入；不足则视为空（避免半写文件导致错误 CRC）。
        if (got != count) {
            pcrc_.clear();
            std::fclose(f);
            return;
        }
    }
    crc_ = f;  // 复用句柄，后续 SetPageCrc 直接更新
}

void DiskManager::EnsureCrcFile(size_t need_count) {
    if (need_count == 0) return;
    if (crc_ == nullptr) {
        crc_ = std::fopen(crc_path_.c_str(), "w+b");
        if (crc_ == nullptr) return;  // 创建失败：静默降级为「无 CRC」，等同旧行为
        pcrc_.clear();
    }
    if (pcrc_.size() >= need_count) return;
    // 用 0 补齐新增项（0 = 无记录），并同步扩展文件，保证后续可直接定位写。
    size_t old = pcrc_.size();
    pcrc_.resize(need_count, 0);
    std::fseek(crc_, static_cast<long>(old * 4 + (need_count - old - 1) * 4),
               SEEK_SET);
    char zero = 0;
    std::fwrite(&zero, 1, 1, crc_);
    std::fflush(crc_);
}

void DiskManager::SetPageCrc(page_id_t page_id, const char* data) {
    if (page_id < 0) return;
    size_t idx = static_cast<size_t>(page_id);
    EnsureCrcFile(idx + 1);
    if (crc_ == nullptr) return;  // 无法持久化 CRC：跳过（降级）
    uint32_t crc = Crc32(data, PAGE_SIZE);
    pcrc_[idx] = crc;
    std::fseek(crc_, static_cast<long>(idx * 4), SEEK_SET);
    std::fwrite(reinterpret_cast<const char*>(&crc), 4, 1, crc_);
    std::fflush(crc_);  // 仅到 OS 缓存，不逐页 fsync
}

void DiskManager::ClearPageCrc(page_id_t page_id) {
    if (page_id < 0 || crc_ == nullptr) return;
    size_t idx = static_cast<size_t>(page_id);
    if (idx >= pcrc_.size() || pcrc_[idx] == 0) return;
    pcrc_[idx] = 0;
    std::fseek(crc_, static_cast<long>(idx * 4), SEEK_SET);
    uint32_t zero = 0;
    std::fwrite(reinterpret_cast<const char*>(&zero), 4, 1, crc_);
    std::fflush(crc_);
}

bool DiskManager::GetPageCrc(page_id_t page_id, uint32_t* out) const {
    if (out == nullptr || page_id < 0) return false;
    size_t idx = static_cast<size_t>(page_id);
    if (idx >= pcrc_.size()) return false;  // 无记录（未分配页 / 旧库页）
    if (pcrc_[idx] == 0) return false;      // 0 = 无记录
    *out = pcrc_[idx];
    return true;
}

void DiskManager::FlushCrcFile() {
    if (crc_ == nullptr || pcrc_.empty()) return;
    std::fflush(crc_);  // stdio 缓冲落到 OS；真正的落盘由 Sync() 中的 DurableSync 完成
}

void DiskManager::FinalizePageCrcs() {
    if (crc_ == nullptr) return;
    FlushCrcFile();
    DurableSync(crc_);
    std::fclose(crc_);
    crc_ = nullptr;
}

}  // namespace sqlcompiler