#include "bitcask/oki_state.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>

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

// 删除目录内 gen 不在 keep 集里的全部 run 文件。
// **不能只删旧 manifest 列出的那些**：触发重建的典型场景恰恰是 manifest
// 缺失/损坏（此时内存 manifest_ 为空），那批 run 文件就成了无人回收的
// 孤儿，每重建一次多一批。在途 ReadView 经 shared_ptr 持旧 Reader——
// POSIX 下 unlink 后 fd 仍可读到 close；B4：删除失败（非 POSIX 语义下
// 文件仍被打开等）的路径回填 backlog，下次 sweep 重试（延迟删除队列的
// OKI 侧形态——正确性不依赖 unlink-while-open）。
void sweep_runs(std::string_view dir, std::span<const std::uint64_t> keep,
                std::vector<std::string>& backlog) {
    // 先重试上一轮滞留（可能已无读者）。
    std::vector<std::string> still;
    for (const auto& p : backlog) {
        std::error_code rm_ec;
        std::filesystem::remove(p, rm_ec);
        if (rm_ec && std::filesystem::exists(p)) still.push_back(p);
    }
    backlog = std::move(still);
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const auto name = e.path().filename().string();
        // S36-1：外排 spill 残件（kv.oki.spill-*，仅崩溃遗留）一律清理。
        if (name.rfind("kv.oki.spill-", 0) == 0) {
            std::error_code rm_ec;
            std::filesystem::remove(e.path(), rm_ec);
            continue;
        }
        const auto g = parse_run_gen(name);
        if (!g) continue;
        if (std::find(keep.begin(), keep.end(), *g) != keep.end()) continue;
        std::error_code rm_ec;
        std::filesystem::remove(e.path(), rm_ec);
        if (rm_ec && std::filesystem::exists(e.path())) {
            backlog.push_back(e.path().string());  // B4：滞留重试
        }
    }
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
    index_row_locked(delta_.size() - 1);
    update_flush_hint_locked();
}

