#include "bitcask/oki_state.hpp"

#include <chrono>
#include <thread>

#include "bitcask/oki_merge_cursor.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <limits>
#include "bitcask/detail/path_utf8.hpp"

namespace bitcask::oki {

namespace {

// `kv.oki.seg-<gen>` → gen；非该形态 / 数字非法 → nullopt。
std::optional<std::uint64_t> parse_run_gen(std::string_view filename) {
    constexpr std::string_view kPrefix = "kv.oki.seg-";
    if (!filename.starts_with(kPrefix)) return std::nullopt;
    const auto digits = filename.substr(kPrefix.size());
    if (digits.empty()) return std::nullopt;
    std::uint64_t g = 0;
    const auto* end = digits.data() + digits.size();
    auto [p, ec] = std::from_chars(digits.data(), end, g);
    if (ec != std::errc{} || p != end) return std::nullopt;
    return g;
}

// 目录内现存 run 文件的最大 gen（含孤儿/滞留件）；无 → 0。
std::uint64_t max_gen_in_dir(std::string_view dir) {
    std::uint64_t g = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(
             bitcask::detail::from_utf8(dir), ec)) {
        if (auto v = parse_run_gen(bitcask::detail::to_utf8(e.path().filename()))) {
            g = std::max(g, *v);
        }
    }
    return g;
}

// 删除目录内 gen 不在 keep 集里的全部 run 文件。
// **不能只删旧 manifest 列出的那些**：触发重建的典型场景恰恰是 manifest
// 缺失/损坏（此时内存 manifest_ 为空），那批 run 文件就成了无人回收的
// 孤儿，每重建一次多一批。在途 ReadView 经 shared_ptr 持旧 Reader——
// POSIX 下 unlink 后 fd 仍可读到 close；B4：删除失败（非 POSIX 语义下
// 文件仍被打开等）的路径回填 backlog，下次 sweep 重试（延迟删除队列的
// OKI 侧形态——正确性不依赖 unlink-while-open）。
// inflight：已预留、正在锁外构建的 gen（重建 / 后台归并）。它们的 run 与
// spill 文件不属于任何 manifest，但绝不能被并发提交的 sweep 删掉。
void sweep_runs(std::string_view dir, std::span<const std::uint64_t> keep,
                std::vector<std::string>& backlog,
                std::span<const std::uint64_t> inflight = {}) {
    const auto is_inflight = [&](std::uint64_t g) {
        return std::find(inflight.begin(), inflight.end(), g) != inflight.end();
    };
    // 先重试上一轮滞留（可能已无读者）。
    std::vector<std::string> still;
    for (const auto& p : backlog) {
        std::error_code rm_ec;
        const auto stale_path = bitcask::detail::from_utf8(p);
        std::filesystem::remove(stale_path, rm_ec);
        if (rm_ec && std::filesystem::exists(stale_path)) still.push_back(p);
    }
    backlog = std::move(still);
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(bitcask::detail::from_utf8(dir), ec)) {
        const auto name = bitcask::detail::to_utf8(e.path().filename());
        // S36-1：外排 spill 残件（kv.oki.spill-*，仅崩溃遗留）一律清理。
        if (name.rfind("kv.oki.spill-", 0) == 0) {
            // kv.oki.spill-<gen>-<n>：在途构建的 spill 保留。
            const auto rest = std::string_view(name).substr(13);
            std::uint64_t sg = 0;
            auto [p, sec] = std::from_chars(rest.data(), rest.data() + rest.size(), sg);
            if (sec == std::errc{} && p != rest.data() && is_inflight(sg)) continue;
            std::error_code rm_ec;
            std::filesystem::remove(e.path(), rm_ec);
            continue;
        }
        const auto g = parse_run_gen(name);
        if (!g) continue;
        if (std::find(keep.begin(), keep.end(), *g) != keep.end()) continue;
        if (is_inflight(*g)) continue;
        std::error_code rm_ec;
        std::filesystem::remove(e.path(), rm_ec);
        if (rm_ec && std::filesystem::exists(e.path())) {
            backlog.push_back(bitcask::detail::to_utf8(e.path()));  // B4：滞留重试
        }
    }
}

// S43-2：caller 快照进入 manifest 前的最后一道结构/水位门。这比 CRC
// 更严格：reader 拒收能保护 open，但提交端必须在发布前拦住本地协议错误。
[[nodiscard]] bool valid_commit_snapshot(
    const OkiManifest& next,
    const OkiManifestMetadata& g,
    const std::vector<OkiManifestFStats>& fs) {
    const auto max_cover = [&]() {
        std::uint64_t v = 0;
        for (const auto& r : next.runs) v = std::max(v, r.cover_ord);
        return v;
    };
    return g.reserved == 0 &&
           g.next_file_id == g.biggest_file_id + 1 &&
           g.next_ord >= next.wm &&
           g.next_ord >= max_cover() &&
           (g.active_keys == 0) == (g.live_bytes == 0) &&
           !(g.oldest_tstamp == 0 && g.newest_tstamp != 0) &&
           !(g.active_keys == 0 &&
             (g.oldest_tstamp != 0 || g.newest_tstamp != 0)) &&
           !(g.active_keys != 0 &&
             (g.oldest_tstamp == 0 || g.newest_tstamp == 0)) &&
           fs.size() <= (1u << 24);
}

}  // namespace

namespace {

// 按 (key, ord) 稳定排序后每 key 取末行（(ord, 到达序) 胜出者）。
std::vector<OkiState::DeltaRow> sort_dedup(std::vector<OkiState::DeltaRow> rows) {
    std::stable_sort(rows.begin(), rows.end(),
                     [](const OkiState::DeltaRow& a, const OkiState::DeltaRow& b) {
                         if (a.key != b.key) return a.key < b.key;
                         return a.ord < b.ord;
                     });
    // out == i 的自移动赋值对 std::string 是 valid-but-unspecified，必须跳过。
    std::size_t out = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i + 1 < rows.size() && rows[i + 1].key == rows[i].key) continue;
        if (out != i) rows[out] = std::move(rows[i]);
        ++out;
    }
    rows.resize(out);
    return rows;
}

