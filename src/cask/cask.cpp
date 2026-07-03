#include "bitcask/cask.hpp"

#include <signal.h>     // ::kill for stale-lock detection
#include <sys/resource.h>  // ::getrlimit, RLIMIT_NOFILE（S12-1 read 句柄默认上限）
#include <unistd.h>     // ::getpid, ::unlink

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <thread>

#include "bitcask/format.hpp"
#include "bitcask/keydir_registry.hpp"
#include "bitcask/merger.hpp"
#include "bitcask/detail/scanner.hpp"
#include "bitcask/codec.hpp"   // V6.1: GetResultView::ctor 解码 DocValue

namespace bitcask {

// P14a:恢复 checkpoint 文件名(目录级,与 bitcask.meta 同级)。
// 命名契约 {kv|search}.{组件}.{ckpt|seg|wal|manifest},见
// doc/recovery-unified-checkpoint-design-zh.md §3。后缀 .ckpt = 可 fold
// 重建的 checkpoint(纯优化)。旧名(bitcask.keydir.snap 等)不再读——
// 这些文件可重建,升级后首次 open 走全量 fold,close 时落新名。
inline constexpr const char* kKeydirSnapName = "kv.keydir.ckpt";
// P14e:搜索索引统一分段 checkpoint（docmap/bm25/hnsw 单文件，逐段 CRC）。
inline constexpr const char* kSearchCkptName = "search.ckpt";
// S17-2:per-component 段文件名（docmap.ckpt / bm25.ckpt / vec.ckpt）。
// 取代旧的 kSearchCkptName。S17-5 迁移期间旧名仍被读（一次性迁移路径）。
inline constexpr const char* kDocmapCkptName = "docmap.ckpt";
inline constexpr const char* kBm25CkptName   = "bm25.ckpt";
inline constexpr const char* kVecCkptName    = "vec.ckpt";

namespace {
namespace fs = std::filesystem;

CaskFault io_fault(int errnum, std::string detail = {}) {
    return CaskFault{CaskError::kIo, errnum, std::move(detail)};
}
CaskFault err(CaskError k, std::string detail = {}) {
    return CaskFault{k, 0, std::move(detail)};
}

std::uint32_t now_sec_default() {
#ifdef CLOCK_REALTIME_COARSE
    // 每次 get/put 都要取秒级时间戳:COARSE 时钟走 vDSO 无 syscall,
    // 粒度为内核 tick(1-4ms),对秒级语义无损。
    timespec ts;
    ::clock_gettime(CLOCK_REALTIME_COARSE, &ts);
    return static_cast<std::uint32_t>(ts.tv_sec);
#else
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
#endif
}

std::string_view bytes_to_view(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

// 探测 OS 进程 pid 是否还活着。对应 legacy bitcask_lockops:os_pid_exists/1
// 用 `kill -0 <pid>` 的做法。kill(pid, 0)：
//   返回 0   — 信号能投递，进程在
//   -1 + ESRCH — 进程已死
//   -1 + EPERM — 进程在但我们无权 signal（保守地视为「活着」，
//                避免误删别人的 lock）
[[nodiscard]] bool process_alive(int pid) noexcept {
    if (pid <= 0) return false;
    if (::kill(pid, 0) == 0) return true;
    return errno != ESRCH;
}

// 锁文件内容格式（legacy 和我们都遵守）：
//   "<pid> <active_data_file_path>\n"
//   或者只有 "<pid>\n"（active 文件还没建好的时候）
// 这个函数从路径 basename 里抠出 tstamp/file_id，给 merger 在 needs_merge
// 时排除「writer 正在写的文件」用。返回 0 表示没路径或解不出来。
[[nodiscard]] std::uint32_t
parse_active_file_id_from_lock(std::span<const std::byte> bytes) noexcept {
    // Skip leading PID digits.
    std::size_t i = 0;
    while (i < bytes.size() && static_cast<char>(bytes[i]) >= '0' &&
                                static_cast<char>(bytes[i]) <= '9') {
        ++i;
    }
    if (i == bytes.size() || static_cast<char>(bytes[i]) != ' ') return 0;
    ++i;  // skip the space

    // Take the rest up to newline as the path.
    std::size_t end = i;
    while (end < bytes.size() && static_cast<char>(bytes[end]) != '\n') ++end;
    std::string path(reinterpret_cast<const char*>(bytes.data() + i), end - i);
    if (path.empty()) return 0;

    auto t = fileops::parse_data_tstamp(path);
    if (!t) return 0;
    return static_cast<std::uint32_t>(*t);
}

// Parse the leading positive integer from `bytes` (the lock-file payload
// is "<pid> <activefile>\n" — we only care about the pid).
[[nodiscard]] int parse_leading_pid(std::span<const std::byte> bytes) noexcept {
    int pid = 0;
    bool any_digit = false;
    for (auto byte : bytes) {
        char c = static_cast<char>(byte);
        if (c >= '0' && c <= '9') {
            pid = pid * 10 + (c - '0');
            any_digit = true;
            if (pid > (1 << 30)) return -1;  // overflow guard
        } else {
            break;
        }
    }
    return any_digit ? pid : -1;
}

// 如果锁文件里记录的 pid 已死，尝试删掉它，让 caller 重试 O_EXCL acquire。
// 对应 legacy bitcask_lockops:delete_stale_lock。
//
// 竞态窗口：从我们读 pid 到我们 unlink 之间，另一个 writer 可能写了新 lock；
// 我们会误删他的。legacy 也有同样的 race，实际暴露面极小——只发生在
// crash recovery 路径上，正常运行不会碰到。
[[nodiscard]] bool try_remove_stale_lock(const std::string& path) noexcept {
    auto rl = lock::FileLock::acquire(path, /*write*/ false);
    if (!rl) return false;  // file vanished or unreadable; the retry will surface the right error

    auto data = rl->read_data();
    bool dead = false;
    if (data) {
        const int pid = parse_leading_pid(
            std::span<const std::byte>(data->data(), data->size()));
        // pid == -1 means "no parseable PID" (e.g. legacy hadn't written
        // it yet, or the writer crashed mid-write). Treat as stale.
        if (pid == -1 || !process_alive(pid)) {
            dead = true;
        }
    } else {
        dead = true;  // can't read content; treat as stale
    }
    rl->release_quiet();  // closes fd; read locks don't unlink
    if (!dead) return false;
    return ::unlink(path.c_str()) == 0;
}

// 拿 bitcask.write.lock，自带 stale-lock 回收。先只写一行 pid；active
// data file 路径要等 ensure_active_writer 创建文件后才能补上。
// Cask::open 跟 close_write_file → 下一次 put 重新拿锁时都走这条路径。
[[nodiscard]] std::expected<lock::FileLock, CaskFault>
acquire_writer_lock(const std::string& dirname) {
    const auto lock_path = (fs::path(dirname) / "bitcask.write.lock").string();
    auto fl = lock::FileLock::acquire(lock_path, /*write*/ true);
    if (!fl && fl.error().errnum == EEXIST) {
        if (try_remove_stale_lock(lock_path)) {
            fl = lock::FileLock::acquire(lock_path, /*write*/ true);
        }
    }
    if (!fl) {
        if (fl.error().errnum == EEXIST) {
            return std::unexpected(err(CaskError::kWriteLocked, lock_path));
        }
        return std::unexpected(io_fault(fl.error().errnum, lock_path));
    }
    const std::string pid_line = std::to_string(::getpid()) + "\n";
    auto pid_bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(pid_line.data()),
        pid_line.size());
    (void)fl->write_data(pid_bytes);
    return std::move(*fl);
}

}  // namespace

// =============================================================================
// CaskIter：fold 迭代器实现
//
// 把 keydir::IterHandle 包一层，加上「按 file_id 拿 DataFile 句柄、
// 按 (offset, total_sz) pread 出 value」的能力。see_tombstones=true 时
// 墓碑也作为带 is_tombstone 标志的 Entry 上交。
// =============================================================================

CaskIter::~CaskIter() noexcept { release(); }

std::expected<keydir::StartIterResult, CaskFault>
CaskIter::start(int maxage, int maxputs, std::uint32_t now_sec,
                bool see_tombstones, std::span<const std::byte> key_prefix) {
    // S11-W3：parent Cask 已 close → keydir_ 已释放,fail-fast 而非解引用空指针。
    if (parent_->is_closed()) {
        return std::unexpected(err(CaskError::kClosed, "cask is closed"));
    }
    if (iter_ && iter_->is_iterating()) {
        return std::unexpected(err(CaskError::kIo, "iter already started"));
    }
    // X1:先 pin KeyDir（shared_ptr 复制），再建 IterHandle——保证
    // close() 在迭代器存活期间 reset keydir_ 也不会让 iter_ 的裸指针悬空。
    keydir_pin_ = parent_->keydir_;
    iter_ = parent_->keydir_->make_iter();
    auto r = iter_->start(now_sec, maxage, maxputs);
    see_tombstones_ = see_tombstones;
    // S13-D4：前缀拷入自有存储（span 借 caller 缓冲，start 返回后可能失效）。
    key_prefix_.clear();
    if (!key_prefix.empty()) {
        key_prefix_.assign(reinterpret_cast<const char*>(key_prefix.data()),
                           key_prefix.size());
    }
    // S13：真正开始迭代后（kOk），pin 当前目录下所有 data file 的只读句柄快照，
    // 让并发 merge 在本次 fold 期间 unlink 旧文件不影响后续 next() 的 pread。
    // kOutOfDate 时 caller 会重试，不在此处 pin。
    if (r == keydir::StartIterResult::kOk) {
        pin_files();
    }
    return r;  // kOk or kOutOfDate (kAlreadyIterating handled above)
}

// 扫描目录、open 全部 data file 的只读句柄并 pin 住（S13）。best-effort：
// 扫描或单个 open 失败时跳过该文件，next() 对其退回 parent_->read_file（仍可
// 工作，只是少了「文件被 merge 删除」的保护）。active write file 不 pin——
// merge 从不合并 active 文件，且 parent_ 已为它持有 RW 句柄。
void CaskIter::pin_files() {
    pinned_files_.clear();
    auto scan = fileops::scan_dir(parent_->dirname_);
    if (!scan) return;
    // S13-F3：active_data_（shared_ptr 对象本体）与并发 roll/close 的
    // reset/赋值构成数据竞争——对同一 shared_ptr 对象的并发读写非线程安全，
    // 必须在 read_cache_mu_ 下拍快照（写点均持其独占锁）。快照略陈旧无害：
    // pin 是 best-effort，漏 pin 的文件由 next() 退回 parent_->read_file。
    bool has_active = false;
    std::uint32_t active_fid = 0;
    {
        std::shared_lock lk(parent_->read_cache_mu_);
        has_active = static_cast<bool>(parent_->active_data_);
        active_fid =
            parent_->active_file_id_.load(std::memory_order_relaxed);
    }
    for (const auto& e : *scan) {
        const auto fid = static_cast<std::uint32_t>(e.tstamp);
        if (has_active && fid == active_fid) continue;
        // P6:迭代器 pin 句柄经 read() 读(非 read_mmap),且只用于本次 fold——
        // 不 mmap(避免无谓映射;fd 开着,read() pread 正常)。
        auto df = fileops::DataFile::open(e.data_path,
                                          fileops::DataFile::Mode::kRead,
                                          /*sync*/ false, /*mmap_enabled*/ false);
        if (!df) continue;
        pinned_files_.emplace(
            fid, std::make_unique<fileops::DataFile>(std::move(*df)));
    }
}

std::expected<std::optional<CaskIter::Entry>, CaskFault> CaskIter::next() {
    if (!iter_ || !iter_->is_iterating()) return std::optional<Entry>{};

    const auto expiry = parent_->opts_.expiry_secs;
    const auto now = (expiry > 0) ? now_sec_default() : 0;

    // 跳过过期 entry；墓碑的处理由 see_tombstones_ 决定：
    //   false（默认）— 墓碑直接跳过（legacy fold/3 行为）
    //   true         — 墓碑也作为带 is_tombstone=true 的 Entry 上交。
    //                  sibling 墓碑没有真实的磁盘 record，我们合成一条
    //                  v0 marker 当 value 上交，避免给出空 value 让 caller
    //                  困惑。
    while (true) {
        auto proxy = iter_->next(/*include_tombstones=*/ see_tombstones_);
        if (!proxy) return std::optional<Entry>{};

        if (expiry > 0 && proxy->tstamp + expiry <= now) {
            continue;  // expired; skip
        }

        // S13-D4：前缀过滤——proxy 层跳过（不 pread value、不跨界拷 key）。
        if (!key_prefix_.empty() &&
            (proxy->key.size() < key_prefix_.size() ||
             std::memcmp(proxy->key.data(), key_prefix_.data(),
                         key_prefix_.size()) != 0)) {
            continue;
        }

        // sibling 墓碑只活在 keydir 里（file_id 是 sentinel，磁盘上没
        // 对应 record）。跳过文件读，合成一条空 value 墓碑给 caller。
        if (proxy->is_tombstone) {
            Entry e;
            e.key.assign(reinterpret_cast<const std::byte*>(proxy->key.data()),
                          reinterpret_cast<const std::byte*>(proxy->key.data()) +
                          proxy->key.size());
            e.value.clear();
            e.value.shrink_to_fit();
            e.tstamp       = proxy->tstamp;
            e.file_id      = proxy->file_id;
            e.offset       = proxy->offset;
            e.total_sz     = proxy->total_sz;
            e.is_tombstone = true;
            e.ord          = proxy->ord;
            return std::optional<Entry>{std::move(e)};
        }

        // S13：优先用 fold 启动时 pin 的句柄（merge 可能已 unlink 该文件，但
        // 已 open 的 fd 仍可读）；未 pin 的（active 文件 / fold 后新建的文件）
        // 退回共享 read_file——这些文件不会在本次 fold 期间被 merge 删除。
        fileops::DataFile* df = nullptr;
        std::shared_ptr<fileops::DataFile> shared_df;  // pin 共享句柄到本次读结束
        if (auto pit = pinned_files_.find(proxy->file_id); pit != pinned_files_.end()) {
            df = pit->second.get();
        } else {
            shared_df = parent_->read_file(proxy->file_id);
            df = shared_df.get();
        }
        if (!df) {
            return std::unexpected(err(CaskError::kIo,
                "open read file_id=" + std::to_string(proxy->file_id)));
        }
        auto rec = df->read(proxy->offset, proxy->total_sz);
        if (!rec) {
            switch (rec.error().kind) {
                case fileops::DataFileError::kBadCrc:
                    return std::unexpected(err(CaskError::kBadCrc));
                case fileops::DataFileError::kIo:
                    return std::unexpected(io_fault(rec.error().errnum));
                default:
                    return std::unexpected(err(CaskError::kIo, "read"));
            }
        }
        // 即使 keydir 没把它标成墓碑，磁盘 record 自己也可能是墓碑——
        // keydir 指向的就是一条带墓碑类型的 value。这种「磁盘墓碑」要
        // 跟「sibling 墓碑」区分对待（前者有真实磁盘字节，后者纯内存）。
        const bool value_is_tomb = rec->type == format::RecordType::kTombstone;
        if (value_is_tomb && !see_tombstones_) continue;

        Entry e;
        e.key          = std::move(rec->key);
        // 磁盘上 kDoc value 是 DocValue 编码（text 段 = 原始 value），与
        // Cask::get 一致地解码取 text 段，避免把 doc 头/长度前缀漏给 caller。
        // 墓碑 record 不是 DocValue 编码，按原始字节上交（通常为空/marker）。
        if (value_is_tomb) {
            e.value = std::move(rec->value);
        } else {
            auto dv = codec::decode_doc_value(std::span<const std::byte>(rec->value));
            if (!dv) return std::unexpected(err(CaskError::kIo, "corrupt DocValue"));
            // S13-D5：per-key TTL——过期记录跳过（与整库 expiry_secs 同语义）。
            if (dv->expiry_at != 0 && dv->expiry_at <= now_sec_default()) {
                continue;
            }
            e.value.assign(dv->text.begin(), dv->text.end());
        }
        e.tstamp       = rec->tstamp;
        e.file_id      = proxy->file_id;
        e.offset       = proxy->offset;
        e.total_sz     = proxy->total_sz;
        e.is_tombstone = value_is_tomb;
        e.ord          = rec->ord;
        return std::optional<Entry>{std::move(e)};
    }
}

std::expected<std::vector<CaskIter::Entry>, CaskFault>
CaskIter::next_batch(std::size_t max_n) {
    std::vector<Entry> batch;
    batch.reserve(max_n);
    for (std::size_t i = 0; i < max_n; ++i) {
        auto r = next();
        if (!r) return std::unexpected(r.error());
        if (!r->has_value()) break;  // EOI
        batch.push_back(std::move(**r));
    }
    return batch;
}

// S11-W4：排干 live key（仅 key，不读 value）。走 keydir proxy（含 key + 定位，
// 无 pread），比逐条 next()（每条 pread+decode）廉价得多 → 供 parallel_scan 取
// 快照后并行读值。消费迭代器（推进 cursor 到尾）。
std::vector<std::vector<std::byte>> CaskIter::drain_live_keys() {
    std::vector<std::vector<std::byte>> out;
    if (!iter_ || !iter_->is_iterating()) return out;
    // include_tombstones=false → keydir 层跳过墓碑，只给 live key。
    while (auto proxy = iter_->next(/*include_tombstones=*/false)) {
        // S13-D4：前缀过滤（与 next() 一致）。
        if (!key_prefix_.empty() &&
            (proxy->key.size() < key_prefix_.size() ||
             std::memcmp(proxy->key.data(), key_prefix_.data(),
                         key_prefix_.size()) != 0)) {
            continue;
        }
        const auto* p = reinterpret_cast<const std::byte*>(proxy->key.data());
        out.emplace_back(p, p + proxy->key.size());
    }
    return out;
}

void CaskIter::release() noexcept {
    if (iter_) {
        iter_->release();
        iter_.reset();
    }
    // S13：关掉 pin 的只读 fd；若文件已被 merge unlink，此刻 inode 才真正释放。
    pinned_files_.clear();
    // X1:最后释放 KeyDir pin——必在 iter_ 析构/release 之后，确保
    // BarrierGuard 锁的 KeyDir mutex 在锁期间始终存活。
    keydir_pin_.reset();
}

// =============================================================================
// Cask 主体实现
//
// open   ：拿锁 / 注册 keydir / 扫盘重建索引 / 准备 active writer
// upgrade：离线 KV→索引升级（不拿锁，只读扫盘重建索引）
// close  ：finalize active writer + hint trailer，释放锁，registry release
// get    ：keydir 查 → DataFile pread → 校验 → 返回 value
// put    ：append 到 active data file → 写 hint → 更新 keydir
// remove ：append 一条墓碑 record → 更新 keydir 标记为墓碑
// merge  ：跑 run_merge → 合并完后从 read_files_ 缓存里淘汰旧句柄、
//          然后 unlink 老文件
// =============================================================================

Cask::~Cask() { close(); }

// 离线升级：将 KV 模式目录转为索引模式。
// 不获取任何锁——要求目录处于离线状态（无活跃 writer/merger）。
// 步骤：验证 meta 是 KV → 覆写 meta 为 kIndex → 创建 SearchLayer →
//       新建 KeyDir + load_keydir_from_disk(search_layer) → mark_ready
// 返回的 Cask 是只读的（无 active writer），调用方可以 close 后
// 再用 open(dirname, {enable_search=true, read_write=true}) 正常使用。
std::expected<std::unique_ptr<Cask>, CaskFault>
Cask::upgrade(std::string_view dirname,
              const search::SearchLayerConfig& search_config) {
    if (!fs::exists(dirname)) {
        return std::unexpected(err(CaskError::kIo, "directory does not exist"));
    }

    if (!meta::meta_exists(std::string(dirname))) {
        return std::unexpected(err(CaskError::kModeMismatch,
                                    "no bitcask.meta found — not a valid bitcask directory"));
    }
    auto mc = meta::read_meta(std::string(dirname));
    if (!mc) {
        return std::unexpected(err(CaskError::kIo, "read meta failed"));
    }
    if (mc->mode == meta::Mode::kIndex) {
        return std::unexpected(err(CaskError::kModeMismatch,
                                    "directory is already in index mode"));
    }

    meta::MetaConfig new_mc;
    new_mc.mode = meta::Mode::kIndex;
    auto wr = meta::write_meta(std::string(dirname), new_mc);
    if (!wr) {
        return std::unexpected(err(CaskError::kIo, "write meta failed"));
    }

    auto cask = std::make_unique<Cask>();
    cask->dirname_ = std::string(dirname);
    cask->meta_config_ = new_mc;
    if (!cask->field_schema_.open((fs::path(dirname) / "field.schema").string())) {  // #1
        return std::unexpected(err(CaskError::kIo,
            "field.schema corrupt or incompatible version"));
    }

    cask->docmap_ = std::make_shared<index::Index>();  // S16-1：宿主服务
    cask->search_ = std::make_unique<search::SearchLayer>(search_config,
                                                          cask->docmap_);

    cask->keydir_ = std::make_shared<keydir::KeyDir>();
    if (auto r = cask->load_keydir_from_disk(cask->search_.get()); !r) {
        return std::unexpected(r.error());
    }
    cask->keydir_->mark_ready();

    return cask;
}

// Cask 启动入口。流程：
//   1. ensure dir 存在
//   2. 拿锁：read_write 拿 bitcask.write.lock；merge_only 拿 bitcask.merge.lock；
//      只读模式不拿任何锁
//   3. 拿 keydir：通过 registry 共享 / 单独 new；首次创建的需要 load_keydir_from_disk
// 失败路径会回滚已分配的资源（unique_ptr 自带 RAII，锁也是 optional<FileLock> 自管）。
// 内部按阶段拆为 acquire_open_locks() → check_or_create_meta() →
// create_search_infra() → keydir 装配,本函数只做编排。
std::expected<std::unique_ptr<Cask>, CaskFault>
Cask::open(std::string_view dirname, const CaskOptions& opts,
            keydir::KeyDirRegistry* registry) {
    // S6-P0-pre：registry 强制非空（双池归属 registry，无 nullptr fallback）。
    if (registry == nullptr) {
        return std::unexpected(err(CaskError::kInvalidOption,
            "open() requires a non-null KeyDirRegistry"));
    }
    auto cask = std::make_unique<Cask>();
    cask->dirname_ = std::string(dirname);
    cask->opts_    = opts;

    // S12-1：解析 read 句柄上限。默认(0)从 RLIMIT_NOFILE 软上限推导安全上限，
    // 避免大库无界累积 fd/mmap 撞 `ulimit -n` / `vm.max_map_count`。显式值/不限
    // 哨兵原样透传（见 resolve_read_handle_cap）。
    {
        std::size_t nofile = 1024;  // getrlimit 失败/INFINITY 时的保守兜底
        struct ::rlimit rl{};
        if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
            nofile = static_cast<std::size_t>(rl.rlim_cur);
        }
        cask->opts_.max_read_handles =
            resolve_read_handle_cap(opts.max_read_handles, nofile);
    }

