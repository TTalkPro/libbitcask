#include "bitcask/oki_state.hpp"

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

void OkiState::append(std::string_view key, std::uint64_t ord, bool tomb,
                      const RowLoc* loc) {
    // 水位门（wm = 排他上界 = 尚未覆盖的最小 ord；首个合法 LSN 是 0，
    // 故不能有 ord==0 特判）：tail 重放只收 wm 起的行。
    if (ord < wm_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(mu_);
    delta_bytes_ += key.size() + sizeof(DeltaRow);
    delta_.push_back(DeltaRow{std::string(key), ord, tomb,
                              /*has_loc=*/loc != nullptr,
                              loc != nullptr ? *loc : RowLoc{}});
    delta_.back().seq = ++append_seq_;
    ++delta_version_;  // 排序视图缓存失效
    index_row_locked(delta_.size() - 1);
    update_flush_hint_locked();
}

void OkiState::append_update(std::string_view key, std::uint64_t ord,
                             bool tomb, const RowLoc* loc,
                             std::uint32_t dur_file, std::uint64_t dur_end) {
    // 免水位门（旧 ord、新信息——merge 搬迁 / TTL 墓碑，见头文件）。
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

void OkiState::index_row_locked(std::size_t idx) {
    if (!point_query_.load(std::memory_order_relaxed)) return;
    const DeltaRow& row = delta_[idx];
    auto [it, inserted] = delta_idx_.try_emplace(row.key, idx);
    // (ord, 到达序) 胜出：等 ord 后到者顶替（搬迁行），更小 ord 不顶替
    // （恢复期并行 fold 的乱序到达）。
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

// S43-5：写前探测是 locate 的显式语义别名。实现暂共用同一条组合视图；
// 后续把 memdelta 辅助哈希提升为写路径专用热缓存时，读视图的替换策略
// 不会再反向影响写 CAS 的可用性判定。
OkiState::WriteProbe OkiState::probe_for_write(std::string_view key) const {
    const auto r = locate(key);
    return WriteProbe{r.status, r.ord, r.tomb, r.has_loc, r.loc};
}

bool OkiState::write_probe_ready() const noexcept {
    if (!point_query_.load(std::memory_order_acquire) ||
        !loaded_.load(std::memory_order_acquire)) {
        return false;
    }
    const auto runs = runs_snap_.load();
    if (!runs) return false;
    return std::none_of(
        runs->begin(), runs->end(),
        [](const auto& run) {
            return run.second->version() == kRunVersion;
        });
}

// 运行期应用一律绕过 wm 门：调用方（IndexAuthority）已在写序闸门内做过
// probe + CAS 裁决，行必然是该 key 的最新态。wm 门只服务恢复期的 tail
// 重放；用在运行期会误丢在途写——例如运行期重建把 wm 设为 next_ord（含已
// 分配、尚未应用的 ord），重建发布后这条写被 `ord < wm` 静默丢弃，账本却
// 已计入（data 在、索引无）。
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

// S43-1：坏块运行期降级。先关点查，再置 unloaded；顺序保证不会出现
// 「点查仍开但视图已消失」的窗口。flush_mu_ 与 flush/compact/rebuild
// 串行，避免降级动作插在半途提交之后造成状态回退。
void OkiState::degrade_for_rebuild() noexcept {
    std::lock_guard<std::mutex> flk(flush_mu_);
    point_query_.store(false, std::memory_order_release);
    loaded_.store(false, std::memory_order_release);
}

void OkiState::publish_runs_locked() {
    runs_snap_.emplace(readers_);
}

void OkiState::load(std::string_view dir) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    auto m = read_manifest(dir);
    if (!m) return;  // 缺失/损坏 → 未加载态（caller 决定重建）
    // S33-5：随 load 打开全部 run Reader（全量 CRC eager 校验——S33-3 既定
    // 取舍：派生缓存安全优先）。任一 run 坏 → 整体弃用（未加载态 → 重建）。
    RunsVec rds;
    rds.reserve(m->runs.size());
    for (const auto& r : m->runs) {
        auto rd = OkiRunReader::open(mk_run_filename(dir, r.gen));
        if (!rd) return;  // 未加载态
        rds.emplace_back(r.gen,
                         std::make_shared<OkiRunReader>(*std::move(rd)));
    }
    manifest_ = *std::move(m);
    readers_ = std::move(rds);
    publish_runs_locked();
    for (const auto& r : manifest_.runs) gen_floor_ = std::max(gen_floor_, r.gen);
    manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
    wm_.store(manifest_.wm, std::memory_order_release);
    loaded_.store(true, std::memory_order_release);
}

// S36-4：降级戳（level_b 关而盘上戳开 → 改写 manifest 清戳）。升级不走
// 此路——必须经全量 rebuild（见头文件注释）。best-effort：写失败保持原
// 状（戳仍在，Level B 下次 open 会做一次多余重建，方向安全）。
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

bool OkiState::flush(
    std::string_view dir,
    std::span<const std::pair<std::uint32_t, std::uint64_t>> durable_wms,
    std::optional<OkiCommitSnapshot> commit_snapshot,
    bool async_compaction) {
    bool need_compact = false;
    const bool ok = flush_locked_impl(dir, durable_wms,
                                      std::move(commit_snapshot),
                                      need_compact);
    // S33-6：run 数超阈值 → 全归并（设计 §5.2）。归并的 IO 不再持
    // flush_mu_（原实现在 flush 锁内同步全归并：写线程阈值 flush 持着
    // write_mu_ 陪等整个索引的顺序重写，range/merge 视图构建也被挡住）。
    // best-effort——失败不影响本次 flush 的成功语义。
    if (ok && need_compact) {
        if (async_compaction) {
            schedule_compaction(dir);
        } else {
            (void)compact_runs(dir);
        }
    }
    return ok;
}

bool OkiState::flush_locked_impl(
    std::string_view dir,
    std::span<const std::pair<std::uint32_t, std::uint64_t>> durable_wms,
    std::optional<OkiCommitSnapshot> commit_snapshot, bool& need_compact) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    if (!loaded_.load(std::memory_order_relaxed)) return false;

    // S43-2：Level A / 空 memdelta 也是合法提交点。尤其 v3→v4 迁移目录
    // 可能没有任何待固化 delta，但仍要把刚捕获的标量/fstats 随 manifest
    // 原子发布；否则“空 flush”会静默放弃迁移。
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
            next.level_b == manifest_level_b_.load(
                std::memory_order_acquire)) {
            return true;
        }
        if (!write_manifest(dir, next)) return false;
        manifest_ = std::move(next);
        manifest_level_b_.store(manifest_.level_b,
                                std::memory_order_release);
        return true;
    };

    // S36-2：**拷贝前缀**而非换出——IO 期间行仍留在 delta_（locate 可见，
    // 组合视图无「在写盘路上不可见」的窗口），提交后才删前缀。失败路径
    // 因此零恢复动作（原 swap+restore 退役）。代价是一次前缀拷贝——flush
    // 本就伴随全量排序 + 文件 IO，量级淹没。
    std::vector<DeltaRow> all;
    {
        std::lock_guard<std::mutex> lk(mu_);
        all.assign(delta_.begin(), delta_.end());
    }
    if (all.empty()) return commit_metadata_only();
    const std::size_t prefix_n = all.size();

    // S36-5 B1：持久性分拣——引用了未持久字节的行持留在 delta（语义见头
    // 文件）。修订：
    //   1) 按 key 分组后只对胜出行判定（被同 key 新行覆盖的旧行随组走），
    //      避免指向已退役文件的陈旧行永久滞留；
    //   2) 墓碑也参与判定（dur_file/dur_end）——否则未持久的墓碑会先进 run；
    //   3) 带快照时以一致切面（cut_seq）划界：切面之后到达的行一律持留，
    //      切面之内的行必须全部可固化——metadata 与 run 覆盖必须一致，否则
    //      崩溃重放会把行当「已在 run」跳过记账，或重复记账；
    //   4) wm 不越过任何持留行的 ord（纵深防御；恢复重放已改为与组合视图
    //      比对而非单靠 wm 门，见 IndexAuthority::recover_row）。
    std::uint32_t max_snap_fid = 0;
    for (const auto& [fid, wm] : durable_wms) max_snap_fid = std::max(max_snap_fid, fid);
    const auto beyond_durable = [&](std::uint32_t fid, std::uint64_t end) {
        if (durable_wms.empty() || fid == 0) return false;
        for (const auto& [f, wm] : durable_wms) {
            if (f == fid) return end > wm;
        }
        // 快照之后新建的文件（id 更大）尚无持久保证；更小 id 的缺席文件
        // 是已退役/删除的旧文件，其行必然已被覆盖，不持留。
        return fid > max_snap_fid;
    };
    const auto row_beyond = [&](const DeltaRow& r) {
        if (r.has_loc) return beyond_durable(r.loc.file_id, r.loc.offset + r.loc.total_sz);
        if (r.tomb) return beyond_durable(r.dur_file, r.dur_end);
        return false;
    };
    const bool with_snapshot = commit_snapshot.has_value();
    const std::uint64_t cut_seq =
        with_snapshot ? commit_snapshot->cut_seq
                      : std::numeric_limits<std::uint64_t>::max();

    // (key, ord, 到达序) 排序的下标序；只有 seq ≤ cut 的行参与本次固化。
    std::vector<std::size_t> order;
    order.reserve(prefix_n);
    std::vector<char> held(prefix_n, 0);
    std::uint64_t min_held_ord = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t i = 0; i < prefix_n; ++i) {
        if (all[i].seq > cut_seq) {  // 快照之后到达：不在账本里
            held[i] = 1;
            min_held_ord = std::min(min_held_ord, all[i].ord);
        } else {
            order.push_back(i);
        }
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) {
                         if (all[a].key != all[b].key) return all[a].key < all[b].key;
                         return all[a].ord < all[b].ord;
                     });
    std::vector<DeltaRow> rows;  // key 升序、每 key 一行（胜出者）
    rows.reserve(order.size());
    std::size_t flushed_bytes = 0;
    for (std::size_t gb = 0; gb < order.size();) {
        std::size_t ge = gb + 1;
        while (ge < order.size() && all[order[ge]].key == all[order[gb]].key) ++ge;
        const DeltaRow& win = all[order[ge - 1]];  // (ord, 到达序) 最大
        if (row_beyond(win)) {
            // 已计入快照账本的行却引用未持久字节：metadata 与 run 无法同时
            // 自洽（崩溃重放会重复记账）。带快照时放弃本轮（delta 原状，下
            // 次提交重试）；无快照的旧路径维持「整组持留」。
            if (with_snapshot) return false;
            for (std::size_t j = gb; j < ge; ++j) held[order[j]] = 1;
            min_held_ord = std::min(min_held_ord, win.ord);
        } else {
            for (std::size_t j = gb; j < ge; ++j) {
                flushed_bytes += all[order[j]].key.size() + sizeof(DeltaRow);
            }
            rows.push_back(win);
        }
        gb = ge;
    }
    all.clear();
    // 全持留：本轮无新 run；metadata 快照仍是一致切面（持留行都在切面后）。
    if (rows.empty()) return commit_metadata_only();

    std::uint64_t cover = 0;  // 排他上界 = 本批最大 ord + 1
    for (const auto& r : rows) cover = std::max(cover, r.ord + 1);
    const std::uint64_t uniq = rows.size();

    // S43-1：新 run 落 v3（全字段 + bloom + per-block CRC）——组合视图
    // 点查的能力前提；开库不再整读 data 索引文件。
    const std::uint64_t gen = next_gen_locked(dir);
    auto w = OkiRunWriter::create(mk_run_filename(dir, gen),
                                  kDefaultBlockBytes, kRunVersion3,
                                  /*expected_entries=*/uniq);
    if (!w) return false;
    bool io_ok = true;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        auto a = w->add(std::span<const std::byte>(
                            reinterpret_cast<const std::byte*>(r.key.data()),
                            r.key.size()),
                        r.ord, r.tomb, r.has_loc ? &r.loc : nullptr);
        if (!a) { io_ok = false; break; }
    }
    if (io_ok && !w->finish(/*fsync_dir=*/true)) io_ok = false;
    if (!io_ok) return false;

    // S33-5：manifest 提交前先开 Reader（刚写完页缓存热，CRC 校验便宜；
    // 开失败按 IO 失败处理，不留半态）。
    auto rd = OkiRunReader::open(mk_run_filename(dir, gen));
    if (!rd) {
        std::error_code ec;
        std::filesystem::remove(bitcask::detail::from_utf8(mk_run_filename(dir, gen)), ec);
        return false;
    }

    // manifest 提交（唯一 commit point）。失败则删刚写的 run（delta_ 未动）。
    OkiManifest next = manifest_;
    // S43-3：本次 caller 持久水位和 run/metadata 同为一个提交点。空水位
    // 表示 caller 没有新落点（如测试直调）；此时保留上一份 data 水位。
    if (!durable_wms.empty()) {
        next.data_wms.assign(durable_wms.begin(), durable_wms.end());
    }
    next.runs.push_back({gen, cover, /*format_ver=*/3});
    next.wm = std::max(next.wm, std::min(cover, min_held_ord));
    next.level_b = level_b_.load(std::memory_order_acquire);  // S36-4 模式戳
    if (commit_snapshot.has_value()) {
        // v4 是随本次 run 一起过 CRC 的提交快照。校验放在 run fsync 后、
        // manifest 发布前：非法 caller 快照只浪费一次 run，不会发布超前
        // 标量。无快照时保留上一份 v4 元数据（现有 v4 目录的普通 flush
        // 可继续），v3 目录则保持 nullopt 等真实迁移点。
        next.metadata = commit_snapshot->metadata;
        next.fstats = std::move(commit_snapshot->fstats);
        if (!valid_commit_snapshot(next, *next.metadata, next.fstats)) {
            std::error_code ec;
            std::filesystem::remove(
                bitcask::detail::from_utf8(mk_run_filename(dir, gen)), ec);
            return false;
        }
    }
    if (!write_manifest(dir, next)) {
        std::error_code ec;
        std::filesystem::remove(bitcask::detail::from_utf8(mk_run_filename(dir, gen)), ec);
        return false;
    }
    // 「发布新 run + 裁 delta 前缀」对读视图必须成对可见（view_gate_ 写
    // 窗口）。只靠「先取 delta、后取 runs」的顺序能保证不缺行，但会把等 ord
    // 的两行判反：读者手里的旧 delta 含原始行，新 run 含同 ord 的搬迁行 /
    // 条件墓碑，归并时 delta 胜 ⟹ 旧位置压过新位置（输入文件删除后 fold
    // 报 location disappeared）或已删除的 key 被当成活的。
    {
    const auto view_window = view_gate_.write();
    manifest_ = std::move(next);
    readers_.emplace_back(gen, std::make_shared<OkiRunReader>(*std::move(rd)));
    publish_runs_locked();
    for (const auto& r : manifest_.runs) gen_floor_ = std::max(gen_floor_, r.gen);
    manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
    wm_.store(manifest_.wm, std::memory_order_release);

    // 提交完成，删已固化的前缀。IO 期间新 append 的行在前缀之后，保留；
    // S36-5：持留行（held）留在队头，相对序不变（(ord, 到达序) 格保持）。
    {
        std::lock_guard<std::mutex> lk(mu_);
        // 命名为 kept 而非 next：本函数上方已有一个 OkiManifest next（下一份
        // manifest），同名会 shadow 它（-Wshadow，CI 的 werror-lib 带 -Werror）。
        // 且这里装的确实是「留下来的行」——前缀里被持留的 + 前缀之后新追加的。
        std::vector<DeltaRow> kept;
        kept.reserve(prefix_n + delta_.size() - prefix_n);
        for (std::size_t i = 0; i < prefix_n; ++i) {
            if (held[i] != 0) kept.push_back(std::move(delta_[i]));
        }
        for (std::size_t j = prefix_n; j < delta_.size(); ++j) {
            kept.push_back(std::move(delta_[j]));
        }
        delta_ = std::move(kept);
        delta_bytes_ -= std::min(delta_bytes_, flushed_bytes);
        ++delta_version_;  // 前缀已固化——缓存里的行已在 run 中，立即释放
        rebuild_index_locked();
        update_flush_hint_locked();
    }
    }  // view_window
    {
        std::lock_guard<std::mutex> vlk(view_mu_);
        sorted_view_.reset();
    }

    need_compact = manifest_.runs.size() > kCompactRunLimit;
    return true;
}