const OkiState::DeltaRow* segment_find(const OkiState::Segment& seg,
                                       std::string_view key) {
    auto it = std::lower_bound(
        seg.begin(), seg.end(), key,
        [](const OkiState::DeltaRow& r, std::string_view k) { return r.key < k; });
    if (it == seg.end() || it->key != key) return nullptr;
    return &*it;
}

constexpr std::size_t kCancelCheckRows = 4096;

}  // namespace

// ---------------------------------------------------------------------------
// active memdelta（只追加；mu_）
// ---------------------------------------------------------------------------

void OkiState::append(std::string_view key, std::uint64_t ord, bool tomb,
                      const RowLoc* loc) {
    // 水位门（wm = 排他上界 = 尚未覆盖的最小 ord；首个合法 LSN 是 0，
    // 故不能有 ord==0 特判）：tail 重放只收 wm 起的行。
    if (ord < wm_.load(std::memory_order_acquire)) return;
    append_update(key, ord, tomb, loc);
}

void OkiState::append_update(std::string_view key, std::uint64_t ord,
                             bool tomb, const RowLoc* loc,
                             std::uint32_t dur_file, std::uint64_t dur_end) {
    std::lock_guard<std::mutex> lk(mu_);
    delta_bytes_ += key.size() + sizeof(DeltaRow);
    delta_.push_back(DeltaRow{std::string(key), ord, tomb,
                              /*has_loc=*/loc != nullptr,
                              loc != nullptr ? *loc : RowLoc{}});
    delta_.back().seq = ++append_seq_;
    delta_.back().dur_file = dur_file;
    delta_.back().dur_end = dur_end;
    ++delta_version_;  // 排序视图缓存失效
    index_row_locked(delta_.size() - 1);
    update_flush_hint_locked();
}

// S63：批量发布。锁外只构造行（含 key 拷贝），临界区内只做 move 追加、
// 到达序号分配与点查哈希登记。delta_version_ 只 bump 一次——排序视图
// 缓存对整批只失效一次；flush_hint 同样在批末更新。
void OkiState::apply_batch(std::span<const BatchRow> rows) {
    if (rows.empty()) return;
    std::vector<DeltaRow> prepared;
    prepared.reserve(rows.size());
    std::size_t bytes = 0;
    for (const auto& row : rows) {
        bytes += row.key.size() + sizeof(DeltaRow);
        DeltaRow out{std::string(row.key), row.ord, row.tomb,
                     /*has_loc=*/!row.tomb,
                     row.tomb ? RowLoc{} : row.loc};
        out.dur_file = row.tomb ? row.dur_file : 0;
        out.dur_end = row.tomb ? row.dur_end : 0;
        prepared.push_back(std::move(out));
    }

    std::lock_guard<std::mutex> lk(mu_);
    const std::size_t first = delta_.size();
    delta_.insert(delta_.end(), std::make_move_iterator(prepared.begin()),
                  std::make_move_iterator(prepared.end()));
    delta_bytes_ += bytes;
    for (std::size_t i = first; i < delta_.size(); ++i) {
        delta_[i].seq = ++append_seq_;
        index_row_locked(i);
    }
    ++delta_version_;  // 排序视图缓存失效
    update_flush_hint_locked();
}

void OkiState::index_row_locked(std::size_t idx) {
    if (!point_query_.load(std::memory_order_relaxed)) return;
    const DeltaRow& row = delta_[idx];
    auto [it, inserted] = delta_idx_.try_emplace(row.key, idx);
    // (ord, 到达序) 胜出：等 ord 后到者顶替（搬迁行），更小 ord 不顶替。
    if (!inserted && delta_[it->second].ord <= row.ord) it->second = idx;
}

void OkiState::rebuild_index_locked() {
    delta_idx_.clear();
    if (!point_query_.load(std::memory_order_relaxed)) return;
    for (std::size_t i = 0; i < delta_.size(); ++i) index_row_locked(i);
}

void OkiState::enable_point_query() {
    std::lock_guard<std::mutex> lk(mu_);
    if (point_query_.load(std::memory_order_relaxed)) return;
    point_query_.store(true, std::memory_order_release);
    rebuild_index_locked();
}

void OkiState::drop_active_through(std::uint64_t through) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<DeltaRow> kept;
    kept.reserve(delta_.size());
    std::size_t bytes = 0;
    for (auto& r : delta_) {
        if (r.seq > through) {
            bytes += r.key.size() + sizeof(DeltaRow);
            kept.push_back(std::move(r));
        }
    }
    delta_ = std::move(kept);
    delta_bytes_ = bytes;
    ++delta_version_;
    rebuild_index_locked();
    update_flush_hint_locked();
}

// S43-5：写前探测是 locate 的显式语义别名。
OkiState::WriteProbe OkiState::probe_for_write(std::string_view key) const {
    const auto r = locate(key);
    return WriteProbe{r.status, r.ord, r.tomb, r.has_loc, r.loc};
}

bool OkiState::write_probe_ready() const noexcept {
    if (!point_query_.load(std::memory_order_acquire) ||
        !loaded_.load(std::memory_order_acquire)) {
        return false;
    }
    const auto view = view_.load();
    if (!view) return false;
    return std::none_of(
        view->runs.begin(), view->runs.end(),
        [](const auto& run) { return run.second->version() == kRunVersion; });
}

// 运行期应用一律绕过 wm 门：调用方（IndexAuthority）已在写序闸门内做过
// probe + CAS 裁决，行必然是该 key 的最新态。wm 门只服务恢复期的 tail
// 重放；用在运行期会误丢在途写（运行期重建把 wm 设为 next_ord，含已分配、
// 尚未应用的 ord）。
void OkiState::apply_put(std::string_view key, std::uint64_t ord,
                         const RowLoc& loc) {
    append_update(key, ord, /*tomb=*/false, &loc);
}

void OkiState::apply_remove(std::string_view key, std::uint64_t ord,
                            std::uint32_t dur_file, std::uint64_t dur_end) {
    append_update(key, ord, /*tomb=*/true, nullptr, dur_file, dur_end);
}

void OkiState::apply_conditional_remove(std::string_view key,
                                        std::uint64_t victim_ord) {
    append_update(key, victim_ord, /*tomb=*/true, nullptr);
}