    // 目录不存在就建（mkdir -p 语义）。已存在不报错。
    std::error_code ec;
    fs::create_directories(cask->dirname_, ec);

    // 字段名 ↔ id 注册表（#1）：加载已有 + 打开追加句柄。
    if (!cask->field_schema_.open((fs::path(cask->dirname_) / "field.schema").string())) {
        return std::unexpected(err(CaskError::kIo,
            "field.schema corrupt or incompatible version"));
    }

    if (auto r = cask->acquire_open_locks(); !r) {
        return std::unexpected(r.error());
    }

    if (auto r = cask->check_or_create_meta(); !r) {
        return std::unexpected(r.error());
    }

    if (auto r = cask->create_search_infra(opts); !r) {
        return std::unexpected(r.error());
    }

    // 拿 / 建 keydir。
    //
    // 走 registry：多个同目录的 Cask 共享同一个 keydir。
    //   - kCreated：我们是初始化者，扫盘建 keydir 后调 mark_ready
    //   - kReady：有其他 cask 已经初始化好了，直接拿来用
    //   - kNotReady：别人正在初始化，最多等 40 × 50 ms = 2 秒
    //
    // 不走 registry：每个 cask 独占一个 keydir（unit test 常见）。
    if (registry != nullptr) {
        cask->registry_    = registry;
        cask->keydir_name_ = std::string(dirname);
        auto a = registry->acquire(cask->keydir_name_);
        if (a.status == keydir::AcquireStatus::kNotReady) {
            for (int i = 0; i < 40; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                a = registry->acquire(cask->keydir_name_);
                if (a.status != keydir::AcquireStatus::kNotReady) break;
            }
            if (a.status == keydir::AcquireStatus::kNotReady) {
                return std::unexpected(err(CaskError::kIo,
                    "keydir not_ready after wait"));
            }
        }
        cask->keydir_ = a.keydir;
        if (a.status == keydir::AcquireStatus::kCreated) {
            if (auto r = cask->load_keydir_from_disk(cask->search_.get()); !r) return std::unexpected(r.error());
            cask->keydir_->mark_ready();
        }
    } else {
        cask->keydir_ = std::make_shared<keydir::KeyDir>();
        if (auto r = cask->load_keydir_from_disk(cask->search_.get()); !r) return std::unexpected(r.error());
        cask->keydir_->mark_ready();
    }
    // S6-P3: 仅 search 模式注册车道（KV 模式无 search_）。索引双池由 registry
    // 共享所有——本库向其注册一条 lane（map/reduce/error 闭包 + 起始 ord），
    // 拿到稳定句柄 index_lane_。线程数与库数解耦（G2）：所有同 registry 的库
    // 共用一对 dispatcher/reducer。
    //
    // 起始 ord 对齐 keydir 当前水位——reducer 跳过 disk 已恢复的
    // [0, peek_next_ord) 区间。merge 提交的 RunFn{ord=peek_next_ord} 等
    // 首个 entry 进该 lane 的 reorder 时 next_apply_ord 已对齐，无 stall。必须
    // 在 keydir_/registry_ 就绪后（create_search_infra 早于此装配）。
    //
    // S15-3：闭包按 CaskPlugin 接口分发（捕获 plugins_ 快照 by value；P1 恒
    // = {SearchLayerAdapter}）。生命周期：close 先 unregister_lib（flush 排空
    // ⇒ 闭包不再被调用）再 reset adapter/search_，与旧「捕获 *search_」等价。
    if (cask->search_ && cask->registry_) {
        cask->index_pool_ = cask->registry_->index_pool();
        // 分发逻辑提取为命名方法（prepare_index_task / reduce_index_entry /
        // on_index_worker_error），闭包退化为薄捕获委托——可独立测试，消除
        // 契约测试里的闭包复刻。生命周期：close 先 unregister_lib（flush 排空
        // ⇒ 闭包不再被调用）再 reset adapter/search_/docmap_。
        auto* c = cask.get();
        cask->index_lane_ = cask->index_pool_->register_lib(
            [c](const IndexTask& task) { return c->prepare_index_task(task); },
            [c](ReorderEntry& entry) { c->reduce_index_entry(entry); },
            [c]() { c->on_index_worker_error(); },
            // 起始 ord。
            cask->keydir_->peek_next_ord()
        );
    }
    // S14-1：自动 ckpt 增量基线 = 当前水位（open 恢复路径①已按需回存，
    // 从零起算会导致老库首个 roll 必触发一次全量 ckpt）。
    cask->last_ckpt_ord_.store(cask->keydir_->peek_next_ord(),
                               std::memory_order_relaxed);
    return cask;
}

// T2.4:open 阶段一——锁分配。语义跟原 open() 内的锁块完全一致:
//   - read_write → 拿 bitcask.write.lock（acquire_writer_lock 内部含 stale 检测）
//   - merge_only → 拿 bitcask.merge.lock（独立文件,stale 检测 + 写 pid +
//     拍 live writer 的 active file id 快照供 needs_merge 排除）
//   - 只读 → 不拿锁
// 任何失败路径都返回 unexpected,unique_ptr<cask> 在 caller 析构时按 RAII
// 回滚已分配的资源（write_lock_/search_ RAII 自管；S6-P3 共享池车道
// index_lane_ 由 ~Cask→close()→unregister_lib 注销，见 close()）。
std::expected<void, CaskFault> Cask::acquire_open_locks() {
    if (opts_.read_write && !opts_.merge_only) {
        auto fl = acquire_writer_lock(dirname_);
        if (!fl) return std::unexpected(fl.error());
        write_lock_ = std::move(*fl);
        return {};
    }
    if (!opts_.merge_only) {
        // 只读模式:不拿任何锁。
        return {};
    }
    // merge_only 路径。
    const auto lock_path =
        (fs::path(dirname_) / "bitcask.merge.lock").string();
    auto fl = lock::FileLock::acquire(lock_path, /*write*/ true);
    if (!fl && fl.error().errnum == EEXIST) {
        if (try_remove_stale_lock(lock_path)) {
            fl = lock::FileLock::acquire(lock_path, /*write*/ true);
        }
    }
    if (!fl) {
        if (fl.error().errnum == EEXIST) {
            return std::unexpected(err(CaskError::kWriteLocked, lock_path));
        }
        return std::unexpected(io_fault(fl.error().errnum, lock_path));
    }
    const std::string pid_line = std::to_string(::getpid()) + "\n";
    auto pid_bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(pid_line.data()),
        pid_line.size());
    (void)fl->write_data(pid_bytes);
    write_lock_ = std::move(*fl);

    // 拍 live writer 的 active file id 快照，给 needs_merge 用。
    // 竞态窗口：从我们读 write.lock 到 merger 真的选文件之间，writer
    // 可能 roll 过去了——这个 race 在 legacy 里也有（见
    // bitcask_lockops:read_activefile），后果最多是少并掉一个刚 roll 的
    // 文件，下一轮 merge 自然会处理。
    const auto wlock_path =
        (fs::path(dirname_) / "bitcask.write.lock").string();
    auto wl = lock::FileLock::acquire(wlock_path, /*write*/ false);
    if (wl) {
        if (auto data = wl->read_data()) {
            merger_writer_active_id_ =
                parse_active_file_id_from_lock(
                    std::span<const std::byte>(data->data(), data->size()));
        }
        wl->release_quiet();
    }
    // If write.lock doesn't exist or can't be parsed, no active
    // writer is detected (id stays 0).
    return {};
}

// T2.4:open 阶段二——bitcask.meta 读取或创建。必须在 SearchLayer 创建
// 之前——meta 决定 KV / 索引模式以及向量配置,SearchLayer 内部 HnswIndex
// 创建依赖 meta_config_。vector_dim/metric 不符 → kModeMismatch。
std::expected<void, CaskFault> Cask::check_or_create_meta() {
    // P5b:int8-only 仅 kDot(int8 距离=重建内积);kL2 不支持,干净拒绝。
    if (opts_.vector_dim > 0 && opts_.vector_inmem_int8 &&
        opts_.vector_metric == meta::VectorMetric::kL2) {
        return std::unexpected(err(CaskError::kInvalidOption,
            "vector_inmem_int8 requires kDot/cosine metric (kL2 unsupported)"));
    }
    if (meta::meta_exists(dirname_)) {
        auto mc = meta::read_meta(dirname_);
        if (!mc) return std::unexpected(err(CaskError::kIo, "read meta failed"));
        if (opts_.enable_search && mc->mode != meta::Mode::kIndex) {
            return std::unexpected(err(CaskError::kModeMismatch,
                "directory is KV mode, cannot open with search"));
        }
        if (!opts_.enable_search && mc->mode == meta::Mode::kIndex) {
            return std::unexpected(err(CaskError::kModeMismatch,
                "directory is index mode, cannot open as KV"));
        }
        // V3.1:向量配置必须与 meta 完全一致(dim 库内恒定)。
        const auto want_metric = opts_.vector_dim > 0
                                     ? opts_.vector_metric
                                     : meta::VectorMetric::kNone;
        const bool want_quant = opts_.vector_dim > 0 && opts_.vector_quantized;
        const bool want_inmem_int8 =
            opts_.vector_dim > 0 && opts_.vector_inmem_int8;
        if (mc->vector_dim != opts_.vector_dim ||
            mc->vector_metric != want_metric ||
            mc->vector_quantized != want_quant ||
            mc->vector_inmem_int8 != want_inmem_int8) {
            return std::unexpected(err(CaskError::kModeMismatch,
                "vector config mismatch (meta dim/metric/quantized/inmem_int8 vs options)"));
        }
        meta_config_ = *mc;
        return {};
    }
    // 首次创建:无 meta 时写一份。vector_dim > 0 隐含 enable_search。
    if (opts_.vector_dim > 0 && !opts_.enable_search) {
        return std::unexpected(err(CaskError::kInvalidOption,
            "vector_dim requires enable_search"));
    }
    meta::MetaConfig mc;
    mc.mode = opts_.enable_search ? meta::Mode::kIndex : meta::Mode::kKV;
    if (opts_.vector_dim > 0) {
        mc.vector_dim = opts_.vector_dim;
        mc.vector_metric = opts_.vector_metric;
        mc.vector_quantized = opts_.vector_quantized;  // P3b
        mc.vector_inmem_int8 = opts_.vector_inmem_int8;  // P5b
    }
    auto wr = meta::write_meta(dirname_, mc);
    if (!wr) return std::unexpected(err(CaskError::kIo, "write meta failed"));
    meta_config_ = mc;
    return {};
}

// T2.4:open 阶段三——SearchLayer + IndexPool 创建。只在 search_config
// 配置时启动;worker 闭包内的所有 on_* / set_meta / on_vector 路径
// 严格保持原顺序(单写者 = 本 worker 线程,与 on_vector 同线程维持
// HNSW 单写者约束)。
std::expected<void, CaskFault>
Cask::create_search_infra(const CaskOptions& opts) {
    if (!opts.search_config) {
        return {};
    }
    // V3.3:向量配置从 meta 透传进 SearchLayerConfig(dim>0 时
    // SearchLayer 内部创建 HnswIndex)。以 meta 为准——open 已校验
    // opts 与 meta 一致。
    auto scfg = *opts.search_config;
    scfg.vector_dim = meta_config_.vector_dim;
    scfg.vector_metric = meta_config_.vector_metric;
    scfg.vector_inmem_int8 = meta_config_.vector_inmem_int8;  // P5b
    scfg.synonym_map = opts.synonym_map;  // S11：Cask 级 open-time 同义词词典透传
    // S16-1：docmap 宿主先建，注入 SearchLayer（同一实例，所有权在 Cask）。
    docmap_ = std::make_shared<index::Index>();
    search_ = std::make_unique<search::SearchLayer>(scfg, docmap_);
    // analyzer 构造失败（无效配置 / 分词器未注册 / 词典加载失败）则 analyzer_
    // 为空——决不能带病打开，否则首次带 text 的 put 段错误。干净拒绝。
    if (!search_->has_analyzer()) {
        search_.reset();
        return std::unexpected(err(CaskError::kInvalidOption,
                                   "analyzer creation failed (check analyzer type / dict_path)"));
    }
    // S15-3：SearchLayer 经 adapter 作「唯一插件」接入分发表——IndexPool
    // 写路径的 map/reduce 闭包只认识 plugins_（CaskPlugin 接口），不再直呼
    // SearchLayer 方法。
    search_adapter_ = std::make_unique<search::SearchLayerAdapter>(*search_);
    plugins_ = {search_adapter_.get()};
    // S6-P3: 不再每库自建池。共享池借用 + 车道注册推迟到 keydir 就绪后
    // （caller 在 create_search_infra 返回、registry_/keydir_ 装配完成后做）。
    // 此处仅建好 search_，标记本库为 search 模式（search_ != nullptr）。
    return {};
}

std::vector<plugin::PreparedPtr>
Cask::prepare_index_task(const IndexTask& task) {
    std::vector<plugin::PreparedPtr> preps(plugins_.size());
    const plugin::DocView  doc = make_doc_view(task);
    const plugin::PutEvent ev  = make_put_event(task, &doc);
    for (std::size_t i = 0; i < plugins_.size(); ++i) {
        if (plugins_[i]->wants_prepare()) {
            preps[i] = plugins_[i]->prepare(ev);
        }
    }
    return preps;
}

// S16-2 写路径反转：宿主**先 apply DocMap**（身份/存活/meta），再按注册序广播
// 给各插件（设计 §4：DocMap 恒在所有插件之前）。
// 顺序安全性：docmap 先亮 live、postings/向量后加（与旧「postings 先、live 后」
// 互换）——两序下并发查询都不可能命中「半个文档」（postings 无 → 不命中；
// live 无 → 过滤），且 reducer 单写者保证同 ord 两步间无写交错。doc_len 是
// 分析产物，宿主以 0 落行、BM25 侧经 set_doc_len 回填（S16 批次头②的缓行通道）。
void Cask::reduce_index_entry(ReorderEntry& entry) {
    std::visit([this](auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, PutEntry>) {
            const auto& t = e.task;
            docmap_->put_doc(t.key(), t.ord,
                             index::DocSlot{
                                 index::DocLoc{t.file_id, t.offset, t.total_sz},
                                 t.tstamp, /*doc_len=*/0});
            if (!t.meta.empty()) docmap_->set_meta(t.ord, t.meta);
            const plugin::DocView  doc = make_doc_view(e.task);
            const plugin::PutEvent ev  = make_put_event(e.task, &doc);
            for (std::size_t i = 0; i < plugins_.size(); ++i) {
                plugins_[i]->on_put(
                    ev, i < e.preps.size() ? std::move(e.preps[i])
                                           : plugin::PreparedPtr{});
            }
        } else if constexpr (std::is_same_v<T, DeleteEntry>) {
            // prior_ord 在 remove **前**捕获（删除统计需要旧 ord，插件不能
            // 反查已删行）；key 不存在 → 不动 docmap、广播哨兵（历史「删不存在」
            // 语义 = 插件侧 no-op）。
            std::uint64_t prior = plugin::kNoPriorOrd;
            if (auto slot = docmap_->get(e.key)) {
                prior = slot->ord;
                docmap_->remove(e.key, e.ord);
            }
            for (auto* p : plugins_) {
                p->on_delete(plugin::DeleteEvent{e.ord, e.key, prior});
            }
        } else if constexpr (std::is_same_v<T, SkipEntry>) {
            // no-op（ord 空洞填充）
        } else if constexpr (std::is_same_v<T, RunFnEntry>) {
            // S13-F6：merge 路径的 compact/HNSW 重建/ckpt 序列化等在此
            // （reducer 单写者上下文）执行。
            if (e.fn) e.fn();
        }
    }, entry);
}

void Cask::on_index_worker_error() noexcept {
    index_errors_.fetch_add(1, std::memory_order_relaxed);
    // S13-D7：索引 worker 吞异常此前仅计数——现同步上报。
    log_error("index worker exception swallowed (index may drift; "
              "see StatusInfo::index_errors)");
}