// 全归并：把**开始时刻**的全部 run 归并成单个新 run（结果落盘、墓碑丢弃）。
// 三段式，IO 段不持 flush_mu_：
//   1) 锁内快照输入 run 集合、预留输出 gen（gen_floor_ 抬高，之后的 flush
//      必取更大 gen——「行只随时间进入更高 gen」不变量保持）；
//   2) 锁外用 OkiMergeCursor 归并写新 run；
//   3) 锁内校验输入仍全部在册（期间若有 rebuild 替换了 run 集合则放弃），
//      用「新 run + 期间新增的更高 gen run」替换，元数据/水位原样沿用。
// 墓碑丢弃仍成立：输入是开始时刻的**全部** run，同 key 的旧 put 必在其中；
// 期间新增的 run 只含更新的行，gen 更高，裁决不受影响。
bool OkiState::compact_runs(std::string_view dir) {
    std::lock_guard<std::mutex> one(compact_mu_);  // 同一时刻至多一个归并
    RunsVec inputs;
    std::uint64_t cover = 0;
    std::uint64_t gen = 0;
    std::uint32_t out_ver = kRunVersion3;
    std::uint64_t est_entries = 0;
    {
        std::lock_guard<std::mutex> flk(flush_mu_);
        if (!loaded_.load(std::memory_order_acquire) || readers_.size() < 2) {
            return true;
        }
        inputs = readers_;
        for (const auto& r : manifest_.runs) cover = std::max(cover, r.cover_ord);
        // S43-1：全 v2/v3 输入才出 v3（v1 行无位置字段，混入会产出无位置
        // 活行毒化点查）；含 v1 则整体降 v1，待 rebuild 淘汰后自愈。
        for (const auto& [g, rd] : inputs) {
            if (rd->version() == kRunVersion) out_ver = kRunVersion;
            est_entries += rd->entry_count();
        }
        gen = next_gen_locked(dir);
        inflight_gens_.push_back(gen);  // 预留：期间的 flush 必取更大 gen
    }
    // 任何退出路径都撤销在途登记（提交路径在锁内先撤销）。
    struct Release {
        OkiState* self;
        std::uint64_t g;
        bool armed = true;
        ~Release() {
            if (!armed) return;
            std::lock_guard<std::mutex> flk(self->flush_mu_);
            self->release_gen_locked(g);
        }
    } release{this, gen};

    const auto out_path = mk_run_filename(dir, gen);
    auto abort = [&] {
        std::error_code ec;
        std::filesystem::remove(bitcask::detail::from_utf8(out_path), ec);
        return false;
    };
    auto w = OkiRunWriter::create(out_path, kDefaultBlockBytes, out_ver,
                                  /*expected_entries=*/est_entries);
    if (!w) return false;
    {
        std::vector<std::shared_ptr<OkiRunReader>> rs;
        rs.reserve(inputs.size());
        for (const auto& [g, rd] : inputs) rs.push_back(rd);
        auto mc = OkiMergeCursor::open(std::move(rs), nullptr);
        if (!mc) return abort();
        DeltaRow row;
        while (true) {
            auto n = mc->next(row);  // 墓碑在归并层抵消 ⟹ 真正丢弃
            if (!n) return abort();
            if (!*n) break;
            auto a = w->add(std::span<const std::byte>(
                                reinterpret_cast<const std::byte*>(row.key.data()),
                                row.key.size()),
                            row.ord, /*tomb=*/false,
                            (out_ver != kRunVersion && row.has_loc) ? &row.loc
                                                                     : nullptr);
            if (!a) return abort();
        }
    }
    if (!w->finish(/*fsync_dir=*/true)) return abort();
    auto rd = OkiRunReader::open(out_path);
    if (!rd) return abort();

    std::lock_guard<std::mutex> flk(flush_mu_);
    release.armed = false;
    release_gen_locked(gen);
    if (!loaded_.load(std::memory_order_acquire)) return abort();
    // 输入必须仍全部在册，且其余 run 的 gen 都高于输出（期间 flush 产出）。
    RunsVec kept_newer;
    std::size_t matched = 0;
    for (const auto& [g, r] : readers_) {
        const bool is_input = std::any_of(
            inputs.begin(), inputs.end(),
            [&](const auto& in) { return in.first == g; });
        if (is_input) {
            ++matched;
        } else if (g > gen) {
            kept_newer.emplace_back(g, r);
        } else {
            return abort();
        }
    }
    if (matched != inputs.size()) return abort();

    // 从当前 manifest 派生：元数据 / fstats / data 水位 / wm 原样保留，
    // 只替换 run 集合（原实现用空 manifest 提交，三者被清零）。
    OkiManifest next = manifest_;
    next.runs.clear();
    next.runs.push_back({gen, cover, static_cast<std::uint8_t>(out_ver)});
    for (const auto& e : manifest_.runs) {
        if (e.gen > gen) next.runs.push_back(e);
    }
    next.level_b = level_b_.load(std::memory_order_acquire);  // S36-4
    if (!write_manifest(dir, next)) return abort();

    manifest_ = std::move(next);
    readers_.clear();
    readers_.emplace_back(gen, std::make_shared<OkiRunReader>(*std::move(rd)));
    for (auto& e : kept_newer) readers_.push_back(std::move(e));
    publish_runs_locked();
    for (const auto& r : manifest_.runs) gen_floor_ = std::max(gen_floor_, r.gen);
    manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
    // 旧 run + 崩溃残留孤儿一并清（在途 ReadView 持 shared_ptr，unlink 后
    // fd 仍可读——安全）。
    std::vector<std::uint64_t> keep_gen;
    for (const auto& r : manifest_.runs) keep_gen.push_back(r.gen);
    sweep_runs(dir, keep_gen, sweep_backlog_, inflight_gens_);
    block_cache_.purge_except(keep_gen);  // S36-3：死 gen 块不占缓存
    return true;
}

