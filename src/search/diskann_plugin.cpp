// DiskannPlugin 实现（S32-M5）。结构与 ivf_plugin.cpp 同构（sealed 段 +
// 窗口 + DeltaLog + 组件链）；差异仅段类型/文件族/kDiskann 段/建图参数。

#include "bitcask/diskann_plugin.hpp"

#include <sys/time.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "bitcask/detail/int8_kernels.hpp"
#include "bitcask/ckpt_chain.hpp"  // walk_chain / remove_chain_files
#include "bitcask/search_checkpoint.hpp"

namespace bitcask::vec {

namespace sc = bitcask::search;

namespace {

constexpr const char* kDiskannCkptName = "diskann.ckpt";

std::string comp_path(std::string_view dir) {
    return (std::filesystem::path(dir) / kDiskannCkptName).string();
}
std::string bda_path_of(const std::string& fp) {
    return std::filesystem::path(fp).replace_extension(".bda").string();
}

// kDiskann 段布局：gen u64 | count u64 | max_ord u64 | dim u16 | R u32。
constexpr std::size_t kDiskannBlobSize = 8 + 8 + 8 + 2 + 4;

// payload 代号 nonce（同 HnswIndex::ensure_payload_gen 熵源风格）。
std::uint64_t make_gen(const void* self, std::uint64_t salt) {
    std::uint64_t g =
        (static_cast<std::uint64_t>(::time(nullptr)) << 24) ^
        (reinterpret_cast<std::uintptr_t>(self) >> 4) ^ salt;
    if (g == 0) g = 1;
    return g;
}

}  // namespace

DiskannPlugin::DiskannPlugin(const VectorPluginConfig& config,
                     const bm25::DocTable& docs)
    : config_(config), docs_(docs) {
    if (config_.dim > 0) {
        window_.store(make_window(), std::memory_order_release);
    }
}

std::shared_ptr<HnswIndex> DiskannPlugin::make_window() const {
    if (config_.dim == 0) return nullptr;
    HnswConfig hc;
    hc.dim = config_.dim;
    hc.metric = HnswMetric::kDot;  // v1 仅 kDot（cosine 上游归一化）
    hc.inmem_int8 = true;          // 窗口恒 int8-only（分数与 sealed 可比）
    if (config_.hnsw_m != 0) hc.M = config_.hnsw_m;
    if (config_.hnsw_ef_construction != 0) {
        hc.ef_construction = config_.hnsw_ef_construction;
    }
    return std::make_shared<HnswIndex>(hc);
}

// v1 标量归一化（IVF put 路径；AVX2 版共享件抽取挂 M0b/M3 尾）。
std::expected<std::span<const float>, const char*>
DiskannPlugin::normalize_for_write(std::span<const float> input,
                               std::vector<float>& norm_buf) const {
    if (input.empty()) return std::span<const float>{};
    if (config_.dim == 0) {
        return std::unexpected("collection has no vector config");
    }
    if (input.size() != config_.dim) {
        return std::unexpected("vector dim mismatch");
    }
    if (config_.metric == meta::VectorMetric::kCosineNormalized) {
        double sq = 0.0;
        for (const float x : input) sq += static_cast<double>(x) * x;
        if (sq <= 0.0) {
            return std::unexpected("zero vector not allowed under cosine metric");
        }
        const auto inv = static_cast<float>(1.0 / std::sqrt(sq));
        norm_buf.resize(input.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            norm_buf[i] = input[i] * inv;
        }
        return std::span<const float>(norm_buf);
    }
    return input;
}

void DiskannPlugin::insert(std::uint64_t ord, std::span<const float> v) {
    auto w = window_.load(std::memory_order_acquire);
    if (!w || v.size() != config_.dim) return;
    // 重放幂等门（open 覆盖水位之下的事件已在 sealed/链里）。
    if (replay_gate_ != static_cast<std::uint64_t>(-1) &&
        ord <= replay_gate_) {
        return;
    }
    dirty_.store(true, std::memory_order_relaxed);
    delta_.record(ord, v);  // 窗口门内建（S32-M0b）
    const std::size_t before = w->size();
    w->insert(ord, v);
    vec_docs_since_base_ += w->size() - before;
}

std::expected<std::vector<search::SearchHit>, search::SearchError>
DiskannPlugin::search(std::span<const float> query, std::size_t k, std::size_t ef,
                  const meta::MetaFilter* filter) const {
    auto seg = sealed_.load(std::memory_order_acquire);
    auto w   = window_.load(std::memory_order_acquire);
    if (config_.dim == 0) {
        return std::unexpected(search::SearchError::kNoVectorIndex);
    }
    if (query.size() != config_.dim) {
        return std::unexpected(search::SearchError::kVectorDimMismatch);
    }
    // cosine：查询侧归一化（零向量返回空，与 VectorPlugin 同约定）。
    std::vector<float> qn;
    std::span<const float> q = query;
    if (config_.metric == meta::VectorMetric::kCosineNormalized) {
        double sq = 0.0;
        for (const float x : query) sq += static_cast<double>(x) * x;
        if (sq <= 0.0) return std::vector<search::SearchHit>{};
        const auto inv = static_cast<float>(1.0 / std::sqrt(sq));
        qn.resize(query.size());
        for (std::size_t i = 0; i < query.size(); ++i) {
            qn[i] = query[i] * inv;
        }
        q = qn;
    }

    // live 组合（同 VectorPlugin：is_live + meta 过滤）。
    const bm25::DocTable& dt = docs_;
    std::function<bool(std::uint64_t)> live;
    if (filter != nullptr) {
        live = [&dt, filter](std::uint64_t ord) -> bool {
            if (!dt.is_live(ord)) return false;
            return dt.eval_meta(ord, *filter);
        };
    } else {
        live = [&dt](std::uint64_t ord) -> bool { return dt.is_live(ord); };
    }

    // 双路：sealed（ef → beam 宽 L；0 = 段默认 max(2k,64)）+ window
    //（HNSW ef 语义）。
    std::vector<std::pair<float, std::uint64_t>> merged;
    if (seg && seg->opened() && seg->size() > 0) {
        const auto l = static_cast<std::uint32_t>(ef);
        for (const auto& h : seg->search(q, k, l, &live)) {
            merged.push_back({h.score, h.ord});
        }
    }
    if (w && !w->empty()) {
        const std::size_t wef = ef != 0 ? ef : std::max<std::size_t>(k, 64);
        for (const auto& h : w->search(q, k, wef, &live)) {
            merged.push_back({h.score, h.ord});
        }
    }
    std::sort(merged.begin(), merged.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return a.second < b.second;  // 平局 ord 小者（稳定）
              });