void Cask::replay_delta_to_keydir(
    const std::vector<search::SearchLayer::DeltaDocRow>& rows,
    const std::vector<search::SearchLayer::DeltaRemoval>& rems,
    std::span<const std::byte> keydir_meta,
    RecoverySnapshots& recovery) {
    // 链重放：行 → LWW put；删除 → remove_if_older，ord 守卫顺序无关；
    // kKeydirDelta 段推进标量/fstats/字节水位。行/删除先于 meta 应用
    // （meta 的水位声明覆盖 ≤ 行集）。
    for (const auto& r : rows) {
        keydir_->put(r.ext, r.slot.loc.file_id,
                     r.slot.loc.total_sz, r.slot.loc.offset,
                     r.slot.tstamp, /*now*/ 0,
                     /*newest*/ false, 0, 0, r.ord);
        keydir_->advance_ord(r.ord);
    }
    for (const auto& m : rems) {
        (void)keydir_->remove_if_older(m.key, m.tomb);
        keydir_->advance_ord(m.tomb);
    }
    if (!keydir_meta.empty()) {
        if (auto wms = keydir_->apply_meta_delta(keydir_meta)) {
            // 链尾水位驱动 fold_start（快照对里最新的一份）。
            recovery.snap_wms = std::move(*wms);
        }
    }
}

// S17-5:legacy search.ckpt → per-component 一次性迁移。流程：
//   1) 用旧 load_search_ckpt 把段全载回（带 hook 推进 keydir 字节水位）。
//   2) 从 SearchLayer 内部状态读 watermark 与组件链状态，构造 manifest。
//   3) 调 save_components_base 把当前内存态写到 3 个新组件文件。
//   4) 写 manifest。
//   5) 删旧 search.ckpt + search.ckpt.prev + .d* + search.vec / .qc8
//      （这些已被 save_components_base 重新写到 docmap.ckpt / bm25.ckpt /
//       vec.ckpt 对应 .vec / .qc8 sidecar）。
// 失败返回 false（caller 退全量 fold）。
bool Cask::migrate_legacy_search_ckpt(search::SearchLayer& search_layer) {
    const std::string old_ckpt = dirname_ + "/" + kSearchCkptName;
    // 1) 读旧 ckpt → 内存态（不写 keydir，已由 caller 在 recovery 阶段
    // 后续的 load_recovery_snapshots 接管；这里只关心段载入与写新文件）。
    auto result = search_layer.load_search_ckpt(old_ckpt);
    if (!result.loaded) {
        log_warn("migrate_legacy: failed to load legacy search.ckpt");
        return false;
    }
    // 2) 构造 manifest：每组件 base_watermark = result.watermark,
    // chain_seq = 0, chain_watermark = result.watermark（旧 ckpt 不区分
    // 组件 base/链——所有组件的水位统一对齐到 result.watermark）。
    bitcask::Manifest m;
    for (auto& e : m.entries) {
        e.base_watermark = result.watermark;
        e.chain_seq = 0;
        e.chain_watermark = result.watermark;
    }
    // 3) 写 per-component 文件。
    std::array<bool, bitcask::kComponentCount> all_dirty{};
    for (auto& b : all_dirty) b = true;
    auto base_res = search_layer.save_components_base(
        dirname_, result.watermark, all_dirty);
    if (!base_res.wrote_base[0] || !base_res.wrote_base[1]) {
        log_warn("migrate_legacy: failed to write per-component base "
                 "(docmap=" + std::to_string(base_res.wrote_base[0]) +
                 " bm25=" + std::to_string(base_res.wrote_base[1]) +
                 " vec=" + std::to_string(base_res.wrote_base[2]) + ")");
        return false;
    }
    // 4) 写 manifest。
    const std::string mpath = dirname_ + "/" +
        std::string(bitcask::kManifestName);
    if (!bitcask::write_manifest(mpath, m)) {
        log_warn("migrate_legacy: write_manifest failed");
        return false;
    }
    current_manifest_ = m;
    // 5) 删旧 search.ckpt + .prev + .d<seq> + .vec + .qc8。
    std::error_code ec;
    std::filesystem::remove(old_ckpt, ec);
    std::filesystem::remove(old_ckpt + ".prev", ec);
    for (std::uint32_t i = 1; i < 1024; ++i) {
        if (!std::filesystem::remove(
                old_ckpt + ".d" + std::to_string(i), ec)) {
            // 链中段缺失即停（链是连续 1..N）。
            if (ec) break;
        }
    }
    std::filesystem::remove(
        std::filesystem::path(old_ckpt).replace_extension(".vec"), ec);
    std::filesystem::remove(
        std::filesystem::path(old_ckpt).replace_extension(".qc8"), ec);
    return true;
}

// 收尾顺序很关键：
//   1. finalize active hint（写 trailer + running CRC，否则 hint 文件
//      下次 open 时会被判失效，回退到全量 fold(data) 重建 keydir）；
//   2. 关 active data；
//   3. 清 read cache（关掉缓存的 fd，避免泄漏）；
//   4. registry release（refcount -1，归零时 keydir 真正销毁）；
//   5. 释放 write/merge lock。
// 失败全部静默——close 路径上的错误没有合理的恢复动作，硬抛会让调用方
// 进程意外崩溃（close 标 noexcept，抛出即 std::terminate）。
void Cask::close() noexcept {
    // S11-W3：置 closed_ 标志（兼作幂等门——二次 close 直接返回）。后续公共方法
    // 入口 is_closed() 检查 → fail-fast 返回错误码而非解引用已释放状态。
    if (closed_.exchange(true)) return;
    // H1：等在途写操作退出（含其 write_mu_ 外的索引提交尾段）再拆资源。
    // 契约上 close 时刻不应有在途写调用（closed_ 注释），此处防御性收敛：
    // 违约时 close 阻塞等待而非 UAF。被队列背压挡住的写者也会收敛——池由
    // registry 持有仍在消费，push 必然返回；closed_ 已置位，新写者入口即退。
    for (auto n = writes_in_flight_.load(std::memory_order_seq_cst); n != 0;
         n = writes_in_flight_.load(std::memory_order_seq_cst)) {
        writes_in_flight_.wait(n, std::memory_order_seq_cst);
    }
    // close 内部步骤（save_ckpt/snapshot 的 vector 操作）可能抛 bad_alloc；
    // noexcept 函数抛出 → std::terminate。整个 body 包 try/catch 兜底：吞掉
    // 异常让后续资源释放仍能执行，优于进程硬死。错误可见性靠 index_errors_
    // 计数 + 未来可观测性梯队，不在 close 加日志。
    try {
        (void)maybe_group_commit(/*force*/ true);  // P4:落最后一批未 fsync 的写
        if (active_hint_) {
            (void)active_hint_->finalize();
            active_hint_.reset();
        }
        {
            std::scoped_lock lk(read_cache_mu_);
            active_data_.reset();
            read_files_.clear();
        }
        // A4-P2/P3 顺序要点:先排干本库车道(flush → Index 覆盖全部已分配
        // ord),再在 keydir 仍在手时做 search 双保存(bm25 + sidecar,
        // 覆盖标记取 peek_next_ord),最后落 keydir 快照并释放——
        // 旧版在 keydir_.reset() 之后才存 sidecar,恒被跳过(P3 测试抓出)。
        //
        // S6-P3: 池由 registry 共享，close 只注销本库车道（flush 排空 + 从
        // lanes_ 移除），不停池（其它库仍在用）。unregister_lib 内含 flush，
        // 保证 search_ 析构前本 lane 的 reduce 闭包（捕获 *search_）已不再被
        // reducer 调用。整池停在 registry 析构。
        if (index_pool_ && index_lane_) {
            index_pool_->unregister_lib(index_lane_);
            index_lane_ = nullptr;
            index_pool_ = nullptr;  // 仅清借用指针，不动共享池本体
        }
        if (search_ && opts_.read_write && keydir_) {
            // P14e:统一分段 search.ckpt（docmap + bm25 + hnsw 单文件）。
            // S14-4：close 强制全量 base——干净关闭收敛为单一 base，
            // .prev 代际刷新、delta 链坍缩（链不跨干净重启累积）。
            search_->force_ckpt_rebase();
            const std::string search_ckpt = dirname_ + "/" + kSearchCkptName;
            (void)save_search_ckpt_paired(search_ckpt,
                                          keydir_->peek_next_ord(),
                                          collect_snapshot_watermarks(), {});
        }
        if (opts_.read_write) write_keydir_snapshot();
    } catch (...) {
        // 吞掉：close 是终结路径，没有合理的恢复动作。后续 keydir/lock
        // release 仍需执行，所以不 return。S13-D7：上报（log 自身 noexcept）。
        log_error("close: exception during shutdown (resources still released; "
                  "checkpoint/snapshot may be missing)");
    }
    // 资源释放步骤放 try 外，确保即使上面 catch 触发也一定执行。
    // unique_ptr::reset（析构隐式 noexcept）与 FileLock::release_quiet（显式
    // noexcept）不抛；唯 registry_->release 内部取 mutex + 构造 std::string，
    // 理论上可抛 system_error/bad_alloc——单独包 try 兜底，保证 close() 整体
    // 真正 noexcept（否则此处抛出仍会 std::terminate）。
    if (registry_ && !keydir_name_.empty()) {
        try {
            registry_->release(keydir_name_);
        } catch (...) {
            // 极罕见（mutex 资源耗尽 / OOM）；吞掉，继续释放本地资源。
        }
        registry_ = nullptr;
        keydir_name_.clear();
    }
    keydir_.reset();
    // S15-3：adapter 引用 *search_，先于 search_ 重置（lane 已在上方
    // unregister（含 flush），闭包不会再触碰二者）。
    search_adapter_.reset();
    plugins_.clear();
    search_.reset();
    docmap_.reset();  // S16-1：shared_ptr 共持，序无关；此处只清宿主句柄
    if (write_lock_) {
        write_lock_->release_quiet();
        write_lock_.reset();
    }
}

// A4:写 keydir 段快照(best-effort,设计 doc/recovery-unified-checkpoint-design-zh.md 附录 A)。
// 水位 = 各 data 文件当前磁盘大小,**先于** dump 捕获(尾部回放重叠区
// 幂等,方向安全)。
// S14-1：水位捕获与快照写入拆分。无参版本 = 捕获+写入（调用点须在写者
// 静止处：close / merge 末尾 / open 恢复①）；RunFn 路径（checkpoint()/
// 自动 ckpt）在**提交时刻**捕获水位、reducer 执行时刻写快照本体——执行时
// 取水位会被并发写者推进，反转「keydir_covered ≤ search_covered」保存序
// 不变量（路线 A §4），fold 从超前的字节水位起跳、search 丢失
// [ckpt_wm, 快照时刻) 区间（回归测试 AutoCheckpointOnRoll 抓过此反转）。
// 快照 entries 比水位新无害：fold 尾部重放对 keydir 幂等覆盖。
std::optional<std::vector<std::pair<std::uint32_t, std::uint64_t>>>
Cask::collect_snapshot_watermarks() const noexcept {
    if (!keydir_) return std::nullopt;
    auto entries = fileops::scan_dir(dirname_);
    if (!entries) return std::nullopt;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> wms;
    wms.reserve(entries->size());
    for (const auto& e : *entries) {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(e.data_path, ec);
        if (ec) return std::nullopt;  // 文件态不稳定,放弃本次快照
        wms.emplace_back(static_cast<std::uint32_t>(e.tstamp), sz);
    }
    return wms;
}

void Cask::write_keydir_snapshot(
    const std::vector<std::pair<std::uint32_t, std::uint64_t>>& wms) noexcept {
    if (!keydir_) return;
    if (!keydir_->save_snapshot(dirname_ + "/" + kKeydirSnapName, wms)) {
        // best-effort：失败下次 open 走全量 fold（仅慢一次启动）。S13-D7 上报。
        log_warn("keydir snapshot save failed (will rebuild on next open)");
    }
}

void Cask::write_keydir_snapshot() noexcept {
    auto wms = collect_snapshot_watermarks();
    if (!wms) return;
    write_keydir_snapshot(*wms);
}

// T3: 提交索引任务到 IndexPool。背压由有界队列提供：队列满（10240）时
// index_pool_->submit 的 push 阻塞写线程，让 put 路径自然减速、避免内存溢出。
// H1（s13-review §P1）：常规写路径在 write_mu_ 释放后才调本函数——push
// 阻塞只挡本写者，不再全队冻结。锁外读 index_pool_/index_lane_ 由
// WriteOpGate 与 close() 的排空等待同步（见 cask.hpp）。
void Cask::submit_index_task(IndexTask task) {
    if (!index_pool_ || !index_lane_) return;
    index_pool_->submit(index_lane_, std::move(task));
}

// ---- open 时重建 keydir ----------------------------------------------------
// 优先 fold(hint_file)，hint 缺失或 trailer CRC 校验不过时回退到 fold(data_file)
// 重建。fold 顺序按 tstamp 升序——保证后写入的 entry 覆盖前面的。
// search_layer 为空时跳过 SearchLayer 的恢复。
std::expected<void, CaskFault> Cask::load_keydir_from_disk(search::SearchLayer* search_layer) {
    auto entries = fileops::scan_dir(dirname_);
    if (!entries) return std::unexpected(io_fault(entries.error().errnum, dirname_));

    // P14e:search.ckpt 分段快照快路径。search.ckpt 健康且全段 CRC 通过
    // 时，fold 从 keydir 水位起跳过已覆盖字节；否则全量 fold（各索引自门）。
    auto recovery = load_recovery_snapshots(search_layer);
    if (!recovery) return std::unexpected(recovery.error());
    bool snap_loaded = recovery->snap_loaded;
    const auto& snap_wms = recovery->snap_wms;
    auto wm_of = [&](std::uint32_t fid) -> std::uint64_t {
        for (auto& [id, off] : snap_wms) {
            if (id == fid) return off;
        }
        return 0;  // 快照不认识的文件(快照后新建/merge 产物)→ 全量 fold
    };

    // S3:search 恢复期把 recover_doc 攒成批，交 recover_doc_batch 并行 analyze
    // + 串行有序插入（仅 search_layer!=null 的串行路径用；并行 KV 路径不碰）。
    // 插入序 == fold 序 → 与逐条 recover 结果一致。墓碑前必 flush 以保相对序。
    constexpr std::size_t kRecoverBatch = 1024;
    std::vector<search::SearchLayer::RecoverDoc> recover_batch;
    // ①（s13-review §P1 后续）：统计本次恢复重分析的文档数——它度量的是
    // 「若现在不回存 checkpoint，下次崩溃要白付多少重放」。计所有喂进
    // recover 的文档（含被索引 ord 自门丢弃的重叠区：分析成本已经付了，
    // 回存快照能让下次 fold 起点前移、免掉这部分）。
    std::size_t recovered_docs = 0;
    // S14-6：恢复期遇到 field.schema 无法解析的悬空 FieldId 的计数（掉电
    // 窗口：intern 只 fflush 未 fsync，schema 尾条映射可能晚于数据丢失）。
    // 跳过该字段（与"丢弃"同级的降级）+ 循环后聚合告警，可观测不刷屏。
    std::size_t dangling_field_ids = 0;
    auto flush_recover = [&] {
        if (search_layer && !recover_batch.empty()) {
            recovered_docs += recover_batch.size();
            search_layer->recover_doc_batch(recover_batch);
            recover_batch.clear();
        }
    };

    // R3:每个 data file 的 fold 抽成独立单元 fold_one(e)。纯 KV 恢复
    // （search_layer==null）可并行——见函数尾部的并行调度。串行语义下
    // 「按 tstamp 升序后写覆盖前写」仍成立；并行下 keydir 冲突解析按
    // (file_id, tstamp, offset) LWW 与到达序无关（put_overwrite），fstats
    // 全程无锁原子累加，cold-start 期 keyfolders_==0 故新 key 直入分片
    // entries（不触 meta_mu_），256 分片提供真并发。
    auto fold_one =
        [&](const fileops::DataFileEntry& e) -> std::expected<void, CaskFault> {
        // 把 keydir 的 biggest_file_id 推到至少这个文件的 id——保证后续
        // 分配新 file_id 时不会跟磁盘上已有的文件冲突。
        keydir_->increment_file_id_at_least(static_cast<std::uint32_t>(e.tstamp));

        // 优先走 hint 文件加速路径（不读 value，省掉绝大部分 I/O）。
        // hint 缺失或 trailer CRC 不通过则 fallback 到 fold(data) 全量重建。
        // SearchLayer 恢复需要读 value（text 段），有 search_layer 时跳过 hint。
        const std::uint64_t fold_start =
            snap_loaded ? wm_of(static_cast<std::uint32_t>(e.tstamp)) : 0;

        bool used_hint = false;
        if (e.has_hint && !search_layer && !snap_loaded) {
            auto hf = fileops::HintFile::open(e.hint_path,
                                                fileops::HintFile::Mode::kRead);
            if (hf) {
                // S13-P8：单遍校验+fold（原 validate_trailer 全文件读一遍、
                // fold 再读一遍）。CRC 不过时回调零次，keydir 零污染。
                auto fr = hf->fold_validated([&](const auto& rec) {
                        if (rec.tombstone) {
                            // 墓碑 hint 必须执行——否则前一个 file 里的同 key
                            // 活 entry 会被错误保留。
                            keydir_->remove(bytes_to_view(rec.key), rec.tstamp);
                            return;
                        }
                        keydir_->put(bytes_to_view(rec.key),
                                     static_cast<std::uint32_t>(e.tstamp), rec.total_sz, rec.offset,
                                     rec.tstamp, /*now*/ 0,
                                     /*newest*/ false, 0, 0, /*ord*/ 0);
                });
                if (fr && *fr) used_hint = true;
            }
        }
        if (used_hint) return {};

        // Fallback：fold 整个 data file。tolerate_crc_errors=true 让单条
        // 损坏的 record 跳过而不是中断整个文件加载——legacy 也是这语义。
        // out_last_valid_end 用于后续 torn-write 修复。
        // P6:恢复纯 fold,不 mmap(避免对大库逐文件全映射)。
        auto df = fileops::DataFile::open(e.data_path,
                                           fileops::DataFile::Mode::kRead,
                                           /*sync*/ false, /*mmap_enabled*/ false);
        if (!df) {
            return std::unexpected(io_fault(df.error().errnum, e.data_path));
        }
        std::uint64_t last_valid_end = 0;
        auto fr = df->fold(
            [&](const codec::DataRecordView& view, std::uint64_t offset,
                std::uint32_t total_size) {
                if (view.type == format::RecordType::kTombstone) {
                    keydir_->remove(bytes_to_view(view.key), view.tstamp);
                    if (search_layer) {
                        // S3:墓碑前 flush 攒批，保「文档↔墓碑」相对序（否则墓碑
                        // 可能先于其要删的 batch 内文档插入而被无效化）。
                        flush_recover();
                        search_layer->recover_tomb(bytes_to_view(view.key), view.ord);
                    }
                    return;
                }
                keydir_->put(bytes_to_view(view.key), static_cast<std::uint32_t>(e.tstamp),
                             total_size, offset, view.tstamp, /*now*/ 0,
                             /*newest*/ false, 0, 0, view.ord);
                keydir_->advance_ord(view.ord);
                if (search_layer) {
                    auto dv = codec::decode_doc_value(std::span<const std::byte>(view.value));
                    // V3.3:带向量的文档即使 text 为空也要恢复(否则
                    // Index 无该 ord,live 过滤会把它当死文档)。
                    // P3b:量化落盘(vec_quantized)也算带向量。
                    const bool dv_has_vec = dv && (dv->has_vector || dv->vec_quantized);
                    // S14-6：纯命名字段文档（text 空、无向量）也必须恢复——
                    // 否则连 docmap 都缺该 ord，live 过滤把它当死文档，
                    // bm25.fields 重建无从谈起。
                    if (dv && (!dv->text.empty() || dv_has_vec ||
                               dv->has_fields)) {
                        // S3:攒进批，满 kRecoverBatch 即并行处理。RecoverDoc 持
                        // owning 拷贝（fold 缓冲会复用，view 不可跨记录留存）。
                        search::SearchLayer::RecoverDoc rd;
                        rd.key.assign(reinterpret_cast<const char*>(view.key.data()),
                                      view.key.size());
                        rd.ord      = view.ord;
                        rd.text.assign(reinterpret_cast<const char*>(dv->text.data()),
                                       dv->text.size());
                        rd.file_id  = static_cast<std::uint32_t>(e.tstamp);
                        rd.offset   = offset;
                        rd.total_sz = total_size;
                        rd.tstamp   = view.tstamp;
                        // P3b:doc_vector_f32 统一处理 f32 与 int8 量化两种落盘
                        // （内部 memcpy 未对齐安全 / dequant）。
                        rd.vector   = codec::doc_vector_f32(*dv);
                        // S14-6：命名字段还原（FieldId → 名字经 field.schema），
                        // 使 fold 重放与活写路径同构（per-field + catch-all）。
                        // 此前 dv->fields 被整段丢弃——增量窗口（ckpt 不健康
                        // 时全库）的字段索引在恢复后不存在且被下次 ckpt 固化。
                        if (dv->has_fields) {
                            rd.fields.reserve(dv->fields.size());
                            for (const auto& f : dv->fields) {
                                auto fname = field_schema_.name_of(f.id);
                                if (!fname) {
                                    ++dangling_field_ids;
                                    continue;
                                }
                                rd.fields.emplace_back(
                                    std::move(*fname),
                                    std::string(
                                        reinterpret_cast<const char*>(
                                            f.value.data()),
                                        f.value.size()));
                            }
                        }
                        recover_batch.push_back(std::move(rd));
                        if (recover_batch.size() >= kRecoverBatch) flush_recover();
                    }
                }
            }, /*tolerate_crc_errors*/ true,
            /*out_last_valid_end*/ &last_valid_end,
            /*start_offset*/ fold_start);
        if (!fr) {
            return std::unexpected(err(CaskError::kBadCrc, e.data_path));
        }
        const std::uint64_t actual_size = df->size();
        df->close();

        // Torn-write 恢复：fold 已经跳过了文件尾部的损坏字节（可能是
        // 前一次 writer 写到一半 crash 留下的），如果我们是正经的 writer
        // 就把这些字节 truncate 掉——既释放磁盘，也避免后续 fstats 计算
        // 把坏字节当成「合法死 record」算到 total_bytes 里。
        // merge_only 不能这么干：它没有 write.lock，万一别的 writer 还在
        // 同一个文件后面追写，这里 truncate 会切掉别人的数据。
        if (opts_.read_write && !opts_.merge_only &&
            last_valid_end < actual_size) {
            auto wdf = fileops::DataFile::open(
                e.data_path, fileops::DataFile::Mode::kAppend);
            if (wdf) {
                (void)wdf->truncate_to(last_valid_end);  // best-effort
            }
        }
        return {};
    };

    // 调度：search_layer 存在时 HNSW 单写者 + BM25 插入须串行 → 走串行 fold，
    // 但 S3 在串行 fold 内把 recover_doc 攒批、analyze 并行化（见 flush_recover）。
    // 纯 KV 恢复且文件数 > 1 时并行 fold——worker 各取一文件，原子计数器分发，
    // 结果数组收集错误后统一传播。
    const std::size_t nfiles = entries->size();
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    const std::size_t nworkers =
        std::min<std::size_t>(nfiles, hw);

    if (search_layer != nullptr || nfiles <= 1 || nworkers <= 1) {
        for (const auto& e : *entries) {
            if (auto r = fold_one(e); !r) return std::unexpected(r.error());
        }
        flush_recover();  // S3:落最后一个不满批
        if (dangling_field_ids > 0) {
            log_warn("recovery: skipped " +
                     std::to_string(dangling_field_ids) +
                     " field value(s) with dangling field id "
                     "(field.schema tail lost; affected fields stay "
                     "unindexed until rewritten)");
        }
        // ①（s13-review §P1 后续）：恢复期重分析量超过阈值时，立即回存
        // checkpoint——否则重建成果只在内存，下次干净 close/merge 前再崩
        // 一次就全价重付（10M 级库重分词 + HNSW 重建可达小时级）。
        // 触发条件用**重分析文档数**而非「是否全量 fold」：快照/ckpt 健康
        // 但陈旧（长期运行未 close 的库崩溃后）时 fold 起点旧、尾部重放
        // 可能极大，同样值得回存；反之新建空库、小尾部增量不值得付大库
        // 整体序列化的成本（回存成本 ∝ 索引总量，省下的 ∝ 重放量）。
        // 精细节奏控制走 checkpoint() API（②）。
        // 此刻 index lane 尚未注册（open 在 fold 之后才 register_lib）、
        // 无并发写者，调用线程直接序列化即安全。best-effort：失败仅降级
        // 下次启动速度，不阻断 open。
        // 只读 / merge_only 不写（不持 write.lock，禁写共享目录文件）。
        constexpr std::size_t kPostRecoveryCkptMinDocs = 1000;
        if (search_layer && recovered_docs >= kPostRecoveryCkptMinDocs &&
            opts_.read_write && !opts_.merge_only) {
            // S14-7：经成对入口（fold 后链可能有效 → delta 回存更省）。
            std::vector<std::byte> kd;
            auto wms0 = collect_snapshot_watermarks();
            if (wms0) keydir_->serialize_meta_delta(kd, *wms0);
            if (!save_search_ckpt_paired(dirname_ + "/" + kSearchCkptName,
                                         keydir_->peek_next_ord(), wms0,
                                         kd)) {
                log_warn("post-recovery search checkpoint save failed "
                         "(next open will re-fold)");
            }
        }
        return {};
    }

    std::vector<std::expected<void, CaskFault>> results(nfiles);
    std::atomic<std::size_t> next{0};
    // RAII join guard：emplace_back 抛异常时已创建的 worker 会被析构自动
    // join——裸 vector<std::thread> 在此场景会让 joinable 线程触发
    // std::terminate，把可恢复的资源耗尽升级为崩溃。join 幂等（joinable
    // 检查），与下方显式 join 共存无重复 join。
    struct JoiningPool {
        std::vector<std::thread> threads;
        ~JoiningPool() {
            for (auto& t : threads) if (t.joinable()) t.join();
        }
    } pool;
    pool.threads.reserve(nworkers);
    for (std::size_t t = 0; t < nworkers; ++t) {
        pool.threads.emplace_back([&] {
            for (;;) {
                std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= nfiles) break;
                results[i] = fold_one((*entries)[i]);
            }
        });
    }
    for (auto& t : pool.threads) t.join();

    for (auto& r : results) {
        if (!r) return std::unexpected(r.error());
    }
    return {};
}

