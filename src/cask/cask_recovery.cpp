// cask_recovery.cpp — Cask 恢复域：离线升级（upgrade）、open 时索引权威
// 重建（load_authority_from_disk）、恢复快照装载（load_recovery_snapshots）、
// legacy search.ckpt 一次性迁移与 delta 链重放。S21-3 B1：从 cask.cpp 纯
// 物理平移拆出（函数体不变），先例同 meta_file.cpp / legacy_ckpt.cpp。
#include "bitcask/cask.hpp"
#include "bitcask/detail/icu_util.hpp"  // S38：Unicode 数据版本记录

#include "legacy_ckpt.hpp"  // S19-2：pre-S17 统一 ckpt 迁移读取器

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <thread>
#include <unordered_map>

#include "bitcask/codec.hpp"           // decode_doc_value / doc_vector_f32
#include "bitcask/format.hpp"          // RecordType（fold 墓碑判定）
#include "bitcask/oki_state.hpp"       // S33-4：OKI load/缺口检查/重建
#include "bitcask/detail/scanner.hpp"  // scan_dir（fold 数据文件枚举）
#include "bitcask/detail/committed_fold.hpp"  // 原子批语义的统一 fold

#include "cask_internal.hpp"  // err / io_fault / bytes_to_view / ckpt 常量 / component_of_plugin
#include "bitcask/detail/path_utf8.hpp"

namespace bitcask {

namespace {
namespace fs = std::filesystem;
}  // namespace

// 离线升级：将 KV 模式目录转为索引模式。
// 不获取任何锁——要求目录处于离线状态（无活跃 writer/merger）。
// 步骤：验证 meta 是 KV → 覆写 meta 为 kIndex → 创建搜索插件（Text/Vector）→
//       新建 authority + load_authority_from_disk → mark_ready
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
        // S33：透传 MetaError 详情——纪元门禁的迁移提示（如 v4 → hintord）
        // 必须到达用户，不能吞成笼统的 "read meta failed"。
        return std::unexpected(err(CaskError::kIo,
                                   "read meta failed: " + mc.error().message));
    }
    if (mc->mode == meta::Mode::kIndex) {
        return std::unexpected(err(CaskError::kModeMismatch,
                                    "directory is already in index mode"));
    }

    meta::MetaConfig new_mc;
    new_mc.mode = meta::Mode::kIndex;
    new_mc.version = mc->version;  // S35：保留纪元（v6 目录不得降回 v5）
    // S38：此刻才开始有文本分析（KV → 索引模式），故此刻记 Unicode 数据版本。
    // 原 KV 目录的该字段恒为 0（未记录），这里是它第一次被真正写入。
    new_mc.icu_major = text::detail::icu_major_version();
    new_mc.unicode_major = text::detail::unicode_major_version();
    new_mc.analyzer_fp =  // C1：同上，此刻起按该配置分词
        text::analyzer_fingerprint(search_config.analyzer_config);
    auto wr = meta::write_meta(std::string(dirname), new_mc);
    if (!wr) {
        return std::unexpected(err(CaskError::kIo, "write meta failed"));
    }

    auto cask = std::make_unique<Cask>();
    cask->dirname_ = std::string(dirname);
    cask->meta_config_ = new_mc;
    if (!cask->field_schema_.open(bitcask::detail::to_utf8(bitcask::detail::from_utf8(dirname) / "field.schema"))) {  // #1
        return std::unexpected(err(CaskError::kIo,
            "field.schema corrupt or incompatible version"));
    }

    cask->docmap_ = std::make_shared<index::Index>();  // S16-1：宿主服务
    // S19-2：直构插件（shim 退役）。三个窄接口引用绑同一 Index 实例。
    cask->text_ = std::make_unique<text::TextPlugin>(
        search_config.text_config(), *cask->docmap_, *cask->docmap_,
        *cask->docmap_);
    cask->vec_plugin_ = cask->create_vector_plugin(search_config);
    cask->hybrid_.emplace(*cask->text_, *cask->vec_plugin_);
    // S18-8：恢复重放经 plugins_ 广播——upgrade 手工装配路径同样要注册。
    cask->plugins_ = {cask->text_.get(), cask->vec_plugin_.get()};

    cask->authority_ = std::make_shared<oki::IndexAuthority>();
    if (auto r = cask->load_authority_from_disk(); !r) {
        return std::unexpected(r.error());
    }
    cask->authority_->mark_ready();

    return cask;
}

void Cask::replay_delta_to_authority(
    const std::vector<index::DocmapDeltaRow>& rows,
    const std::vector<index::DocmapDeltaRemoval>& rems,
    std::span<const std::byte> authority_meta,
    RecoverySnapshots& recovery) {
    // S43-9：data 文件是唯一物理权威；docmap checkpoint 只恢复搜索侧。
    // 不再把搜索链 delta 回写 OKI，否则会与 data rebuild 双记。
    (void)rows;
    (void)rems;
    (void)authority_meta;
    (void)recovery;
}

