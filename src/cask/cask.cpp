#include "bitcask/cask.hpp"

#include <signal.h>     // ::kill for stale-lock detection
#include <unistd.h>     // ::getpid, ::unlink

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

std::span<const std::byte> str_to_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
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
                bool see_tombstones) {
    if (iter_ && iter_->is_iterating()) {
        return std::unexpected(err(CaskError::kIo, "iter already started"));
    }
    // X1:先 pin KeyDir（shared_ptr 复制），再建 IterHandle——保证
    // close() 在迭代器存活期间 reset keydir_ 也不会让 iter_ 的裸指针悬空。
    keydir_pin_ = parent_->keydir_;
    iter_ = parent_->keydir_->make_iter();
    auto r = iter_->start(now_sec, maxage, maxputs);
    see_tombstones_ = see_tombstones;
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
    for (const auto& e : *scan) {
        const auto fid = static_cast<std::uint32_t>(e.tstamp);
        if (parent_->active_data_ && fid == parent_->active_file_id_) continue;
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
    cask->field_schema_.open((fs::path(dirname) / "field.schema").string());  // #1

    cask->search_ = std::make_unique<search::SearchLayer>(search_config);

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

    // 目录不存在就建（mkdir -p 语义）。已存在不报错。
    std::error_code ec;
    fs::create_directories(cask->dirname_, ec);

    // 字段名 ↔ id 注册表（#1）：加载已有 + 打开追加句柄。
    cask->field_schema_.open((fs::path(cask->dirname_) / "field.schema").string());

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
    // [0, peek_next_ord) 区间。merge 提交的 RebuildHnsw{ord=peek_next_ord} 等
    // 首个 entry 进该 lane 的 reorder 时 next_apply_ord 已对齐，无 stall。必须
    // 在 keydir_/registry_ 就绪后（create_search_infra 早于此装配）。
    if (cask->search_ && cask->registry_) {
        cask->index_pool_ = cask->registry_->index_pool();
        cask->index_lane_ = cask->index_pool_->register_lib(
            // Map fn（并行 TBB）：IndexTask → ReduceEntry。map_analyze 是纯函数
            // （analyzer_ const、无共享可变态），跨线程安全。
            [&search = *cask->search_](const IndexTask& task) -> ReduceEntry {
                auto job = search.map_analyze(task.key(), task.ord, task.fields,
                                              task.file_id, task.offset,
                                              task.total_sz, task.tstamp);
                return ReduceEntry{std::move(job), task.meta, task.vec};
            },
            // Reduce fn（串行 reducer，per-lane ord 序）：ReorderEntry → apply。
            [&search = *cask->search_](ReorderEntry& entry) {
                std::visit([&search](auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, ReduceEntry>) {
                        search.reduce_apply(e.job, e.meta, e.vec);
                    } else if constexpr (std::is_same_v<T, OnWriteEntry>) {
                        search.on_write(e.key, e.ord, e.text,
                                        e.file_id, e.offset, e.total_sz, e.tstamp);
                        if (!e.meta.empty()) search.index().set_meta(e.ord, e.meta);
                        if (!e.vec.empty())  search.on_vector(e.ord, e.vec);
                    } else if constexpr (std::is_same_v<T, DeleteEntry>) {
                        search.on_delete(e.key, e.ord);
                    } else if constexpr (std::is_same_v<T, SkipEntry>) {
                        // no-op（ord 空洞填充）
                    } else if constexpr (std::is_same_v<T, RebuildEntry>) {
                        search.rebuild_hnsw();
                    }
                }, entry);
            },
            // Error fn：异常计数器自增（best-effort 保活 lane）。
            [cask_ptr = cask.get()]() { cask_ptr->index_errors_.fetch_add(1, std::memory_order_relaxed); },
            // 起始 ord。
            cask->keydir_->peek_next_ord()
        );
    }
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
    search_ = std::make_unique<search::SearchLayer>(scfg);
    // analyzer 构造失败（无效配置 / 分词器未注册 / 词典加载失败）则 analyzer_
    // 为空——决不能带病打开，否则首次带 text 的 put 段错误。干净拒绝。
    if (!search_->has_analyzer()) {
        search_.reset();
        return std::unexpected(err(CaskError::kInvalidOption,
                                   "analyzer creation failed (check analyzer type / dict_path)"));
    }
    // S6-P3: 不再每库自建池。共享池借用 + 车道注册推迟到 keydir 就绪后
    // （caller 在 create_search_infra 返回、registry_/keydir_ 装配完成后做）。
    // 此处仅建好 search_，标记本库为 search 模式（search_ != nullptr）。
    return {};
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
            const std::string search_ckpt = dirname_ + "/" + kSearchCkptName;
            (void)search_->save_search_ckpt(search_ckpt,
                                             keydir_->peek_next_ord());
        }
        if (opts_.read_write) write_keydir_snapshot();
    } catch (...) {
        // 吞掉：close 是终结路径，没有合理的恢复动作。后续 keydir/lock
        // release 仍需执行，所以不 return。
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
    search_.reset();
    if (write_lock_) {
        write_lock_->release_quiet();
        write_lock_.reset();
    }
}