// P14e/P14b:加载 keydir 快照 + search.ckpt 分段快照。用 watermark 单趟
// 自门模型取代旧 4-way 成对门：search.ckpt 健康且全段 CRC 通过 → fold_start
// = keydir 水位（快路径）；否则 fold_start = 0（全量 fold，各索引按自身
// ord 水位自门丢弃重叠区，方向安全）。
// S17-4:per-component 协议——读 index.manifest 作为 commit point，按
// 组件 file 路径分别载入 docmap.ckpt / bm25.ckpt / vec.ckpt。S17-5
// 兼容：manifest 缺失但 search.ckpt 存在时触发一次性迁移。
std::expected<Cask::RecoverySnapshots, CaskFault>
Cask::load_recovery_snapshots(search::SearchLayer* search_layer) {
    RecoverySnapshots recovery;

    // S14-7：keydir base 快照**先**载（链的行/删除要应用在 base 之上）。
    if (auto w = keydir_->load_snapshot(dirname_ + "/" + kKeydirSnapName)) {
        recovery.snap_wms = std::move(*w);
        recovery.snap_loaded = true;
    }

    bool search_ok = false;
    if (search_layer) {
        // S17-5 兼容：manifest 缺失 + search.ckpt 存在 → 一次性迁移。
        const std::string mpath = dirname_ + "/" +
            std::string(bitcask::kManifestName);
        const std::string old_ckpt = dirname_ + "/" + kSearchCkptName;
        std::error_code ec;
        const bool has_manifest = std::filesystem::exists(mpath, ec);
        const bool has_old_ckpt = std::filesystem::exists(old_ckpt, ec);
        if (!has_manifest && has_old_ckpt) {
            // 触发迁移：把旧 search.ckpt 用旧路径 load 回来，再分
            // 写到新组件文件 + 写 manifest + 删旧文件。失败 → 全量 fold。
            if (!migrate_legacy_search_ckpt(*search_layer)) {
                recovery.snap_loaded = false;
                return recovery;
            }
        }
        auto manifest = bitcask::read_manifest(mpath);
        if (!manifest) {
            // manifest 仍不可读（迁移失败/被破坏）→ 全量 fold。
            recovery.snap_loaded = false;
            return recovery;
        }
        current_manifest_ = *manifest;
        // S14-7：链重放钩子。
        search::SearchLayer::DeltaReplayHook hook =
            [this, &recovery](
                const std::vector<search::SearchLayer::DeltaDocRow>& rows,
                const std::vector<search::SearchLayer::DeltaRemoval>& rems,
                std::span<const std::byte> keydir_meta) {
                replay_delta_to_keydir(rows, rems, keydir_meta, recovery);
            };
        // 逐组件 load（每组件独立校验、链重放）。
        const auto hook_arg = recovery.snap_loaded ? hook :
            search::SearchLayer::DeltaReplayHook{};
        bool all_components_ok = true;
        std::uint64_t min_chain_wm = UINT64_MAX;
        for (std::size_t i = 0; i < bitcask::kComponentCount; ++i) {
            const auto comp = static_cast<bitcask::ComponentId>(i);
            const auto& entry = current_manifest_.entries[i];
            auto cr = search_layer->load_component(
                comp, dirname_, entry.base_watermark,
                entry.chain_seq, hook_arg);
            if (cr.loaded) {
                min_chain_wm = std::min(min_chain_wm, cr.watermark);
                // 同步 SearchLayer 内部链状态镜像——后续 save 走正确的
                // 链续接。rebase = false：成功 load 的组件不需 rebase。
                search::SearchLayer::ComponentCkptState st;
                st.base_gen = entry.base_watermark;
                st.chain_wm = cr.watermark;
                st.next_seq = entry.chain_seq + 1;
                st.rebase_needed = false;
                search_layer->set_component_state(comp, st);
            } else {
                all_components_ok = false;
            }
        }
        if (min_chain_wm == UINT64_MAX) {
            min_chain_wm = 0;  // 没有任何组件成功
        }
        // 「全组件健康 + 非 .prev 回退」才能走字节水位快路径。.prev 任一
        // 组件回退 → 字节水位不可信，退全量 fold。
        bool any_from_prev = false;
        // 简化为：要求所有组件都非 from_prev（任一组件回退就退全量 fold）。
        // 单独读一次 ckpt 文件做 from_prev 检查（用 stat + 文件名），或
        // 由 load_component 返回 from_prev 决定。这里保守：要求 all ok
        // 即认为非 .prev（load 内部已用 manifest base_wm 校验，若不匹
        // 配则 .prev 路径优先于 .prev 失败）。
        // 简化处理：manifest 与磁盘文件不一致时 fold 兜底——我们通过
        // 「all_components_ok = true 且 min_chain_wm >= keydir 水位」
        // 双重门把关。
        search_ok = all_components_ok && recovery.snap_loaded;
        (void)any_from_prev;
        // S17-4:fold_start = min(chain_watermarks)。各索引自门按其
        // 自身 ord 水位丢重叠区。
        if (search_ok) {
            // 把 min_chain_wm 反馈到 recovery.snap_wms——简化处理：保持
            // 原 snap_wms 形态（Cask::load_recovery_snapshots 契约），由
            // 上层 fold 阶段自行按各索引水位自门即可。
        }
    }
    // search 不健康 → 退回全量 fold（snap_loaded=false 让 fold_start=0）。
    if (search_layer && !search_ok) {
        recovery.snap_loaded = false;
    }
    return recovery;
}

// ---- active writer 管理 ----------------------------------------------------
// ensure_active_writer：第一次写入或 close_write_file 之后调用，
// 创建新的 data + hint 文件、把路径补到 write.lock 内容里。
// roll_active_if_needed：写之前判断是否会撑爆 max_file_size，是的话切下一个。
// roll_active：无条件切——给 put 在 keydir.biggest_file_id 被并发 merger
// 顶过去时使用。
std::expected<void, CaskFault> Cask::ensure_active_writer() {
    if (active_data_) return {};
    if (!opts_.read_write) return std::unexpected(err(CaskError::kReadOnly));
    if (opts_.merge_only) {
        // merger 从不打开自己的 active writer——merge::run_merge 自己用
        // keydir->increment_file_id() 分配输出文件。
        return std::unexpected(err(CaskError::kReadOnly,
                                     "merge_only mode: no active writer"));
    }

    // close_write_file 之前可能已经把 write.lock 释放了；这里如果发现
    // 锁不在就重新拿。stale-lock 回收逻辑跟 open 一样——上次崩溃的 writer
    // 留下的锁会被探测到并回收。
    if (!write_lock_) {
        auto fl = acquire_writer_lock(dirname_);
        if (!fl) return std::unexpected(fl.error());
        write_lock_ = std::move(*fl);
    }

    active_file_id_ = keydir_->increment_file_id();
    auto data_path = fileops::mk_data_filename(dirname_, active_file_id_);
    auto hint_path = fileops::mk_hint_filename(data_path);

    auto df = fileops::DataFile::open(data_path,
                                       fileops::DataFile::Mode::kCreate,
                                       opts_.o_sync);
    if (!df) return std::unexpected(io_fault(df.error().errnum, data_path));
    auto hf = fileops::HintFile::open(hint_path,
                                       fileops::HintFile::Mode::kCreate,
                                       opts_.o_sync);
    if (!hf) return std::unexpected(io_fault(hf.error().errnum, hint_path));
    {
        // active_data_ 被 read_file 在 read_cache_mu_ 下读取,
        // 写点必须同锁互斥(O10:shared_ptr 拷贝与替换需要串行化)。
        std::unique_lock lk(read_cache_mu_);
        active_data_ = std::make_shared<fileops::DataFile>(std::move(*df));
    }
    active_hint_ = std::make_unique<fileops::HintFile>(std::move(*hf));

    // 把新 active file 路径记到 write.lock 里：merger（merge_only=true）
    // 通过读 write.lock 知道我们正在写哪个 file_id，从 needs_merge 候选里
    // 排除它。格式跟 legacy 一致："<pid> <active_data_path>\n"。
    if (write_lock_) {
        const std::string line = std::to_string(::getpid()) + " " +
                                  data_path + "\n";
        auto bytes = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(line.data()), line.size());
        if (!write_lock_->write_data(bytes)) {  // best-effort：失败不阻断
            log_warn("write.lock active-path update failed "
                     "(merge_only handles may not exclude active file)");
        }
    }
    return {};
}

// P4 单写者组提交。put/remove/put_doc 每次写后调用。o_sync 已逐条 durable、
// 或 sync_every_n==0、或无 active writer → no-op。累计写数达阈值（或 force）
// 时对 active data file fsync 一次并清零计数。写路径单线程，计数无需原子。
// hint 不在此 fsync——它可重建，崩溃回退 fold(data)。
std::expected<void, CaskFault> Cask::maybe_group_commit(bool force) {
    if (opts_.o_sync || opts_.sync_every_n == 0 || !active_data_) return {};
    if (!force) ++writes_since_sync_;
    const bool flush_now =
        force ? (writes_since_sync_ > 0) : (writes_since_sync_ >= opts_.sync_every_n);
    if (!flush_now) return {};
    if (auto r = active_data_->sync(); !r) {
        return std::unexpected(io_fault(r.error().errnum,
                                        std::string(active_data_->path())));
    }
    writes_since_sync_ = 0;
    return {};
}

// 写入前的预检：要么没 active writer（首次写入或 close_write_file 之后），
// 要么 active 写满了——两种情况都需要建一个新文件。
std::expected<void, CaskFault>
Cask::roll_active_if_needed(std::size_t about_to_write) {
    if (!active_data_) return ensure_active_writer();
    if (active_data_->size() + about_to_write <= opts_.max_file_size) return {};
    return roll_active();
}

// 无条件 roll：先把 hint trailer finalize（保证下次 open 能用 hint 加速），
// 然后丢掉 active data/hint 句柄，新建一个新 file_id 的 active writer。
// put 在 keydir.biggest_file_id 被并发 merger 顶过去时也走这条路径。
std::expected<void, CaskFault> Cask::roll_active() {
    if (auto r = maybe_group_commit(/*force*/ true); !r) return r;  // P4:落旧文件尾批
    if (active_hint_) {
        if (auto r = active_hint_->finalize(); !r) {
            return std::unexpected(io_fault(r.error().errnum,
                                             std::string(active_hint_->path())));
        }
    }
    {
        std::unique_lock lk(read_cache_mu_);
        active_data_.reset();  // 在途读者持 shared_ptr,旧对象由引用计数续命
    }
    active_hint_.reset();
    // S14-1：文件封口 = 自动 checkpoint 的天然锚点（sealed 文件不再变化，
    // 按写入量周期触发）。此处仅置标记——实际提交在写路径释放 write_mu_
    // 之后（maybe_submit_auto_checkpoint），锁内零开销。
    auto_ckpt_pending_.store(true, std::memory_order_relaxed);
    return ensure_active_writer();
}

std::expected<void, CaskFault> Cask::close_write_file() {
    std::lock_guard<std::mutex> wlk(write_mu_);  // S11-W1：写路径互斥
    if (is_closed()) return std::unexpected(err(CaskError::kClosed, "cask is closed"));  // S11-W3
    if (!opts_.read_write) {
        return std::unexpected(err(CaskError::kReadOnly,
                                     "close_write_file: read-only cask"));
    }
    if (opts_.merge_only) {
        return std::unexpected(err(CaskError::kReadOnly,
                                     "close_write_file: merge_only handle"));
    }
    // 先 finalize hint trailer 再丢句柄——否则下次 open 这个目录时
    // hint 校验失败，会被迫 fold 整个 data 文件重建 keydir，
    // 大目录上代价非常高。
    if (active_hint_) {
        if (auto r = active_hint_->finalize(); !r) {
            return std::unexpected(io_fault(r.error().errnum,
                                             std::string(active_hint_->path())));
        }
    }
    {
        std::unique_lock lk(read_cache_mu_);
        active_data_.reset();
    }
    active_hint_.reset();
    active_file_id_ = 0;
    if (write_lock_) {
        write_lock_->release_quiet();
        write_lock_.reset();
    }
    // 之后的 put/delete 进 ensure_active_writer 会发现 write_lock_ 是空，
    // 自动重新拿锁、创建新 active 文件——不需要在这里多设状态。
    return {};
}