void OkiState::schedule_compaction(std::string_view dir) {
    std::lock_guard<std::mutex> lk(compact_thread_mu_);
    if (compacting_.exchange(true, std::memory_order_acq_rel)) return;
    // 上一个后台线程已跑完（compacting_ 在其最后一步才清），join 立即返回。
    if (compact_thread_.joinable()) compact_thread_.join();
    compact_thread_ = std::thread([this, d = std::string(dir)] {
        // 归并期间到达的 flush 不会另起归并（compacting_ 为真），因此收尾时
        // 复查 run 数并续跑；清标志后再复查一次，堵住「flush 恰在清标志前
        // 判定并放弃调度」的窗口。归并失败（IO）则停，等下次 flush 再触发。
        for (;;) {
            const bool ok = compact_runs(d);
            if (ok && run_count() > kCompactRunLimit) continue;
            compacting_.store(false, std::memory_order_release);
            if (ok && run_count() > kCompactRunLimit &&
                !compacting_.exchange(true, std::memory_order_acq_rel)) {
                continue;
            }
            break;
        }
    });
}

void OkiState::wait_compaction() {
    std::lock_guard<std::mutex> lk(compact_thread_mu_);
    if (compact_thread_.joinable()) compact_thread_.join();
}

OkiState::~OkiState() { wait_compaction(); }