    // 归并去重（base 换代瞬间 sealed/window 可能短暂同持一 ord）+ 翻译。
    std::vector<search::SearchHit> hits;
    hits.reserve(k);
    std::uint64_t prev_ord = static_cast<std::uint64_t>(-1);
    for (const auto& [score, ord] : merged) {
        if (ord == prev_ord) continue;
        // 去重需全局视角：k 小，线性查已收 hits 即可。
        bool dup = false;
        for (const auto& h : hits) {
            if (h.ord == ord) { dup = true; break; }
        }
        if (dup) continue;
        prev_ord = ord;
        auto ext = dt.ord_to_ext(ord);
        if (!ext) continue;
        hits.push_back(search::SearchHit{std::move(*ext), ord,
                                         static_cast<double>(score)});
        if (hits.size() >= k) break;
    }
    return hits;
}

void DiskannPlugin::rebuild() {
    // 盘上段死清理推迟到下次 base（build source 内建 live 过滤）；
    // 查询侧 live 已滤，无正确性差。
    dirty_.store(true, std::memory_order_relaxed);
    rebase_needed_.store(true, std::memory_order_relaxed);
}

void DiskannPlugin::on_merge_commit(const plugin::MergeCommitEvent&) {
    if (!enabled()) return;
    if (host_ != nullptr) {
        host_->run_serialized([this] { rebuild(); });
    } else {
        rebuild();
    }
}

std::size_t DiskannPlugin::size() const {
    return sealed_size() + window_size();
}
std::size_t DiskannPlugin::sealed_size() const {
    auto seg = sealed_.load(std::memory_order_acquire);
    return seg && seg->opened() ? static_cast<std::size_t>(seg->size()) : 0;
}
std::size_t DiskannPlugin::window_size() const {
    auto w = window_.load(std::memory_order_acquire);
    return w ? w->size() : 0;
}