void OkiState::append_update(std::string_view key, std::uint64_t ord,
                             bool tomb, const RowLoc* loc) {
    // 免水位门（旧 ord、新信息——merge 搬迁 / TTL 墓碑，见头文件）。
    std::lock_guard<std::mutex> lk(mu_);
    delta_bytes_ += key.size() + sizeof(DeltaRow);
    delta_.push_back(DeltaRow{std::string(key), ord, tomb,
                              /*has_loc=*/loc != nullptr,
                              loc != nullptr ? *loc : RowLoc{}});
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

void OkiState::publish_runs_locked() {
    runs_snap_.store(std::make_shared<const RunsVec>(readers_),
                     std::memory_order_release);
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

std::uint64_t OkiState::next_gen_locked() const noexcept {
    std::uint64_t g = 0;
    for (const auto& r : manifest_.runs) g = std::max(g, r.gen);
    return g + 1;
}

bool OkiState::flush(
    std::string_view dir,
    std::span<const std::pair<std::uint32_t, std::uint64_t>> durable_wms) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    if (!loaded_.load(std::memory_order_relaxed)) return false;

    // S36-2：**拷贝前缀**而非换出——IO 期间行仍留在 delta_（locate 可见，
    // 组合视图无「在写盘路上不可见」的窗口），提交后才删前缀。失败路径
    // 因此零恢复动作（原 swap+restore 退役）。代价是一次前缀拷贝——flush
    // 本就伴随全量排序 + 文件 IO，量级淹没。
    std::vector<DeltaRow> all;
    {
        std::lock_guard<std::mutex> lk(mu_);
        all.assign(delta_.begin(), delta_.end());
    }
    if (all.empty()) return true;
    const std::size_t prefix_n = all.size();

    // S36-5 B1：持久性分拣——loc 越过其文件持久水位的行持留（语义见头
    // 文件）。held 与前缀下标对齐，提交后据此选择性删除。
    std::vector<char> held(prefix_n, 0);
    std::vector<DeltaRow> rows;
    rows.reserve(prefix_n);
    std::size_t flushed_bytes = 0;
    for (std::size_t i = 0; i < prefix_n; ++i) {
        const DeltaRow& r = all[i];
        bool hold = false;
        if (r.has_loc && !durable_wms.empty()) {
            for (const auto& [fid, wm] : durable_wms) {
                if (fid == r.loc.file_id) {
                    hold = r.loc.offset + r.loc.total_sz > wm;
                    break;
                }
            }
        }
        if (hold) {
            held[i] = 1;
        } else {
            flushed_bytes += r.key.size() + sizeof(DeltaRow);
            rows.push_back(all[i]);
        }
    }
    all.clear();
    if (rows.empty()) return true;  // 全持留：本轮无事（持久水位推进后再来）

    // 排序 + 同 key 去重。**stable**：同 key 同 ord 的多行按到达序取末
    //（merge 搬迁行与被搬迁行同 ord，后到的新位置必须胜——(ord, 到达序)
    // 胜出格的 delta 段，与 SpillingRunBuilder/locate 同一规则）。
    std::stable_sort(rows.begin(), rows.end(),
                     [](const DeltaRow& a, const DeltaRow& b) {
                         if (a.key != b.key) return a.key < b.key;
                         return a.ord < b.ord;
                     });
    std::uint64_t cover = 0;  // 排他上界 = 本批最大 ord + 1
    std::uint64_t uniq = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        cover = std::max(cover, rows[i].ord + 1);
        if (i + 1 == rows.size() || rows[i + 1].key != rows[i].key) ++uniq;
    }

    // S36-2：run 落 v2（全字段 + bloom）——组合视图点查的能力前提；
    // manifest 版本由 write_manifest 惰性选择（含 v2 条目 → BCOM v2）。
    const std::uint64_t gen = next_gen_locked();
    auto w = OkiRunWriter::create(mk_run_filename(dir, gen),
                                  kDefaultBlockBytes, kRunVersion2,
                                  /*expected_entries=*/uniq);
    if (!w) return false;
    bool io_ok = true;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        // 同 key 连续段取最后一条（(ord, 到达序) 最大者）。
        if (i + 1 < rows.size() && rows[i + 1].key == rows[i].key) continue;
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
        std::filesystem::remove(mk_run_filename(dir, gen), ec);
        return false;
    }

    // manifest 提交（唯一 commit point）。失败则删刚写的 run（delta_ 未动）。
    OkiManifest next = manifest_;
    next.runs.push_back({gen, cover, /*format_ver=*/2});
    next.wm = std::max(next.wm, cover);
    next.level_b = level_b_.load(std::memory_order_acquire);  // S36-4 模式戳
    if (!write_manifest(dir, next)) {
        std::error_code ec;
        std::filesystem::remove(mk_run_filename(dir, gen), ec);
        return false;
    }
    manifest_ = std::move(next);
    readers_.emplace_back(gen, std::make_shared<OkiRunReader>(*std::move(rd)));
    publish_runs_locked();
    manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
    wm_.store(manifest_.wm, std::memory_order_release);

    // 提交完成，删已固化的前缀（先发布 run 快照再删——中间态两边可见，
    // delta 优先，语义不变）。IO 期间新 append 的行在前缀之后，保留；
    // S36-5：持留行（held）留在队头，相对序不变（(ord, 到达序) 格保持）。
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<DeltaRow> next;
        next.reserve(prefix_n + delta_.size() - prefix_n);
        for (std::size_t i = 0; i < prefix_n; ++i) {
            if (held[i] != 0) next.push_back(std::move(delta_[i]));
        }
        for (std::size_t j = prefix_n; j < delta_.size(); ++j) {
            next.push_back(std::move(delta_[j]));
        }
        delta_ = std::move(next);
        delta_bytes_ -= std::min(delta_bytes_, flushed_bytes);
        rebuild_index_locked();
        update_flush_hint_locked();
    }

    // S33-6：run 数超阈值 → 全归并（设计 §5.2）。best-effort——失败不影响
    // 本次 flush 的成功语义（run 集合原状保留，下次 flush 再试）。
    if (manifest_.runs.size() > kCompactRunLimit) {
        (void)compact_all_locked(dir);
    }
    return true;
}