// A4:写 keydir 段快照(best-effort,设计 doc/recovery-unified-checkpoint-design-zh.md 附录 A)。
// 水位 = 各 data 文件当前磁盘大小,**先于** dump 捕获(尾部回放重叠区
// 幂等,方向安全);调用点都在写者静止处(close / merge 末尾)。
void Cask::write_keydir_snapshot() noexcept {
    if (!keydir_) return;
    auto entries = fileops::scan_dir(dirname_);
    if (!entries) return;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> wms;
    wms.reserve(entries->size());
    for (const auto& e : *entries) {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(e.data_path, ec);
        if (ec) return;  // 文件态不稳定,放弃本次快照
        wms.emplace_back(static_cast<std::uint32_t>(e.tstamp), sz);
    }
    (void)keydir_->save_snapshot(dirname_ + "/" + kKeydirSnapName, wms);
}

// T3: 提交索引任务到 IndexPool。背压由有界队列提供：队列满（10240）时
// index_pool_->submit 的 push 阻塞写线程，让 put 路径自然减速、避免内存溢出。
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
    auto flush_recover = [&] {
        if (search_layer && !recover_batch.empty()) {
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
                auto v = hf->validate_trailer();
                if (v && *v) {
                    auto fr = hf->fold([&](const auto& rec) {
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
                    if (fr) used_hint = true;
                }
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
                    if (dv && (!dv->text.empty() || dv_has_vec)) {
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
std::expected<Cask::RecoverySnapshots, CaskFault>
Cask::load_recovery_snapshots(search::SearchLayer* search_layer) {
    RecoverySnapshots recovery;

    bool search_ok = false;
    if (search_layer) {
        auto result = search_layer->load_search_ckpt(
            dirname_ + "/" + kSearchCkptName);
        search_ok = result.loaded && result.all_segments_ok;
    }
    if (auto w = keydir_->load_snapshot(dirname_ + "/" + kKeydirSnapName)) {
        recovery.snap_wms = std::move(*w);
        recovery.snap_loaded = true;
    }
    // search.ckpt 不健康 → 退回全量 fold（snap_loaded=false 让 fold_start=0）。
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
        (void)write_lock_->write_data(bytes);  // best-effort：失败不阻断
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
    return ensure_active_writer();
}

std::expected<void, CaskFault> Cask::close_write_file() {
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
        return std::unexpected(err(CaskError::kAlreadyExists));
    }
    // S6-P1: 原始 ord 在 keydir 竞争中落败（kAlreadyExists），数据已写入但
    // keydir 未收录。发 Skip 填充 ord 空洞，防 reorder buffer stall。
    // 必须在 caller 提交 ord2 的真任务之前提交（队列 FIFO 保序）。
    submit_index_task(IndexTask::make(IndexOp::Skip, {}, ord, {}, 0, 0, 0, 0, 0));
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
    auto entry = keydir_->get(bytes_to_view(key));
    if (!entry) return std::unexpected(err(CaskError::kNotFound));

    if (opts_.expiry_secs > 0) {
        const auto now = now_sec_default();
        if (entry->tstamp + opts_.expiry_secs <= now) {
            return std::unexpected(err(CaskError::kNotFound));
        }
    }

    auto df = read_file(entry->file_id);
    if (!df) return std::unexpected(err(CaskError::kIo,
        "open file_id=" + std::to_string(entry->file_id)));

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
        return GetResultView(std::move(df), rv->value, rv->type,
                             rv->tstamp, rv->ord);
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

    return GetResultView(std::move(*rec));
}

std::expected<GetResult, CaskFault>
Cask::get_owned(std::span<const std::byte> key) {
    auto v = get(key);
    if (!v) return std::unexpected(v.error());
    return v->to_owned();
}

// --- GetResultView implementation ---

void GetResultView::derive_from_storage() {
    if (rec_type_ != format::RecordType::kDoc) return;  // tombstone 等不解码
    if (value_bytes_.empty()) return;
    auto dv = codec::decode_doc_value(value_bytes_);
    if (!dv) return;  // corrupt DocValue → empty spans
    value = dv->text;
    meta  = dv->meta;
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
          std::uint32_t tstamp) {
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
    // ⑩ thread_local 复用：encode_doc_value 是 append 语义，clear 后重填；
    // 并发 put 各线程独占一份，消除每次 put 的 encoded 堆分配。
    thread_local std::vector<std::byte> encoded;
    encoded.clear();
    encoded.reserve(value.size() + 16);
    codec::DocValueParts parts;
    parts.text = value;
    codec::encode_doc_value(encoded, parts);

    auto persisted = write_and_keydir(key, encoded, tstamp, ord);
    if (!persisted) return std::unexpected(persisted.error());
    submit_index_task(IndexTask::make(
        IndexOp::Add, bytes_to_view(key), persisted->ord,
        std::string_view(reinterpret_cast<const char*>(value.data()),
                         value.size()),
        persisted->file_id, persisted->offset, persisted->total_size, tstamp, 0));
    if (auto r = maybe_group_commit(); !r) return std::unexpected(r.error());
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
    auto w = active_data_->write(format::RecordType::kTombstone, tstamp,
                                  ord, key, tomb_value);
    if (!w) return std::unexpected(io_fault(w.error().errnum));
    // hint 文件也要追一条墓碑——下次 open fold(hint) 重建时才能正确删 key。
    auto h = active_hint_->write(tstamp, w->total_size, w->offset,
                                  /*tomb*/ true, key);
    if (!h) return std::unexpected(io_fault(h.error().errnum));
    keydir_->remove(bytes_to_view(key), tstamp);
    if (index_pool_) {
        submit_index_task(IndexTask::make(
            IndexOp::Delete, bytes_to_view(key), ord, {}, 0, 0, 0, tstamp, 0));
    } else if (search_) {
        search_->on_delete(bytes_to_view(key), ord);
    }
    if (auto r = maybe_group_commit(); !r) return std::unexpected(r.error());
    return {};
}

// put_doc：写入结构化文档（text + 选填 meta）。用于索引模式。
// 逻辑跟 put 类似，但 DocValue 编码包含 text 和 meta 两段。
std::expected<void, CaskFault>
Cask::put_doc(std::span<const std::byte> key, const DocInput& doc,
              std::uint32_t tstamp) {
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
    // S8.6：把多字段拷成 IndexTask.fields（name→text string，异步路径需独立存储）。
    auto task_fields = [&doc]() {
        std::vector<std::pair<std::string, std::string>> fs;
        fs.reserve(doc.fields.size());
        for (auto& [name, val] : doc.fields) {
            fs.push_back({name,
                std::string(reinterpret_cast<const char*>(val.data()), val.size())});
        }
        return fs;
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
    std::vector<std::byte> encoded;
    encoded.reserve(doc.text.size() + doc.meta.size() +
                    vec_out.size() * sizeof(float) + 16);
    codec::DocValueParts parts;
    parts.text = doc.text;
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
    auto task = IndexTask::make(
        IndexOp::Add, bytes_to_view(key), persisted->ord,
        std::string_view(reinterpret_cast<const char*>(doc.text.data()),
                         doc.text.size()),
        persisted->file_id, persisted->offset, persisted->total_size, tstamp, 0,
        task_fields());
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
    if (auto r = maybe_group_commit(); !r) return std::unexpected(r.error());
    return {};
}

// S8-R3: 单条搜索公共骨架。flush → 可选 vector 校验 → 跑内核 → 包错误/结果。
std::expected<TextSearchResult, CaskFault>
Cask::run_search_one(
    bool require_vector, CaskError err_kind,
    const std::function<
        std::expected<std::vector<search::SearchHit>, std::string>()>& run) {
    if (auto g = prepare_search(); !g) return std::unexpected(g.error());
    if (require_vector && meta_config_.vector_dim == 0) {
        return std::unexpected(err(CaskError::kInvalidOption,
            "collection has no vector config"));
    }
    auto hits = run();
    if (!hits) return std::unexpected(err(err_kind, hits.error()));
    return TextSearchResult{std::move(*hits)};
}

// search_vector：HNSW 向量检索(V3.3)。归一化/live 过滤/ord 翻译都在 SearchLayer。
std::expected<TextSearchResult, CaskFault>
Cask::search_vector(std::span<const float> query, std::size_t k,
                     std::size_t ef, const meta::MetaFilter* filter) {
    return run_search_one(/*require_vector=*/true, CaskError::kInvalidOption,
        [&] { return search_->search_vector(query, k, ef, filter); });
}

// search_hybrid:RRF 混合检索(V3.6)。两路检索与 RRF 融合在 SearchLayer::search_hybrid。
std::expected<TextSearchResult, CaskFault>
Cask::search_hybrid(std::string_view text_query,
                     std::span<const float> vec_query, std::size_t k,
                     const meta::MetaFilter* filter) {
    return run_search_one(/*require_vector=*/true, CaskError::kInvalidOption,
        [&] { return search_->search_hybrid(text_query, vec_query, k, filter); });
}

// search_text：BM25 词袋模式搜索。
std::expected<TextSearchResult, CaskFault>
Cask::search_text(std::string_view query, std::size_t k,
                  const meta::MetaFilter* filter) {
    return run_search_one(/*require_vector=*/false, CaskError::kIo,
        [&] { return search_->search_text(query, k, nullptr, filter); });
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
            if (!hits) return std::unexpected(err(CaskError::kIo, hits.error()));
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
            if (!hits) return std::unexpected(err(CaskError::kInvalidOption, hits.error()));
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
            if (!hits) return std::unexpected(err(CaskError::kInvalidOption, hits.error()));
            return TextSearchResult{std::move(*hits)};
        });
}

// search_phrase：BM25 短语模式搜索。
std::expected<TextSearchResult, CaskFault>
Cask::search_phrase(std::string_view query, std::size_t k) {
    return run_search_one(/*require_vector=*/false, CaskError::kIo,
        [&] { return search_->search_phrase(query, k); });
}

// search_fields：BM25 多字段搜索（S8.6），支持 field:term^boost。
std::expected<TextSearchResult, CaskFault>
Cask::search_fields(std::string_view query, std::size_t k) {
    return run_search_one(/*require_vector=*/false, CaskError::kIo,
        [&] { return search_->search_fields(query, k); });
}

// search_near：BM25 近邻搜索（S8.7）。
std::expected<TextSearchResult, CaskFault>
Cask::search_near(std::string_view query, std::uint32_t slop, std::size_t k) {
    return run_search_one(/*require_vector=*/false, CaskError::kIo,
        [&] { return search_->search_near(query, slop, k); });
}

// bool_search：BM25 布尔搜索（AND/OR/NOT）。
std::expected<TextSearchResult, CaskFault>
Cask::bool_search(std::string_view query, std::size_t k) {
    return run_search_one(/*require_vector=*/false, CaskError::kIo,
        [&] { return search_->bool_search(query, k); });
}

// S8.3：模糊搜索（Levenshtein 编辑距离匹配）。
std::expected<TextSearchResult, CaskFault>
Cask::search_fuzzy(std::string_view query, std::size_t k, std::uint32_t max_edit_distance) {
    return run_search_one(/*require_vector=*/false, CaskError::kIo,
        [&] { return search_->search_fuzzy(query, k, max_edit_distance); });
}

// S8.4：通配符搜索（* / ? 模式匹配）。
std::expected<TextSearchResult, CaskFault>
Cask::search_wildcard(std::string_view pattern, std::size_t k) {
    return run_search_one(/*require_vector=*/false, CaskError::kIo,
        [&] { return search_->search_wildcard(pattern, k); });
}

// S8.2：设置同义词词典。
void Cask::set_synonym_map(std::unique_ptr<text::SynonymMap> map) {
    if (search_) search_->set_synonym_map(std::move(map));
}

std::expected<void, CaskFault> Cask::sync() {
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

StatusInfo Cask::status() {
    StatusInfo s;
    auto info = keydir_->info();
    s.key_count = info.key_count;
    s.key_bytes = info.key_bytes;
    s.epoch     = info.epoch;
    s.files.reserve(info.fstats.size());
    for (const auto& f : info.fstats) {
        s.files.push_back(merge::summarize(dirname_, f));
    }
    s.index_errors = index_errors_.load(std::memory_order_relaxed);
    return s;
}

bool Cask::is_empty_estimate() {
    return keydir_->info().key_count == 0;
}

bool Cask::is_frozen() {
    return keydir_->info().iter_info.frozen;
}

// 包装 merge::decide。关键工作是「排除不该被合的 active file」：
//   - 普通 writer 模式：排除自己的 active_file_id_（不能合自己正在写的）
//   - merge_only 模式：排除 open 时从 write.lock 抠出来的「live writer 当
//     前 active id」；为了应对「writer 在我们 snapshot 之后 roll 过去」，
//     防御性地排除所有 file_id >= snapshot 的文件。代价是少并几个文件，
//     下一轮 merge 自然处理。
Cask::NeedsMerge Cask::needs_merge(std::uint32_t now_sec) {
    auto info = keydir_->info();
    const std::uint32_t exclude_id =
        opts_.merge_only ? merger_writer_active_id_ : active_file_id_;
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
    if (files.empty()) {
        auto n = needs_merge(now_sec);
        if (!n.needs) {
            // 不需要 merge——返回空 stats。
            return merge::MergeStats{};
        }
        files = std::move(n.files);
    }
    auto r = merge::run_merge(files, dirname_, *keydir_, opts_.o_sync, search_.get());
    if (!r) {
        return std::unexpected(err(CaskError::kIo, r.error().detail));
    }

    if (search_) {
        // P3 顺序约定:先落 keydir 快照(取较早的 next_ord),再 flush
        // IndexPool,之后保存的 bm25/sidecar 覆盖必然 ≥ keydir 快照——
        // 并发写入下成对性门依然可判。
        write_keydir_snapshot();
        if (index_pool_ && index_lane_) index_pool_->flush(index_lane_);

        // P2:merge 不再全量重读+重分词重建倒排。merge::run_merge 已通过
        // on_relocate 把每条 live 文档的存储定位更新到新文件;倒排 posting 以
        // 稳定 ord 为键、与文件位置无关;死文档查询时由 is_live 过滤(正确性
        // 不依赖压实)。这里只按阈值压实死 posting 回收空间——不读数据文件、
        // 不重分词,省掉 merge 的全量 NLP 重算。死占比 < 阈值的 posting list
        // 留待后续 merge 累积到阈值再压。
        constexpr double kMergeCompactDeadRatio = 0.2;
        search_->compact(kMergeCompactDeadRatio);
        search_->compact_index_chunks();

        // V4:merge 后同步重建 HNSW（物理清除死节点）。重建在 IndexPool
        // worker 内执行（单写者约束），flush 阻塞等待完成。
        // S6-P2: RebuildHnsw 现在携带 ord（alloc_ord 分配），通过 reorder
        // buffer 与本 merge 期间累积的 put/delete 同序串行 apply——保持 HNSW
        // 单写者约束在 ord 维度上的严格性。该 ord 在数据语义上不指向任何
        // 文档（类似 Skip），仅用于 occupy ord 序列中的位置。
        if (meta_config_.vector_dim > 0 && index_pool_ && index_lane_) {
            auto rebuild_ord = keydir_->alloc_ord();
            IndexTask t;
            t.op  = IndexOp::RebuildHnsw;
            t.ord = rebuild_ord;
            index_pool_->submit(index_lane_, std::move(t));
            index_pool_->flush(index_lane_);
        }

        // P14e:统一分段 search.ckpt 替代旧多文件保存。
        const std::string search_ckpt = dirname_ + "/" + kSearchCkptName;
        search_->save_search_ckpt(search_ckpt,
                                   keydir_->peek_next_ord());
    }

    // After run_merge, every live record from `files` has been CAS-rewritten
    // into the new merge file, and stale records were already pointing
    // elsewhere. So nothing in the keydir references these inputs anymore —
    // safe to unlink the .data + .hint pair and drop the fstats entry.
    //
    // erase + unlink 收在同一临界区(O10):放锁后再 unlink 会留一个窗口,
    // 持旧 keydir 快照的在途 get 在 unlink 后 lazy reopen 报 ENOENT 假失败。
    // 持锁做文件系统操作可接受——merge 收尾是冷路径。被 erase 的句柄若仍
    // 被在途读者持有,由 shared_ptr 引用计数续命(UAF 修复)。
    //
    // Failures here are best-effort: the keydir is already consistent. A
    // residual file just wastes disk until the next process tries the same.
    std::vector<std::uint32_t> trimmed_ids;
    trimmed_ids.reserve(files.size());
    {
        std::scoped_lock lk(read_cache_mu_);
        for (const auto& path : files) {
            std::error_code ec;
            if (auto t = fileops::parse_data_tstamp(path)) {
                read_files_.erase(static_cast<std::uint32_t>(*t));
                trimmed_ids.push_back(static_cast<std::uint32_t>(*t));
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