// ---- delta 日志（kHnswDelta 通用格式，与 VectorPlugin 位级同构）----

void DiskannPlugin::serialize_delta_log(std::vector<std::byte>& out) const {
    delta_.serialize(config_.dim, out);  // S32-M0b：格式单一真源
}

bool DiskannPlugin::apply_delta_log(std::span<const std::byte> payload) {
    auto w = window_.load(std::memory_order_acquire);
    const std::size_t before = w ? w->size() : 0;
    // 解析走 DeltaLog::parse（S32-M0b 格式单一真源）;重放条数计入 base
    // 窗口（恢复欠账可见，S32-M1 同款）。
    const bool ok = DeltaLog::parse(
        payload, config_.dim,
        [&](std::uint64_t ord, std::span<const float> v) {
            if (w) w->insert(ord, v);
        });
    if (w) vec_docs_since_base_ += w->size() - before;
    return ok;
}

// ---- 组件链 ----

bool DiskannPlugin::save_component_base(std::string_view dir,
                                    std::uint64_t watermark) {
    const std::string fp = comp_path(dir);
    if (config_.dim == 0) {
        std::error_code ec;
        std::filesystem::remove(fp, ec);
        std::filesystem::remove(fp + ".prev", ec);
        std::filesystem::remove(bda_path_of(fp), ec);
        dirty_.store(false, std::memory_order_relaxed);
        return false;
    }
    auto seg = sealed_.load(std::memory_order_acquire);
    auto w   = window_.load(std::memory_order_acquire);

    // 活集枚举（单线程；build 回调阶段只读多线程）。
    struct Entry {
        std::uint32_t idx;
        bool          from_window;
    };
    std::vector<Entry> entries;
    const std::uint64_t sn = seg && seg->opened() ? seg->size() : 0;
    const std::uint32_t wn = w ? static_cast<std::uint32_t>(w->size()) : 0;
    entries.reserve(static_cast<std::size_t>(sn) + wn);
    for (std::uint64_t i = 0; i < sn; ++i) {
        if (docs_.is_live(seg->record_at(i).ord)) {
            entries.push_back({static_cast<std::uint32_t>(i), false});
        }
    }
    for (std::uint32_t id = 0; id < wn; ++id) {
        if (docs_.is_live(w->node_ord(id))) {
            entries.push_back({id, true});
        }
    }

    const std::uint64_t gen = make_gen(this, watermark ^ entries.size());
    const std::uint16_t dim = config_.dim;
    IvfBuildSource src;
    src.count = static_cast<std::uint32_t>(entries.size());
    src.get = [&](std::uint32_t i, std::uint64_t& ord, const float*& vec) {
        const Entry e = entries[i];
        if (e.from_window) {
            ord = w->node_ord(e.idx);
            vec = w->node_vec(e.idx).data();  // thread_local 反量化缓冲
            return;
        }
        const auto r = seg->record_at(e.idx);
        ord = r.ord;
        // sealed 记录反量化（对称 max-abs 量化的往返无损：再量化位级还原）。
        thread_local std::vector<float> buf;
        buf.resize(dim);
        const float factor = r.scale / 127.0f;
        for (std::uint16_t j = 0; j < dim; ++j) {
            buf[j] = static_cast<float>(r.codes[j]) * factor;
        }
        vec = buf.data();
    };
    const std::string bda = bda_path_of(fp);
    if (!DiskannSegment::build(bda, dim, src, config_.diskann_r,
                               config_.diskann_l_build, gen)) {
        return false;
    }
    auto fresh = std::make_shared<DiskannSegment>();
    // 刚 build 完（CRC 回读通过、页缓存热），open 免逐簇再验。
    if (!fresh->open(bda, dim, gen, /*verify_crc=*/false)) {
        return false;
    }

    // diskann.ckpt：rename → .prev + kDiskann 段（交叉校验锚）。
    const std::string prev = fp + ".prev";
    std::error_code ec;
    if (std::filesystem::exists(fp, ec)) {
        std::filesystem::rename(fp, prev, ec);
    }
    sc::SectionWriter sw;
    {
        std::vector<std::byte> b;
        sc::detail::put_u64(b, gen);
        sc::detail::put_u64(b, fresh->size());
        sc::detail::put_u64(b, fresh->max_ord());
        sc::detail::put_u16(b, dim);
        sc::detail::put_u32(b, fresh->r());
        sw.add(sc::CkptSectionType::kDiskann, std::move(b));
    }
    if (!sc::SearchCheckpoint::write(fp, watermark, sw.sections())) {
        return false;  // 旧内存态继续服务；新 biv 配旧 ckpt 由 gen 守卫拒载
    }
    sc::remove_chain_files(fp);
    chain_ = ChainState{watermark, watermark, 1};
    delta_.set_window(watermark);
    clear_delta_log();
    dirty_.store(false, std::memory_order_relaxed);
    // 换代发布：先 sealed 后 window（瞬间双持同 ord 由 search 去重兜住）。
    sealed_.store(std::move(fresh), std::memory_order_release);
    window_.store(make_window(), std::memory_order_release);
    return true;
}