// S17-5:legacy search.ckpt → per-component 一次性迁移。流程：
//   1) 用旧 load_search_ckpt 把段全载回（带 hook 推进 keydir 字节水位）。
//   2) 从各插件读 watermark 与组件链状态，构造 manifest。
//   3) 调 save_components_base 把当前内存态写到 3 个新组件文件。
//   4) 写 manifest。
//   5) 删旧 search.ckpt + search.ckpt.prev + .d* + search.vec / .qc8
//      （这些已被 save_components_base 重新写到 docmap.ckpt / bm25.ckpt /
//       vec.ckpt 对应 .vec / .qc8 sidecar）。
// 失败返回 false（caller 退全量 fold）。
bool Cask::migrate_legacy_search_ckpt() {
    const std::string old_ckpt = dirname_ + "/" + kSearchCkptName;
    // 1) 读旧 ckpt → 内存态（不写 keydir，已由 caller 在 recovery 阶段
    // 后续的 load_recovery_snapshots 接管；这里只关心段载入与写新文件）。
    // S19-2：legacy 读取器收编（load-only；shim 已降级测试夹具）。
    // legacy 统一 ckpt 是 HNSW 纪元产物（IVF 库按构造无 legacy 形态）；
    // 非 HNSW 引擎直接判失败退全量 fold（防御，不可达）。
    if (meta_config_.vector_engine != meta::VectorEngine::kHnsw) {
        log_warn("migrate_legacy: non-hnsw engine cannot own legacy ckpt");
        return false;
    }
    auto& hnsw_vp = static_cast<vec::VectorPlugin&>(*vec_plugin_);
    auto result = legacy_ckpt::load(old_ckpt, *docmap_, *text_, hnsw_vp);
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
    // 3) 写 per-component 文件。S18-2：docmap 组件由宿主直写。
    const bool docmap_ok = index::save_docmap_base(
        *docmap_, dirname_, result.watermark);
    if (docmap_ok) {
        docmap_chain_ = bitcask::ManifestEntry{
            result.watermark, 0, result.watermark};
    }
    std::array<bool, bitcask::kComponentCount> all_dirty{};
    for (auto& b : all_dirty) b = true;
    // S19-2：组件 base 直调插件（原 shim save_components_base 调度壳）。
    (void)all_dirty;
    const bool bm25_ok = text_->save_component_base(dirname_,
                                                    result.watermark);
    const bool vec_ok = hnsw_vp.save_component_base(dirname_,
                                                    result.watermark);
    if (!docmap_ok || !bm25_ok) {
        log_warn("migrate_legacy: failed to write per-component base "
                 "(docmap=" + std::to_string(docmap_ok) +
                 " bm25=" + std::to_string(bm25_ok) +
                 " vec=" + std::to_string(vec_ok) + ")");
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
    std::filesystem::remove(bitcask::detail::from_utf8(old_ckpt), ec);
    std::filesystem::remove(bitcask::detail::from_utf8(old_ckpt + ".prev"), ec);
    for (std::uint32_t i = 1; i < 1024; ++i) {
        if (!std::filesystem::remove(
                bitcask::detail::from_utf8(old_ckpt + ".d" + std::to_string(i)), ec)) {
            // 链中段缺失即停（链是连续 1..N）。
            if (ec) break;
        }
    }
    std::filesystem::remove(
        bitcask::detail::from_utf8(old_ckpt).replace_extension(".vec"), ec);
    std::filesystem::remove(
        bitcask::detail::from_utf8(old_ckpt).replace_extension(".qc8"), ec);
    return true;
}

// ---- open 时重建索引权威 ---------------------------------------------------
// data-only fold 重建。fold 顺序按 tstamp 升序——保证后写入的 entry 覆盖前面的。
// search_layer 为空时跳过 SearchLayer 的恢复。
// S43-3：健康 BCOM v4 + 有效 run 时优先走 data-only streaming recovery。
// 已载 manifest 与目录实况是否自洽（Level A / Level B 共用的唯一判据）。
// OKI-only（S43-7）后 merge 搬迁恒经 IndexAuthority::relocate 进 OKI，与
// Level A/B 无关；能让 run 位置失真的只剩「别的进程删掉了 run 仍引用的
// 文件」（merge_only 旁车）或提交点回滚。可检出的迹象：
//   - manifest 的 data 水位引用了不存在 / 变短的文件（提交点越过 EOF）；
//   - v4 fstats 里仍有活键的文件已不存在（写者不知情的删除）。
// 目录里多出的新文件（崩溃 tail）不影响自洽：恢复会从 0 重放它们。
// v1 run 不在此判：它是「点查不可用」而非「位置失真」，由访问时的受控
// 重建处理。
bool Cask::oki_manifest_consistent() const {
    const auto& oki = authority_->state();
    if (!oki.loaded()) return false;
    auto file_ok = [&](std::uint32_t fid, std::uint64_t min_size) {
        std::error_code ec;
        const auto path = bitcask::detail::from_utf8(
            fileops::mk_data_filename(dirname_, fid));
        const auto sz = fs::file_size(path, ec);
        return !ec && sz >= min_size;
    };
    for (const auto& [fid, wm] : oki.manifest_data_wms()) {
        if (!file_ok(fid, wm)) return false;
    }
    for (const auto& f : oki.manifest_fstats()) {
        if (f.live_keys > 0 && !file_ok(f.file_id, 0)) return false;
    }
    return true;
}

// 返回 false 不代表 open 失败，caller 继续走传统全量 fold / rebuild。
bool Cask::try_data_only_recovery(
    const std::vector<fileops::DataFileEntry>& entries) {
    auto& oki = authority_->state();
    const bool search_on = text_ != nullptr;
    if (search_on || opts_.merge_only) return false;  // value 恢复仍走旧路径
    const auto commit_opt = oki.manifest_metadata();  // 按值快照（防悬空）
    if (!oki.loaded() || !commit_opt.has_value()) return false;

    // S43-3：mode-stamp mismatch 是一级公民 fallback，不做“顺手升级”。
    // 只有 level_b 戳可信时，run 内 loc 才是 merge/写路径在线维护的
    // 权威。Level A 目录的 merge relocation 不入 OKI；若这里启用冷侧，
    // 重开后会把 stale loc 当权威，可能指向已退休文件。
    const bool intended_level_b =
        opts_.read_write && opts_.keydir_cache_entries > 0;
    const bool level_b_ok = oki.manifest_level_b() && intended_level_b;
    // Level A 可写句柄：manifest 通过可信检查即同样走 data-only（原实现
    // 一律退回兜底：open 一遍 ledger 全量扫描 + 首访一遍全量重建）。
    // 两种模式都必须过同一道自洽判据（v4 元数据在场已在上方检查）。
    const bool level_a_ok = opts_.read_write &&
                            opts_.keydir_cache_entries == 0;
    if (!level_b_ok && !level_a_ok) return false;
    if (!oki_manifest_consistent()) return false;
    const auto& commit = *commit_opt;

    // v1 行没有位置和 bloom；混入即组合视图不可点查。这是显式覆盖缺口，
    // 不能靠空 authority 上的普通 get 兜底。v2/v3 load 时已完成结构校验。
    for (const auto& r : oki.manifest_runs()) {
        if (r.format_ver == oki::kRunVersion) return false;
    }

    // v4 全局段和 run 覆盖必须自洽。active key >0 但 0 run 是明显缺口；
    // next_ord >= wm 允许 delta/tail 超前，但反过来会吞掉已提交 tail。
    if (commit.next_ord < oki.wm() ||
        (commit.active_keys != 0 && oki.manifest_runs().empty())) {
        return false;
    }

    // data 字节水位是 ord 水位的物理落点。缺失（旧 v4）时从 0 全扫但仍
    // 只收 ord>=wm；水位超前文件大小是不可修复的落点错误，交全量 rebuild。
    std::unordered_map<std::uint32_t, std::uint64_t> committed_ends;
    committed_ends.reserve(oki.manifest_data_wms().size());
    for (const auto& [file_id, end] : oki.manifest_data_wms()) {
        committed_ends.emplace(file_id, end);
    }

    oki::OkiManifest manifest;
    manifest.metadata = commit;
    manifest.fstats = oki.manifest_fstats();
    if (!authority_->adopt_manifest(manifest)) {
        return false;
    }

    oki.enable_point_query();

    std::unordered_map<std::uint32_t, std::uint64_t> progress;
    progress.reserve(entries.size());
    auto durable_now = [&](std::uint32_t current_id,
                           std::uint64_t current_end) {
        std::vector<std::pair<std::uint32_t, std::uint64_t>> wms;
        wms.reserve(entries.size());
        for (const auto& e : entries) {
            const std::uint32_t fid =
                static_cast<std::uint32_t>(e.tstamp);
            if (fid == current_id) {
                wms.emplace_back(fid, current_end);
            } else if (auto it = progress.find(fid); it != progress.end()) {
                wms.emplace_back(fid, it->second);
            }
        }
        return wms;
    };

    // 串行按 file_id 升序 fold：恢复期不需要并发抢 CPU，也保证同一
    // data file 内的 append/墓碑序不被打乱；跨文件乱序由 ord 裁决。
    for (const auto& e : entries) {
        const std::uint32_t fid = static_cast<std::uint32_t>(e.tstamp);
        (void)authority_->increment_file_id_at_least(fid);

        std::error_code size_ec;
        const std::uint64_t file_size = std::filesystem::file_size(
            bitcask::detail::from_utf8(e.data_path), size_ec);
        if (size_ec) return false;

        std::uint64_t start = 0;
        if (auto it = committed_ends.find(fid); it != committed_ends.end()) {
            if (it->second > file_size) return false;
            start = it->second;
        }
        if (start >= file_size) {
            progress[fid] = start;
            continue;
        }

        auto df = fileops::DataFile::open(
            e.data_path, fileops::DataFile::Mode::kRead,
            /*sync=*/false, /*mmap_enabled=*/false);
        if (!df) return false;

        std::uint64_t current_end = start;
        std::uint64_t last_valid_end = start;
        bool flush_failed = false;
        fileops::CommittedFoldOptions fopts;
        fopts.out_last_valid_end = &last_valid_end;
        fopts.start_offset = start;
        fopts.keep_batch_values = false;  // 只需 key / 位置
        auto fr = fileops::fold_committed(
            *df,
            [&](const codec::DataRecordView& view, std::uint64_t offset,
                std::uint32_t total_size) {
                if (flush_failed) return;
                // 与组合视图比对裁决（而非 wm 门）：视图已有同 key 且 ord ≥
                // 者即已覆盖；否则记账 + 入行。见 IndexAuthority::recover_row。
                authority_->recover_row(
                    bytes_to_view(view.key), fid, total_size, offset,
                    view.tstamp, view.ord,
                    view.type == format::RecordType::kTombstone);
                current_end = offset + total_size;

                // S43-3：恢复 flush 和普通写路径同一条协议（run fsync →
                // Reader 校验 → manifest 原子发布 → delta 截断）。当前文件
                // 已扫描到的记录先 fsync，才允许 run 引用该位置。
                if (oki.should_flush()) {
                    // 恢复期提交同样必须带一致切面（账本 + 水位 + cut）。
                    auto cut = authority_->capture_commit_cut(
                        [&]() -> std::optional<oki::IndexAuthority::DataWms> {
                            if (!df->sync_fd_only(current_end)) {
                                return std::nullopt;
                            }
                            return durable_now(fid, current_end);
                        });
                    if (!cut ||
                        !oki.flush(dirname_, cut->second,
                                   std::move(cut->first))) {
                        // 失败时交全量兜底；已提交前缀仍有效，delta 保留。
                        flush_failed = true;
                    }
                }
            },
            // 批成员收口后才可引用；批头 ord 推进分配水位。
            [&](const codec::DataRecordView& view) {
                authority_->advance_ord(view.ord);
            },
            fopts);
        if (!fr || flush_failed) return false;

        // tail 的 torn write 语义与传统恢复一致：写打开且持锁时截掉尾部。
        // 这里一定不是 merge_only；RO 不截断，留给下一次可写 open。
        if (opts_.read_write && last_valid_end < file_size) {
            auto wdf = fileops::DataFile::open(
                e.data_path, fileops::DataFile::Mode::kAppend);
            if (wdf) (void)wdf->truncate_to(last_valid_end);
            progress[fid] = last_valid_end;
        } else {
            progress[fid] = last_valid_end;
        }
        df->close();
    }
    return true;
}

std::expected<void, CaskFault> Cask::load_authority_from_disk() {
    // S19-2：搜索模式判定改成员（text_ 非空 = 索引模式）。
    const bool search_on = text_ != nullptr;
    auto entries = fileops::scan_dir(dirname_);
    if (!entries) return std::unexpected(io_fault(entries.error().errnum, dirname_));

    // S43-8：hint 从本版起只是历史加速缓存。可写普通句柄已持有
    // write.lock，这里惰性清掉所有旧产物（含无 data 配对的孤儿）；
    // 只读句柄不改变目录，merge_only 不代表独占写权。清理失败不阻断
    // data-only recovery / fold(data)。
    if (opts_.read_write && !opts_.merge_only) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dirname_, ec)) {
            if (ec) break;
            std::error_code hint_ec;
            if (!entry.is_regular_file(hint_ec) || hint_ec) continue;
            if (entry.path().extension() != ".hint") continue;
            std::error_code remove_ec;
            (void)std::filesystem::remove(entry.path(), remove_ec);
        }
    }

    // S33-4：OKI manifest 先载——wm 就位后，后续 fold/链重放经权威
    // put/remove 咽喉点的挂钩自动只收 ord ≥ wm 的 tail 行（零逐点改动）。
    // 缺失/损坏 → 未加载态，收尾 finish_oki_recovery 整体重建。
    // S36-4 Level B open 协议（设计 §D3/§8 + S36-2/3 立的硬要求）：
    //   1. 写者先声明模式（此后所有 manifest 提交携带/清除 level_b 戳）；
    //   2. merge_only 旁车 × Level B 目录互斥（旁车 merge 无挂钩，会静默
    //      腐蚀组合视图的位置权威）；
    //   3. Level A 写者立刻清戳（其 merge 同样无挂钩——戳必须先失效）；
    //   4. 带戳目录 + Level B 意图 → 恢复期即启点查（挂钩入锁 + 冷视图
    //      计数裁决在 tail 重放期间就位；v4 子集快照亦以此为前提）。
    const bool level_b_intent = opts_.keydir_cache_entries > 0;
    authority_->state().set_level_b(level_b_intent && opts_.read_write &&
                               !opts_.merge_only);
    authority_->state().load(dirname_);
    if (opts_.merge_only && authority_->state().loaded() &&
        authority_->state().manifest_level_b()) {
        return std::unexpected(err(
            CaskError::kIo,
            "merge_only sidecar cannot open a Level B "
            "(keydir_cache_entries) maintained directory"));
    }
    if (opts_.read_write && !opts_.merge_only) {
        authority_->state().stamp_mode(dirname_);  // 仅降级清戳（升级走重建）
    }
    const bool level_b_trusted = level_b_intent &&
                                 authority_->state().loaded() &&
                                 authority_->state().manifest_level_b();
    if (level_b_trusted) authority_->state().enable_point_query();

    // S43-3：data-only 主路径。成功时返回，完全不装 BCKS、不扫 hint、
    // 也不构建全量内存索引；失败自然落入下方传统兜底路径。
    if (try_data_only_recovery(*entries)) {
        return {};
    }

    // P14e:search.ckpt 分段快照快路径。search.ckpt 健康且全段 CRC 通过
    // 时，fold 从 keydir 水位起跳过已覆盖字节；否则全量 fold（各索引自门）。
    auto recovery = load_recovery_snapshots();
    if (!recovery) return std::unexpected(recovery.error());
    bool snap_loaded = recovery->snap_loaded;
    (void)snap_loaded;
    const auto& snap_wms = recovery->snap_wms;
    auto wm_of = [&](std::uint32_t fid) -> std::uint64_t {
        for (auto& [id, off] : snap_wms) {
            if (id == fid) return off;
        }
        return 0;  // 快照不认识的文件(快照后新建/merge 产物)→ 全量 fold
    };

    // R3:pass 1——逐文件 fold：推进 file_id、截断 torn tail。不再承担 search
    // 重放（S45 移到 pass 2），故 search 模式也可按文件并行。
    auto fold_one =
        [&](const fileops::DataFileEntry& e) -> std::expected<void, CaskFault> {
        // 把 keydir 的 biggest_file_id 推到至少这个文件的 id——保证后续
        // 分配新 file_id 时不会跟磁盘上已有的文件冲突。
        (void)authority_->increment_file_id_at_least(
            static_cast<std::uint32_t>(e.tstamp));
        const std::uint64_t fold_start =
            snap_loaded ? wm_of(static_cast<std::uint32_t>(e.tstamp)) : 0;
        // S43-8：恢复恒 fold(data)。tolerate_crc_errors=true 让单条
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
        // S35 原子批语义由 fileops::fold_committed 统一实现（批成员收口后
        // 才放行；区间不完整时 fold 停在批头，随后截断）。
        auto noop_rec = [](const codec::DataRecordView&, std::uint64_t,
                           std::uint32_t) {};
        fileops::CommittedFoldOptions fopts;
        fopts.out_last_valid_end = &last_valid_end;
        fopts.start_offset = fold_start;
        auto fr = fileops::fold_committed(*df, noop_rec, {}, fopts);
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

    // 调度：文件数 > 1 时并行 fold——worker 各取一文件，原子计数器分发，
    // 结果数组收集错误后统一传播。
    const std::size_t nfiles = entries->size();
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    const std::size_t nworkers = std::min<std::size_t>(nfiles, hw);
    if (nfiles <= 1 || nworkers <= 1) {
        for (const auto& e : *entries) {
            if (auto r = fold_one(e); !r) return std::unexpected(r.error());
        }
    } else {
        std::vector<std::expected<void, CaskFault>> results(nfiles);
        std::atomic<std::size_t> next{0};
        // RAII join guard：emplace_back 抛异常时已创建的 worker 会被析构自动
        // join——裸 vector<std::thread> 在此场景会让 joinable 线程触发
        // std::terminate，把可恢复的资源耗尽升级为崩溃。
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
    }

    // S33-4：权威收尾（补 tail 行 / 必要时重建）。S45 起先于 search 重放——
    // 重放要拿 OKI 做 key + LSN 裁决。
    finish_oki_recovery(snap_loaded, recovery->snap_next_ord);
    if (!search_on) return {};

    // pass 2（S45-5）：search 恢复重放。按文件序 fold，任务经与在线写相同的
    // 分片执行器应用（同 key 按 fold 序串行、不同 key 并行 prepare+apply，
    // 向量走串行道）。正确性：规则 R（OKI）丢弃已删除 / 已被覆盖的旧版本；
    // 当前版本由 docmap / 倒排 / 向量各自的 key+LSN 守卫判重（重叠区与乱序
    // 完成皆幂等）。起点：replay_floor 之下的记录必已在各组件 ckpt 里，直接
    // 跳过（S45 之前每次重开都全量重新分词）。
    const std::uint64_t replay_floor = search_on ? recovery->replay_floor : 0;
    IndexPool* rpool = nullptr;
    IndexLane* replay_lane = nullptr;
    if (search_on) {
        dispatcher_.set_plugins(plugins_);
        dispatcher_.docmap = docmap_.get();
        // S45：规则 R（严格版）——本 pass 在 finish_oki_recovery 之后执行，
        // OKI 已补齐 tail，是**终态**：只有「OKI 命中、非墓碑、LSN 恰好相等」
        // 的版本才是当前版本，其余（已删除 / 已被覆盖 / OKI 不认的）一律丢弃
        // （不分析、不进 docmap、不进任何插件）。
        // 不能沿用在线版的「OKI 版本更旧则放行」：在线时那是任务先于 OKI
        // 安装的防御；重放时 OKI 已是终态，「更旧」只说明它记的墓碑 LSN 不
        // 精确——实测崩溃恢复后 OKI 对已删 key 可能报更早的墓碑 LSN（删除
        // 状态正确、LSN 偏旧），按旧规则会把中间版本放进来变成死节点，向量
        // 定位表快照又不存墓碑，下一次重开再插一遍。
        // OKI 不可判定（重建失败降级）时放行，退由各结构的 key+LSN 守卫兜底。
        dispatcher_.is_current = [this](std::string_view k, std::uint64_t o) {
            if (!index_arbitration_.load(std::memory_order_relaxed) || !authority_) {
                return true;
            }
            const auto r = authority_->state().locate(k);
            using LS = oki::OkiState::LocateStatus;
            if (r.status == LS::kUnavailable) return true;
            return r.status == LS::kHit && !r.tomb && r.ord == o;
        };
        dispatcher_.on_error = [this] { on_index_worker_error(); };
        if (registry_) {
            rpool = registry_->index_pool();
            LaneCallbacks cb;
            cb.shard_apply = [this](IndexTask& t, unsigned sh) {
                return dispatcher_.shard_apply(t, sh);
            };
            if (dispatcher_.has_serial()) {
                cb.serial_apply = [this](IndexTask& t) { dispatcher_.serial_apply(t); };
            }
            cb.on_error = [this] { on_index_worker_error(); };
            replay_lane = rpool->register_lib(std::move(cb), 0);
        }
    }
    // 任何退出路径（含 fold 失败早退）都必须排空并注销重放车道——其回调
    // 引用本 Cask。
    struct ReplayLaneGuard {
        IndexPool*& pool;
        IndexLane*& lane;
        void finish() {
            if (pool && lane) {
                (void)pool->flush(lane);
                (void)pool->unregister_lib(lane);
                lane = nullptr;
            }
        }
        ~ReplayLaneGuard() { finish(); }
    } replay_guard{rpool, replay_lane};
    auto submit_replay = [&](IndexTask t) {
        t.replay = true;
        if (rpool && replay_lane) {
            rpool->submit(replay_lane, std::move(t));
            return;
        }
        // 无共享池（理论不可达：search 模式 open 强制 registry）：调用线程直跑。
        try {
            if (dispatcher_.shard_apply(t, index_shard_of(t.key())) ==
                ApplyResult::kForwardSerial) {
                dispatcher_.serial_apply(t);
            }
        } catch (...) {
            on_index_worker_error();
        }
    };
    // ①（s13-review §P1 后续）：统计本次恢复重放的文档数——度量「若现在不
    // 回存 checkpoint，下次崩溃要白付多少重放」。
    std::size_t recovered_docs = 0;
    // S14-6：恢复期遇到 field.schema 无法解析的悬空 FieldId 的计数（掉电
    // 窗口：intern 只 fflush 未 fsync，schema 尾条映射可能晚于数据丢失）。
    // 跳过该字段（与"丢弃"同级的降级）+ 循环后聚合告警，可观测不刷屏。
    std::size_t dangling_field_ids = 0;
    for (const auto& e : *entries) {
        auto df = fileops::DataFile::open(e.data_path,
                                           fileops::DataFile::Mode::kRead,
                                           /*sync*/ false, /*mmap_enabled*/ false);
        if (!df) {
            return std::unexpected(io_fault(df.error().errnum, e.data_path));
        }
        auto apply_rec = [&](const codec::DataRecordView& view,
                             std::uint64_t offset, std::uint32_t total_size) {
                if (view.type == format::RecordType::kTombstone) {
                    // S45-5：墓碑重放 = 删除任务（docmap 只删更旧版本、插件
                    // on_delete 按 ord 判重）。同 key 与其文档同分片 FIFO，
                    // 相对序天然保持（原「墓碑前 flush 攒批」不再需要）。
                    // S43-9：权威账本随后用 rebuild_from_data 从同一份 data 重建。
                    if (search_on && view.ord >= replay_floor) {
                        submit_replay(IndexTask::make(
                            IndexOp::Delete,
                            std::string_view(
                                reinterpret_cast<const char*>(view.key.data()),
                                view.key.size()),
                            view.ord, {}, 0, 0, 0, view.tstamp, 0));
                    }
                    return;
                }
                if (search_on && view.ord >= replay_floor) {
                    auto dv = codec::decode_doc_value(std::span<const std::byte>(view.value));
                    // V3.3:带向量的文档即使 text 为空也要恢复(否则
                    // Index 无该 ord,live 过滤会把它当死文档)。
                    // P3b:量化落盘(vec_quantized)也算带向量。
                    const bool dv_has_vec = dv && (dv->has_vector || dv->vec_quantized);
                    // S14-6：纯命名字段文档（text 空、无向量）也必须恢复。
                    // V5：仅带 meta 的文档同理（活写路径无条件登记 docmap）。
                    if (dv && (!dv->text.empty() || dv_has_vec ||
                               dv->has_fields || dv->has_meta)) {
                        auto t = IndexTask::make(
                            IndexOp::Add,
                            std::string_view(
                                reinterpret_cast<const char*>(view.key.data()),
                                view.key.size()),
                            view.ord,
                            std::string_view(
                                reinterpret_cast<const char*>(dv->text.data()),
                                dv->text.size()),
                            static_cast<std::uint32_t>(e.tstamp), offset,
                            total_size, view.tstamp, 0);
                        // P3b:doc_vector_f32 统一处理 f32 与 int8 量化两种落盘。
                        t.vec = codec::doc_vector_f32(*dv);
                        // S14-6：命名字段还原（FieldId → 名字经 field.schema），
                        // 打包进 fields_store（同活写路径 make_doc_add_task）。
                        if (dv->has_fields) {
                            std::vector<std::pair<std::string, std::span<const std::byte>>>
                                named;
                            named.reserve(dv->fields.size());
                            std::size_t total = 0;
                            for (const auto& f : dv->fields) {
                                auto fname = field_schema_.name_of(f.id);
                                if (!fname) {
                                    ++dangling_field_ids;
                                    continue;
                                }
                                total += fname->size() + f.value.size();
                                named.emplace_back(std::move(*fname), f.value);
                            }
                            std::vector<char> store;
                            store.reserve(total);
                            std::vector<std::pair<std::string_view, std::string_view>> views;
                            views.reserve(named.size());
                            for (const auto& [name, val] : named) {
                                const auto name_off = store.size();
                                store.insert(store.end(), name.begin(), name.end());
                                const auto val_off = store.size();
                                store.insert(store.end(),
                                             reinterpret_cast<const char*>(val.data()),
                                             reinterpret_cast<const char*>(val.data()) +
                                                 val.size());
                                views.emplace_back(
                                    std::string_view(store.data() + name_off, name.size()),
                                    std::string_view(store.data() + val_off, val.size()));
                            }
                            t.fields_store = std::move(store);
                            t.fields = std::move(views);
                        }
                        if (dv->has_meta) {
                            t.meta.assign(dv->meta.begin(), dv->meta.end());
                        }
                        ++recovered_docs;
                        submit_replay(std::move(t));
                    }
                }
        };
        fileops::CommittedFoldOptions fopts;
        fopts.start_offset =
            snap_loaded ? wm_of(static_cast<std::uint32_t>(e.tstamp)) : 0;
        if (!fileops::fold_committed(*df, apply_rec, {}, fopts)) {
            return std::unexpected(err(CaskError::kBadCrc, e.data_path));
        }
        df->close();
    }
    replay_guard.finish();  // 排空重放车道（全部重放任务已应用）
    dispatcher_.is_current = nullptr;  // 在线车道注册时重新装配
    if (dangling_field_ids > 0) {
        log_warn("recovery: skipped " + std::to_string(dangling_field_ids) +
                 " field value(s) with dangling field id "
                 "(field.schema tail lost; affected fields stay "
                 "unindexed until rewritten)");
    }
    // ①（s13-review §P1 后续）：恢复期重分析量超阈值时立即回存
    // checkpoint——否则重建成果只在内存，下次干净 close/merge 前再崩一次
    // 就全价重付。触发条件用**重分析文档数**而非「是否全量 fold」。此刻
    // index lane 未注册、无并发写者，调用线程直接序列化即安全。best-effort：
    // 失败仅降级下次启动速度，不阻断 open。只读 / merge_only 不写。
    constexpr std::size_t kPostRecoveryCkptMinDocs = 1000;
    if (recovered_docs >= kPostRecoveryCkptMinDocs && opts_.read_write &&
        !opts_.merge_only) {
        // S14-7：经成对入口（fold 后链可能有效 → delta 回存更省）。
        auto wms0 = collect_snapshot_watermarks();
        std::vector<std::byte> kd;
        if (!save_search_ckpt_paired(dirname_ + "/" + kSearchCkptName,
                                     authority_->peek_next_ord(), wms0, kd)) {
            log_warn("post-recovery search checkpoint save failed "
                     "(next open will re-fold)");
        }
    }
    return {};
}

