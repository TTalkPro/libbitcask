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

#include "cask_internal.hpp"  // S21-3 B1：拆分 TU 共用助手/常量

namespace bitcask {

namespace {
namespace fs = std::filesystem;

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
            // S25-M4:先检查再乘，避免有符号整数溢出 UB。
            if (pid > (1 << 30)) return -1;
            pid = pid * 10 + (c - '0');
            any_digit = true;
        } else {
            break;
        }
    }
    return any_digit ? pid : -1;
}

// 如果锁文件里记录的 pid 已死，尝试删掉它，让 caller 重试 O_EXCL acquire。
//
// 竞态窗口：从读 pid 到 unlink 之间，另一 writer 可能写了新 lock 而被误删；
// 暴露面极小——只在 crash recovery 路径出现，正常运行不会碰到。
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
            if (auto r = cask->load_keydir_from_disk(); !r) return std::unexpected(r.error());
            cask->keydir_->mark_ready();
        }
    } else {
        cask->keydir_ = std::make_shared<keydir::KeyDir>();
        if (auto r = cask->load_keydir_from_disk(); !r) return std::unexpected(r.error());
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
    // = {TextPlugin, VectorPlugin}，S18-5）。生命周期见 close()：先 unregister_lib（flush
    // 排空 ⇒ 闭包不再被调用）再 reset adapter/search_/docmap_。
    if (cask->text_ && cask->registry_) {
        cask->index_pool_ = cask->registry_->index_pool();
        // 分发逻辑提取为命名方法（prepare_index_task / reduce_index_entry /
        // on_index_worker_error），闭包退化为薄捕获委托。
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

// T2.4:open 阶段一——锁分配:
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

// T2.4:open 阶段二——bitcask.meta 读取或创建。必须在插件创建之前——meta
// 决定 KV / 索引模式以及向量配置,VectorPlugin 内部 HnswIndex 创建依赖
// meta_config_。vector_dim/metric 不符 → kModeMismatch。
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

// T2.4:open 阶段三——搜索插件（Text/Vector）+ IndexPool 创建。只在 search_config
// 配置时启动;worker 闭包内的所有 on_* / set_meta / on_vector 路径
// 严格保持原顺序(单写者 = 本 worker 线程,与 on_vector 同线程维持
// HNSW 单写者约束)。

// S18-7：docmap 的 merge 搬迁参与者（设计 §3.9）——不入 plugins_ 注册表，
// 仅由 Cask::merge 置于 run_merge 插件 span 首位（docmap 恒先于插件收到
// relocate）。语义 = 原 SearchLayer::on_relocate：get 旧 slot → put_doc 换
// loc（tstamp/doc_len 保留）。Index 自带锁 → 与 reducer 并发安全（现状
// merger 直调时代的并发面不变）。
namespace {
class DocmapRelocator final : public bitcask::plugin::CaskPlugin {
public:
    explicit DocmapRelocator(bitcask::index::Index* d) : docmap_(d) {}
    std::string_view name() const override { return "docmap-relocator"; }
    bitcask::plugin::PluginStatus
    open(const bitcask::plugin::OpenContext&) override {
        return bitcask::plugin::PluginStatus::kOk;
    }
    std::uint64_t watermark() const override { return 0; }
    bitcask::plugin::PluginStatus close() override {
        return bitcask::plugin::PluginStatus::kOk;
    }
    void on_put(const bitcask::plugin::PutEvent&,
                bitcask::plugin::PreparedPtr) override {}
    void on_delete(const bitcask::plugin::DeleteEvent&) override {}
    bitcask::plugin::FlushResult
    flush(const bitcask::plugin::FlushRequest&) override {
        return {bitcask::plugin::PluginStatus::kOk, 0, 0};
    }
    void on_relocate(const bitcask::plugin::RelocateEvent& e) override {
        auto slot = docmap_->get(e.key);
        if (!slot) return;
        docmap_->put_doc(e.key, e.ord,
                         bitcask::index::DocSlot{
                             bitcask::index::DocLoc{.offset   = e.loc.offset,
                                                    .file_id  = e.loc.file_id,
                                                    .total_sz = e.loc.total_sz},
                             slot->tstamp, slot->doc_len});
    }
private:
    bitcask::index::Index* docmap_;
};
}  // namespace

// ---- S18-5：CaskPluginHost（plugin::PluginHost 实现）----

std::optional<std::string>
Cask::CaskPluginHost::read_at(plugin::RecordLoc loc) {
    auto df = cask_->read_file(loc.file_id);
    if (!df) return std::nullopt;
    auto rec = df->read(loc.offset, loc.total_sz);
    if (!rec || rec->type == format::RecordType::kTombstone) {
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char*>(rec->value.data()),
                       rec->value.size());
}

void Cask::CaskPluginHost::run_serialized(std::function<void()> fn) {
    Cask* c = cask_;
    if (c->index_pool_ && c->index_lane_) {
        IndexTask t;
        t.op  = IndexOp::RunFn;
        t.ord = c->keydir_->alloc_ord();
        t.fn  = std::move(fn);
        c->submit_index_task(std::move(t));
    } else {
        fn();  // 无池（纯 KV / 理论不可达）：调用线程直跑
    }
}

void Cask::CaskPluginHost::log(plugin::LogLevel level, std::string_view msg) {
    if (level == plugin::LogLevel::kError) {
        cask_->log_error(msg);
    } else {
        cask_->log_warn(msg);
    }
}

std::expected<void, CaskFault>
Cask::create_search_infra(const CaskOptions& opts) {
    if (!opts.search_config) {
        return {};
    }
    // V3.3:向量配置从 meta 透传进 SearchLayerConfig(dim>0 时
    // VectorPlugin 内部创建 HnswIndex)。以 meta 为准——open 已校验
    // opts 与 meta 一致。
    auto scfg = *opts.search_config;
    scfg.vector_dim = meta_config_.vector_dim;
    scfg.vector_metric = meta_config_.vector_metric;
    scfg.vector_inmem_int8 = meta_config_.vector_inmem_int8;  // P5b
    scfg.synonym_map = opts.synonym_map;  // S11：Cask 级 open-time 同义词词典透传
    // S16-1/S19-2：docmap 宿主先建，插件直构注入（shim 退役）。
    docmap_ = std::make_shared<index::Index>();
    text_ = std::make_unique<text::TextPlugin>(scfg.text_config(), *docmap_,
                                               *docmap_, *docmap_);
    // analyzer 构造失败（无效配置 / 分词器未注册 / 词典加载失败）则 analyzer_
    // 为空——决不能带病打开，否则首次带 text 的 put 段错误。干净拒绝。
    if (!text_->has_analyzer()) {
        text_.reset();
        docmap_.reset();
        return std::unexpected(err(CaskError::kInvalidOption,
                                   "analyzer creation failed (check analyzer type / dict_path)"));
    }
    vec_plugin_ = std::make_unique<vec::VectorPlugin>(scfg.vector_config(),
                                                      *docmap_);
    hybrid_.emplace(*text_, *vec_plugin_);
    // S18-5：TextPlugin/VectorPlugin 直接注册进分发表。注册序 text 先 vec
    // 后 = 原 reduce_apply 内「add_doc → on_vector」顺序。
    plugins_ = {text_.get(), vec_plugin_.get()};
    // S6-P3: 不再每库自建池。共享池借用 + 车道注册推迟到 keydir 就绪后。
    // 此处仅建好插件，标记本库为 search 模式（text_ != nullptr）。
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

// S16-2 写路径：宿主**先 apply DocMap**（身份/存活/meta），再按注册序广播
// 给各插件（设计 §4：DocMap 恒在所有插件之前）。
// 顺序安全性：docmap 先亮 live、postings/向量后加——并发查询都不可能命中
// 「半个文档」（postings 无 → 不命中；live 无 → 过滤），且 reducer 单写者
// 保证同 ord 两步间无写交错。doc_len 是分析产物，宿主以 0 落行、BM25 侧经
// set_doc_len 回填（S16 批次头②的缓行通道）。
void Cask::reduce_index_entry(ReorderEntry& entry) {
    std::visit([this](auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, PutEntry>) {
            const auto& t = e.task;
            docmap_->put_doc(t.key(), t.ord,
                             index::DocSlot{
                                 index::DocLoc{.offset   = t.offset,
                                               .file_id  = t.file_id,
                                               .total_sz = t.total_sz},
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
    // S25-T1:加 30s 超时兜底——若写线程被 kill（pthread_cancel/信号）致
    // writes_in_flight_ 卡在非零，close 不再永久阻塞；超时后 log_error 并
    // 强制继续释放（接受潜在 UAF 风险优于进程永久挂死）。
    {
        constexpr auto kCloseWaitTimeout = std::chrono::seconds(30);
        auto start = std::chrono::steady_clock::now();
        for (auto n = writes_in_flight_.load(std::memory_order_seq_cst); n != 0;
             n = writes_in_flight_.load(std::memory_order_seq_cst)) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= kCloseWaitTimeout) {
                log_error("close: writes_in_flight_=" + std::to_string(n) +
                          " after 30s — proceeding with teardown (write thread "
                          "may have been killed; potential UAF risk)");
                break;
            }
            // 等到计数变化或超时
            writes_in_flight_.wait(n, std::memory_order_seq_cst);
        }
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
        // ord),再在 keydir 仍在手时做 search 双保存(bm25 + sidecar,覆盖标记
        // 取 peek_next_ord),最后落 keydir 快照并释放。顺序不可颠倒:sidecar
        // 存点须在 keydir_.reset() 之前，否则恒被跳过。
        //
        // S6-P3: 池由 registry 共享，close 只注销本库车道（flush 排空 + 从
        // lanes_ 移除），不停池（其它库仍在用）。unregister_lib 内含 flush，
        // 保证 search_ 析构前本 lane 的 reduce 闭包已不再被 reducer 调用。
        // 整池停在 registry 析构。
        if (index_pool_ && index_lane_) {
            index_pool_->unregister_lib(index_lane_);
            index_lane_ = nullptr;
            index_pool_ = nullptr;  // 仅清借用指针，不动共享池本体
        }
        if (text_ && opts_.read_write && keydir_) {
            // P14e:统一分段 search.ckpt（docmap + bm25 + hnsw 单文件）。
            // S14-4：close 强制全量 base——干净关闭收敛为单一 base，
            // .prev 代际刷新、delta 链坍缩（链不跨干净重启累积）。
            force_ckpt_rebase();
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
    // S18-5：plugins_ 指向 search_ 内部插件，先清分发表再重置 search_
    // （lane 已在上方 unregister（含 flush），闭包不会再触碰）。
    plugins_.clear();
    // S19-2：析构序——融合器（引用两插件）先行，插件次之，docmap 最后。
    hybrid_.reset();
    text_.reset();
    vec_plugin_.reset();
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
// 不变量（路线 A §4），fold 从超前字节水位起跳、search 丢失
// [ckpt_wm, 快照时刻) 区间。快照 entries 比水位新无害：fold 尾部重放对
// keydir 幂等覆盖。
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
    // S14-1 曾在此置 auto-ckpt pending(roll = 字节锚点);S31.5 改为写路径
    // 直接按 ord 增量评估(字节锚点与分词算力脱钩,见
    // maybe_submit_auto_checkpoint 注),roll 点无需再做任何事。
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
    if (!text_) return std::unexpected(err(CaskError::kNoIndex));
    flush_index();
    // S27-4 P2:reducer 排干只保证 job 已**派发**;builder 模式下 apply 可能
    // 仍在途。drain 钩子补齐 read-your-writes(设计 §3;内联模式为空操作)。
    for (auto* p : plugins_) p->drain();
    return {};
}

std::expected<Cask::PersistedRecord, CaskFault>
Cask::write_and_keydir(std::span<const std::byte> key,
                       std::span<std::byte> record,
                       std::uint32_t tstamp, std::uint64_t ord) {
    // S29-7 铺垫：record 已在锁外编码（占位 ord）——此处补真实 ord + CRC
    // 后直接 pwrite。锁内 O(V) 工作从「编码 memcpy + CRC」降为一次 CRC 扫描。
    codec::patch_data_record_ord(record, ord);
    auto w = active_data_->write_encoded(record);
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
    codec::patch_data_record_ord(record, ord2);  // S29-7：重试换 ord2 重 patch
    auto w2 = active_data_->write_encoded(record);
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

// ---------------------------------------------------------------------------
// S29-7:组提交(设计见 cask.hpp GcRequest 注释)
// ---------------------------------------------------------------------------

std::expected<Cask::PersistedRecord, CaskFault>
Cask::submit_group_commit(GcRequest& req) {
    {
        std::lock_guard<std::mutex> g(gc_mu_);
        gc_queue_.push_back(&req);
    }
    for (;;) {
        if (req.done.load(std::memory_order_acquire)) break;
        if (write_mu_.try_lock()) {
            // 我是 leader:循环整批处理(必含自己——入队先于 try_lock 成功)。
            process_gc_batch_locked();
            write_mu_.unlock();
            gc_cv_.notify_all();
            continue;  // 回到循环头取结果
        }
        // follower 先自旋(~几 µs):leader 每记录 ~1.5µs,绝大多数请求在
        // 自旋窗口内完成——cv futex 睡/醒一轮 ~10-20µs,直接睡会把组提交
        // 的收益全部吃掉(首版实测 4/8 线程反而 3× 恶化)。
        bool spun_done = false;
        for (int i = 0; i < 4096; ++i) {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#endif
            if (req.done.load(std::memory_order_acquire)) {
                spun_done = true;
                break;
            }
        }
        if (spun_done) break;
        // 退化:cv 睡,100µs 超时兜底(write_mu_ 可能被 remove/put_batch/
        // merge 等非组提交持有者占着——它们不 notify,超时自转 leader)。
        std::unique_lock<std::mutex> g(gc_mu_);
        if (req.done.load(std::memory_order_acquire)) break;
        gc_cv_.wait_for(g, std::chrono::microseconds(100), [&] {
            return req.done.load(std::memory_order_acquire);
        });
        if (req.done.load(std::memory_order_acquire)) break;
    }
    if (!req.ok) return std::unexpected(req.err);
    return req.rec;
}

// 前置:持 write_mu_。按队列序整批:逐条 alloc_ord + patch + 累积 →
// 一次 pwrite → 逐条 hint/keydir/组提交计数。队列序 == ord 序 == 文件序
// (恢复硬不变量)。失败的请求补 Skip(ord 必须被 Add 或 Skip 覆盖,防
// reorder stall——与旧 OrdSkipGuard 同义,含 in-lock 提交的既有行为)。
void Cask::process_gc_batch_locked() {
    // Leader 循环 re-drain:处理一批期间新到的请求由**当前** leader 继续
    // 消化(否则批大小恒 ≈1——请求总是错过 drain 点,组提交名存实亡,
    // 首版实测仅 +10%)。批大小随争用自然增长:N 并发 → 上一批处理期间
    // 积累 ~N-1 条 → 每 pwrite 合并 ~N 条。
    for (;;) {
        process_one_gc_round_locked();
        std::lock_guard<std::mutex> g(gc_mu_);
        if (gc_queue_.empty()) return;
    }
}

void Cask::process_one_gc_round_locked() {
    static thread_local std::vector<GcRequest*> batch;  // 稳态零分配
    batch.clear();
    {
        std::lock_guard<std::mutex> g(gc_mu_);
        std::swap(batch, gc_queue_);
        // swap 后 gc_queue_ 拿走了 batch 的旧容量——留给下轮复用亦可;
        // 但 thread_local 容量在两个容器间往返,均摊仍零分配。
    }
    if (batch.empty()) return;

    auto finish_all = [&] {
        // done 是发布点:结果字段已写毕,release 发布;自旋 follower 免锁
        // 立即可见。cv 兜底睡眠者仍需 notify。
        for (auto* r : batch) r->done.store(true, std::memory_order_release);
        gc_cv_.notify_all();
    };
    auto fail = [](GcRequest* r, CaskFault e) {
        r->ok = false;
        r->err = std::move(e);
    };
    auto skip_ord = [this](std::uint64_t ord) {
        submit_index_task(
            IndexTask::make(IndexOp::Skip, {}, ord, {}, 0, 0, 0, 0, 0));
    };

    if (is_closed()) {
        for (auto* r : batch) fail(r, err(CaskError::kClosed, "cask is closed"));
        finish_all();
        return;
    }

    // merge race:并发 merger 顶过 active_file_id_ → 先 roll(同旧 put)。
    if (active_data_ && active_file_id_ < keydir_->biggest_file_id()) {
        if (auto r = roll_active(); !r) {
            for (auto* q : batch) fail(q, r.error());
            finish_all();
            return;
        }
    }

    // 累积批:combine 是本批所有 record 的连续拼接(同一文件内);跨
    // max_file_size 边界时先落当前段再 roll。
    thread_local std::vector<std::byte> combine;
    combine.clear();
    static thread_local std::vector<GcRequest*> sub;  // 与 combine 对应(队列序)
    sub.clear();
    bool io_dead = false;
    CaskFault io_err{};

    // 落当前段:一次 pwrite + 逐条 hint/keydir/组提交。返回 false = IO 故障
    // (sub 内全部已 fail + Skip;调用方停止整批)。
    auto flush_sub = [&]() -> bool {
        if (sub.empty()) return true;
        auto w = active_data_->write_encoded(combine);
        if (!w) {
            const auto e = io_fault(w.error().errnum,
                                    std::string(active_data_->path()));
            for (auto* r : sub) {
                fail(r, e);
                skip_ord(r->rec.ord);
            }
            sub.clear();
            combine.clear();
            return false;
        }
        std::uint64_t off = w->offset;
        bool tail_dead = false;  // hint 流故障 → 保守 fail 本段剩余
        CaskFault tail_err{};
        for (auto* r : sub) {
            const auto sz = static_cast<std::uint32_t>(r->record.size());
            if (tail_dead) {
                fail(r, tail_err);
                skip_ord(r->rec.ord);
                off += sz;
                continue;
            }
            auto h = active_hint_->write(r->tstamp, sz, off, /*tomb*/ false,
                                         r->key);
            if (!h) {
                tail_dead = true;
                tail_err = io_fault(h.error().errnum,
                                    std::string(active_hint_->path()));
                fail(r, tail_err);
                skip_ord(r->rec.ord);
                off += sz;
                continue;
            }
            auto pr = keydir_->put(bytes_to_view(r->key), active_file_id_, sz,
                                   off, r->tstamp, /*now*/ 0, /*newest*/ true,
                                   0, 0, r->rec.ord);
            if (pr == keydir::PutResult::kAlreadyExists) {
                // merge race(罕见):单条重写路径(内部 roll + ord2 +
                // 原 ord 的 Skip 语义;见 write_and_keydir)。
                auto p2 = write_and_keydir(r->key, r->record, r->tstamp,
                                           r->rec.ord);
                if (!p2) {
                    fail(r, p2.error());
                    skip_ord(r->rec.ord);
                } else {
                    r->rec = *p2;
                    r->ok = true;
                }
            } else {
                r->rec.offset = off;
                r->rec.total_size = sz;
                r->rec.file_id = active_file_id_;
                r->ok = true;
            }
            if (r->ok) {
                // 组提交计数与旧单条 put 逐一对齐(数据已持久,fsync 失败
                // 沿旧语义:Add 照提、error 照返——post_err)。
                if (auto gcr = maybe_group_commit(); !gcr) {
                    r->post_err = gcr.error();
                }
            }
            off += sz;
        }
        sub.clear();
        combine.clear();
        return true;
    };

    for (auto* r : batch) {
        if (io_dead) {
            fail(r, io_err);
            continue;  // 未分配 ord,无需 Skip
        }
        // roll 判断含未落盘的 combine 尺寸(同段字节必须进同一文件)。
        const bool would_roll =
            !active_data_ ||
            active_data_->size() + combine.size() + r->record.size() >
                opts_.max_file_size;
        if (would_roll) {
            if (!flush_sub()) {
                io_dead = true;
                io_err = err(CaskError::kIo, "group commit flush failed");
                fail(r, io_err);
                continue;
            }
            if (auto rr = roll_active_if_needed(r->record.size()); !rr) {
                io_dead = true;
                io_err = rr.error();
                fail(r, io_err);
                continue;
            }
        }
        const std::uint64_t ord = keydir_->alloc_ord();
        r->rec.ord = ord;
        codec::patch_data_record_ord(r->record, ord);
        combine.insert(combine.end(), r->record.begin(), r->record.end());
        sub.push_back(r);
    }
    if (!io_dead) (void)flush_sub();

    finish_all();
}

std::expected<std::span<const float>, CaskFault>
Cask::prepare_vector(std::span<const float> input,
                     std::vector<float>& norm_buf) const {
    // S18-3：归一化领域核心下沉 VectorPlugin::normalize_for_write（V3.1
    // 「存储即归一化」与同步错误契约不变——Cask 是装配方，同步调用具体
    // 插件；错误消息逐字保留，翻译成 CaskFault）。
    if (input.empty()) return {};
    if (!vec_plugin_ || meta_config_.vector_dim == 0) {
        return std::unexpected(err(CaskError::kInvalidOption,
            "collection has no vector config"));
    }
    auto r = vec_plugin_->normalize_for_write(input, norm_buf);
    if (!r) return std::unexpected(err(CaskError::kInvalidOption, r.error()));
    return *r;
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
        // S25-M1:RAII join guard——emplace_back 抛异常（资源耗尽）时已创建的
        // joinable 线程会被析构自动 join（否则 std::terminate）。与 cask_recovery.cpp
        // 的 JoiningPool 同模式。
        struct JoiningPool {
            std::vector<std::thread> threads;
            ~JoiningPool() {
                for (auto& t : threads) if (t.joinable()) t.join();
            }
        } ws;
        ws.threads.reserve(nthr);
        const std::size_t chunk = (total + nthr - 1) / nthr;
        for (std::size_t t = 0; t < nthr; ++t) {
            const std::size_t b = t * chunk;
            if (b >= total) break;
            ws.threads.emplace_back(worker, b, std::min(b + chunk, total));
        }
        // 显式 join（ws 析构也兜底，但显式 join 让错误在 return 前传播）
        for (auto& w : ws.threads) w.join();
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
        // S24 补：record 内的向量字节偏移不保证 4 对齐——misaligned
        // float* 解引是 UB（UBSan 实证；x86 碰巧能跑，SIMD 消费或严格
        // 平台会炸）。对齐才零拷贝 view；未对齐拷进拥有缓冲对齐化
        // （复用量化路径的 vector_dequant_）。
        const auto* p = dv->vector_raw.data();
        if (reinterpret_cast<std::uintptr_t>(p) % alignof(float) == 0) {
            vector = std::span<const float>(
                reinterpret_cast<const float*>(p), dv->dim);
        } else {
            vector_dequant_.resize(dv->dim);
            std::memcpy(vector_dequant_.data(), p,
                        static_cast<std::size_t>(dv->dim) * sizeof(float));
            vector = std::span<const float>(vector_dequant_.data(), dv->dim);
        }
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
    // S29-7 铺垫：校验/tstamp/DocValue 编码/record 预编码全部**锁外**完成
    // （纯函数或 const 配置读，均不依赖 write_mu_ 保护的状态）。ord 必须锁
    // 内分配（恢复按 fold 序回放 + add_doc 水位自门 ⇒ 文件序 == ord 序是
    // 硬不变量），故 record 先用占位 ord=0 编码，锁内 patch。
    if (!opts_.read_write || opts_.merge_only) {
        return std::unexpected(err(CaskError::kReadOnly));
    }
    if (key.size()   > format::kMaxKeySize)   return std::unexpected(err(CaskError::kKeyTooLarge));
    if (value.size() > format::kMaxValueSize) return std::unexpected(err(CaskError::kValueTooLarge));

    if (tstamp == 0) tstamp = now_sec_default();

    // ⑩ thread_local 复用：encode_doc_value 是 append 语义，clear 后重填；
    // 并发 put 各线程独占一份，消除每次 put 的 encoded 堆分配。
    thread_local std::vector<std::byte> encoded;
    encoded.clear();
    encoded.reserve(value.size() + 16);
    codec::DocValueParts parts;
    parts.text = value;
    parts.expiry_at = expiry_at;  // S13-D5
    codec::encode_doc_value(encoded, parts);
    // record 预编码（占位 ord；锁内 write_and_keydir 补真实 ord + CRC）。
    thread_local std::vector<std::byte> record_buf;
    record_buf.clear();
    codec::encode_data_record(record_buf, format::RecordType::kDoc, tstamp,
                              /*ord 占位*/ 0, key, encoded);

    // S29-7:组提交——入队 + leader/follower(闭锁检查/roll/merge-race/
    // alloc_ord/pwrite/hint/keydir/组提交计数全部 leader 侧,见
    // process_gc_batch_locked)。字节仍在返回前持久(仅合并 syscall)。
    GcRequest req;
    req.key = key;
    req.record = record_buf;
    req.tstamp = tstamp;
    auto persisted = submit_group_commit(req);
    if (!persisted) return std::unexpected(persisted.error());

    // H1(不变):索引提交在锁外——leader 已释放 write_mu_,队列背压只
    // 阻塞本写者。ord 已由 Add 覆盖(失败路径 leader 补 Skip)。
    const PersistedRecord rec = *persisted;
    submit_index_task(IndexTask::make(
        IndexOp::Add, bytes_to_view(key), rec.ord,
        std::string_view(reinterpret_cast<const char*>(value.data()),
                         value.size()),
        rec.file_id, rec.offset, rec.total_size, tstamp, 0));
    maybe_submit_auto_checkpoint();  // S14-1：roll 封口的异步 ckpt（锁外）
    // 旧语义:组提交 fsync 失败 → Add 照提、error 照返。
    if (req.post_err) return std::unexpected(*req.post_err);
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
            // S29-7：write_and_keydir 改收预编码 record——罕见路径，就地编码
            // （占位 ord，函数内 patch）。
            std::vector<std::byte> record;
            codec::encode_data_record(record, format::RecordType::kDoc, tstamp,
                                      /*ord 占位*/ 0, items[i].key, encoded);
            const std::uint64_t ord2 = keydir_->alloc_ord();
            OrdSkipGuard g2(this, ord2);
            auto p2 = write_and_keydir(items[i].key, record, tstamp, ord2);
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
//       (P：盘格式统一小端。)
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
    // S29-7 铺垫：校验/tstamp/向量归一化/字段 intern/DocValue 编码/record
    // 预编码全部**锁外**完成——prepare_vector 读 const 配置、intern 自带
    // shared_mutex、编码是纯函数，均不依赖 write_mu_。ord 锁内分配（文件序
    // == ord 序不变量），record 占位 ord 编码、锁内 patch（同 put）。
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

    // V3.1:向量校验 + cosine 写入归一化(存储即归一化值,merge/恢复
    // 不再重算;hnsw-design §1)。归一化缓冲在双编码点(roll 重试)间复用。
    std::vector<float> vec_norm;
    auto vec_result = prepare_vector(doc.vector, vec_norm);
    if (!vec_result) return std::unexpected(vec_result.error());
    auto vec_out = *vec_result;

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
    // record 预编码（占位 ord；锁内 write_and_keydir 补真实 ord + CRC）。
    // 附带修正：roll 的 about 从「header+key+text+meta 估算」（低估——漏
    // vector/fields 段）变为 record 精确长度。
    thread_local std::vector<std::byte> record_buf;
    record_buf.clear();
    codec::encode_data_record(record_buf, format::RecordType::kDoc, tstamp,
                              /*ord 占位*/ 0, key, encoded);

    // S29-7:组提交(同 put——leader 侧统一处理,见 process_gc_batch_locked)。
    GcRequest req;
    req.key = key;
    req.record = record_buf;
    req.tstamp = tstamp;
    auto persisted = submit_group_commit(req);
    if (!persisted) return std::unexpected(persisted.error());

    // H1(不变):任务构造(fields 打包、vec 移交、meta 拷贝)与提交在锁外。
    const PersistedRecord rec = *persisted;
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
    if (req.post_err) return std::unexpected(*req.post_err);  // 旧组提交语义
    return {};
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
    if (text_) {
        s.hnsw_nodes = vec_plugin_->size();
        s.search_cache_entries = text_->cache_entries();
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
    if (text_) {
        auto idx_info = docmap_->info();
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
    if (!text_) {
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
//（chain_watermark == base_watermark 起始世代 && chain_seq < max）且
// !rebase_needed → 走 delta；否则全量 base（docmap 侧经 docmap_chain_
// 镜像、插件侧经各自 chain_state()）。混合：base 走全量，delta
// 走单文件——但本版每组件一致性优先，要么全 base 要么全 delta。
bool Cask::save_checkpoint_paired(
    const std::string& dir, std::uint64_t wm,
    const std::optional<std::vector<
        std::pair<std::uint32_t, std::uint64_t>>>& wms,
    const std::vector<std::byte>& keydir_delta) {
    if (!text_) return true;  // 纯 KV 库无 search ckpt
    // S18-6：base/delta 决策下沉——宿主只决策 docmap（自己的组件）+ 全局
    // 收链提示（close/静止全量），插件在 flush() 内自决（自身 rebase 标志 +
    // 链长上限）。旧「全体 base or 全体 delta」放宽为 per-component（S17
    // manifest per-component 语义本就支持；rebase 解耦红利：rebuild_hnsw
    // 只 rebase vec 链，bm25/docmap 链不动）。
    // S19-2：脏掩码直接组装（原 shim dirty_mask()）。
    std::array<bool, bitcask::kComponentCount> dirty_mask{};
    dirty_mask[0] = docmap_->dirty();
    dirty_mask[1] = text_->dirty();
    dirty_mask[2] = vec_plugin_->dirty();
    bool any_dirty = false;
    for (bool d : dirty_mask) {
        if (d) { any_dirty = true; break; }
    }
    std::uint32_t max_delta_chain = 0;
    if (opts_.search_config.has_value()) {
        max_delta_chain = opts_.search_config->max_delta_chain;
    }
    const bool docmap_cap = max_delta_chain > 0 &&
                            docmap_chain_.chain_seq >= max_delta_chain;
    // 宿主全局判据（≈ 旧 can_delta）：无脏（静止收尾全量）或全局 rebase
    // （close/compact/rebuild/legacy）→ 全量 base；docmap 链达上限 → docmap
    // 走 base（插件自查各自上限）。
    const bool global_base = !any_dirty ||
        ckpt_rebase_needed_.load(std::memory_order_relaxed);

    bitcask::Manifest new_manifest = current_manifest_;
    bool docmap_base_taken = false;
    if (global_base || docmap_cap) {
        // docmap base（S18-2 宿主直驱）。
        if (index::save_docmap_base(*docmap_, dir, wm)) {
            docmap_chain_ = bitcask::ManifestEntry{wm, 0, wm};
            new_manifest.entries[0] = docmap_chain_;
            docmap_base_taken = true;
        }
    } else if (dirty_mask[0]) {
        const std::uint32_t seq = docmap_chain_.chain_seq + 1;
        if (index::save_docmap_delta(
                *docmap_, dir, wm, docmap_chain_.base_watermark,
                docmap_chain_.chain_watermark, seq,
                std::span<const std::byte>(keydir_delta.data(),
                                            keydir_delta.size()))) {
            docmap_chain_.chain_seq = seq;
            docmap_chain_.chain_watermark = wm;
            new_manifest.entries[0] = docmap_chain_;
        }
    }
    // 插件 flush（S18-6）：force_rebase 传宿主全局判据；插件内部叠加自身
    // rebase/链上限。成功（覆盖水位推进到 wm）才更新 manifest 对应项。
    plugin::FlushRequest freq;
    freq.reason = plugin::FlushRequest::Reason::kManual;
    freq.force_rebase = global_base;
    freq.watermark = wm;
    for (auto* p : plugins_) {
        const auto comp = component_of_plugin(p->name());
        if (!comp) continue;
        auto fr = p->flush(freq);
        if (fr.status == plugin::PluginStatus::kOk && fr.covered_ord == wm) {
            // S20-3 B-B2：链回执随 FlushResult 多态回传，宿主不再下探具体
            // 插件类型读 chain_state()（第三组件零 else 分支即可接入）。
            new_manifest.entries[static_cast<std::size_t>(*comp)] =
                bitcask::ManifestEntry{fr.generation, fr.chain_seq, fr.chain_wm};
        }
    }
    // base 轮落成 → 清 legacy 全局 rebase（旧 save_components_base 尾部
    // 语义；插件自身标志由各自 flush 成功时清）。
    if (global_base) {
        ckpt_rebase_needed_.store(false, std::memory_order_relaxed);
    }
    const std::string mpath = std::string(dir) + "/" +
        std::string(bitcask::kManifestName);
    if (!bitcask::write_manifest(mpath, new_manifest)) {
        log_warn("save_checkpoint_paired: write_manifest failed "
                 "(existing manifest unchanged)");
        return false;
    }
    current_manifest_ = new_manifest;
    // keydir 快照与 docmap base 成对（delta 路径下 keydir 元数据已内联进
    // docmap delta 的 kKeydirDelta 段）。
    if (docmap_base_taken && wms) {
        write_keydir_snapshot(*wms);
    }
    return true;
}

void Cask::maybe_submit_auto_checkpoint() {
    if (opts_.auto_checkpoint_min_docs == 0) return;          // 未启用
    if (!text_ || !index_pool_ || !index_lane_) return;     // 仅索引模式
    // S31.5(下游反馈跟进):ord 增量**本身**即锚点。原先还要求数据文件
    // roll 置 pending 标记——roll 按 KV 字节触发,与分词算力完全脱钩:
    // 大文档语料(zhwiki 单篇数百 KB)一个 roll 周期可积累数万篇 analyze
    // 工作量,崩溃后全部重放(且 S30 后段文件在预算/阈值封口时已在盘上,
    // 只差清单提交,重放纯属浪费)。改为每写评估 ord 增量(两次 relaxed
    // load,热路径零 RMW),恢复重放窗口恒 ≤ min_docs。
    const std::uint64_t now_ord = keydir_->peek_next_ord();
    if (now_ord - last_ckpt_ord_.load(std::memory_order_relaxed) <
        opts_.auto_checkpoint_min_docs) {
        return;
    }
    // 在途窗口先 relaxed 预检再 exchange——防「达阈值后每写一次 RMW」
    // 打在共享 cacheline(S29-6/7 教训:热路径共享 RMW 即弹跳)。
    if (auto_ckpt_inflight_.load(std::memory_order_relaxed)) return;
    if (auto_ckpt_inflight_.exchange(true, std::memory_order_acq_rel)) {
        return;  // 已有在途 RunFn,完成后由后续写触发重试
    }
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
    // S18-7：merge 参与插件 span——首位 DocmapRelocator（宿主 docmap 搬迁），
    // 其余按注册序。纯 KV 库空 span。生命周期：栈上，覆盖 run_merge 全程
    //（事件同步派发；插件收尾闭包经 run_serialized 已入 RunFn 队列，不引用
    // relocator）。
    DocmapRelocator relocator(docmap_.get());
    std::vector<plugin::CaskPlugin*> merge_plugins;
    if (text_) {
        merge_plugins.reserve(plugins_.size() + 1);
        merge_plugins.push_back(&relocator);
        for (auto* p : plugins_) merge_plugins.push_back(p);
    }
    auto r = merge::run_merge(files, dirname_, *keydir_, opts_.o_sync,
                              merge_plugins,
                              now_sec ? now_sec : now_sec_default());
    if (!r) {
        return std::unexpected(err(CaskError::kIo, r.error().detail));
    }

    if (text_) {
        // P3 顺序约定（S14-4 修订）:**捕获**较早水位（此刻文件大小），但
        // 文件**写入**延后到 search.ckpt 保存成功之后——写序反了的话，
        // 两写之间崩溃会留下「新快照+旧 ckpt」，fold 从超前水位起跳、
        // search 永久丢窗口。捕获早 + 写入晚，两个约束同时满足。
        const auto merge_snap_wms = collect_snapshot_watermarks();
        if (index_pool_ && index_lane_) index_pool_->flush(index_lane_);

        // S18-7：倒排压实与 HNSW 重建已随 on_merge_commit 由各插件自主
        // 提交（run_serialized → RunFn FIFO，先于下方成对保存点执行——
        // 顺序涌现 = 旧「compact → rebuild → ckpt」硬编码序）。宿主只剩
        // docmap 侧收尾（chunk 回收）+ merge 恒 rebase + 成对保存点。

        // P14e:统一分段 search.ckpt 替代旧多文件保存。
        // best-effort:checkpoint 保存失败非致命——下次 open 回退到全量 fold 重建
        // 搜索索引（仅慢一次启动，数据不受影响），故显式忽略返回值而非让 merge 失败。
        // S13-F6：序列化同样遍历 concurrent_hash_map——经 RunFn 在
        // reducer 线程执行（S22-B2 前还有 truncate_wal 竞争一由，WAL 已退役）。
        // 水位在提交时取值（by-value 捕获），语义与旧版一致。
        const std::string search_ckpt = dirname_ + "/" + kSearchCkptName;
        if (index_pool_ && index_lane_) {
            IndexTask t;
            t.op  = IndexOp::RunFn;
            t.ord = keydir_->alloc_ord();
            // S14-7：merge 恒 rebase → paired 走 base + 全量快照（用早段
            // 捕获的水位）。S18-7：rebase 标志改在此显式置位（原经 compact
            // shim 顺带置全局位；插件版 compact 只置自身位）；docmap chunk
            // 回收（原 compact_index_chunks）一并收进本 RunFn。
            t.fn  = [this, search_ckpt, wms = merge_snap_wms,
                     wm = keydir_->peek_next_ord()] {
                docmap_->compact_chunks();
                force_ckpt_rebase();
                if (!save_search_ckpt_paired(search_ckpt, wm, wms, {})) {
                    log_warn("search checkpoint save failed after merge "
                             "(will rebuild on next open)");  // S13-D7
                }
            };
            index_pool_->submit(index_lane_, std::move(t));
            index_pool_->flush(index_lane_);
        } else {
            docmap_->compact_chunks();
            force_ckpt_rebase();
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