// 全归并：k 路归并全部 run → 单个新 run。语义与 CaskRangeIter 的归并同款
// （同 key 取 max-ord、tomb 抵消），差别是**结果落盘**且 tomb 行直接丢弃。
bool OkiState::compact_all_locked(std::string_view dir) {
    if (readers_.size() < 2) return true;

    // 覆盖上界取并集（= 归并前的联合水位，归并不改变覆盖范围）。
    std::uint64_t cover = 0;
    for (const auto& r : manifest_.runs) cover = std::max(cover, r.cover_ord);
    const std::uint64_t keep_wm = manifest_.wm;

    // S36-2：输出版本——全 v2 输入才出 v2（v1 行无位置字段，混入会产出
    // 「无位置的活行」毒化点查能力）；含 v1 则整体降 v1（Level A 语义保
    // 留，点查能力等旧 run 被 rebuild 淘汰后自愈）。
    std::uint32_t out_ver = kRunVersion2;
    std::uint64_t est_entries = 0;
    for (const auto& [g, rd] : readers_) {
        if (rd->version() != kRunVersion2) out_ver = kRunVersion;
        est_entries += rd->entry_count();
    }

    const std::uint64_t gen = next_gen_locked();
    auto w = OkiRunWriter::create(mk_run_filename(dir, gen),
                                  kDefaultBlockBytes, out_ver,
                                  /*expected_entries=*/est_entries);
    if (!w) return false;
    auto abort = [&] {
        std::error_code ec;
        std::filesystem::remove(mk_run_filename(dir, gen), ec);
        return false;
    };

    // 各 run 一个游标 + 预取头元素。
    std::vector<OkiRunReader::Cursor> cs;
    std::vector<std::optional<OkiRunReader::Entry>> heads;
    cs.reserve(readers_.size());
    heads.reserve(readers_.size());
    for (const auto& [g, rd] : readers_) {
        auto c = rd->begin();
        OkiRunReader::Entry e;
        auto n = c.next(e);
        if (!n) return abort();
        cs.push_back(std::move(c));
        heads.push_back(*n ? std::optional<OkiRunReader::Entry>(std::move(e))
                           : std::nullopt);
    }

    while (true) {
        std::string_view min_key;
        bool any = false;
        for (const auto& h : heads) {
            if (h && (!any || std::string_view(h->key) < min_key)) {
                min_key = h->key;
                any = true;
            }
        }
        if (!any) break;

        const std::string key(min_key);  // 推进游标会失效 view，先物化
        std::uint64_t win_ord = 0;
        bool win_tomb = false;
        bool win_has_loc = false;
        RowLoc win_loc{};
        bool first = true;
        for (std::size_t i = 0; i < heads.size(); ++i) {
            auto& h = heads[i];
            if (!h || h->key != key) continue;
            // S36-2：>= 使等 ord 时**更高 gen 胜**（heads 按 readers_ 的
            // gen 升序排列）——merge 搬迁行与被搬迁行同 ord，新位置在更高
            // gen run，(ord, gen) 胜出格（设计 §D2）。
            if (first || h->ord >= win_ord) {
                win_ord = h->ord;
                win_tomb = h->tomb;
                win_has_loc = h->has_loc;
                win_loc = h->loc;
                first = false;
            }
            OkiRunReader::Entry e;
            auto n = cs[i].next(e);
            if (!n) return abort();
            if (*n) {
                h = std::move(e);
            } else {
                h.reset();
            }
        }
        if (win_tomb) continue;  // 全归并 ⟹ 墓碑真正丢弃（见头文件约束）
        auto a = w->add(std::span<const std::byte>(
                            reinterpret_cast<const std::byte*>(key.data()),
                            key.size()),
                        win_ord, /*tomb=*/false,
                        (out_ver == kRunVersion2 && win_has_loc) ? &win_loc
                                                                 : nullptr);
        if (!a) return abort();
    }
    if (!w->finish(/*fsync_dir=*/true)) return abort();

    auto rd = OkiRunReader::open(mk_run_filename(dir, gen));
    if (!rd) return abort();

    OkiManifest next;
    next.runs.push_back({gen, cover,
                         static_cast<std::uint8_t>(
                             out_ver == kRunVersion2 ? 2 : 1)});
    next.wm = keep_wm;  // 归并不推进水位
    next.level_b = level_b_.load(std::memory_order_acquire);  // S36-4
    if (!write_manifest(dir, next)) return abort();

    manifest_ = std::move(next);
    readers_.clear();
    readers_.emplace_back(gen, std::make_shared<OkiRunReader>(*std::move(rd)));
    publish_runs_locked();
    manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
    // 旧 run + 此前崩溃残留的孤儿一并清（在途 ReadView 持 shared_ptr，
    // unlink 后 fd 仍可读——安全）。
    {
        const std::uint64_t keep_gen[] = {gen};
        sweep_runs(dir, keep_gen, sweep_backlog_);
        block_cache_.purge_except(keep_gen);  // S36-3：死 gen 块不占缓存
    }
    return true;
}