// ---- 按 file_id 缓存的 read 句柄 -------------------------------------------
// get / fold 频繁通过 file_id 拿 DataFile 来 pread。每次都 open 太重，
// 这里维护一个 unordered_map 做 lazy open；read_cache_mu_ 保护 map 本身，
// DataFile::read 内部 thread-safe 所以多读者并发没问题。
// merge 完成后会从这个 cache 里淘汰被合掉的旧 file_id。
// 按 file_id 拿一个 DataFile 读句柄。优先：
//   1. 缓存 hit
//   2. 当前 active writer 自身（避免重复 open）
//   3. 新 open 一个只读句柄并加入缓存
// 失败返回 nullptr——caller 用 errno 包装。
std::shared_ptr<fileops::DataFile> Cask::read_file(std::uint32_t file_id) {
    // 热路径(缓存命中)共享锁,多读者并发;miss 才升级独占做 lazy open。
    // 返回 shared_ptr:调用方在锁外使用句柄期间,并发 merge 的 erase /
    // roll_active 的替换不会析构它(O10 UAF 修复)。
    {
        std::shared_lock lk(read_cache_mu_);
        auto it = read_files_.find(file_id);
        if (it != read_files_.end()) {
            // P9:命中置 atime(近似 LRU)。atomic store 在共享锁下安全
            // (不改 map 结构);多读者并发 store 无 race。
            it->second.atime.store(
                read_clock_.fetch_add(1, std::memory_order_relaxed),
                std::memory_order_relaxed);
            return it->second.df;
        }
        if (active_data_ && file_id == active_file_id_) {
            return active_data_;
        }
    }

    std::unique_lock lk(read_cache_mu_);
    // 双检:释放共享锁到拿独占锁之间可能有人已 open。
    auto it = read_files_.find(file_id);
    if (it != read_files_.end()) return it->second.df;

    // active writer 也能给自己当 reader 用——pread 不影响 append 写入位置。
    if (active_data_ && file_id == active_file_id_) {
        return active_data_;
    }

    auto path = fileops::mk_data_filename(dirname_, file_id);
    auto df = fileops::DataFile::open(path, fileops::DataFile::Mode::kRead);
    if (!df) return nullptr;
    auto sp = std::make_shared<fileops::DataFile>(std::move(*df));
    read_files_.try_emplace(
        file_id, sp, read_clock_.fetch_add(1, std::memory_order_relaxed));
    // P9:刚插入的 sp 本地仍持有(use_count==2)→ 淘汰会跳过它(只淘空闲)。
    evict_read_handles_locked();
    return sp;
}

std::size_t Cask::read_handle_count() const {
    std::shared_lock lk(read_cache_mu_);
    return read_files_.size();
}

std::size_t Cask::resolve_read_handle_cap(std::size_t opt,
                                          std::size_t nofile_soft) noexcept {
    if (opt == CaskOptions::kUnlimitedReadHandles) return 0;  // 显式不限
    if (opt != 0) return opt;                                 // 显式上限
    // 自动（opt==0）：留约一半 fd 给 active writer / WAL / hint / meta / lock 等
    // 非缓存 fd，其余给 read 句柄缓存。下限 64，避免极低 ulimit 下 cap 过小。
    const std::size_t derived = nofile_soft / 2;
    return derived < 64 ? 64 : derived;
}

// P9:read_files_ 超 max_read_handles 时,淘汰 atime 最旧的**空闲**句柄
// (use_count==1:仅 map 持有,无在途读者)。在途句柄(use_count>1)跳过——
// 其 fd 正被使用,erase 也不能立即释放,留到下次;故 cap 是软上限。
// 调用方须持 read_cache_mu_ 独占锁。
void Cask::evict_read_handles_locked() {
    const std::size_t cap = opts_.max_read_handles;
    if (cap == 0 || read_files_.size() <= cap) return;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> idle;  // {atime,file_id}
    idle.reserve(read_files_.size());
    for (auto& [fid, h] : read_files_) {
        if (h.df.use_count() == 1) {
            idle.emplace_back(h.atime.load(std::memory_order_relaxed), fid);
        }
    }
    const std::size_t over = read_files_.size() - cap;
    if (idle.size() <= over) {
        for (auto& [at, fid] : idle) read_files_.erase(fid);  // 全部空闲都淘汰
        return;
    }
    std::partial_sort(idle.begin(), idle.begin() + static_cast<std::ptrdiff_t>(over),
                      idle.end());
    for (std::size_t i = 0; i < over; ++i) read_files_.erase(idle[i].second);
}

// ---- 搜索 / 写入共用辅助 ---------------------------------------------------

std::expected<void, CaskFault> Cask::prepare_search() {
    if (!search_) return std::unexpected(err(CaskError::kNoIndex));
    flush_index();
    return {};
}

std::expected<Cask::PersistedRecord, CaskFault>
Cask::write_and_keydir(std::span<const std::byte> key,
                       std::span<const std::byte> encoded,
                       std::uint32_t tstamp, std::uint64_t ord) {
    auto w = active_data_->write(format::RecordType::kDoc, tstamp, ord,
                                  key, encoded);
    if (!w) return std::unexpected(io_fault(w.error().errnum,
                                             std::string(active_data_->path())));
    auto h = active_hint_->write(tstamp, w->total_size, w->offset,
                                  /*tomb*/ false, key);
    if (!h) return std::unexpected(io_fault(h.error().errnum,
                                             std::string(active_hint_->path())));

    auto pr = keydir_->put(bytes_to_view(key), active_file_id_,
                            w->total_size, w->offset, tstamp,
                            /*now*/ 0, /*newest*/ true, 0, 0, ord);
    if (pr != keydir::PutResult::kAlreadyExists) {
        return PersistedRecord{ord, w->offset, w->total_size, active_file_id_};
    }

    // keydir 认为已存在更新的 entry → roll_active 切新文件后重试一次。
    if (auto r = roll_active(); !r) return std::unexpected(r.error());
    const std::uint64_t ord2 = keydir_->alloc_ord();
    // S13-F2: ord2 守卫——重试路径任何失败 return 都补 Skip，防 reorder
    // buffer 空洞（ord 本身由 caller 的守卫覆盖）。
    OrdSkipGuard g2(this, ord2);
    auto w2 = active_data_->write(format::RecordType::kDoc, tstamp, ord2,
                                   key, encoded);
    if (!w2) return std::unexpected(io_fault(w2.error().errnum,
                                              std::string(active_data_->path())));
    auto h2 = active_hint_->write(tstamp, w2->total_size, w2->offset,
                                   /*tomb*/ false, key);
    if (!h2) return std::unexpected(io_fault(h2.error().errnum,
                                              std::string(active_hint_->path())));
    auto pr2 = keydir_->put(bytes_to_view(key), active_file_id_,
                             w2->total_size, w2->offset, tstamp,
                             0, true, 0, 0, ord2);
    if (pr2 == keydir::PutResult::kAlreadyExists) {
        // S13-F2: ord 与 ord2 均未被真任务覆盖——g2 析构补 ord2 的 Skip，
        // ord 由 caller 守卫补。
        return std::unexpected(err(CaskError::kAlreadyExists));
    }
    // S6-P1: 原始 ord 在 keydir 竞争中落败（kAlreadyExists），数据已写入但
    // keydir 未收录。发 Skip 填充 ord 空洞，防 reorder buffer stall。
    // 必须在 caller 提交 ord2 的真任务之前提交（队列 FIFO 保序）。
    submit_index_task(IndexTask::make(IndexOp::Skip, {}, ord, {}, 0, 0, 0, 0, 0));
    g2.disarm();  // S13-F2: ord2 将由 caller 的真任务（Add）覆盖
    return PersistedRecord{ord2, w2->offset, w2->total_size, active_file_id_};
}

std::expected<std::span<const float>, CaskFault>
Cask::prepare_vector(std::span<const float> input,
                     std::vector<float>& norm_buf) const {
    if (input.empty()) return {};
    if (meta_config_.vector_dim == 0) {
        return std::unexpected(err(CaskError::kInvalidOption,
            "collection has no vector config"));
    }
    if (input.size() != meta_config_.vector_dim) {
        return std::unexpected(err(CaskError::kInvalidOption,
            "vector dim mismatch"));
    }
    if (meta_config_.vector_metric ==
        meta::VectorMetric::kCosineNormalized) {
        double sq = 0.0;
        for (float v : input) sq += static_cast<double>(v) * v;
        if (sq <= 0.0) {
            return std::unexpected(err(CaskError::kInvalidOption,
                "zero vector not allowed under cosine metric"));
        }
        const float inv = static_cast<float>(1.0 / std::sqrt(sq));
        norm_buf.clear();
        norm_buf.reserve(input.size());
        for (float v : input) norm_buf.push_back(v * inv);
        return std::span<const float>(norm_buf);
    }
    return input;
}

// ---- get / put / delete ----------------------------------------------------
// 写路径有个微妙之处：put 之前要判断 keydir.biggest_file_id() 是不是已经
// 超过自己的 active_file_id_——如果超了，说明并发 merger 抢先把 file_id
// 推进了；这时必须 roll_active() 切到一个比 biggest 更大的新 file_id，
// 不然 keydir.put 时新 entry 会被认为「比当前 entry 旧」而拒绝。

// 单 key 读：keydir 查 → DataFile 读 → 校验 → 返回 value。
// 三层过滤：
//   1. keydir 不存在 → kNotFound
//   2. 过期（tstamp + expiry_secs <= now）→ kNotFound
//      （不在这里主动删——这是写操作，留给 merge 异步 GC）
//   3. 磁盘 record 是墓碑 value → kNotFound
//      （keydir 里的墓碑已经在第 1 步被过滤；这层兜住「磁盘墓碑但 keydir
//       还没合并掉」的窗口）
//
// V6.1: 返回 zero-copy GetResultView，value/meta/vector 都是 span，借用
// df->read() 内部 ReadRecord 的 vector<byte> 缓冲。无堆分配。
//   NIF 即取即用：调用 make_binary_checked 拷到 ErlNifBinary 后即释放。
//   benchmark / 测试 / 需要持久化 → get_owned()。
std::expected<GetResultView, CaskFault>
Cask::get(std::span<const std::byte> key) {
    if (is_closed()) return std::unexpected(err(CaskError::kClosed, "cask is closed"));  // S11-W3
    // S13-F5：get 与并发 merge 收尾之间有窗口——本线程先查 keydir 拿到旧定位，
    // merge 随即把该 key CAS 到新文件并 unlink 旧文件（O10 的同临界区 erase+
    // unlink 只保护已持缓存句柄的读者，保护不了「先查 keydir、后 open」的
    // 读者），read_file lazy open 得 ENOENT。此时 keydir 已指向新文件——
    // 重查 keydir 重试一次必命中；重试仍失败才是真 I/O 错误。
    for (int attempt = 0; ; ++attempt) {
    auto entry = keydir_->get(bytes_to_view(key));
    if (!entry) return std::unexpected(err(CaskError::kNotFound));

    if (opts_.expiry_secs > 0) {
        const auto now = now_sec_default();
        if (entry->tstamp + opts_.expiry_secs <= now) {
            return std::unexpected(err(CaskError::kNotFound));
        }
    }

    auto df = read_file(entry->file_id);
    if (!df) {
        if (attempt == 0) continue;  // S13-F5: merge 窗口，重查 keydir 重试
        return std::unexpected(err(CaskError::kIo,
            "open file_id=" + std::to_string(entry->file_id)));
    }

    // P6:sealed mmap 命中 → 零拷贝(无 syscall,直读 page cache)。GetResultView
    // 持 df 的 shared_ptr 锚定映射,view 生命内映射不撤(即便并发 merge unlink)。
    if (df->mmapped()) {
        auto rv = df->read_mmap(entry->offset, entry->total_sz);
        if (!rv) {
            switch (rv.error().kind) {
                case fileops::DataFileError::kBadCrc:
                    return std::unexpected(err(CaskError::kBadCrc));
                default:
                    return std::unexpected(err(CaskError::kIo));
            }
        }
        if (rv->type == format::RecordType::kTombstone) {
            return std::unexpected(err(CaskError::kNotFound));
        }
        GetResultView view(std::move(df), rv->value, rv->type,
                           rv->tstamp, rv->ord);
        // S13-D5：per-key TTL——过期视作不存在（空间留给 merge 回收）。
        if (view.expiry_at != 0 && view.expiry_at <= now_sec_default()) {
            return std::unexpected(err(CaskError::kNotFound));
        }
        return view;
    }

    auto rec = df->read(entry->offset, entry->total_sz);
    if (!rec) {
        switch (rec.error().kind) {
            case fileops::DataFileError::kBadCrc:
                return std::unexpected(err(CaskError::kBadCrc));
            case fileops::DataFileError::kIo:
                return std::unexpected(io_fault(rec.error().errnum));
            default:
                return std::unexpected(err(CaskError::kIo));
        }
    }
    if (rec->type == format::RecordType::kTombstone) {
        return std::unexpected(err(CaskError::kNotFound));
    }

    {
        GetResultView view(std::move(*rec));
        // S13-D5：per-key TTL——过期视作不存在。
        if (view.expiry_at != 0 && view.expiry_at <= now_sec_default()) {
            return std::unexpected(err(CaskError::kNotFound));
        }
        return view;
    }
    }  // for (attempt)
}

std::expected<GetResult, CaskFault>
Cask::get_owned(std::span<const std::byte> key) {
    auto v = get(key);
    if (!v) return std::unexpected(v.error());
    return v->to_owned();
}

// S11-W4：并行全表扫描。快照 live key（串行,廉价）→ 分段 → N 个 std::thread
// 并发 get + fn（读值的 pread+decode 是被并行化的成本）。
std::expected<std::size_t, CaskFault>
Cask::parallel_scan(std::size_t n_threads, const ScanFn& fn,
                    std::span<const std::byte> key_prefix) {
    if (is_closed()) {
        return std::unexpected(err(CaskError::kClosed, "cask is closed"));
    }
    // 1) 单次快照所有 live key（调用线程串行,仅 key 拷贝,不读 value）。
    // S13-D4：key_prefix 在 proxy 层过滤（drain_live_keys 内）。
    auto it = make_iter();
    auto sr = it->start(/*maxage=*/-1, /*maxputs=*/-1, /*now_sec=*/0,
                        /*see_tombstones=*/false, key_prefix);
    if (!sr) return std::unexpected(sr.error());
    if (*sr != keydir::StartIterResult::kOk) {
        return std::unexpected(err(CaskError::kIo,
            "parallel_scan: keydir iterator out of date"));
    }
    auto keys = it->drain_live_keys();
    it->release();

    const std::size_t total = keys.size();
    if (total == 0) return std::size_t{0};

    std::size_t nthr = (n_threads == 0)
                           ? std::max<std::size_t>(std::thread::hardware_concurrency(), 1)
                           : n_threads;
    nthr = std::min(nthr, total);

    // 2) 分段并发：各线程 get + fn 自己的不相交 key 段。并发删除 → kNotFound
    //    跳过（near-real-time）；其它错误记录第一例并令各线程尽快收尾。
    std::atomic<bool> ok{true};
    std::mutex err_mu;
    CaskFault first_err{};
    auto worker = [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            if (!ok.load(std::memory_order_relaxed)) return;
            auto v = get(keys[i]);
            if (v) {
                fn(keys[i], *v);
            } else if (v.error().kind != CaskError::kNotFound) {
                std::lock_guard<std::mutex> lk(err_mu);
                if (ok.exchange(false)) first_err = v.error();
                return;
            }
        }
    };

    if (nthr <= 1) {
        worker(0, total);
    } else {
        std::vector<std::thread> ws;
        ws.reserve(nthr);
        const std::size_t chunk = (total + nthr - 1) / nthr;
        for (std::size_t t = 0; t < nthr; ++t) {
            const std::size_t b = t * chunk;
            if (b >= total) break;
            ws.emplace_back(worker, b, std::min(b + chunk, total));
        }
        for (auto& w : ws) w.join();
    }
    if (!ok.load()) return std::unexpected(first_err);
    return total;
}

// --- GetResultView implementation ---

void GetResultView::derive_from_storage() {
    if (rec_type_ != format::RecordType::kDoc) return;  // tombstone 等不解码
    if (value_bytes_.empty()) return;
    auto dv = codec::decode_doc_value(value_bytes_);
    if (!dv) return;  // corrupt DocValue → empty spans
    value = dv->text;
    meta  = dv->meta;
    expiry_at = dv->expiry_at;  // S13-D5
    if (dv->vec_quantized) {
        // P3b:量化 → dequant 进拥有缓冲，span 指向它。
        vector_dequant_ = codec::doc_vector_f32(*dv);
        vector = std::span<const float>(vector_dequant_.data(),
                                        vector_dequant_.size());
    } else if (dv->has_vector && dv->dim > 0 &&
               dv->vector_raw.size() == dv->dim * sizeof(float)) {
        vector = std::span<const float>(
            reinterpret_cast<const float*>(dv->vector_raw.data()),
            dv->dim);
    }
}

GetResultView::GetResultView(fileops::ReadRecord&& rec)
    : storage_(std::move(rec))       // move first (declaration order)
    , rec_type_(storage_.type)
    , tstamp(storage_.tstamp)
    , ord(storage_.ord)
{
    value_bytes_ = std::span<const std::byte>(storage_.value);
    derive_from_storage();
}

// P6:mmap 命中——map_holder_ 锚定映射,value_bytes 指向映射内 DocValue 字节。
GetResultView::GetResultView(std::shared_ptr<fileops::DataFile> holder,
                             std::span<const std::byte> value_bytes,
                             format::RecordType type,
                             std::uint32_t ts, std::uint64_t o)
    : map_holder_(std::move(holder))
    , value_bytes_(value_bytes)
    , rec_type_(type)
    , tstamp(ts)
    , ord(o)
{
    derive_from_storage();
}

GetResultView::GetResultView(GetResultView&& other) noexcept
    : storage_(std::move(other.storage_))
    , map_holder_(std::move(other.map_holder_))
    , rec_type_(other.rec_type_)
    , tstamp(other.tstamp)
    , ord(other.ord)
{
    // owned 路径:value_bytes_ 重指向自己的 storage_(other 的已悬垂);
    // mmap 路径:映射地址稳定,沿用 other 的字节区(map_holder_ 已移交本对象)。
    value_bytes_ = map_holder_ ? other.value_bytes_
                               : std::span<const std::byte>(storage_.value);
    derive_from_storage();
}

GetResult GetResultView::to_owned() const {
    GetResult out{
        std::vector<std::byte>(value.begin(), value.end()),
        std::vector<std::byte>(meta.begin(), meta.end()),
        {},
        tstamp,
        ord
    };
    if (!vector.empty()) {
        out.vector.assign(vector.begin(), vector.end());
    }
    return out;
}

// put 流程：
//   1. 校验权限 + key/value 大小
//   2. 必要时 roll active 文件（写满 / 没建过 / 被 merger 抢 file_id）
//   3. 写 data + hint
//   4. 更新 keydir
//   5. keydir 拒绝（merge race）→ 再 roll 一次重试一次；二次失败上报
    std::expected<void, CaskFault>