void OkiState::apply_relocation(std::string_view key, std::uint64_t ord,
                                const RowLoc& loc) {
    append_update(key, ord, /*tomb=*/false, &loc);
}

// S43-1：坏块运行期降级。先关点查，再置 unloaded。
void OkiState::degrade_for_rebuild() noexcept {
    std::lock_guard<std::mutex> flk(flush_mu_);
    point_query_.store(false, std::memory_order_release);
    loaded_.store(false, std::memory_order_release);
}

void OkiState::publish_view_locked() {
    auto v = std::make_shared<OkiView>();
    v->runs = readers_;
    v->frozen = frozen_;
    v->frozen_through = frozen_through_;
    for (const auto& r : manifest_.runs) gen_floor_ = std::max(gen_floor_, r.gen);
    view_.publish(std::move(v));
}

// ---------------------------------------------------------------------------
// load（S44-5：run 缺失时重读 manifest 重试）
// ---------------------------------------------------------------------------

void OkiState::load(std::string_view dir) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    // 另一个进程（可写句柄）的归并 / flush 可能恰在我们读完 manifest、打开
    // run 之前把旧 run 提交掉并 sweep。run 文件「不存在」时重读 manifest 重
    // 试（有界次数）；CRC / 结构损坏仍整体弃用（未加载态 → caller 重建）。
    constexpr int kMaxAttempts = 8;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        auto m = read_manifest(dir);
        if (!m) return;  // 缺失/损坏 → 未加载态（caller 决定重建）
        RunsVec rds;
        rds.reserve(m->runs.size());
        bool missing = false;
        bool broken = false;
        for (const auto& r : m->runs) {
            const auto path = mk_run_filename(dir, r.gen);
            auto rd = OkiRunReader::open(path);
            if (!rd) {
                std::error_code ec;
                if (!std::filesystem::exists(bitcask::detail::from_utf8(path), ec)) {
                    missing = true;
                } else {
                    broken = true;
                }
                break;
            }
            rds.emplace_back(r.gen,
                             std::make_shared<OkiRunReader>(*std::move(rd)));
        }
        if (broken) return;
        if (missing) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1 << attempt));
            continue;
        }
        manifest_ = *std::move(m);
        readers_ = std::move(rds);
        publish_view_locked();
        manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
        wm_.store(manifest_.wm, std::memory_order_release);
        loaded_.store(true, std::memory_order_release);
        return;
    }
}

// S36-4：降级戳（level_b 关而盘上戳开 → 改写 manifest 清戳）。
void OkiState::stamp_mode(std::string_view dir) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    if (!loaded_.load(std::memory_order_relaxed)) return;
    const bool want = level_b_.load(std::memory_order_acquire);
    if (manifest_.level_b == want) return;
    if (want) return;  // 升级只经 rebuild
    OkiManifest next = manifest_;
    next.level_b = false;
    if (!write_manifest(dir, next)) return;
    manifest_ = std::move(next);
    manifest_level_b_.store(false, std::memory_order_release);
}

std::uint64_t OkiState::next_gen_locked(std::string_view dir) {
    std::uint64_t g = gen_floor_;
    for (const auto& r : manifest_.runs) g = std::max(g, r.gen);
    for (const auto x : inflight_gens_) g = std::max(g, x);
    g = std::max(g, max_gen_in_dir(dir));
    return g + 1;
}

void OkiState::release_gen_locked(std::uint64_t gen) {
    std::erase(inflight_gens_, gen);
}

// ---------------------------------------------------------------------------
// flush：冻结 → 落盘 → 单次发布
// ---------------------------------------------------------------------------

bool OkiState::flush(
    std::string_view dir,
    std::span<const std::pair<std::uint32_t, std::uint64_t>> durable_wms,
    std::optional<OkiCommitSnapshot> commit_snapshot) {
    bool need_compact = false;
    const bool ok = flush_locked_impl(dir, durable_wms,
                                      std::move(commit_snapshot), need_compact);
    if (ok && need_compact) {
        schedule_compaction(dir);
        wait_backpressure();
    }
    return ok;
}