std::uint64_t OkiState::reserve_gen(std::string_view dir) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    // 只登记在途、不抬 gen_floor_：放弃的 gen 从未发布 Reader（块缓存里
    // 没有它的块），可以复用；在途期间 next_gen_locked 会越过它。
    const auto g = next_gen_locked(dir);
    inflight_gens_.push_back(g);
    return g;
}

void OkiState::release_gen_locked(std::uint64_t gen) {
    std::erase(inflight_gens_, gen);
}

bool OkiState::rebuild(std::string_view dir, std::vector<DeltaRow>&& rows,
                       std::uint64_t cover_ord,
                       std::optional<OkiCommitSnapshot> commit_snapshot) {
    // S33-6：零活 key（空库 / 全删）**不落空 run**——「0 个 run + wm =
    // cover_ord」已完整表达语义，且不留空文件与常驻 fd。
    if (rows.empty()) {
        return install_built_run(dir, 0, cover_ord, std::move(commit_snapshot));
    }
    // S36-1：外排构建（64MiB 分段 spill + k 路归并）——排序工作集
    // O(spill_bytes)。S43-1：出 v3（全字段 + bloom + per-block CRC）。
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

void OkiState::abandon_gen(std::string_view dir, std::uint64_t gen) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    release_gen_locked(gen);
    std::error_code ec;
    std::filesystem::remove(bitcask::detail::from_utf8(mk_run_filename(dir, gen)), ec);
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
        auto rd = OkiRunReader::open(run_path);  // S33-5
        if (!rd) {
            drop_run();
            return false;
        }
        new_reader = std::make_shared<OkiRunReader>(*std::move(rd));
    }

    OkiManifest next;
    if (new_reader) next.runs.push_back({gen, cover_ord, /*format_ver=*/3});
    next.wm = cover_ord;
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

    // 提交后清理：目录内一切非本次 run 的 seg 文件 + memdelta（其内容已被
    // 全量快照覆盖）。用目录扫描而非「遍历旧 manifest」——见 sweep_runs 注释
    // （manifest 丢失正是重建的触发场景，那批 run 不在内存里）。
    {
        std::vector<std::uint64_t> keep;
        if (new_reader) keep.push_back(gen);
        sweep_runs(dir, keep, sweep_backlog_, inflight_gens_);
        block_cache_.purge_except(keep);  // S36-3：死 gen 块不占缓存
    }
    // runs 与 delta 一起替换：读视图经 view_gate_ 检出这一窗口并重试。
    {
    const auto view_window = view_gate_.write();
    manifest_ = std::move(next);
    readers_.clear();
    if (new_reader) readers_.emplace_back(gen, std::move(new_reader));
    publish_runs_locked();
    for (const auto& r : manifest_.runs) gen_floor_ = std::max(gen_floor_, r.gen);
    manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
    wm_.store(manifest_.wm, std::memory_order_release);
    loaded_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(mu_);
        delta_.clear();
        delta_bytes_ = 0;
        ++delta_version_;
        rebuild_index_locked();
        update_flush_hint_locked();
    }
    }  // view_window
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

    // 只读视图仍要有明确提交边界：空 run 集 + wm 表达“内存行覆盖全序”。
    // 旧 Reader 只经 shared_ptr 存活到在途视图结束，本进程不修改盘面。
    {
    const auto view_window = view_gate_.write();
    manifest_ = std::move(next);
    readers_.clear();
    publish_runs_locked();
    for (const auto& r : manifest_.runs) gen_floor_ = std::max(gen_floor_, r.gen);
    manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
    wm_.store(manifest_.wm, std::memory_order_release);
    loaded_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(mu_);
        delta_ = std::move(rows);
        delta_bytes_ = 0;
        for (const auto& row : delta_) {
            delta_bytes_ += row.key.size() + sizeof(DeltaRow);
        }
        ++delta_version_;
        rebuild_index_locked();
        update_flush_hint_locked();
    }
    }  // view_window
    {
        std::lock_guard<std::mutex> vlk(view_mu_);
        sorted_view_.reset();
    }
}