bool OkiState::rebuild(std::string_view dir, std::vector<DeltaRow>&& rows,
                       std::uint64_t cover_ord) {
    std::lock_guard<std::mutex> flk(flush_mu_);

    // S33-6：零活 key（空库 / 全删）**不落空 run**——空 run 归并不出任何行，
    // 却占一个文件 + 一个常驻 Reader fd，且要等下次 rebuild 才被清理。
    // 「0 个 run + wm = cover_ord」已完整表达语义：[0, cover_ord) 里没有任何
    // 需要被覆盖的活 key，其后的行照常走 memdelta。
    const std::uint64_t gen = next_gen_locked();
    std::shared_ptr<OkiRunReader> new_reader;
    if (!rows.empty()) {
        // S36-1：外排构建（64MiB 分段 spill + k 路归并）取代全内存 sort——
        // 排序工作集从 O(全部行) 降到 O(spill_bytes)；同 key 去重规则不变
        //（max ord 胜，builder 内 (ord, 到达序) 等价于原 (key, ord) 排序取末）。
        // S36-2：出 v2（全字段 + bloom）——rows 由 caller 从 keydir 收集，
        // 位置字段齐备。
        auto b = SpillingRunBuilder::create(std::string(dir), gen,
                                            kRunVersion2);
        if (!b) return false;
        for (const auto& r : rows) {
            auto a = b->add(std::span<const std::byte>(
                                reinterpret_cast<const std::byte*>(r.key.data()),
                                r.key.size()),
                            r.ord, r.tomb, r.has_loc ? &r.loc : nullptr);
            if (!a) return false;
        }
        if (!b->finish(/*fsync_dir=*/true)) return false;

        auto rd = OkiRunReader::open(mk_run_filename(dir, gen));  // S33-5
        if (!rd) {
            std::error_code ec;
            std::filesystem::remove(mk_run_filename(dir, gen), ec);
            return false;
        }
        new_reader = std::make_shared<OkiRunReader>(*std::move(rd));
    }

    OkiManifest next;
    if (new_reader) next.runs.push_back({gen, cover_ord, /*format_ver=*/2});
    next.wm = cover_ord;
    next.level_b = level_b_.load(std::memory_order_acquire);  // S36-4 模式戳
    if (!write_manifest(dir, next)) {
        std::error_code ec;
        if (new_reader) std::filesystem::remove(mk_run_filename(dir, gen), ec);
        return false;
    }

    // 提交后清理：目录内一切非本次 run 的 seg 文件 + memdelta（其内容已被
    // 全量快照覆盖）。用目录扫描而非「遍历旧 manifest」——见 sweep_runs 注释
    // （manifest 丢失正是重建的触发场景，那批 run 不在内存里）。
    {
        std::vector<std::uint64_t> keep;
        if (new_reader) keep.push_back(gen);
        sweep_runs(dir, keep, sweep_backlog_);
        block_cache_.purge_except(keep);  // S36-3：死 gen 块不占缓存
    }
    manifest_ = std::move(next);
    readers_.clear();
    if (new_reader) readers_.emplace_back(gen, std::move(new_reader));
    publish_runs_locked();
    manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
    wm_.store(manifest_.wm, std::memory_order_release);
    loaded_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(mu_);
        delta_.clear();
        delta_bytes_ = 0;
        rebuild_index_locked();
        update_flush_hint_locked();
    }
    return true;
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
    const auto runs = runs_snap_.load(std::memory_order_acquire);
    if (!runs) return out;  // kUnavailable（loaded 但快照未发布——不可达兜底）
    const std::span<const std::byte> kspan(
        reinterpret_cast<const std::byte*>(key.data()), key.size());
    for (auto it = runs->rbegin(); it != runs->rend(); ++it) {
        const OkiRunReader& rd = *it->second;
        if (rd.version() != kRunVersion2) {
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
    if (!loaded_.load(std::memory_order_acquire)) return std::nullopt;
    ReadView v;
    {
        // 锁序与 flush 一致：flush_mu_ → mu_。
        std::lock_guard<std::mutex> flk(flush_mu_);
        v.runs.reserve(readers_.size());
        for (const auto& [gen, rd] : readers_) v.runs.push_back(rd);
        std::lock_guard<std::mutex> lk(mu_);
        v.delta = delta_;  // 拷贝快照（排序去重在锁外做）
    }
    // S36-2：stable——同 key 同 ord 按到达序取末（(ord, 到达序) 胜出格，
    // 与 flush/locate 同一规则；merge 搬迁行与被搬迁行同 ord）。
    std::stable_sort(v.delta.begin(), v.delta.end(),
                     [](const DeltaRow& a, const DeltaRow& b) {
                         if (a.key != b.key) return a.key < b.key;
                         return a.ord < b.ord;
                     });
    // 同 key 保留 (ord, 到达序) 最大者（尾元素）。
    std::vector<DeltaRow> dedup;
    dedup.reserve(v.delta.size());
    for (std::size_t i = 0; i < v.delta.size(); ++i) {
        if (i + 1 < v.delta.size() && v.delta[i + 1].key == v.delta[i].key) {
            continue;
        }
        dedup.push_back(std::move(v.delta[i]));
    }
    v.delta = std::move(dedup);
    return v;
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