bool OkiState::flush_locked_impl(
    std::string_view dir,
    std::span<const std::pair<std::uint32_t, std::uint64_t>> durable_wms,
    std::optional<OkiCommitSnapshot> commit_snapshot, bool& need_compact) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    if (!loaded_.load(std::memory_order_relaxed)) return false;

    // S43-2：空 memdelta 也是合法提交点（v3→v4 迁移目录可能没有待固化行，
    // 但仍要把刚捕获的标量/fstats 随 manifest 原子发布）。
    const auto commit_metadata_only = [&]() -> bool {
        if (!commit_snapshot.has_value()) return true;
        const auto& g = commit_snapshot->metadata;
        if (!valid_commit_snapshot(manifest_, g, commit_snapshot->fstats)) {
            return false;
        }
        OkiManifest next = manifest_;
        if (!durable_wms.empty()) {
            next.data_wms.assign(durable_wms.begin(), durable_wms.end());
        }
        next.metadata = g;
        next.fstats = commit_snapshot->fstats;
        next.level_b = level_b_.load(std::memory_order_acquire);
        if (next.metadata == manifest_.metadata &&
            next.fstats == manifest_.fstats &&
            next.level_b == manifest_level_b_.load(std::memory_order_acquire) &&
            next.data_wms == manifest_.data_wms) {
            return true;
        }
        if (!write_manifest(dir, next)) return false;
        manifest_ = std::move(next);
        manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
        return true;
    };

    // ---- 1) 切面之内的 active 行 → 胜出行 ----
    // 带快照：切面 = cut_seq（seq ≤ cut 的行恰是账本已计入的行）；无快照：
    // 当前全部行。
    const std::uint64_t cut_seq =
        commit_snapshot.has_value() ? commit_snapshot->cut_seq
                                    : std::numeric_limits<std::uint64_t>::max();
    std::vector<DeltaRow> eligible;
    std::uint64_t max_eligible_seq = 0;
    std::uint64_t min_remaining_ord = std::numeric_limits<std::uint64_t>::max();
    {
        std::lock_guard<std::mutex> lk(mu_);
        eligible.reserve(delta_.size());
        for (const auto& r : delta_) {
            if (r.seq <= cut_seq) {
                eligible.push_back(r);
                max_eligible_seq = std::max(max_eligible_seq, r.seq);
            } else {
                min_remaining_ord = std::min(min_remaining_ord, r.ord);
            }
        }
    }
    auto winners = sort_dedup(std::move(eligible));

    // 持久性：胜出行引用了未持久字节 → 放弃本轮（状态不变）。带快照时这些行
    // 已计入账本，run 不含而 data 水位又在其前，崩溃重放会重复记账；无快照
    // 时同样不得发布引用可能随掉电蒸发的字节的 run。
    if (!durable_wms.empty()) {
        std::uint32_t max_snap_fid = 0;
        for (const auto& [fid, wm] : durable_wms) {
            max_snap_fid = std::max(max_snap_fid, fid);
        }
        const auto beyond = [&](std::uint32_t fid, std::uint64_t end) {
            if (fid == 0) return false;
            for (const auto& [f, wm] : durable_wms) {
                if (f == fid) return end > wm;
            }
            // 快照之后新建的文件（id 更大）尚无持久保证；更小 id 的缺席文件
            // 是已退役的旧文件，其行必然已被覆盖。
            return fid > max_snap_fid;
        };
        for (const auto& r : winners) {
            const bool b = r.has_loc ? beyond(r.loc.file_id,
                                              r.loc.offset + r.loc.total_sz)
                                     : (r.tomb && beyond(r.dur_file, r.dur_end));
            if (b) return false;
        }
    }

    if (winners.empty() && frozen_.empty()) return commit_metadata_only();

    // ---- 2) 冻结：发布后从 active 物理删除 ----
    if (!winners.empty()) {
        frozen_.push_back(std::make_shared<const Segment>(std::move(winners)));
        frozen_through_ = std::max(frozen_through_, max_eligible_seq);
        publish_view_locked();
        drop_active_through(frozen_through_);
    }

    // ---- 3) 落盘：全部待落盘段归并成一个 run（墓碑保留）----
    std::vector<OkiMergeCursor::MemSource> segs(frozen_.begin(), frozen_.end());
    std::uint64_t est = 0;
    for (const auto& sp : frozen_) est += sp->size();
    const std::uint64_t gen = next_gen_locked(dir);
    const auto run_path = mk_run_filename(dir, gen);
    const auto drop_run = [&] {
        std::error_code ec;
        std::filesystem::remove(bitcask::detail::from_utf8(run_path), ec);
    };
    auto w = OkiRunWriter::create(run_path, kDefaultBlockBytes, kRunVersion3,
                                  /*expected_entries=*/est);
    if (!w) return false;
    std::uint64_t cover = 0;
    std::uint64_t written = 0;
    {
        auto mc = OkiMergeCursor::open({}, std::move(segs));
        if (!mc) return false;
        DeltaRow r;
        while (true) {
            auto n = mc->next(r, /*include_tombs=*/true);
            if (!n) return false;
            if (!*n) break;
            auto a = w->add(std::span<const std::byte>(
                                reinterpret_cast<const std::byte*>(r.key.data()),
                                r.key.size()),
                            r.ord, r.tomb, r.has_loc ? &r.loc : nullptr);
            if (!a) {
                drop_run();
                return false;
            }
            cover = std::max(cover, r.ord + 1);
            ++written;
        }
    }
    if (!w->finish(/*fsync_dir=*/true)) {
        drop_run();
        return false;
    }
    auto rd = OkiRunReader::open(run_path);
    if (!rd) {
        drop_run();
        return false;
    }

    // ---- 4) manifest 提交（唯一 commit point）----
    OkiManifest next = manifest_;
    if (!durable_wms.empty()) {
        next.data_wms.assign(durable_wms.begin(), durable_wms.end());
    }
    next.runs.push_back({gen, cover, /*format_ver=*/3});
    next.wm = std::max(next.wm, std::min(cover, min_remaining_ord));
    next.level_b = level_b_.load(std::memory_order_acquire);  // S36-4 模式戳
    if (commit_snapshot.has_value()) {
        next.metadata = commit_snapshot->metadata;
        next.fstats = std::move(commit_snapshot->fstats);
        if (!valid_commit_snapshot(next, *next.metadata, next.fstats)) {
            drop_run();
            return false;
        }
    }
    if (!write_manifest(dir, next)) {
        drop_run();
        return false;
    }

    // ---- 5) 单次发布：run 入视图、段出视图 ----
    manifest_ = std::move(next);
    readers_.emplace_back(gen, std::make_shared<OkiRunReader>(*std::move(rd)));
    frozen_.clear();
    publish_view_locked();
    manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
    wm_.store(manifest_.wm, std::memory_order_release);
    {
        std::lock_guard<std::mutex> sl(stats_mu_);
        ++stats_.flushes;
        stats_.flushed_rows += written;
    }
    need_compact = needs_compaction_locked();
    return true;
}

// ---------------------------------------------------------------------------
// 归并（S44-3 / S44-4）
// ---------------------------------------------------------------------------

void OkiState::set_compaction_policy(const CompactionPolicyOptions& opts) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    policy_ = opts;
}

CompactionPolicyOptions OkiState::compaction_policy() const {
    std::lock_guard<std::mutex> flk(flush_mu_);
    return policy_;
}

void OkiState::set_compaction_row_hook_for_test(CompactionRowHook hook) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    row_hook_ = std::move(hook);
}

OkiState::CompactionStats OkiState::compaction_stats() const {
    std::lock_guard<std::mutex> sl(stats_mu_);
    return stats_;
}

bool OkiState::needs_compaction_locked() const {
    std::vector<std::uint64_t> sizes;
    sizes.reserve(readers_.size());
    for (const auto& [g, rd] : readers_) sizes.push_back(rd->entry_count());
    return choose_compaction(sizes, 0, policy_,
                             force_full_next_.load(std::memory_order_relaxed))
        .merge;
}