DiskannPlugin::DeltaSaveResult
DiskannPlugin::save_component_delta(std::string_view dir,
                                std::uint64_t watermark) {
    DeltaSaveResult result;
    if (delta_.empty()) {
        dirty_.store(false, std::memory_order_relaxed);
        return result;
    }
    const std::string fp = comp_path(dir);
    const std::uint32_t seq = chain_.next_seq;
    const std::string dpath = fp + ".d" + std::to_string(seq);
    sc::SectionWriter sw;
    {
        std::vector<std::byte> b;
        sc::detail::put_u64(b, chain_.base_gen);
        sc::detail::put_u64(b, chain_.chain_wm);
        sc::detail::put_u32(b, seq);
        sw.add(sc::CkptSectionType::kDeltaInfo, std::move(b));
    }
    {
        std::vector<std::byte> b;
        serialize_delta_log(b);
        sw.add(sc::CkptSectionType::kHnswDelta, std::move(b));
    }
    if (!sc::SearchCheckpoint::write(dpath, watermark, sw.sections())) {
        return result;
    }
    chain_.chain_wm = watermark;
    chain_.next_seq = seq + 1;
    delta_.set_window(watermark);
    clear_delta_log();
    dirty_.store(false, std::memory_order_relaxed);
    result.wrote = true;
    result.new_seq = seq;
    return result;
}

DiskannPlugin::LoadResult
DiskannPlugin::load_component(std::string_view dir,
                          std::uint64_t expected_base_wm,
                          std::uint32_t chain_seq) {
    LoadResult result;
    const std::string fp = comp_path(dir);
    auto lc = sc::SearchCheckpoint::read(fp);
    bool from_prev = false;
    if (lc && lc->watermark != expected_base_wm) lc.reset();
    if (!lc) {
        lc = sc::SearchCheckpoint::read(fp + ".prev");
        if (lc && lc->watermark != expected_base_wm) lc.reset();
        if (lc) from_prev = true;
    }
    auto fail = [&]() {
        result = LoadResult{};
        chain_ = ChainState{};
        delta_.set_window(0);
        return result;
    };
    if (!lc) {
        return fail();
    }

    bool segments_ok = true;
    bool any = false;
    for (const auto& ls : lc->sections) {
        if (!ls.crc_ok) { segments_ok = false; continue; }
        if (ls.type != static_cast<std::uint16_t>(sc::CkptSectionType::kDiskann)) {
            continue;
        }
        if (ls.payload.size() != kDiskannBlobSize) { segments_ok = false; continue; }
        const auto* p = ls.payload.data();
        const std::uint64_t gen   = sc::detail::get_u64(p); p += 8;
        const std::uint64_t count = sc::detail::get_u64(p); p += 8;
        p += 8;  // max_ord：信息留档（段头亦有）
        const std::uint16_t dim   = sc::detail::get_u16(p); p += 2;
        p += 4;  // R：信息留档
        if (dim != config_.dim) { segments_ok = false; continue; }
        auto seg = std::make_shared<DiskannSegment>();
        if (seg->open(bda_path_of(fp), config_.dim, gen) &&
            seg->size() == count) {
            sealed_.store(std::move(seg), std::memory_order_release);
            window_.store(make_window(), std::memory_order_release);
            any = true;
        } else {
            segments_ok = false;
        }
    }
    if (config_.dim > 0 && !any) segments_ok = false;

    std::uint64_t coverage = lc->watermark;
    std::uint32_t next_seq = 1;
    bool chain_ok = true;
    if (segments_ok && !from_prev) {
        const auto walk = sc::walk_chain(
            fp, coverage, coverage, chain_seq, /*unbounded=*/false,
            [&](const sc::LoadedCheckpoint& dc) -> bool {
                for (const auto& dls : dc.sections) {
                    if (!dls.crc_ok) return false;
                }
                for (const auto& dls : dc.sections) {
                    if (dls.type ==
                        static_cast<std::uint16_t>(
                            sc::CkptSectionType::kHnswDelta)) {
                        if (!apply_delta_log(std::span<const std::byte>(
                                dls.payload.data(), dls.payload.size()))) {
                            return false;
                        }
                    }
                }
                return true;
            });
        coverage = walk.coverage;
        next_seq = walk.next_seq;
        chain_ok = walk.ok;
    } else {
        chain_ok = false;
    }
    result.loaded = segments_ok && chain_ok;
    result.watermark = coverage;
    result.from_prev = from_prev;
    result.all_segments_ok = segments_ok && chain_ok;
    if (!result.loaded) return fail();
    chain_ = ChainState{expected_base_wm, coverage, next_seq};
    delta_.set_window(coverage);
    clear_delta_log();
    dirty_.store(false, std::memory_order_relaxed);
    return result;
}