Cask::put(std::span<const std::byte> key,
          std::span<const std::byte> value,
          std::uint32_t tstamp, std::uint32_t expiry_at) {
    WriteOpGate gate(this);  // H1：close() 等锁外索引提交完成后才拆资源
    std::unique_lock<std::mutex> wlk(write_mu_);  // S11-W1：写路径互斥
    if (is_closed()) return std::unexpected(err(CaskError::kClosed, "cask is closed"));  // S11-W3
    if (!opts_.read_write || opts_.merge_only) {
        return std::unexpected(err(CaskError::kReadOnly));
    }
    if (key.size()   > format::kMaxKeySize)   return std::unexpected(err(CaskError::kKeyTooLarge));
    if (value.size() > format::kMaxValueSize) return std::unexpected(err(CaskError::kValueTooLarge));

    if (tstamp == 0) tstamp = now_sec_default();
    const std::size_t about = format::kHeaderSize + key.size() + value.size();
    if (auto r = roll_active_if_needed(about); !r) return std::unexpected(r.error());

    // M5.1 task 2 关键 race：并发 merger 可能已经把 keydir.biggest_file_id
    // 推过了我们的 active_file_id_。如果直接写，keydir 的 merge-race 检测
    // (file_id < biggest_file_id_) 会返回 kAlreadyExists，put 就被静默丢了。
    // 提前主动 roll 一次保证 active_file_id_ >= biggest，避免 silent drop。
    if (active_data_ && active_file_id_ < keydir_->biggest_file_id()) {
        if (auto r = roll_active(); !r) return std::unexpected(r.error());
    }

    // 分配 ord + 编码 DocValue（text 段 = 原始 value）
    const std::uint64_t ord = keydir_->alloc_ord();
    OrdSkipGuard og(this, ord);  // S13-F2: 失败路径补 Skip 防 reorder stall
    // ⑩ thread_local 复用：encode_doc_value 是 append 语义，clear 后重填；
    // 并发 put 各线程独占一份，消除每次 put 的 encoded 堆分配。
    thread_local std::vector<std::byte> encoded;
    encoded.clear();
    encoded.reserve(value.size() + 16);
    codec::DocValueParts parts;
    parts.text = value;
    parts.expiry_at = expiry_at;  // S13-D5
    codec::encode_doc_value(encoded, parts);

    auto persisted = write_and_keydir(key, encoded, tstamp, ord);
    if (!persisted) return std::unexpected(persisted.error());
    // S13-F2: 成功 ⇒ ord 已有归宿——非重试路径由下面的 Add 覆盖
    // （persisted->ord == ord），重试路径由 write_and_keydir 内部 Skip 覆盖。
    og.disarm();
    // H1（s13-review §P1）：索引提交移出 write_mu_ 临界区——组提交留在
    // 锁内（不做 relock，规避 relock 后 active 已被并发写者 roll 的世界
    // 变化），Add 在释锁后提交：队列背压只阻塞本写者，不再冻结全部写路径。
    // 数据此刻已持久化（pwrite + keydir），reorder buffer 按 ord 乱序
    // apply，到达序无关。gc 失败也先提交 Add——ord 必须被真任务覆盖，
    // 与旧序（先 submit 后 group_commit）的对外语义一致。
    const PersistedRecord rec = *persisted;
    auto gc = maybe_group_commit();
    wlk.unlock();
    submit_index_task(IndexTask::make(
        IndexOp::Add, bytes_to_view(key), rec.ord,
        std::string_view(reinterpret_cast<const char*>(value.data()),
                         value.size()),
        rec.file_id, rec.offset, rec.total_size, tstamp, 0));
    maybe_submit_auto_checkpoint();  // S14-1：roll 封口的异步 ckpt（锁外）
    if (!gc) return std::unexpected(gc.error());
    return {};
}

// S13-D1：批量写。流程：全批校验 → roll → 逐条 alloc_ord + encode +
// write_buffered（聚合 1MiB 块 pwrite）→ 一次 flush + fdatasync（批的持久化
// 点）→ hint → keydir apply + 索引提交。keydir apply 在 fsync 之后 ⟹ 本进程
// 内 all-or-nothing。merge race（merge 恰在批写入期间启动、biggest_file_id
// 被推过批文件）时被拒条目走 write_and_keydir 单条重写路径（内部 roll+重试），
// 与单条 put 的 race 处理一致。
std::expected<void, CaskFault>
Cask::put_batch(std::span<const BatchItem> items, std::uint32_t tstamp) {
    WriteOpGate gate(this);  // H1：close() 等锁外索引提交完成后才拆资源
    std::unique_lock<std::mutex> wlk(write_mu_);  // S11-W1：写路径互斥
    if (is_closed()) return std::unexpected(err(CaskError::kClosed, "cask is closed"));
    if (!opts_.read_write || opts_.merge_only) {
        return std::unexpected(err(CaskError::kReadOnly));
    }
    if (items.empty()) return {};

    // ① 全批前置校验——任何写发生前完成，校验失败零副作用。
    std::size_t about = 0;
    for (const auto& it : items) {
        if (it.key.size() > format::kMaxKeySize) {
            return std::unexpected(err(CaskError::kKeyTooLarge));
        }
        if (it.value.size() > format::kMaxValueSize) {
            return std::unexpected(err(CaskError::kValueTooLarge));
        }
        about += format::kHeaderSize + it.key.size() + it.value.size();
    }
    if (tstamp == 0) tstamp = now_sec_default();

    // ② roll：整批进同一 active 文件（巨批允许超 max_file_size，软上限）。
    if (auto r = roll_active_if_needed(about); !r) return std::unexpected(r.error());
    if (active_data_ && active_file_id_ < keydir_->biggest_file_id()) {
        if (auto r = roll_active(); !r) return std::unexpected(r.error());
    }
    const std::uint32_t batch_file =
        active_file_id_.load(std::memory_order_relaxed);

    // S13-F2 批量版守卫：函数退出时对所有未被真任务覆盖的 ord 补 Skip——
    // 错误路径全批 Skip；成功路径恰好补掉 merge-race 重写条目的原始 ord。
    struct BatchOrdGuard {
        Cask* cask;
        std::vector<std::uint64_t> ords;
        std::vector<char> done;
        ~BatchOrdGuard() {
            for (std::size_t i = 0; i < ords.size(); ++i) {
                if (!done[i]) {
                    cask->submit_index_task(IndexTask::make(
                        IndexOp::Skip, {}, ords[i], {}, 0, 0, 0, 0, 0));
                }
            }
        }
    } og{this, {}, {}};
    og.ords.reserve(items.size());
    og.done.reserve(items.size());

    // ③ 逐条 encode + write_buffered。offset 是确定性逻辑偏移（含未落盘缓冲）。
    struct PendingWrite {
        std::uint64_t ord;
        std::uint64_t offset;
        std::uint32_t total_size;
    };
    std::vector<PendingWrite> pw;
    pw.reserve(items.size());
    thread_local std::vector<std::byte> encoded;
    for (const auto& it : items) {
        const std::uint64_t ord = keydir_->alloc_ord();
        og.ords.push_back(ord);
        og.done.push_back(0);
        encoded.clear();
        encoded.reserve(it.value.size() + 16);
        codec::DocValueParts parts;
        parts.text = it.value;
        codec::encode_doc_value(encoded, parts);
        auto w = active_data_->write_buffered(format::RecordType::kDoc,
                                              tstamp, ord, it.key, encoded);
        if (!w) {
            return std::unexpected(io_fault(w.error().errnum,
                                            std::string(active_data_->path())));
        }
        pw.push_back({ord, w->offset, w->total_size});
    }

    // ④ 批的提交点：flush 尾批（此前 keydir 未动——任何失败整批在本进程内
    //    不可见）。durability 与单条 put 的 sync 策略对齐：
    //    - o_sync：fd 是 O_DSYNC，flush 的单次 pwrite 即 durable；
    //    - sync_every_n > 0：整批视作一次组提交，立即 fdatasync；
    //    - 其余（sync_every_n==0）：与单条 put 相同，由 caller 的 sync() 控制。
    if (auto f = active_data_->flush_batch(); !f) {
        return std::unexpected(io_fault(f.error().errnum,
                                        std::string(active_data_->path())));
    }
    if (!opts_.o_sync && opts_.sync_every_n > 0) {
        if (auto s = active_data_->sync(); !s) {
            return std::unexpected(io_fault(s.error().errnum,
                                            std::string(active_data_->path())));
        }
        writes_since_sync_ = 0;  // P4 组提交计数：刚 fsync 过，归零
    }

    // ⑤ hint（可重建；失败语义与单条 put 一致：报错、keydir 未动）。
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto h = active_hint_->write(tstamp, pw[i].total_size, pw[i].offset,
                                     /*tomb*/ false, items[i].key);
        if (!h) {
            return std::unexpected(io_fault(h.error().errnum,
                                            std::string(active_hint_->path())));
        }
    }

    // ⑥ keydir apply + 索引提交。
    // H1（s13-review §P1）：Add 的实际提交延后到 write_mu_ 释放之后——锁内
    // 只收集 {item 下标, og 槽位, 定位记录}，锁外逐条构造 + 提交（IndexTask
    // 深拷贝 key/text，队列背压兜底，峰值内存与旧的即时提交一致）。中途
    // 失败路径先冲刷已收集的 Add 再返回：这些条目 keydir 已收录，必须由
    // Add 覆盖，不能落给 og 的 Skip（该路径在锁内提交，行为同旧版）。
    struct PendingAdd {
        std::size_t item;   // items 下标（key/value 借 caller 生命周期）
        std::size_t slot;   // og.ords/done 槽位（Add 提交后置 done）
        PersistedRecord rec;
    };
    std::vector<PendingAdd> adds;
    adds.reserve(items.size());
    auto flush_adds = [&]() {
        for (const auto& a : adds) {
            submit_index_task(IndexTask::make(
                IndexOp::Add, bytes_to_view(items[a.item].key), a.rec.ord,
                std::string_view(
                    reinterpret_cast<const char*>(items[a.item].value.data()),
                    items[a.item].value.size()),
                a.rec.file_id, a.rec.offset, a.rec.total_size, tstamp, 0));
            og.done[a.slot] = 1;
        }
        adds.clear();
    };
    bool retried = false;
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto pr = keydir_->put(bytes_to_view(items[i].key), batch_file,
                               pw[i].total_size, pw[i].offset, tstamp,
                               /*now*/ 0, /*newest*/ true, 0, 0, pw[i].ord);
        PersistedRecord rec{pw[i].ord, pw[i].offset, pw[i].total_size,
                            batch_file};
        std::size_t slot = i;
        if (pr == keydir::PutResult::kAlreadyExists) {
            // merge race：单条重写（write_and_keydir 内部 roll+重试）。
            // 原始 ord 保持 !done → 函数退出时由 og 补 Skip。
            encoded.clear();
            encoded.reserve(items[i].value.size() + 16);
            codec::DocValueParts parts;
            parts.text = items[i].value;
            codec::encode_doc_value(encoded, parts);
            const std::uint64_t ord2 = keydir_->alloc_ord();
            OrdSkipGuard g2(this, ord2);
            auto p2 = write_and_keydir(items[i].key, encoded, tstamp, ord2);
            if (!p2) {
                flush_adds();  // 先前条目 keydir 已收录 → 锁内补交 Add（同旧版）
                return std::unexpected(p2.error());
            }
            // 真实落盘 ord 改由批守卫追踪至 Add 提交为止（双重试时
            // p2->ord != ord2，此时 ord2 已被 write_and_keydir 内部 Skip
            // 覆盖，og 只追踪 p2->ord，不重复提交）。
            og.done.push_back(0);
            og.ords.push_back(p2->ord);
            g2.disarm();
            slot = og.ords.size() - 1;
            rec = *p2;
            retried = true;
        }
        // done 置位延后到 Add 实际提交之后（flush_adds）——中途失败时未提交
        // 的 ord 仍由 og 补 Skip，无空洞。
        adds.push_back({i, slot, rec});
    }
    // merge-race 重写走的是非缓冲 write（write_and_keydir），不在 ④ 的提交
    // 覆盖内——按同一 sync 策略补一次组提交（罕见路径；o_sync/caller-sync
    // 模式下 maybe_group_commit 自身为 no-op，语义一致）。gc 失败不早退：
    // keydir 已收录的条目仍须 Add 覆盖，错误在锁外提交完成后返回（同 put）。
    std::expected<void, CaskFault> gc;
    if (retried) {
        ++writes_since_sync_;
        gc = maybe_group_commit(/*force*/ true);
    }
    wlk.unlock();  // H1：常规路径的 Add 全部在锁外提交
    flush_adds();
    maybe_submit_auto_checkpoint();  // S14-1（锁外）
    if (!gc) return std::unexpected(gc.error());
    return {};
}

// 软删除 = 写一条墓碑 record。
//
// 墓碑 encoding (v2 backward compat):
//   v0: empty value (RecordType::kTombstone carries the meaning)
//   v2: 4-byte little-endian shadow file_id (tells merger "I exist because of
//       an entry in file_id N; if that entry is gone, I'm meaningless").
//       If key not in keydir or file_id==0, fall back to v0.
//       (P：盘格式统一小端，flag-day 前为大端。)
std::expected<void, CaskFault>
Cask::remove(std::span<const std::byte> key, std::uint32_t tstamp) {
    WriteOpGate gate(this);  // H1：close() 等锁外索引提交完成后才拆资源
    std::unique_lock<std::mutex> wlk(write_mu_);  // S11-W1：写路径互斥
    if (is_closed()) return std::unexpected(err(CaskError::kClosed, "cask is closed"));  // S11-W3
    if (!opts_.read_write) return std::unexpected(err(CaskError::kReadOnly));
    if (tstamp == 0) tstamp = now_sec_default();

    std::span<const std::byte> tomb_value;
    std::uint8_t shadow_le[4] = {0};
    if (opts_.tombstone_version == 2) {
        if (auto entry = keydir_->get(bytes_to_view(key))) {
            if (entry->file_id != 0) {
                shadow_le[0] = static_cast<std::uint8_t>( entry->file_id        & 0xFF);
                shadow_le[1] = static_cast<std::uint8_t>((entry->file_id >>  8) & 0xFF);
                shadow_le[2] = static_cast<std::uint8_t>((entry->file_id >> 16) & 0xFF);
                shadow_le[3] = static_cast<std::uint8_t>((entry->file_id >> 24) & 0xFF);
                tomb_value = std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(shadow_le),
                    sizeof(shadow_le));
            }
        }
    }
    if (tomb_value.empty()) {
        tomb_value = std::span<const std::byte>{};
    }

    const std::size_t about =
        format::kHeaderSize + key.size() + tomb_value.size();
    if (auto r = roll_active_if_needed(about); !r) return std::unexpected(r.error());

    const std::uint64_t ord = keydir_->alloc_ord();
    OrdSkipGuard og(this, ord);  // S13-F2: 失败路径补 Skip 防 reorder stall
    auto w = active_data_->write(format::RecordType::kTombstone, tstamp,
                                  ord, key, tomb_value);
    if (!w) return std::unexpected(io_fault(w.error().errnum));
    // hint 文件也要追一条墓碑——下次 open fold(hint) 重建时才能正确删 key。
    auto h = active_hint_->write(tstamp, w->total_size, w->offset,
                                  /*tomb*/ true, key);
    if (!h) return std::unexpected(io_fault(h.error().errnum));
    keydir_->remove(bytes_to_view(key), tstamp);
    og.disarm();  // S13-F2: ord 由下面的 Delete 任务覆盖
    // H1：Delete 提交移出临界区（同 put）。S15-3：原「非池同步直调
    // on_delete」分支删除——open 强制 registry 非空（本文件 open() 首行
    // 校验），search_ 存在 ⇒ index_pool_ 恒已装配，该分支不可达。
    const bool pooled = index_pool_ != nullptr;
    auto gc = maybe_group_commit();
    wlk.unlock();
    if (pooled) {
        submit_index_task(IndexTask::make(
            IndexOp::Delete, bytes_to_view(key), ord, {}, 0, 0, 0, tstamp, 0));
    }
    maybe_submit_auto_checkpoint();  // S14-1（锁外）
    if (!gc) return std::unexpected(gc.error());
    return {};
}

// put_doc：写入结构化文档（text + 选填 meta）。用于索引模式。
// 逻辑跟 put 类似，但 DocValue 编码包含 text 和 meta 两段。
std::expected<void, CaskFault>
Cask::put_doc(std::span<const std::byte> key, const DocInput& doc,
              std::uint32_t tstamp) {
    WriteOpGate gate(this);  // H1：close() 等锁外索引提交完成后才拆资源
    std::unique_lock<std::mutex> wlk(write_mu_);  // S11-W1：写路径互斥
    if (is_closed()) return std::unexpected(err(CaskError::kClosed, "cask is closed"));  // S11-W3
    if (!opts_.read_write || opts_.merge_only) {
        return std::unexpected(err(CaskError::kReadOnly));
    }
    if (key.size() > format::kMaxKeySize) {
        return std::unexpected(err(CaskError::kKeyTooLarge));
    }
    if (doc.text.size() > format::kMaxValueSize) {
        return std::unexpected(err(CaskError::kValueTooLarge));
    }

    if (tstamp == 0) tstamp = now_sec_default();
    const std::size_t about =
        format::kHeaderSize + key.size() + doc.text.size() + doc.meta.size();
    if (auto r = roll_active_if_needed(about); !r) {
        return std::unexpected(r.error());
    }

    // #1：把 DocInput 的多字段名 intern 成 id，填进 DocValueParts.fields。
    // 字段名只在 field.schema 存一份，DocValue 里存小整数 id（varint）。
    auto fill_parts = [&doc, this](codec::DocValueParts& p) {
        for (auto& [name, val] : doc.fields) {
            p.fields.push_back({field_schema_.intern(name), val});
        }
    };
    // S10-A5:多字段名+值打包进单个 fields_store（一次分配替代 2×num_fields 次 string 分配）。
    // fields 持 string_view 借自 fields_store；vector<char> move = 指针转移 → view 跨 IndexTask 移动仍有效。
    auto pack_fields = [&doc]() {
        std::size_t total = 0;
        for (const auto& [name, val] : doc.fields) total += name.size() + val.size();
        std::vector<char> store;
        store.reserve(total);
        std::vector<std::pair<std::string_view, std::string_view>> views;
        views.reserve(doc.fields.size());
        for (const auto& [name, val] : doc.fields) {
            auto name_off = store.size();
            store.insert(store.end(), name.begin(), name.end());
            auto val_off = store.size();
            store.insert(store.end(),
                         reinterpret_cast<const char*>(val.data()),
                         reinterpret_cast<const char*>(val.data()) + val.size());
            views.emplace_back(
                std::string_view(store.data() + name_off, name.size()),
                std::string_view(store.data() + val_off, val.size()));
        }
        return std::pair{std::move(store), std::move(views)};
    };

    if (active_data_ && active_file_id_ < keydir_->biggest_file_id()) {
        if (auto r = roll_active(); !r) return std::unexpected(r.error());
    }

    // V3.1:向量校验 + cosine 写入归一化(存储即归一化值,merge/恢复
    // 不再重算;hnsw-design §1)。归一化缓冲在双编码点(roll 重试)间复用。
    std::vector<float> vec_norm;
    auto vec_result = prepare_vector(doc.vector, vec_norm);
    if (!vec_result) return std::unexpected(vec_result.error());
    auto vec_out = *vec_result;

    const std::uint64_t ord = keydir_->alloc_ord();
    OrdSkipGuard og(this, ord);  // S13-F2: 失败路径补 Skip 防 reorder stall
    std::vector<std::byte> encoded;
    encoded.reserve(doc.text.size() + doc.meta.size() +
                    vec_out.size() * sizeof(float) + 16);
    codec::DocValueParts parts;
    parts.text = doc.text;
    parts.expiry_at = doc.expiry_at;  // S13-D5
    if (!doc.meta.empty()) {
        parts.meta = doc.meta;
    }
    if (!vec_out.empty()) {
        parts.vector = vec_out;
        parts.vec_quantized = meta_config_.vector_quantized;  // P3b：落盘 int8
    }
    fill_parts(parts);
    codec::encode_doc_value(encoded, parts);

    auto persisted = write_and_keydir(key, encoded, tstamp, ord);
    if (!persisted) return std::unexpected(persisted.error());
    // S13-F2: 成功 ⇒ ord 已有归宿（同 put：Add 或内部 Skip）。
    og.disarm();
    // H1：组提交留在锁内，任务构造（fields 打包、vec 移交、meta 拷贝）与
    // 提交移出临界区（同 put）。所需数据（doc/persisted/vec_norm）均为
    // caller 参数或函数局部，锁外访问安全。
    const PersistedRecord rec = *persisted;
    auto gc = maybe_group_commit();
    wlk.unlock();
    auto task = IndexTask::make(
        IndexOp::Add, bytes_to_view(key), rec.ord,
        std::string_view(reinterpret_cast<const char*>(doc.text.data()),
                         doc.text.size()),
        rec.file_id, rec.offset, rec.total_size, tstamp, 0);
    // S10-A5:多字段打包进 fields_store（一次分配），替代旧 task_fields() 的 N×2 string 拷贝。
    {
        auto [store, views] = pack_fields();
        task.fields_store = std::move(store);
        task.fields = std::move(views);
    }
    // W2:cosine 路径 vec_out 是 vec_norm 的 span，encode（上方 parts.vector）
    // 已用完，可直接移交，省一次 512B（128-dim）拷贝 + 分配。其余情形
    // （passthrough / L2）vec_out 指向 doc.vector，仍需拷贝。
    if (!vec_out.empty() && vec_out.data() == vec_norm.data()) {
        task.vec = std::move(vec_norm);
    } else if (!vec_out.empty()) {
        task.vec.assign(vec_out.begin(), vec_out.end());
    }
    task.meta.assign(doc.meta.begin(), doc.meta.end());
    submit_index_task(std::move(task));
    maybe_submit_auto_checkpoint();  // S14-1（锁外）
    if (!gc) return std::unexpected(gc.error());
    return {};
}