// 一次归并（三段式；设计 §4.5）：
//   1) 锁内按策略选后缀 Rj..Rn，快照输入，预留输出 gen（在途登记）；
//   2) 锁外 OkiMergeCursor 归并：不含最老 run 时**保留墓碑**（更老的 run
//      里可能还有该 key 的 put，丢墓碑会让它复活）；含最老 run 时丢弃；
//   3) 锁内校验：输入仍在册且连续，其后的 run gen 全部大于输出 gen；替换并
//      单次发布视图（冻结段与 frozen_through 原样沿用——归并只换 runs，新旧
//      内容等价）。
OkiState::CompactResult OkiState::compact_once(std::string_view dir,
                                               Lane lane) {
    if (cancel_.load(std::memory_order_acquire)) return CompactResult::kCancelled;
    const auto t0 = std::chrono::steady_clock::now();

    RunsVec inputs;
    std::uint64_t gen = 0;
    std::uint64_t cover = 0;
    std::uint32_t out_ver = kRunVersion3;
    std::uint64_t est = 0;
    bool full = false;
    CompactionRowHook hook;
    {
        std::lock_guard<std::mutex> flk(flush_mu_);
        if (!loaded_.load(std::memory_order_acquire)) return CompactResult::kNone;
        hook = row_hook_;
        std::size_t floor = 0;
        if (lane == Lane::kYoung) {
            if (!main_out_gen_) return CompactResult::kNone;
            floor = readers_.size();
            for (std::size_t i = 0; i < readers_.size(); ++i) {
                if (readers_[i].first > *main_out_gen_) {
                    floor = i;
                    break;
                }
            }
        }
        std::vector<std::uint64_t> sizes;
        sizes.reserve(readers_.size());
        for (const auto& [g, rd] : readers_) sizes.push_back(rd->entry_count());
        const bool want_full =
            lane == Lane::kMain && force_full_next_.load(std::memory_order_relaxed);
        const auto choice = choose_compaction(sizes, floor, policy_, want_full);
        if (!choice.merge) return CompactResult::kNone;
        full = choice.full;
        inputs.assign(readers_.begin() + static_cast<std::ptrdiff_t>(choice.begin),
                      readers_.end());
        for (std::size_t i = choice.begin; i < manifest_.runs.size(); ++i) {
            cover = std::max(cover, manifest_.runs[i].cover_ord);
        }
        // S43-1：全 v2/v3 输入才出 v3；含 v1 则整体降 v1（待 rebuild 淘汰后自愈）。
        for (const auto& [g, rd] : inputs) {
            if (rd->version() == kRunVersion) out_ver = kRunVersion;
            est += rd->entry_count();
        }
        gen = next_gen_locked(dir);
        inflight_gens_.push_back(gen);  // 期间的 flush 必取更大 gen
        if (lane == Lane::kMain) main_out_gen_ = gen;
    }

    const auto out_path = mk_run_filename(dir, gen);
    // 任何退出路径都撤销在途登记 / 主道标记，并删除未提交的输出。
    bool committed = false;
    const auto finish = [&](CompactResult r) {
        {
            std::lock_guard<std::mutex> flk(flush_mu_);
            release_gen_locked(gen);
            if (lane == Lane::kMain) main_out_gen_.reset();
        }
        if (!committed) {
            std::error_code ec;
            std::filesystem::remove(bitcask::detail::from_utf8(out_path), ec);
        }
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        std::lock_guard<std::mutex> sl(stats_mu_);
        stats_.compaction_ns += ns;
        if (r == CompactResult::kFailed) ++stats_.aborted;
        if (r == CompactResult::kCancelled) ++stats_.cancelled;
        return r;
    };

    std::uint64_t rows = 0;
    std::uint64_t tombs = 0;
    {
        auto w = OkiRunWriter::create(out_path, kDefaultBlockBytes, out_ver, est);
        if (!w) return finish(CompactResult::kFailed);
        std::vector<std::shared_ptr<OkiRunReader>> rs;
        rs.reserve(inputs.size());
        for (const auto& [g, rd] : inputs) rs.push_back(rd);
        auto mc = OkiMergeCursor::open(std::move(rs),
                                       std::vector<OkiMergeCursor::MemSource>{});
        if (!mc) return finish(CompactResult::kFailed);
        DeltaRow row;
        while (true) {
            if (hook && (rows % kHookEveryRows) == 0) {
                hook(lane == Lane::kYoung, rows);
            }
            if ((rows % kCancelCheckRows) == 0 &&
                cancel_.load(std::memory_order_acquire)) {
                return finish(CompactResult::kCancelled);
            }
            auto n = mc->next(row, /*include_tombs=*/!full);
            if (!n) return finish(CompactResult::kFailed);
            if (!*n) break;
            const bool loc = out_ver != kRunVersion && row.has_loc && !row.tomb;
            auto a = w->add(std::span<const std::byte>(
                                reinterpret_cast<const std::byte*>(row.key.data()),
                                row.key.size()),
                            row.ord, row.tomb, loc ? &row.loc : nullptr);
            if (!a) return finish(CompactResult::kFailed);
            ++rows;
            if (row.tomb) ++tombs;
        }
        if (!w->finish(/*fsync_dir=*/true)) return finish(CompactResult::kFailed);
    }
    auto rd = OkiRunReader::open(out_path);
    if (!rd) return finish(CompactResult::kFailed);

    {
        std::lock_guard<std::mutex> flk(flush_mu_);
        if (!loaded_.load(std::memory_order_acquire)) {
            // 期间被 rebuild / 降级：放弃（finish 会再取 flush_mu_，先放锁）。
        } else {
            // 输入必须仍在册且连续；其后每个 run 的 gen 必须大于输出 gen。
            std::size_t pos = readers_.size();
            for (std::size_t i = 0; i < readers_.size(); ++i) {
                if (readers_[i].first == inputs.front().first) {
                    pos = i;
                    break;
                }
            }
            bool ok = pos + inputs.size() <= readers_.size() &&
                      manifest_.runs.size() == readers_.size();
            for (std::size_t k = 0; ok && k < inputs.size(); ++k) {
                ok = readers_[pos + k].first == inputs[k].first &&
                     manifest_.runs[pos + k].gen == inputs[k].first;
            }
            for (std::size_t i = pos + inputs.size(); ok && i < readers_.size();
                 ++i) {
                ok = readers_[i].first > gen;
            }
            // 全归并必须仍从最老 run 开始（否则丢墓碑不安全）。
            if (ok && full) ok = pos == 0;
            if (ok) {
                OkiManifest next = manifest_;
                next.runs.clear();
                next.runs.insert(next.runs.end(), manifest_.runs.begin(),
                                 manifest_.runs.begin() +
                                     static_cast<std::ptrdiff_t>(pos));
                next.runs.push_back(
                    {gen, cover, static_cast<std::uint8_t>(out_ver)});
                next.runs.insert(next.runs.end(),
                                 manifest_.runs.begin() +
                                     static_cast<std::ptrdiff_t>(pos + inputs.size()),
                                 manifest_.runs.end());
                next.level_b = level_b_.load(std::memory_order_acquire);
                if (write_manifest(dir, next)) {
                    RunsVec nr;
                    nr.reserve(next.runs.size());
                    nr.insert(nr.end(), readers_.begin(),
                              readers_.begin() + static_cast<std::ptrdiff_t>(pos));
                    nr.emplace_back(gen, std::make_shared<OkiRunReader>(
                                             *std::move(rd)));
                    nr.insert(nr.end(),
                              readers_.begin() +
                                  static_cast<std::ptrdiff_t>(pos + inputs.size()),
                              readers_.end());
                    manifest_ = std::move(next);
                    readers_ = std::move(nr);
                    publish_view_locked();
                    manifest_level_b_.store(manifest_.level_b,
                                            std::memory_order_release);
                    committed = true;
                    std::vector<std::uint64_t> keep;
                    for (const auto& r : manifest_.runs) keep.push_back(r.gen);
                    sweep_runs(dir, keep, sweep_backlog_, inflight_gens_);
                    std::vector<std::uint64_t> cache_keep = keep;
                    cache_keep.insert(cache_keep.end(), inflight_gens_.begin(),
                                      inflight_gens_.end());
                    block_cache_->purge_except(cache_keep);
                    // 墓碑只在全归并时回收；非全归并输出里墓碑占比过高 →
                    // 下一次主道归并强制全归并。
                    if (full) {
                        force_full_next_.store(false, std::memory_order_relaxed);
                    } else if (rows > 0 && tombs * 4 > rows) {
                        force_full_next_.store(true, std::memory_order_relaxed);
                    }
                }
            }
        }
    }
    if (!committed) return finish(CompactResult::kFailed);
    {
        std::lock_guard<std::mutex> sl(stats_mu_);
        ++stats_.compactions;
        if (full) ++stats_.full_compactions;
        if (lane == Lane::kYoung) ++stats_.young_compactions;
        stats_.rows_written += rows;
        stats_.tomb_rows_written += tombs;
    }
    return finish(CompactResult::kDone);
}