// S43-9：恢复收尾只做可信门检查；权威缺失/缺口由 data 重建兜底。
void Cask::finish_oki_recovery(bool snap_loaded,
                               std::uint64_t snap_next_ord) noexcept {
    // data-only 恢复在成功时提前返回且已经 adopt manifest + ledger。
    // 走到这里的一定是全量/tail fold 路径；S43-9 下该路径只恢复搜索侧，
    // 但 ledger-only 重建不等于重发索引视图：已加载 OKI 是读端故障检测
    // 的对象，尤其语义坏行必须在 locate/get 时按受控重建暴露；v3 目录
    // 也不得在 open 期伪造 v4 元数据。只有未加载态才整体重建并换 run。
    if (opts_.merge_only) return;

    const bool level_b = opts_.keydir_cache_entries > 0;
    const bool read_only = !opts_.read_write;
    const bool state_loaded = authority_->state().loaded();
    bool rebuild_state = !state_loaded ||
        (level_b && !authority_->state().manifest_level_b()) ||
        (read_only && !authority_->state().point_query_enabled());
    // Level A 可写句柄：manifest 可信 → 开点查后只补账本（含 tail 补行），
    // 免重建；不可信 → 此处一次性重建。原流程先 ledger-only 全量扫描、首个
    // get/put 又因点查未开触发第二遍全量重建，期间 range/scan 读的是缺 tail
    // 的旧 run。
    const bool level_a_trusted =
        !level_b && opts_.read_write && !rebuild_state &&
        authority_->state().manifest_metadata().has_value() &&
        oki_manifest_consistent();
    if (!level_b && opts_.read_write && !level_a_trusted) rebuild_state = true;
    if (level_a_trusted) authority_->state().enable_point_query();
    // 已载视图的持久门（两种模式同一判据，见 oki_manifest_consistent）：
    // 水位越过 EOF / 活键文件被删 ⟹ 视图不可保留。
    const bool manifest_consistent = state_loaded && oki_manifest_consistent();
    // 空 run 集但 data 有活 key，是崩溃前的 memdelta 尾部丢失（典型：
    // 可写进程未 close 就退出）。data 是唯一真相，必须全量重建视图；
    // 有 run 的健康 v3 目录仍只补账本，不得在 open 期伪造/替换视图。
    if (state_loaded && authority_->state().manifest_runs().empty() &&
        !rebuild_state) {
        if (!authority_->rebuild_ledger_from_data(dirname_, opts_.read_write) ||
            authority_->info().key_count !=
                authority_->state().manifest_metadata()
                    .value_or(bitcask::oki::OkiManifestMetadata{})
                    .active_keys) {
            rebuild_state = true;
        }
    }
    rebuild_state = rebuild_state || (level_b && !manifest_consistent);

    // 只读句柄只建内存视图：原实现先调 rebuild_from_data（落 run +
    // manifest，且不持 write.lock）再建内存视图——RO 打开改写了目录。
    bool ok = !rebuild_state ? authority_->rebuild_ledger_from_data(dirname_, opts_.read_write)
              : read_only    ? authority_->rebuild_memory_from_data(dirname_)
                             : authority_->rebuild_from_data(dirname_);
    // 兜底：manifest 声明的 data 水位高估了 run 的覆盖（旧版本 OKI 提交切面
    // 缺陷写出的 manifest——roll 新文件的首批记录已追加未应用就被算作覆盖），
    // 账本模式补 tail 会跳过这些记录 → KV 丢写。抽检不过即从 data 全量重建。
    if (ok && !rebuild_state && !authority_->last_ledger_view_consistent()) {
        log_warn("oki manifest overstates data coverage; rebuilding the "
                 "key index from data");
        rebuild_state = true;
        ok = read_only ? authority_->rebuild_memory_from_data(dirname_)
                       : authority_->rebuild_from_data(dirname_);
    }
    if (!ok) {
        log_warn("oki authority recovery failed (oki disabled until "
                 "next open)");
        authority_->state().degrade_for_rebuild();
        oki_rebuild_failed_ = true;
    } else {
        // 未加载重建成功后才有可点查视图；已加载视图沿用 open 早期
        // 的模式门（Level B 才开）。ledger-only 不改变该决策。
        // 重建成功即得到可点查视图（rebuild_from_data 产出全 v3 run）。
        // 原判据在重建**之后**检查 !loaded()，恒为假，Level B 重建后漏开。
        if (read_only || rebuild_state) {
            authority_->state().enable_point_query();
        }
    }
    (void)snap_loaded;
    (void)snap_next_ord;
}