// S36-2：组合视图点查（冷侧——不含哈希 keydir；设计 §5.1 步骤 2-4）。
OkiState::LocateResult OkiState::locate(std::string_view key) const {
    LocateResult out;
    if (!point_query_.load(std::memory_order_acquire)) return out;
    if (!loaded_.load(std::memory_order_acquire)) return out;

    // 1) memdelta：辅助哈希直查（存的即 (ord, 到达序) 胜出行）。delta 命中
    //    短路——同 key 的 delta 行恒不旧于任何 run 行（行只随时间进入更高
    //    gen；delta 视作 gen=∞，等 ord 时 delta 胜）。
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = delta_idx_.find(key);
        if (it != delta_idx_.end()) {
            const DeltaRow& r = delta_[it->second];
            out.status = LocateStatus::kHit;
            out.ord = r.ord;
            out.tomb = r.tomb;
            out.has_loc = r.has_loc;
            out.loc = r.loc;
            return out;
        }
    }

    // 2) 逐 run gen 降序（readers_ 恒按 gen 升序维护）：bloom 试探 →
    //    seek 二分 → 首行比对。首命中即权威（不变量见头文件）。
    const auto runs = runs_snap_.load();
    if (!runs) return out;  // kUnavailable（loaded 但快照未发布——不可达兜底）
    const std::span<const std::byte> kspan(
        reinterpret_cast<const std::byte*>(key.data()), key.size());
    for (auto it = runs->rbegin(); it != runs->rend(); ++it) {
        const OkiRunReader& rd = *it->second;
        if (rd.version() == kRunVersion) {
            // v1 run：无位置字段也无 bloom——排除不了 key 也定位不了。
            // 组合视图对该 key 无点查能力（kUnavailable，待重建自愈）。
            return LocateResult{};
        }
        if (!rd.may_contain(kspan)) continue;
        // S36-3：块经 LRU 缓存（命中零 IO；miss 1 次 4KiB pread）。
        auto e = rd.find(kspan, &block_cache_, it->first);
        if (!e) return LocateResult{};  // IO/损坏 → kUnavailable
        if (e->has_value()) {
            out.status = LocateStatus::kHit;
            out.ord = (*e)->ord;
            out.tomb = (*e)->tomb;
            out.has_loc = (*e)->has_loc;
            out.loc = (*e)->loc;
            return out;
        }
        // 缺席（bloom 假阳性）→ 更旧 run。
    }
    out.status = LocateStatus::kMiss;
    return out;
}