void OkiState::schedule_compaction(std::string_view dir) {
    std::lock_guard<std::mutex> lk(lane_mu_);
    if (cancel_.load(std::memory_order_acquire)) return;
    compact_dir_ = std::string(dir);
    if (!main_running_) {
        if (main_thread_.joinable()) main_thread_.join();  // 已退出，立即返回
        main_running_ = true;
        pending_ = false;
        main_thread_ = std::thread([this] { main_lane_loop(); });
        return;
    }
    pending_ = true;
    // 主道正在一次（可能很大的）归并里：起年轻道收拾期间新 flush 的小 run。
    if (main_busy_ && !young_running_) {
        if (young_thread_.joinable()) young_thread_.join();
        young_running_ = true;
        young_thread_ = std::thread([this] { young_lane_loop(); });
    }
}

void OkiState::main_lane_loop() {
    for (;;) {
        std::string dir;
        {
            std::unique_lock<std::mutex> lk(lane_mu_);
            // 年轻道运行期间主道不开始新一轮（两道输入不得交叠）。
            lane_cv_.wait(lk, [&] {
                return !young_running_ || cancel_.load(std::memory_order_acquire);
            });
            if (cancel_.load(std::memory_order_acquire)) {
                main_running_ = false;
                lane_cv_.notify_all();
                return;
            }
            main_busy_ = true;
            pending_ = false;
            dir = compact_dir_;
        }
        const auto r = compact_once(dir, Lane::kMain);
        std::lock_guard<std::mutex> lk(lane_mu_);
        main_busy_ = false;
        lane_cv_.notify_all();
        if (r == CompactResult::kDone) continue;
        if (r == CompactResult::kNone && pending_) continue;
        main_running_ = false;
        pending_ = false;
        lane_cv_.notify_all();
        return;
    }
}

void OkiState::young_lane_loop() {
    for (;;) {
        std::string dir;
        {
            std::lock_guard<std::mutex> lk(lane_mu_);
            dir = compact_dir_;
        }
        const auto r = compact_once(dir, Lane::kYoung);
        if (r == CompactResult::kDone) continue;
        std::lock_guard<std::mutex> lk(lane_mu_);
        young_running_ = false;
        lane_cv_.notify_all();
        return;
    }
}

// 背压：run 数超过硬上限时等待归并追上；归并已全部停下（无法再推进）即返回。
void OkiState::wait_backpressure() {
    const auto pol = compaction_policy();
    const std::size_t cap = pol.stall_runs != 0 ? pol.stall_runs : 2 * pol.max_runs;
    if (run_count() <= cap) return;
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::unique_lock<std::mutex> lk(lane_mu_);
        while (!cancel_.load(std::memory_order_acquire) &&
               (main_running_ || young_running_)) {
            lane_cv_.wait_for(lk, std::chrono::milliseconds(20));
            lk.unlock();
            const bool done = run_count() <= cap;
            lk.lock();
            if (done) break;
        }
    }
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0)
            .count());
    std::lock_guard<std::mutex> sl(stats_mu_);
    ++stats_.backpressure_waits;
    stats_.backpressure_ns += ns;
}

void OkiState::wait_compaction() {
    std::unique_lock<std::mutex> lk(lane_mu_);
    lane_cv_.wait(lk, [&] { return !main_running_ && !young_running_; });
    // 线程最后一步是在 lane_mu_ 下清标志后返回，此时 join 立即完成。
    if (main_thread_.joinable()) main_thread_.join();
    if (young_thread_.joinable()) young_thread_.join();
}