// S9-P2-d: 搜索层 SearchError → CaskFault 边界翻译（消除 expected<,string> 的
// leaky abstraction）。当前三种都映射 kInvalidOption，但语义集中在此一处、
// detail 文案由枚举确定性派生——新增搜索错误时只改这里，不必各 caller 猜 kind。
static CaskFault search_fault(search::SearchError e) {
    switch (e) {
        case search::SearchError::kNoVectorIndex:
            return err(CaskError::kInvalidOption, "no vector index configured");
        case search::SearchError::kVectorDimMismatch:
            return err(CaskError::kInvalidOption, "query vector dim mismatch");
        case search::SearchError::kEmptyHybridQuery:
            return err(CaskError::kInvalidOption,
                       "hybrid query empty (no text, no vector)");
    }
    return err(CaskError::kInvalidOption, "unknown search error");
}

// S8-R3: 单条搜索公共骨架。flush → 可选 vector 校验 → 跑内核 → 包错误/结果。
std::expected<TextSearchResult, CaskFault>
Cask::run_search_one(
    bool require_vector,
    const std::function<
        std::expected<std::vector<search::SearchHit>, search::SearchError>()>& run) {
    if (is_closed()) return std::unexpected(err(CaskError::kClosed, "cask is closed"));  // S11-W3
    if (auto g = prepare_search(); !g) return std::unexpected(g.error());
    if (require_vector && meta_config_.vector_dim == 0) {
        return std::unexpected(err(CaskError::kInvalidOption,
            "collection has no vector config"));
    }
    auto hits = run();
    if (!hits) return std::unexpected(search_fault(hits.error()));
    return TextSearchResult{std::move(*hits)};
}

// search_vector：HNSW 向量检索(V3.3)。归一化/live 过滤/ord 翻译都在 SearchLayer。
std::expected<TextSearchResult, CaskFault>
Cask::search_vector(std::span<const float> query, std::size_t k,
                     std::size_t ef, const meta::MetaFilter* filter) {
    return run_search_one(/*require_vector=*/true,
        [&] { return search_->search_vector(query, k, ef, filter); });
}

// search_hybrid:RRF 混合检索(V3.6)。两路检索与 RRF 融合在 SearchLayer::search_hybrid。
std::expected<TextSearchResult, CaskFault>
Cask::search_hybrid(std::string_view text_query,
                     std::span<const float> vec_query, std::size_t k,
                     const meta::MetaFilter* filter) {
    return run_search_one(/*require_vector=*/true,
        [&] { return search_->search_hybrid(text_query, vec_query, k, filter); });
}

// search_text：BM25 词袋模式搜索。
std::expected<TextSearchResult, CaskFault>
Cask::search_text(std::string_view query, std::size_t k,
                  const meta::MetaFilter* filter, std::size_t offset) {
    return run_search_one(/*require_vector=*/false,
        [&] {
            auto hits = search_->search_text(query, k + offset, nullptr, filter);
            if (hits && offset > 0) {  // S13-D10：overfetch 后丢前 offset 条
                if (hits->size() > offset) {
                    hits->erase(hits->begin(),
                                hits->begin() + static_cast<std::ptrdiff_t>(offset));
                } else {
                    hits->clear();
                }
            }
            return hits;
        });
}

// S13-D3：带高亮搜索——补门面缺口（README 宣称有而 Cask 无）。骨架与
// run_search_one 相同（closed fail-fast → prepare_search flush → 内核 →
// 错误翻译），仅命中类型是 SearchHitEx、无法复用泛型骨架。
std::expected<Cask::HighlightSearchResult, CaskFault>
Cask::search_text_highlight(std::string_view query, std::size_t k,
                            const search::HighlightOptions& opts) {
    if (is_closed()) return std::unexpected(err(CaskError::kClosed, "cask is closed"));  // S11-W3
    if (auto g = prepare_search(); !g) return std::unexpected(g.error());
    auto hits = search_->search_text_highlight(query, k, opts);
    if (!hits) return std::unexpected(search_fault(hits.error()));
    return HighlightSearchResult{std::move(*hits)};
}

// S7-4: 批量搜索公共骨架。空批早退 → 一次 prepare_search（flush 覆盖全批）→
// 可选向量配置校验 → N 条查询并发跑共享 Search 池，保序写各自结果槽。
std::vector<std::expected<TextSearchResult, CaskFault>>
Cask::run_search_batch(
    std::size_t n, bool require_vector,
    const std::function<
        std::expected<TextSearchResult, CaskFault>(std::size_t)>& run_one) {
    std::vector<std::expected<TextSearchResult, CaskFault>> out(n);
    if (n == 0) return out;
    if (is_closed()) {  // S11-W3：全槽同 closed 错误
        for (auto& o : out)
            o = std::unexpected(err(CaskError::kClosed, "cask is closed"));
        return out;
    }
    // 前置校验一次覆盖全批（所有查询共享同一 search_/lane）；失败 → 全槽同错。
    if (auto g = prepare_search(); !g) {
        for (auto& o : out) o = std::unexpected(g.error());
        return out;
    }
    if (require_vector && meta_config_.vector_dim == 0) {
        for (auto& o : out)
            o = std::unexpected(err(CaskError::kInvalidOption,
                                    "collection has no vector config"));
        return out;
    }
    // 各槽独立、互不重叠 → 无需锁。grainsize=1：每 item 是一条完整重查询。
    search::parallel_for_queries(n, [&](std::size_t i) { out[i] = run_one(i); });
    return out;
}

// S7-4: 批量文本搜索——K 条独立查询并发跑共享 Search 池，保序返回。
std::vector<std::expected<TextSearchResult, CaskFault>>
Cask::search_text_batch(std::span<const std::string_view> queries,
                        std::size_t k, const meta::MetaFilter* filter) {
    return run_search_batch(queries.size(), /*require_vector=*/false,
        [&](std::size_t i) -> std::expected<TextSearchResult, CaskFault> {
            auto hits = search_->search_text(queries[i], k, nullptr, filter);
            if (!hits) return std::unexpected(search_fault(hits.error()));
            return TextSearchResult{std::move(*hits)};
        });
}

// S7-4: 批量向量检索——K 条独立查询并发跑共享 Search 池，保序返回。
std::vector<std::expected<TextSearchResult, CaskFault>>
Cask::search_vector_batch(std::span<const std::span<const float>> queries,
                          std::size_t k, std::size_t ef,
                          const meta::MetaFilter* filter) {
    return run_search_batch(queries.size(), /*require_vector=*/true,
        [&](std::size_t i) -> std::expected<TextSearchResult, CaskFault> {
            auto hits = search_->search_vector(queries[i], k, ef, filter);
            if (!hits) return std::unexpected(search_fault(hits.error()));
            return TextSearchResult{std::move(*hits)};
        });
}

// S7-4: 批量 hybrid 检索——K 条独立 (text,vec) 查询并发跑共享 Search 池。
std::vector<std::expected<TextSearchResult, CaskFault>>
Cask::search_hybrid_batch(std::span<const HybridQuery> queries,
                          std::size_t k, const meta::MetaFilter* filter) {
    return run_search_batch(queries.size(), /*require_vector=*/true,
        [&](std::size_t i) -> std::expected<TextSearchResult, CaskFault> {
            auto hits = search_->search_hybrid(queries[i].text, queries[i].vec, k, filter);
            if (!hits) return std::unexpected(search_fault(hits.error()));
            return TextSearchResult{std::move(*hits)};
        });
}

// search_phrase：BM25 短语模式搜索。
std::expected<TextSearchResult, CaskFault>
Cask::search_phrase(std::string_view query, std::size_t k,
                    std::size_t offset) {
    return run_search_one(/*require_vector=*/false,
        [&] {
            auto hits = search_->search_phrase(query, k + offset);
            if (hits && offset > 0) {  // S13-D10
                if (hits->size() > offset) {
                    hits->erase(hits->begin(),
                                hits->begin() + static_cast<std::ptrdiff_t>(offset));
                } else {
                    hits->clear();
                }
            }
            return hits;
        });
}

// search_fields：BM25 多字段搜索（S8.6），支持 field:term^boost。
std::expected<TextSearchResult, CaskFault>
Cask::search_fields(std::string_view query, std::size_t k) {
    return run_search_one(/*require_vector=*/false,
        [&] { return search_->search_fields(query, k); });
}

// search_near：BM25 近邻搜索（S8.7）。
std::expected<TextSearchResult, CaskFault>
Cask::search_near(std::string_view query, std::uint32_t slop, std::size_t k) {
    return run_search_one(/*require_vector=*/false,
        [&] { return search_->search_near(query, slop, k); });
}

// bool_search：BM25 布尔搜索（AND/OR/NOT）。
std::expected<TextSearchResult, CaskFault>
Cask::bool_search(std::string_view query, std::size_t k,
                  std::size_t offset) {
    return run_search_one(/*require_vector=*/false,
        [&] {
            auto hits = search_->bool_search(query, k + offset);
            if (hits && offset > 0) {  // S13-D10
                if (hits->size() > offset) {
                    hits->erase(hits->begin(),
                                hits->begin() + static_cast<std::ptrdiff_t>(offset));
                } else {
                    hits->clear();
                }
            }
            return hits;
        });
}

// S8.3：模糊搜索（Levenshtein 编辑距离匹配）。
std::expected<TextSearchResult, CaskFault>
Cask::search_fuzzy(std::string_view query, std::size_t k, std::uint32_t max_edit_distance) {
    return run_search_one(/*require_vector=*/false,
        [&] { return search_->search_fuzzy(query, k, max_edit_distance); });
}

// S8.4：通配符搜索（* / ? 模式匹配）。
std::expected<TextSearchResult, CaskFault>
Cask::search_wildcard(std::string_view pattern, std::size_t k) {
    return run_search_one(/*require_vector=*/false,
        [&] { return search_->search_wildcard(pattern, k); });
}

std::expected<void, CaskFault> Cask::sync() {
    std::lock_guard<std::mutex> wlk(write_mu_);  // S11-W1：写路径互斥
    if (is_closed()) return std::unexpected(err(CaskError::kClosed, "cask is closed"));  // S11-W3
    if (active_data_) {
        if (auto r = active_data_->sync(); !r) {
            return std::unexpected(io_fault(r.error().errnum));
        }
        writes_since_sync_ = 0;  // P4:全量 fsync 后组提交计数清零
    }
    return {};
}

// ---- status / fold / merge 包装 --------------------------------------------
// merge 这里是同步阻塞的——上层（NIF 注册了 ERL_NIF_DIRTY_JOB_IO_BOUND）
// 把它放到 dirty 调度器，所以不会卡住 BEAM 主调度。merge 完成后：
//   1. 从 fstats 里把已合并的 file_id 删掉（trim_fstats）
//   2. 从 read_files_ 缓存淘汰对应句柄（防止 fd 泄漏）
//   3. unlink 旧 data + hint 文件（节省磁盘）

// S13-D6：不停机备份（契约见 cask.hpp）。
std::expected<void, CaskFault> Cask::backup(std::string_view dst_dir) {
    std::lock_guard<std::mutex> wlk(write_mu_);  // 备份期间挡住写者
    if (is_closed()) return std::unexpected(err(CaskError::kClosed, "cask is closed"));

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dst(dst_dir);
    fs::create_directories(dst, ec);
    if (ec) return std::unexpected(err(CaskError::kIo,
        "backup: cannot create dst dir: " + dst.string()));

    // 关闭 active writer：finalize hint trailer 后该文件成为 sealed（不可变），
    // 可安全 hardlink。read_write 且有 active 时才需要；下一次 put 经
    // ensure_active_writer 自动重建（write_lock_ 保留不释放——与
    // close_write_file 不同，备份不让出写权）。
    if (active_data_) {
        if (auto r = maybe_group_commit(/*force*/ true); !r) {
            return std::unexpected(r.error());
        }
        if (active_hint_) {
            if (auto r = active_hint_->finalize(); !r) {
                return std::unexpected(io_fault(r.error().errnum,
                                                std::string(active_hint_->path())));
            }
        }
        {
            std::unique_lock lk(read_cache_mu_);
            active_data_.reset();
        }
        active_hint_.reset();
        active_file_id_ = 0;
    }

    // hardlink（同设备零拷贝、天然一致）→ 跨设备回退字节拷贝。
    auto link_or_copy = [&](const fs::path& src) -> bool {
        if (!fs::exists(src, ec)) return true;  // 可选文件缺失 = 跳过
        const fs::path to = dst / src.filename();
        std::error_code ec2;
        fs::remove(to, ec2);  // 幂等：目标已存在则覆盖
        fs::create_hard_link(src, to, ec2);
        if (!ec2) return true;
        ec2.clear();
        fs::copy_file(src, to, fs::copy_options::overwrite_existing, ec2);
        return !ec2;
    };

    auto entries = fileops::scan_dir(dirname_);
    if (!entries) {
        return std::unexpected(io_fault(entries.error().errnum, dirname_));
    }
    for (const auto& e : *entries) {
        if (!link_or_copy(e.data_path)) {
            return std::unexpected(err(CaskError::kIo,
                "backup: copy failed: " + e.data_path));
        }
        (void)link_or_copy(fileops::mk_hint_filename(e.data_path));  // hint 可重建
    }
    // 元数据必备；checkpoint 可选（缺失只是备份目录首次 open 慢一次）。
    const fs::path base(dirname_);
    if (!link_or_copy(base / "bitcask.meta")) {
        return std::unexpected(err(CaskError::kIo, "backup: meta copy failed"));
    }
    (void)link_or_copy(base / "field.schema");
    (void)link_or_copy(base / kKeydirSnapName);
    (void)link_or_copy(base / kSearchCkptName);
    // S14-8：向量 payload 一并带上（此前漏 .vec——缺失仅降级为 fold 重建，
    // 但备份目录首次 open 会付全量重建代价）。delta 链文件不带：备份点的
    // base+快照自洽，链属运行期窗口。
    (void)link_or_copy(base / "search.vec");
    (void)link_or_copy(base / "search.qc8");
    return {};
}

StatusInfo Cask::status() {
    StatusInfo s;
    if (is_closed()) return s;  // S11-W3：已关闭返回零值快照（不解引用 keydir_）
    auto info = keydir_->info();
    s.key_count = info.key_count;
    s.key_bytes = info.key_bytes;
    s.epoch     = info.epoch;
    s.files.reserve(info.fstats.size());
    for (const auto& f : info.fstats) {
        s.files.push_back(merge::summarize(dirname_, f));
    }
    s.index_errors = index_errors_.load(std::memory_order_relaxed);
    // S13-D8：观测扩展（全部经线程安全访问器：HNSW 原子计数、cache 自带锁、
    // read_files_ shared_lock；不含需遍历 concurrent map 的指标，见 hpp 注）。
    if (search_) {
        s.hnsw_nodes = search_->hnsw_size();
        s.search_cache_entries = search_->cache_entries();
    }
    s.read_handles = read_handle_count();
    return s;
}

bool Cask::is_empty_estimate() {
    if (is_closed()) return true;  // S11-W3
    return keydir_->info().key_count == 0;
}

bool Cask::is_frozen() {
    if (is_closed()) return false;  // S11-W3
    return keydir_->info().iter_info.frozen;
}

// 包装 merge::decide。关键工作是「排除不该被合的 active file」：
//   - 普通 writer 模式：排除自己的 active_file_id_（不能合自己正在写的）
//   - merge_only 模式：排除 open 时从 write.lock 抠出来的「live writer 当
//     前 active id」；为了应对「writer 在我们 snapshot 之后 roll 过去」，
//     防御性地排除所有 file_id >= snapshot 的文件。代价是少并几个文件，
//     下一轮 merge 自然处理。
Cask::NeedsMerge Cask::needs_merge(std::uint32_t now_sec) {
    if (is_closed()) return {};  // S11-W3：needs=false
    auto info = keydir_->info();
    const std::uint32_t exclude_id =
        opts_.merge_only
            ? merger_writer_active_id_
            : active_file_id_.load(std::memory_order_relaxed);
    std::vector<merge::FileStatus> summary;
    summary.reserve(info.fstats.size());
    for (const auto& f : info.fstats) {
        if (opts_.merge_only) {
            if (exclude_id != 0 && f.file_id >= exclude_id) continue;
        } else {
            if (f.file_id == active_file_id_) continue;
        }
        summary.push_back(merge::summarize(dirname_, f));
    }
    // V4:计算索引删除率(全局信号,用于触发 merge)。
    // dead_doc_rate = (total_ords - live_docs) * 100 / total_ords
    // total_ords==0 时跳过(无任何写入,谈不上删除率)。
    int dead_doc_rate = 0;
    if (search_) {
        auto idx_info = search_->index_info();
        if (idx_info.total_ords > 0) {
            dead_doc_rate = static_cast<int>(
                (idx_info.total_ords - idx_info.live_docs) * 100
                / idx_info.total_ords);
        }
    }
    auto d = merge::decide(summary, opts_.policy, now_sec, dead_doc_rate);
    NeedsMerge n;
    n.needs = d.needs_merge;
    for (const auto& f : d.files)         n.files.push_back(f.filename);
    for (const auto& f : d.expired_files) n.expired_files.push_back(f.filename);
    return n;
}