// ---- CaskPlugin flush/open（结构同 VectorPlugin，双门槛 S32-M1 同款）----

plugin::PluginStatus DiskannPlugin::open(const plugin::OpenContext& ctx) {
    dir_.assign(ctx.dir);
    host_ = ctx.host;
    auto r = load_component(dir_, ctx.committed_base_watermark,
                            ctx.committed_chain_seq);
    watermark_ = r.loaded ? r.watermark : 0;
    // 重放幂等门：覆盖水位之下的事件已在 sealed/链（窗口重放）里。
    replay_gate_ = r.loaded ? r.watermark : static_cast<std::uint64_t>(-1);
    rebase_needed_.store(!r.loaded, std::memory_order_relaxed);
    return plugin::PluginStatus::kOk;
}

plugin::FlushResult DiskannPlugin::flush(const plugin::FlushRequest& req) {
    plugin::FlushResult res;
    const bool cap_hit = config_.max_delta_chain > 0 &&
                         chain_.next_seq > config_.max_delta_chain;
    const bool window_hit = config_.rebase_min_docs > 0 &&
                            vec_docs_since_base_ >= config_.rebase_min_docs;
    const bool want_base = req.force_rebase ||
                           rebase_needed_.load(std::memory_order_relaxed) ||
                           cap_hit || window_hit;
    if (want_base) {
        const bool ok = save_component_base(dir_, req.watermark);
        if (config_.dim == 0) {
            rebase_needed_.store(false, std::memory_order_relaxed);
            res.covered_ord = chain_.chain_wm;
        } else if (ok) {
            rebase_needed_.store(false, std::memory_order_relaxed);
            vec_docs_since_base_ = 0;
            res.covered_ord = req.watermark;
        } else {
            res.status = plugin::PluginStatus::kFailed;
            res.covered_ord = chain_.chain_wm;
        }
    } else if (!dirty() || delta_.empty()) {
        dirty_.store(false, std::memory_order_relaxed);
        res.covered_ord = chain_.chain_wm;
    } else {
        auto d = save_component_delta(dir_, req.watermark);
        if (d.wrote) {
            res.covered_ord = req.watermark;
        } else {
            res.status = plugin::PluginStatus::kFailed;
            res.covered_ord = chain_.chain_wm;
        }
    }
    res.generation = chain_.base_gen;
    res.chain_seq  = chain_.next_seq - 1;
    res.chain_wm   = chain_.chain_wm;
    return res;
}

}  // namespace bitcask::vec