OkiState::~OkiState() {
    // close 的最后一步。小归并（常见：close 那次 flush 刚调度的）给一个短
    // 宽限期跑完——否则只在 close 时 flush 的短会话永远完不成归并；大归并
    // 超过宽限即取消（每 kCancelCheckRows 行检查一次，半成品输出删除、输入
    // 未动），close 耗时有上界（S44-4）。
    {
        std::unique_lock<std::mutex> lk(lane_mu_);
        lane_cv_.wait_for(lk, std::chrono::milliseconds(500), [&] {
            return !main_running_ && !young_running_;
        });
    }
    cancel_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(lane_mu_);
        lane_cv_.notify_all();
    }
    wait_compaction();
}

// ---------------------------------------------------------------------------
// 重建 / 只读内存视图
// ---------------------------------------------------------------------------

std::uint64_t OkiState::reserve_gen(std::string_view dir) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    // 只登记在途、不抬 gen_floor_：放弃的 gen 从未发布 Reader，可以复用。
    const auto g = next_gen_locked(dir);
    inflight_gens_.push_back(g);
    return g;
}

void OkiState::abandon_gen(std::string_view dir, std::uint64_t gen) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    release_gen_locked(gen);
    std::error_code ec;
    std::filesystem::remove(bitcask::detail::from_utf8(mk_run_filename(dir, gen)), ec);
}

bool OkiState::rebuild(std::string_view dir, std::vector<DeltaRow>&& rows,
                       std::uint64_t cover_ord,
                       std::optional<OkiCommitSnapshot> commit_snapshot) {
    // S33-6：零活 key（空库 / 全删）不落空 run。
    if (rows.empty()) {
        return install_built_run(dir, 0, cover_ord, std::move(commit_snapshot));
    }
    // S36-1：外排构建（64MiB 分段 spill + k 路归并）。S43-1：出 v3。
    const std::uint64_t gen = reserve_gen(dir);
    const auto fail = [&] {
        abandon_gen(dir, gen);
        return false;
    };
    auto b = SpillingRunBuilder::create(std::string(dir), gen, kRunVersion3);
    if (!b) return fail();
    for (const auto& r : rows) {
        auto a = b->add(std::span<const std::byte>(
                            reinterpret_cast<const std::byte*>(r.key.data()),
                            r.key.size()),
                        r.ord, r.tomb, r.has_loc ? &r.loc : nullptr);
        if (!a) return fail();
    }
    if (!b->finish(/*fsync_dir=*/true)) return fail();
    return install_built_run(dir, gen, cover_ord, std::move(commit_snapshot));
}

bool OkiState::install_built_run(
    std::string_view dir, std::uint64_t gen, std::uint64_t cover_ord,
    std::optional<OkiCommitSnapshot> commit_snapshot) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    if (gen != 0) release_gen_locked(gen);
    const auto run_path = gen != 0 ? mk_run_filename(dir, gen) : std::string();
    auto drop_run = [&] {
        if (gen == 0) return;
        std::error_code ec;
        std::filesystem::remove(bitcask::detail::from_utf8(run_path), ec);
    };
    std::shared_ptr<OkiRunReader> new_reader;
    if (gen != 0) {
        auto rd = OkiRunReader::open(run_path);
        if (!rd) {
            drop_run();
            return false;
        }
        new_reader = std::make_shared<OkiRunReader>(*std::move(rd));
    }

    OkiManifest next;
    if (new_reader) next.runs.push_back({gen, cover_ord, /*format_ver=*/3});
    next.wm = cover_ord;
    next.level_b = level_b_.load(std::memory_order_acquire);
    if (commit_snapshot.has_value()) {
        next.metadata = commit_snapshot->metadata;
        next.fstats = std::move(commit_snapshot->fstats);
        if (!valid_commit_snapshot(next, *next.metadata, next.fstats)) {
            drop_run();
            return false;
        }
    }
    if (!write_manifest(dir, next)) {
        drop_run();
        return false;
    }
    {
        std::vector<std::uint64_t> keep;
        if (new_reader) keep.push_back(gen);
        sweep_runs(dir, keep, sweep_backlog_, inflight_gens_);
        std::vector<std::uint64_t> cache_keep = keep;
        cache_keep.insert(cache_keep.end(), inflight_gens_.begin(),
                          inflight_gens_.end());
        block_cache_->purge_except(cache_keep);
    }
    // 新视图覆盖到此刻的全部 active 行：frozen_through 抬到当前最大到达序号，
    // 旧 active 行随即被水位整体作废（读者按 seq 过滤），再物理删除。
    std::uint64_t through = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        through = append_seq_;
    }
    manifest_ = std::move(next);
    readers_.clear();
    if (new_reader) readers_.emplace_back(gen, std::move(new_reader));
    frozen_.clear();
    frozen_through_ = std::max(frozen_through_, through);
    publish_view_locked();
    manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
    wm_.store(manifest_.wm, std::memory_order_release);
    loaded_.store(true, std::memory_order_release);
    drop_active_through(frozen_through_);
    {
        std::lock_guard<std::mutex> vlk(view_mu_);
        sorted_view_.reset();
    }
    return true;
}

void OkiState::install_memory_view(
    std::vector<DeltaRow>&& rows,
    std::uint64_t cover_ord,
    std::optional<OkiCommitSnapshot> commit_snapshot) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    OkiManifest next;
    next.wm = cover_ord;
    next.level_b = level_b_.load(std::memory_order_acquire);
    if (commit_snapshot.has_value()) {
        next.metadata = commit_snapshot->metadata;
        next.fstats = commit_snapshot->fstats;
        if (!valid_commit_snapshot(next, *next.metadata, next.fstats)) return;
    }
    // 只读视图：空 run 集 + 一个冻结段（内存行覆盖全序）。不修改盘面。
    for (auto& r : rows) r.seq = 0;
    auto seg = std::make_shared<const Segment>(sort_dedup(std::move(rows)));
    std::uint64_t through = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        through = append_seq_;
    }
    manifest_ = std::move(next);
    readers_.clear();
    frozen_.clear();
    if (!seg->empty()) frozen_.push_back(std::move(seg));
    frozen_through_ = std::max(frozen_through_, through);
    publish_view_locked();
    manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
    wm_.store(manifest_.wm, std::memory_order_release);
    loaded_.store(true, std::memory_order_release);
    drop_active_through(frozen_through_);
    {
        std::lock_guard<std::mutex> vlk(view_mu_);
        sorted_view_.reset();
    }
}