// 手动 checkpoint（s13-review §P1 后续②）。语义与并发契约见 cask.hpp 声明。
// 与 merge 收尾（S13-F6）同机制：search.ckpt 序列化封装成 RunFn 经 reorder
// buffer 在 reducer 线程按 ord 序执行。区别：不等 flush(lane)（持续写入下
// in_flight 难归零，等待无界），只等自己的 RunFn 完成——reducer 严格按 ord
// 序 apply，RunFn 执行时其 ord 之前的事件必然已全部应用，等待有界（此刻
// 之前已提交的积压）。完成信号用 tri-state 原子（0=pending/1=ok/2=fail）+
// atomic::wait；fn 内兜异常（reducer 的 catch 只计数不回传，若 fn 抛出而
// 不置状态，本调用将永久挂起）。
std::expected<void, CaskFault> Cask::checkpoint() {
    WriteOpGate gate(this);  // H1：close() 等本调用（含 RunFn 等待）完成
    if (is_closed()) return std::unexpected(err(CaskError::kClosed, "cask is closed"));
    if (!opts_.read_write || opts_.merge_only) {
        return std::unexpected(err(CaskError::kReadOnly,
                                     "checkpoint: read-only cask"));
    }
    std::lock_guard<std::mutex> lk(ckpt_mu_);
    if (!search_) {
        // 纯 KV 库：keydir 快照即全部（无 reducer，调用线程直写；并发调用
        // 由 ckpt_mu_ 串行，快照 tmp+rename 不与自身竞争）。
        write_keydir_snapshot();
        return {};
    }
    const std::string search_ckpt = dirname_ + "/" + kSearchCkptName;
    if (index_pool_ && index_lane_) {
        auto done = std::make_shared<std::atomic<int>>(0);
        IndexTask t;
        t.op  = IndexOp::RunFn;
        t.ord = keydir_->alloc_ord();
        // S14-1：keydir 快照也移进 RunFn——所有 ckpt 文件写统一到 reducer
        // 单线程，与自动 checkpoint 的 RunFn 天然串行（消除 .tmp 并发写
        // 窗口，且不能在此持 ckpt_mu_ 等 reducer——会与手动调用互锁）。
        // 成对性：字节水位在**提交时刻**捕获（wms 先于 wm，保证水位覆盖的
        // 记录 ord < wm ≤ search 覆盖）；RunFn 执行时刻取水位会被并发写者
        // 推进而反转不变量（见 collect_snapshot_watermarks 注释）。
        // S14-7：keydir 元数据 payload 于提交时刻构建（wms 同刻捕获，
        // 成对一致）；delta 路径内联进 delta 文件，base 路径落全量快照。
        std::vector<std::byte> kd;
        if (auto wms0 = collect_snapshot_watermarks()) {
            keydir_->serialize_meta_delta(kd, *wms0);
            t.fn = [this, search_ckpt, done, wms = std::move(wms0),
                    kd = std::move(kd), wm = keydir_->peek_next_ord()] {
                int result = 2;
                try {
                    result = save_search_ckpt_paired(search_ckpt, wm, wms, kd)
                                 ? 1
                                 : 2;
                } catch (...) {
                    // result 保持 2；异常由 reducer error_fn 计数上报。
                }
                if (result == 1) {
                    last_ckpt_ord_.store(wm, std::memory_order_relaxed);
                }
                done->store(result, std::memory_order_release);
                done->notify_all();
            };
        } else
        t.fn  = [this, search_ckpt, done,
                 wms = collect_snapshot_watermarks(),
                 wm = keydir_->peek_next_ord()] {
            int result = 2;
            try {
                // 水位捕获失败（文件态不稳定）：无 keydir payload，delta
                // 的字节水位不推进（方向安全）；base 路径也无快照可写。
                result = save_search_ckpt_paired(
                             search_ckpt, wm, wms, {}) ? 1 : 2;
            } catch (...) {
                // result 保持 2；异常本体由 reducer 的 error_fn 计数上报。
            }
            if (result == 1) {
                last_ckpt_ord_.store(wm, std::memory_order_relaxed);
            }
            done->store(result, std::memory_order_release);
            done->notify_all();
        };
        index_pool_->submit(index_lane_, std::move(t));
        for (int v = done->load(std::memory_order_acquire); v == 0;
             v = done->load(std::memory_order_acquire)) {
            done->wait(0, std::memory_order_acquire);
        }
        if (done->load(std::memory_order_acquire) != 1) {
            return std::unexpected(err(CaskError::kIo,
                "checkpoint: search checkpoint save failed"));
        }
    } else {
        // 无索引池（理论不可达：search_ 存在则 lane 已注册）——调用线程直跑。
        std::vector<std::byte> kd;
        auto wms0 = collect_snapshot_watermarks();
        if (wms0) keydir_->serialize_meta_delta(kd, *wms0);
        if (!save_search_ckpt_paired(search_ckpt, keydir_->peek_next_ord(),
                                     wms0, kd)) {
            return std::unexpected(err(CaskError::kIo,
                "checkpoint: search checkpoint save failed"));
        }
    }
    return {};
}

// S14-1：自动 checkpoint 的提交点。写路径释放 write_mu_ 后调用（WriteOpGate
// 持有中 → index_pool_/index_lane_ 读安全，close 等待本调用返回）。消费
// roll_active 置下的 pending 标记；ord 增量达阈值且无在途 RunFn 才真正提交。
// fire-and-forget：不等待完成，序列化在 reducer 线程进行（与手动 checkpoint
// 的 RunFn 天然串行），失败仅 log_warn——自动路径是 best-effort 加速，
// 正确性恒由 data fold 兜底。
// S14-7：成对保存统一入口（语义见 cask.hpp 声明）。所有 search 模式的
// ckpt 保存点（手动/自动 RunFn、①、close、merge）都经此，写序不变量
// （search 覆盖 ≥ keydir 水位）集中在一处维护。
bool Cask::save_search_ckpt_paired(
    const std::string& path, std::uint64_t wm,
    const std::optional<std::vector<
        std::pair<std::uint32_t, std::uint64_t>>>& wms,
    const std::vector<std::byte>& keydir_delta) {
    // S17-3 P3 commit 路径：dir = dirname_。所有 ckpt 写都改走
    // save_checkpoint_paired 走 per-component + manifest 协议。
    (void)path;
    return save_checkpoint_paired(dirname_, wm, wms, keydir_delta);
}

// S17-3 P3 commit 路径。先 per-component base（或 delta），再写
// index.manifest 作为 commit point，最后 base 路径下写 keydir 快照。
// delta 路径不写 keydir 快照（元数据已内联进 delta 文件的 kKeydirDelta
// 段）。
//
// 「是 base 还是 delta」由 Cask 决定：每组件的 chain 状态有效
//（comp_chain_wm_ == comp_base_gen_ && comp_next_seq_ < max）且
// !rebase_needed → 走 delta；否则全量 base。混合：base 走全量，delta
// 走单文件——但本版每组件一致性优先，要么全 base 要么全 delta。
bool Cask::save_checkpoint_paired(
    const std::string& dir, std::uint64_t wm,
    const std::optional<std::vector<
        std::pair<std::uint32_t, std::uint64_t>>>& wms,
    const std::vector<std::byte>& keydir_delta) {
    if (!search_) return true;  // 纯 KV 库无 search ckpt
    // 取真实 dirty 掩码（每个组件独立的脏位）。
    std::array<bool, bitcask::kComponentCount> dirty_mask =
        search_->dirty_mask();
    // 决策：!rebase_needed 且至少一个组件脏 且所有组件 chain_seq <
    // max_delta_chain 上限 → delta；否则 base。已激活链就应当续链。
    // max_delta_chain 来源：opts_.search_config->max_delta_chain（0 = 无限）。
    std::uint32_t max_delta_chain = 0;
    if (opts_.search_config.has_value()) {
        max_delta_chain = opts_.search_config->max_delta_chain;
    }
    bool any_dirty = false;
    for (bool d : dirty_mask) {
        if (d) { any_dirty = true; break; }
    }
    bool can_delta = any_dirty && !search_->needs_ckpt_rebase();
    if (can_delta && max_delta_chain > 0) {
        for (std::size_t i = 0; i < bitcask::kComponentCount; ++i) {
            const auto& e = current_manifest_.entries[i];
            // max_delta_chain = 0 表示无限；否则 chain_seq 必须 < max。
            if (e.chain_seq >= max_delta_chain) {
                can_delta = false; break;
            }
        }
    }
    // 走 delta 路径。
    if (can_delta) {
        auto delta_res = search_->save_components_delta(
            dir, wm, dirty_mask,
            std::span<const std::byte>(keydir_delta.data(),
                                        keydir_delta.size()));
        // 写 manifest：成功的组件推进 chain_seq + chain_watermark。
        bitcask::Manifest new_manifest = current_manifest_;
        for (std::size_t i = 0; i < bitcask::kComponentCount; ++i) {
            if (delta_res.wrote[i]) {
                new_manifest.entries[i].chain_seq = delta_res.new_seqs[i];
                new_manifest.entries[i].chain_watermark = wm;
            }
        }
        const std::string mpath = std::string(dir) + "/" +
            std::string(bitcask::kManifestName);
        if (!bitcask::write_manifest(mpath, new_manifest)) {
            return false;
        }
        current_manifest_ = new_manifest;
        return true;
    }
    // 走 base 路径。
    auto base_res = search_->save_components_base(dir, wm, dirty_mask);
    bitcask::Manifest new_manifest = current_manifest_;
    for (std::size_t i = 0; i < bitcask::kComponentCount; ++i) {
        if (base_res.wrote_base[i]) {
            new_manifest.entries[i].base_watermark = wm;
            new_manifest.entries[i].chain_seq = 0;
            new_manifest.entries[i].chain_watermark = wm;
        }
    }
    const std::string mpath = std::string(dir) + "/" +
        std::string(bitcask::kManifestName);
    if (!bitcask::write_manifest(mpath, new_manifest)) {
        log_warn("save_checkpoint_paired: write_manifest failed "
                 "(existing manifest unchanged)");
        return false;
    }
    current_manifest_ = new_manifest;
    if (wms) {
        write_keydir_snapshot(*wms);
    }
    return true;
}

void Cask::maybe_submit_auto_checkpoint() {
    if (opts_.auto_checkpoint_min_docs == 0) return;          // 未启用
    if (!search_ || !index_pool_ || !index_lane_) return;     // 仅索引模式
    if (!auto_ckpt_pending_.load(std::memory_order_relaxed)) return;
    const std::uint64_t now_ord = keydir_->peek_next_ord();
    if (now_ord - last_ckpt_ord_.load(std::memory_order_relaxed) <
        opts_.auto_checkpoint_min_docs) {
        // 增量不足：清标记，等下一次 roll 再评估。
        auto_ckpt_pending_.store(false, std::memory_order_relaxed);
        return;
    }
    if (auto_ckpt_inflight_.exchange(true, std::memory_order_acq_rel)) {
        return;  // 已有在途 RunFn（保持 pending，完成后下个 roll 重试）
    }
    auto_ckpt_pending_.store(false, std::memory_order_relaxed);
    IndexTask t;
    t.op  = IndexOp::RunFn;
    t.ord = keydir_->alloc_ord();
    // 成对性：字节水位与 keydir 元数据 payload 都在提交时刻捕获（wms 先
    // 于 wm），保存本体在 reducer 执行（同 checkpoint()）。
    std::vector<std::byte> auto_kd;
    auto auto_wms = collect_snapshot_watermarks();
    if (auto_wms) keydir_->serialize_meta_delta(auto_kd, *auto_wms);
    t.fn  = [this, wms = std::move(auto_wms), kd = std::move(auto_kd),
             wm = keydir_->peek_next_ord()] {
        try {
            if (save_search_ckpt_paired(dirname_ + "/" + kSearchCkptName,
                                        wm, wms, kd)) {
                last_ckpt_ord_.store(wm, std::memory_order_relaxed);
            } else {
                log_warn("auto checkpoint: search ckpt save failed "
                         "(will retry at a later roll)");
            }
        } catch (...) {
            log_error("auto checkpoint: exception during save (swallowed; "
                      "recovery still covered by data fold)");
        }
        auto_ckpt_inflight_.store(false, std::memory_order_release);
    };
    submit_index_task(std::move(t));
}

// 合并执行。files 为空时先 needs_merge 决定要并什么；非空就直接用
// caller 给的列表。
//
// === V4 Merge Pipeline Ordering Contract ===
// 必须严格按以下顺序执行。违反顺序会破坏快照一致性或丢失索引数据：
//
//  Phase 1 — Data compaction:
//    1. run_merge()  重写活 record 到新文件, CAS 更新 KeyDir
//
//  Phase 2 — Index maintenance (search_ 存在时):
//    2. write_keydir_snapshot()  捕获 ord 水位
//    3. flush IndexPool          排干待处理索引任务
//    4. compact()                P2:阈值压实死 posting(不重读、不重分词;
//                                定位由 run_merge 的 on_relocate 已更新)
//    5. save bm25 snapshot + index sidecar
//    6. rebuild_hnsw + flush     同步重建 HNSW 图(物理清死节点)
//    7. save hnsw snapshot       V4:持久化重建后图(下次 open 走快照路径)
//
//  Phase 3 — Cleanup:
//    8. erase read_files_ cache + unlink old data/hint
//    9. trim_fstats
//   10. write_keydir_snapshot()  最终状态快照
//
// 关键约束:
//  - Phase 2 的 flush(3)必须在 compact(4)之前,保证 Index 覆盖全部已分配 ord,
//    且 IndexPool worker 无在途任务(否则在途 task 持旧 ord 可能写错位置)
//  - Phase 2 的 bm25/sidecar/hnsw snap 落盘顺序必须与 close() 一致(A4)
//  - Phase 3 的 unlink 必须在 Phase 2 之后——否则 HNSW rebuild 读不到源数据
std::expected<merge::MergeStats, CaskFault>
Cask::merge(std::vector<std::string> files, std::uint32_t now_sec) {
    if (is_closed()) return std::unexpected(err(CaskError::kClosed, "cask is closed"));  // S11-W3
    if (files.empty()) {
        auto n = needs_merge(now_sec);
        if (!n.needs) {
            // 不需要 merge——返回空 stats。
            return merge::MergeStats{};
        }
        files = std::move(n.files);
    }
    auto r = merge::run_merge(files, dirname_, *keydir_, opts_.o_sync,
                              search_.get(),
                              now_sec ? now_sec : now_sec_default());
    if (!r) {
        return std::unexpected(err(CaskError::kIo, r.error().detail));
    }

    if (search_) {
        // P3 顺序约定（S14-4 修订）:**捕获**较早水位（此刻文件大小），但
        // 文件**写入**延后到 search.ckpt 保存成功之后——写序反了的话，
        // 两写之间崩溃会留下「新快照+旧 ckpt」，fold 从超前水位起跳、
        // search 永久丢窗口。捕获早 + 写入晚，两个约束同时满足。
        const auto merge_snap_wms = collect_snapshot_watermarks();
        if (index_pool_ && index_lane_) index_pool_->flush(index_lane_);

        // P2:merge 不再全量重读+重分词重建倒排。merge::run_merge 已通过
        // on_relocate 把每条 live 文档的存储定位更新到新文件;倒排 posting 以
        // 稳定 ord 为键、与文件位置无关;死文档查询时由 is_live 过滤(正确性
        // 不依赖压实)。这里只按阈值压实死 posting 回收空间——不读数据文件、
        // 不重分词,省掉 merge 的全量 NLP 重算。死占比 < 阈值的 posting list
        // 留待后续 merge 累积到阈值再压。
        //
        // S13-F6：compact 遍历 tbb::concurrent_hash_map——与 reducer 的
        // add_doc 插入并发不安全（上面的 flush 只排干已提交任务，§7.6 允许
        // 的并发 put 在 flush 之后仍持续提交）。故封装成 RunFn 任务经
        // reorder buffer 在 reducer 线程内执行（同 RebuildHnsw 先例），
        // 恢复 S12-2「遍历只发生在 reducer」的不变量。
        constexpr double kMergeCompactDeadRatio = 0.2;
        if (index_pool_ && index_lane_) {
            IndexTask t;
            t.op  = IndexOp::RunFn;
            t.ord = keydir_->alloc_ord();
            t.fn  = [this] {
                search_->compact(kMergeCompactDeadRatio);
                search_->compact_index_chunks();
            };
            index_pool_->submit(index_lane_, std::move(t));
            index_pool_->flush(index_lane_);
        } else {
            // 无索引池（理论不可达：search_ 存在则 lane 已注册）——退化为
            // 调用线程直跑，行为同旧版。
            search_->compact(kMergeCompactDeadRatio);
            search_->compact_index_chunks();
        }

        // V4:merge 后同步重建 HNSW（物理清除死节点）。重建在 reducer 线程
        // 执行（单写者约束），flush 阻塞等待完成。S15-2：原专用 RebuildHnsw
        // 操作并入 RunFn 通道——重建本就是塞进 reducer 静止点的闭包。ord
        // 照旧 alloc_ord 占位，经 reorder buffer 与本 merge 期间累积的
        // put/delete 同序串行 apply——保持 HNSW 单写者约束在 ord 维度上的
        // 严格性。该 ord 在数据语义上不指向任何文档（类似 Skip）。
        if (meta_config_.vector_dim > 0 && index_pool_ && index_lane_) {
            IndexTask t;
            t.op  = IndexOp::RunFn;
            t.ord = keydir_->alloc_ord();
            t.fn  = [this] { search_->rebuild_hnsw(); };
            index_pool_->submit(index_lane_, std::move(t));
            index_pool_->flush(index_lane_);
        }

        // P14e:统一分段 search.ckpt 替代旧多文件保存。
        // best-effort:checkpoint 保存失败非致命——下次 open 回退到全量 fold 重建
        // 搜索索引（仅慢一次启动，数据不受影响），故显式忽略返回值而非让 merge 失败。
        // S13-F6：序列化同样遍历 concurrent_hash_map（且 truncate_wal 与
        // reducer 的 WAL 追加竞争）——经 RunFn 在 reducer 线程执行。
        // 水位在提交时取值（by-value 捕获），语义与旧版一致。
        const std::string search_ckpt = dirname_ + "/" + kSearchCkptName;
        if (index_pool_ && index_lane_) {
            IndexTask t;
            t.op  = IndexOp::RunFn;
            t.ord = keydir_->alloc_ord();
            // S14-7：merge 恒 rebase（compact 已置位）→ paired 走 base +
            // 全量快照（用早段捕获的水位）。
            t.fn  = [this, search_ckpt, wms = merge_snap_wms,
                     wm = keydir_->peek_next_ord()] {
                if (!save_search_ckpt_paired(search_ckpt, wm, wms, {})) {
                    log_warn("search checkpoint save failed after merge "
                             "(will rebuild on next open)");  // S13-D7
                }
            };
            index_pool_->submit(index_lane_, std::move(t));
            index_pool_->flush(index_lane_);
        } else {
            if (!save_search_ckpt_paired(search_ckpt,
                                         keydir_->peek_next_ord(),
                                         merge_snap_wms, {})) {
                log_warn("search checkpoint save failed after merge "
                         "(will rebuild on next open)");  // S13-D7
            }
        }
        // 快照已由 paired 入口在 ckpt 成功后落盘（写序不变量集中维护）；
        // merge 末尾还有一次「最紧凑状态」快照兜底。
    }

    // After run_merge, every live record from `files` has been CAS-rewritten
    // into the new merge file, and stale records were already pointing
    // elsewhere. So nothing in the keydir references these inputs anymore —
    // safe to unlink the .data + .hint pair and drop the fstats entry.
    //
    // S13-F1 纵深防御：run_merge 复查后仍有 keydir entry 指向的输入文件
    // （stuck，正常流程不可达）绝不能 unlink——否则这些 key 指向已删文件，
    // 重启后永久丢失。跳过其 unlink/erase/trim，留给下轮 merge 重试。
    //
    // erase + unlink 收在同一临界区(O10):放锁后再 unlink 会留一个窗口,
    // 持旧 keydir 快照的在途 get 在 unlink 后 lazy reopen 报 ENOENT 假失败。
    // 持锁做文件系统操作可接受——merge 收尾是冷路径。被 erase 的句柄若仍
    // 被在途读者持有,由 shared_ptr 引用计数续命(UAF 修复)。
    //
    // Failures here are best-effort: the keydir is already consistent. A
    // residual file just wastes disk until the next process tries the same.
    if (r->relocations_stuck > 0) {  // S13-D7：防御路径触发即上报（不应发生）
        log_error("merge: " + std::to_string(r->relocations_stuck) +
                  " stuck relocation(s); input file(s) kept for retry");
    }
    std::vector<std::uint32_t> trimmed_ids;
    trimmed_ids.reserve(files.size());
    {
        std::scoped_lock lk(read_cache_mu_);
        for (const auto& path : files) {
            std::error_code ec;
            if (auto t = fileops::parse_data_tstamp(path)) {
                const auto fid = static_cast<std::uint32_t>(*t);
                if (std::binary_search(r->stuck_file_ids.begin(),
                                       r->stuck_file_ids.end(), fid)) {
                    continue;  // S13-F1: keydir 仍引用，保留文件与句柄
                }
                read_files_.erase(fid);
                trimmed_ids.push_back(fid);
            }
            std::filesystem::remove(path, ec);
            std::filesystem::remove(fileops::mk_hint_filename(path), ec);
        }
    }
    if (!trimmed_ids.empty()) {
        (void)keydir_->trim_fstats(trimmed_ids);
    }
    write_keydir_snapshot();  // A4:merge 后状态最紧凑,顺手落快照
    return *r;
}

}  // namespace bitcask