std::optional<OkiState::ReadView> OkiState::make_read_view() const {
    // 不再取 flush_mu_：flush/归并/重建持该锁横跨长 IO，原实现让每次
    // range/迭代/merge 视图构建都陪等。一致性论证：
    //   - 凡是同时改动 runs 与 delta 的提交（flush 的「发布 run + 裁前缀」、
    //     rebuild、install_memory_view）都在 view_gate_ 的写窗口里；这里经
    //     view_gate_.read 取 (delta, runs)，跨了窗口自动重取——二者恒属同一
    //     提交点之间的状态，等 ord 裁决（delta 胜）才成立；
    //   - 归并只换 runs、不动 delta，且新旧 run 集合内容等价（墓碑丢弃只
    //     发生在含最老 run 的归并里），与任一时刻的 delta 组合都自洽，
    //     不需要进窗口。
    // 排序视图缓存（sorted_view_）复用逻辑不变：命中只做 shared_ptr 拷贝。
    struct Pair {
        bool loaded = false;
        std::shared_ptr<const std::vector<DeltaRow>> snap;   // 缓存命中
        std::shared_ptr<std::vector<DeltaRow>> fresh;        // 待排序的新拷贝
        std::uint64_t want = 0;
        decltype(runs_snap_.load()) runs;
    };
    {
        Pair p = view_gate_.read([&] {
            Pair out;
            out.loaded = loaded_.load(std::memory_order_acquire);
            if (!out.loaded) return out;
            {
                std::lock_guard<std::mutex> lk(mu_);
                {
                    std::lock_guard<std::mutex> vlk(view_mu_);
                    if (sorted_view_ && sorted_view_version_ == delta_version_) {
                        out.snap = sorted_view_;
                    }
                }
                if (!out.snap) {
                    out.fresh = std::make_shared<std::vector<DeltaRow>>(delta_);
                    out.want = delta_version_;
                }
            }
            out.runs = runs_snap_.load();
            return out;
        });
        if (!p.loaded) return std::nullopt;
        auto& snap = p.snap;
        auto& fresh = p.fresh;
        const std::uint64_t want = p.want;
        const auto& runs = p.runs;

        ReadView v;
        if (runs) {
            v.runs.reserve(runs->size());
            for (const auto& [gen, rd] : *runs) v.runs.push_back(rd);
        }
        if (!fresh) {
            v.delta = std::move(snap);
            return v;
        }
        // S36-2：stable——同 key 同 ord 按到达序取末（与 flush/locate 同一
        // 规则；merge 搬迁行与被搬迁行同 ord）。
        std::stable_sort(fresh->begin(), fresh->end(),
                         [](const DeltaRow& a, const DeltaRow& b) {
                             if (a.key != b.key) return a.key < b.key;
                             return a.ord < b.ord;
                         });
        // 同 key 保留尾元素——原地压实。out == i 的自移动赋值对
        // std::string 是 valid-but-unspecified，必须跳过。
        std::size_t out = 0;
        for (std::size_t i = 0; i < fresh->size(); ++i) {
            if (i + 1 < fresh->size() &&
                (*fresh)[i + 1].key == (*fresh)[i].key) {
                continue;
            }
            if (out != i) (*fresh)[out] = std::move((*fresh)[i]);
            ++out;
        }
        fresh->resize(out);
        {
            std::lock_guard<std::mutex> lk(mu_);
            std::lock_guard<std::mutex> vlk(view_mu_);
            if (delta_version_ == want) {
                sorted_view_ = fresh;
                sorted_view_version_ = want;
            }
        }
        v.delta = std::move(fresh);
        return v;
    }
}

std::size_t OkiState::delta_rows() const {
    std::lock_guard<std::mutex> lk(mu_);
    return delta_.size();
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