// ---------------------------------------------------------------------------
// 读：点查与读视图（S44-1：先 active、后视图、按 seq 过滤）
// ---------------------------------------------------------------------------

// S36-2：组合视图点查。查询序：active（seq > frozen_through 才算数）→ 冻结
// 段（新→旧）→ run（gen 降序）。首命中即权威——依赖不变量「行只随时间进入
// 更新的来源」（flush 按时序冻结、run 按 gen、归并输出取最大 gen）。
OkiState::LocateResult OkiState::locate(std::string_view key) const {
    LocateResult out;
    if (!point_query_.load(std::memory_order_acquire)) return out;
    if (!loaded_.load(std::memory_order_acquire)) return out;

    const auto hit = [&](const auto& r) {
        out.status = LocateStatus::kHit;
        out.ord = r.ord;
        out.tomb = r.tomb;
        out.has_loc = r.has_loc;
        out.loc = r.loc;
        return out;
    };

    // 1) active：必须先于视图读取（反过来会漏掉恰在两步之间被冻结的行）。
    bool have_active = false;
    DeltaRow active;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = delta_idx_.find(key);
        if (it != delta_idx_.end()) {
            const DeltaRow& r = delta_[it->second];
            active.ord = r.ord;
            active.tomb = r.tomb;
            active.has_loc = r.has_loc;
            active.loc = r.loc;
            active.seq = r.seq;
            have_active = true;
        }
    }
    const auto view = view_.load();
    if (!view) return out;  // kUnavailable（loaded 但视图未发布——不可达兜底）
    if (have_active && active.seq > view->frozen_through) return hit(active);

    // 2) 冻结段（新→旧）。
    for (auto it = view->frozen.rbegin(); it != view->frozen.rend(); ++it) {
        if (const DeltaRow* r = segment_find(**it, key)) return hit(*r);
    }

    // 3) run（gen 降序）：bloom 试探 → seek 二分 → 首行比对。哈希只算一次。
    const std::span<const std::byte> kspan(
        reinterpret_cast<const std::byte*>(key.data()), key.size());
    const auto bh = OkiRunReader::bloom_hash(kspan);
    for (auto it = view->runs.rbegin(); it != view->runs.rend(); ++it) {
        const OkiRunReader& rd = *it->second;
        if (rd.version() == kRunVersion) {
            // v1 run：无位置字段也无 bloom——组合视图对该 key 无点查能力。
            return LocateResult{};
        }
        if (!rd.may_contain_hashed(bh)) continue;
        auto e = rd.find(kspan, block_cache_.get(), it->first);
        if (!e) return LocateResult{};  // IO/损坏 → kUnavailable
        if (e->has_value()) return hit(**e);
    }
    out.status = LocateStatus::kMiss;
    return out;
}

std::optional<OkiState::ReadView> OkiState::make_read_view() const {
    // 不持 flush_mu_（flush/归并/重建持该锁横跨长 IO）。成对一致由构造保证：
    // runs 与冻结段来自同一个已发布视图；active 先取、视图后取，丢弃
    // seq ≤ frozen_through 的 active 行（已归入视图）。
    if (!loaded_.load(std::memory_order_acquire)) return std::nullopt;

    // 1) active 排序视图（缓存命中零拷贝；失配则锁内拷贝、锁外排序）。
    std::shared_ptr<const std::vector<DeltaRow>> sorted;
    std::uint64_t sorted_min_seq = 0;
    std::shared_ptr<std::vector<DeltaRow>> fresh;
    std::uint64_t want = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        {
            std::lock_guard<std::mutex> vlk(view_mu_);
            if (sorted_view_ && sorted_view_version_ == delta_version_) {
                sorted = sorted_view_;
                sorted_min_seq = sorted_view_min_seq_;
            }
        }
        if (!sorted) {
            fresh = std::make_shared<std::vector<DeltaRow>>(delta_);
            want = delta_version_;
        }
    }
    // 2) 视图。
    const auto view = view_.load();
    if (!view) return std::nullopt;

    if (fresh) {
        std::uint64_t min_seq = std::numeric_limits<std::uint64_t>::max();
        for (const auto& r : *fresh) min_seq = std::min(min_seq, r.seq);
        *fresh = sort_dedup(std::move(*fresh));
        {
            std::lock_guard<std::mutex> lk(mu_);
            std::lock_guard<std::mutex> vlk(view_mu_);
            if (delta_version_ == want) {
                sorted_view_ = fresh;
                sorted_view_version_ = want;
                sorted_view_min_seq_ = min_seq;
            }
        }
        sorted = fresh;
        sorted_min_seq = min_seq;
    }

    ReadView v;
    v.runs.reserve(view->runs.size());
    v.gens.reserve(view->runs.size());
    for (const auto& [gen, rd] : view->runs) {
        v.runs.push_back(rd);
        v.gens.push_back(gen);
    }
    v.cache = block_cache_;
    v.frozen = view->frozen;
    // 3) seq 过滤。胜出行 seq ≤ frozen_through ⟹ 该 key 的权威在视图里
    //    （冻结段恰持有它），整行剔除。常见情形缓存内全部行都新于水位，零拷贝。
    if (sorted->empty() || sorted_min_seq > view->frozen_through) {
        v.delta = std::move(sorted);
    } else {
        auto filtered = std::make_shared<std::vector<DeltaRow>>();
        filtered->reserve(sorted->size());
        for (const auto& r : *sorted) {
            if (r.seq > view->frozen_through) filtered->push_back(r);
        }
        v.delta = std::move(filtered);
    }
    return v;
}

std::size_t OkiState::delta_rows() const {
    // 尚未落盘的行：active + 待落盘冻结段。
    std::size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        n = delta_.size();
    }
    if (const auto view = view_.load()) {
        for (const auto& seg : view->frozen) n += seg->size();
    }
    return n;
}

std::size_t OkiState::delta_bytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return delta_bytes_;
}

std::size_t OkiState::run_count() const {
    std::lock_guard<std::mutex> lk(flush_mu_);
    return manifest_.runs.size();
}

}  // namespace bitcask::oki