// S43-1：运行期受控重建。写序闸门内重扫 data（唯一持久真相），账本与
// OKI run 一起由 IndexAuthority 原子重建。
bool Cask::rebuild_oki_from_data() noexcept {
    bool ok = opts_.read_write
                  ? authority_->rebuild_from_data(dirname_)
                  : authority_->rebuild_memory_from_data(dirname_);
    if (!ok) log_warn("oki runtime rebuild failed");
    if (ok) authority_->state().enable_point_query();
    return ok;
}

// P14e/P14b:加载 keydir 快照 + search.ckpt 分段快照。用 watermark 单趟
// 自门模型取代旧 4-way 成对门：search.ckpt 健康且全段 CRC 通过 → fold_start
// = keydir 水位（快路径）；否则 fold_start = 0（全量 fold，各索引按自身
// ord 水位自门丢弃重叠区，方向安全）。
// S17-4:per-component 协议——读 index.manifest 作为 commit point，按
// 组件 file 路径分别载入 docmap.ckpt / bm25.ckpt / vec.ckpt。S17-5
// 兼容：manifest 缺失但 search.ckpt 存在时触发一次性迁移。
std::expected<Cask::RecoverySnapshots, CaskFault>
Cask::load_recovery_snapshots() {
    RecoverySnapshots recovery;

    bool search_ok = false;
    if (text_) {
        // S18-6：插件 open 注入器——manifest 提示 + dir/host。**所有路径**
        // （含 manifest 缺失/迁移失败的全量 fold 早退）都必须调用：插件的
        // dir_/host_ 在 open 时注入，flush 依赖之；零提示 = 插件降级自建
        // （watermark 0，rebase 置位 → 首次 flush 全量 base）。
        auto open_plugins = [&](const bitcask::Manifest& m) {
            for (auto* p : plugins_) {
                const auto comp = component_of_plugin(p->name());
                if (!comp) continue;
                const auto& entry =
                    m.entries[static_cast<std::size_t>(*comp)];
                plugin::OpenContext ctx;
                ctx.dir = dirname_;
                ctx.host = &plugin_host_;
                // S45-4：分片数 = 共享索引池的分片线程数（分片插件据此预建
                // 每分片单写者结构；恢复重放同样按 key 哈希落分片）。
                ctx.shards = index_shard_count();
                ctx.committed_base_watermark  = entry.base_watermark;
                ctx.committed_chain_watermark = entry.chain_watermark;
                ctx.committed_chain_seq       = entry.chain_seq;
                (void)p->open(ctx);
            }
        };
        // S17-5 兼容：manifest 缺失 + search.ckpt 存在 → 一次性迁移。
        const std::string mpath = dirname_ + "/" +
            std::string(bitcask::kManifestName);
        const std::string old_ckpt = dirname_ + "/" + kSearchCkptName;
        std::error_code ec;
        const bool has_manifest = std::filesystem::exists(bitcask::detail::from_utf8(mpath), ec);
        const bool has_old_ckpt = std::filesystem::exists(bitcask::detail::from_utf8(old_ckpt), ec);
        if (!has_manifest && has_old_ckpt) {
            // 触发迁移：把旧 search.ckpt 用旧路径 load 回来，再分
            // 写到新组件文件 + 写 manifest + 删旧文件。失败 → 全量 fold。
            if (!migrate_legacy_search_ckpt()) {
                open_plugins(bitcask::Manifest{});  // S18-6：零提示注入
                recovery.snap_loaded = false;
                return recovery;
            }
        }
        auto manifest = bitcask::read_manifest(mpath);
        if (!manifest) {
            // manifest 仍不可读（迁移失败/被破坏/新库）→ 全量 fold。
            open_plugins(bitcask::Manifest{});  // S18-6：零提示注入
            recovery.snap_loaded = false;
            return recovery;
        }
        current_manifest_ = *manifest;
        // S14-7：链重放钩子（S18-2：index:: 类型，仅 docmap 消费）。
        index::DocmapReplayHook hook =
            [this, &recovery](
                const std::vector<index::DocmapDeltaRow>& rows,
                const std::vector<index::DocmapDeltaRemoval>& rems,
                std::span<const std::byte> authority_meta) {
                replay_delta_to_authority(rows, rems, authority_meta, recovery);
            };
        const auto hook_arg = recovery.snap_loaded ? hook :
            index::DocmapReplayHook{};
        bool all_components_ok = true;
        std::uint64_t min_chain_wm = UINT64_MAX;
        // S45-5：重放起点 = min(各组件实际覆盖水位)，任一未载入（水位 0）即 0。
        std::uint64_t replay_wm = UINT64_MAX;
        // S18-2：docmap 组件由宿主直载（keydir 链重放钩子只在这里生效）。
        {
            const auto& entry = current_manifest_.entries[0];
            auto dr = index::load_docmap(*docmap_, dirname_,
                                         entry.base_watermark,
                                         entry.chain_seq, hook_arg);
            replay_wm = std::min(replay_wm, dr.loaded ? dr.watermark : 0);
            if (dr.loaded) {
                min_chain_wm = std::min(min_chain_wm, dr.watermark);
                docmap_chain_ = bitcask::ManifestEntry{
                    entry.base_watermark, entry.chain_seq, dr.watermark};
            } else {
                all_components_ok = false;
                docmap_chain_ = bitcask::ManifestEntry{};
            }
        }
        // S18-6：bm25/vec 经插件 open()（组件载入 + 链续接 + 自身 rebase
        // 自管）。健康判据：watermark() == manifest 的 chain_watermark
        // （损坏/缺失 → 插件自降级报 0 ≠ 非零 entry → 判不健康；新库两侧
        // 皆 0 → 健康）。
        open_plugins(current_manifest_);
        for (auto* p : plugins_) {
            const auto comp = component_of_plugin(p->name());
            if (!comp) continue;
            const auto& entry =
                current_manifest_.entries[static_cast<std::size_t>(*comp)];
            const std::uint64_t pw = p->watermark();
            replay_wm = std::min(replay_wm, pw);
            if (pw == entry.chain_watermark) {
                min_chain_wm = std::min(min_chain_wm, pw);
            } else {
                all_components_ok = false;
            }
        }
        // 全组件健康 → 清 legacy 全局 rebase（细粒度标志各插件 open 自管）。
        if (all_components_ok) {
            ckpt_rebase_needed_.store(false, std::memory_order_relaxed);
        }
        if (min_chain_wm == UINT64_MAX) {
            min_chain_wm = 0;  // 没有任何组件成功
        }
        recovery.replay_floor =
            replay_wm == UINT64_MAX ? 0 : plugin::replay_floor_of(replay_wm);
        // 「全组件健康」才走字节水位快路径；任一组件 .prev 回退 → 字节水位
        // 不可信，退全量 fold。保守以 all_components_ok 近似「非 .prev」
        // （load 内部已用 manifest base_wm 校验），再叠加 snap_loaded 双重门。
        bool any_from_prev = false;
        search_ok = all_components_ok && recovery.snap_loaded;
        (void)any_from_prev;
        // S17-4:fold_start = min(chain_watermarks)，各索引自门按其自身 ord
        // 水位丢重叠区；snap_wms 保持原形态，由上层 fold 阶段自门。
        if (search_ok) {
            // min_chain_wm 不回写 snap_wms（保持契约形态，自门已足够）。
        }
    }
    // search 不健康 → 退回全量 fold（snap_loaded=false 让 fold_start=0）。
    if (text_ && !search_ok) {
        recovery.snap_loaded = false;
    }
    return recovery;
}

}  // namespace bitcask
