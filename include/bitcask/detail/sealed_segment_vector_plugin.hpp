// SealedSegmentVectorPlugin — 磁盘档向量引擎插件的中间基类（S32-M3/M5）。
//
// === 设计意图 ===
// IVF / DiskANN 两个 VectorEnginePlugin 此前 ~518 行近乎逐字复制（实测
// `diff` 仅 91 行差异）；本类按 Template Method 收口公共骨架：持有
// config_/docs_/window_/sealed_/delta_/chain_ 等所有共驻成员，**实现并
// final** 标记所有共享方法（normalize_for_write / insert / search /
// rebuild / size / flush / open / on_merge_commit / save_component_base /
// load_component / save_component_delta / serialize_delta_log /
// apply_delta_log），仅向派生类暴露**纯虚 hook**（sealed 段类型、文件
// 扩展、ckpt 段型、blob 尺寸、build/open/search）。
//
// === 模板形态 ===
// SealedT = 派生引擎段的类型（IvfSegment / DiskannSegment）。模板让
// sealed_/window_ 的 atomic<shared_ptr<const T>> 静态定型，避免引入
// 「SealedSegmentBase」虚基——段间无公共方法表（除了位级同构的 Hit
// 布局，被 EngineHit 透传吸收）。
//
// === 线程模型（继承自 VectorEnginePlugin）===
// 单写者（reducer：insert/flush/open）+ 多读者（search）。sealed_/
// window_ 均 atomic<shared_ptr>：读端 acquire 快照、写端 release 换指针
// 旧代引用计数续命。save_component_base 末尾「先 sealed 后 window」发布
// 顺序保留——瞬间双持同 ord 由 search 归并去重兜住（与 VectorPlugin 同
// 协议，未改动）。
//
// === 持久化面（与 IvfPlugin/DiskannPlugin 旧实现逐字节兼容）===
//   base   = `<ckpt_name>` + 段侧车（.biv / .bda）——段头 gen 与 ckpt 段
//           交叉校验，崩溃窗口语义与原 VectorPlugin 同款。
//   delta  = `<ckpt_name>.d<seq>`（kDeltaInfo + kHnswDelta 通用向量插入
//           日志），恢复重放进 window。
// 双门槛（链长上限 / rebase_min_docs）逻辑收敛在 flush() 里。
//
// === 与 HNSW VectorPlugin 的差 ===
// VectorPlugin（hnsw 引擎）无 sealed 段 + window 双路径（HNSW 即是 window
// 本身），其实现不在本骨架下——保留独立基类 VectorEnginePlugin 即可，
// 本中间类只承担 IVF / DiskANN 的共性。

#pragma once

#include "bitcask/ckpt_chain.hpp"
#include "bitcask/component_ckpt.hpp"
#include "bitcask/doc_table.hpp"
#include "bitcask/hnsw.hpp"
#include "bitcask/ivf_rq.hpp"
#include "bitcask/meta_filter.hpp"
#include "bitcask/plugin_api.hpp"
#include "bitcask/search_checkpoint.hpp"
#include "bitcask/search_types.hpp"
#include "bitcask/vector_delta_log.hpp"
#include "bitcask/vector_engine_plugin.hpp"
#include "bitcask/vector_plugin_config.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include "bitcask/detail/path_utf8.hpp"

namespace bitcask::vec::detail {

// 段级查询命中（位级同构于 IvfSegment::Hit / DiskannSegment::Hit ——
// 后两者结构体 {ord u64, score f32} 布局完全一致，本类用同构 POD 透传）。
struct EngineHit {
    std::uint64_t ord;
    float         score;
};
static_assert(sizeof(EngineHit) == 16);
static_assert(alignof(EngineHit) == 8);

template <typename SealedT>
class SealedSegmentVectorPlugin : public VectorEnginePlugin {
public:
    SealedSegmentVectorPlugin(const VectorPluginConfig& config,
                              const bm25::DocTable& docs)
        : config_(config), docs_(docs) {
        if (config_.dim > 0) {
            window_.store(make_window(), std::memory_order_release);
        }
    }

    SealedSegmentVectorPlugin(const SealedSegmentVectorPlugin&) = delete;
    SealedSegmentVectorPlugin&
    operator=(const SealedSegmentVectorPlugin&) = delete;

    // ---- VectorEnginePlugin（最终，派生类不可覆盖）----

    [[nodiscard]] bool enabled() const noexcept final {
        return config_.dim > 0;
    }
    [[nodiscard]] std::uint16_t dim() const noexcept final {
        return config_.dim;
    }

    [[nodiscard]] std::expected<std::span<const float>, const char*>
    normalize_for_write(std::span<const float> input,
                        std::vector<float>& norm_buf) const final;

    void insert(std::uint64_t ord, std::span<const float> v) final;

    [[nodiscard]] std::expected<std::vector<search::SearchHit>,
                                search::SearchError>
    search(std::span<const float> query, std::size_t k, std::size_t ef,
           const meta::MetaFilter* filter) const final;

    void rebuild() final;

    [[nodiscard]] std::size_t size() const final {
        return sealed_size() + window_size();
    }
    [[nodiscard]] bool dirty() const noexcept final {
        return dirty_.load(std::memory_order_relaxed);
    }
    void force_rebase() noexcept final {
        rebase_needed_.store(true, std::memory_order_relaxed);
    }

    // ---- plugin::CaskPlugin（name() 为 hook，余 final）----

    plugin::PluginStatus open(const plugin::OpenContext& ctx) final;
    [[nodiscard]] std::uint64_t watermark() const final { return watermark_; }
    plugin::PluginStatus close() final { return plugin::PluginStatus::kOk; }
    void on_put(const plugin::PutEvent& e, plugin::PreparedPtr) final {
        if (e.doc && !e.doc->vec.empty()) {
            insert(e.ord, e.doc->vec);
        }
    }
    void on_delete(const plugin::DeleteEvent&) final {}
    plugin::FlushResult flush(const plugin::FlushRequest& req) final;
    void on_merge_commit(const plugin::MergeCommitEvent&) final;

    // ---- 组件链（final；语义同 VectorPlugin 同名方法）----

    using DeltaSaveResult = ckpt::DeltaSaveResult;
    using LoadResult = ckpt::LoadResult;
    using ChainState = ckpt::ChainState;

    [[nodiscard]] bool save_component_base(std::string_view dir,
                                           std::uint64_t watermark);
    [[nodiscard]] DeltaSaveResult save_component_delta(
        std::string_view dir, std::uint64_t watermark);
    [[nodiscard]] LoadResult load_component(std::string_view dir,
                                            std::uint64_t expected_base_wm,
                                            std::uint32_t chain_seq);
    [[nodiscard]] ChainState chain_state() const { return chain_; }

    // 观测（测试用）。
    [[nodiscard]] std::size_t sealed_size() const;
    [[nodiscard]] std::size_t window_size() const;

protected:
    // ---- 纯虚 hook：派生类按引擎语义实现 ----

    // 例："ivf.ckpt" / "diskann.ckpt"。comp_path = dir / ckpt_name。
    [[nodiscard]] virtual const char* ckpt_name() const noexcept = 0;

    // 例：".biv" / ".bda"。base = ckpt 路径 → 返回段侧车路径。
    [[nodiscard]] virtual std::string
    segment_path_ext(std::string_view base) const = 0;

    // Ckpt 段型（base ckpt 内的唯一引擎段）。
    [[nodiscard]] virtual search::CkptSectionType
    section_type() const noexcept = 0;

    // base ckpt 段字节尺寸 = 8(gen) + 8(count) + 8(max_ord) + 2(dim) +
    // 4(tail) = 30。
    [[nodiscard]] virtual std::size_t blob_size() const noexcept = 0;

    // 段打开（封装 IvfSegment::open / DiskannSegment::open）。
    [[nodiscard]] virtual std::shared_ptr<SealedT>
    open_sealed(std::string_view path, std::uint16_t dim,
                std::uint64_t expected_gen, bool verify_crc) const = 0;

    // 段构建（封装 IvfSegment::build / DiskannSegment::build——钩子内部
    // 从 config_ 抽取引擎专属参数：ivf_nlist / diskann_r、diskann_l_build）。
    [[nodiscard]] virtual bool
    build_sealed(std::string_view path, std::uint16_t dim,
                 const IvfBuildSource& src, std::uint64_t gen) const = 0;

    // 段级搜索：唯一真正分叉的逻辑。ef 语义由派生类映射（IVF = nprobe,
    // ef=0→config_.ivf_nprobe；DiskANN = beam 宽 L）。
    [[nodiscard]] virtual std::vector<EngineHit>
    search_sealed(const SealedT& seg, std::span<const float> q,
                  std::size_t k, std::size_t ef,
                  const std::function<bool(std::uint64_t)>* live) const = 0;

    // 写 blob 尾部 u32（IVF = nlist；DiskANN = R）。
    virtual void write_blob_tail(std::vector<std::byte>& b,
                                 const SealedT& seg) const = 0;

    // ---- 共享成员（派生类按需访问）----

    // payload 代号 nonce（与 HnswIndex::ensure_payload_gen 熵源风格一致）。
    [[nodiscard]] std::uint64_t make_gen(std::uint64_t salt) const noexcept {
        std::uint64_t g =
            (static_cast<std::uint64_t>(::time(nullptr)) << 24) ^
            (reinterpret_cast<std::uintptr_t>(this) >> 4) ^ salt;
        if (g == 0) g = 1;
        return g;
    }

    // ckpt 段族路径助手（双方共享）。
    [[nodiscard]] std::string comp_path(std::string_view dir) const {
        return bitcask::detail::to_utf8(bitcask::detail::from_utf8(dir) / ckpt_name());
    }

    // kHnswDelta 通用插入日志（count u64 | dim u16 | 每条 ord u64 + f32[dim]）。
    void serialize_delta_log(std::vector<std::byte>& out) const {
        delta_.serialize(config_.dim, out);
    }
    [[nodiscard]] bool apply_delta_log(std::span<const std::byte> payload);
    void clear_delta_log() { delta_.clear(); }

    // HNSW 窗口（IVF / DiskANN 共用——设计 §5.1「FreshDiskANN 最难的
    // RAM 侧临时索引本库已有」）。
    [[nodiscard]] std::shared_ptr<HnswIndex> make_window() const;

    VectorPluginConfig    config_;
    const bm25::DocTable& docs_;
    std::atomic<std::shared_ptr<const SealedT>> sealed_;
    std::atomic<std::shared_ptr<HnswIndex>>     window_;

    std::atomic<bool> dirty_{true};
    std::atomic<bool> rebase_needed_{true};
    // 重放幂等门：open 载入覆盖水位之下的事件已在 sealed/链里（宿主从
    // min(全插件水位) 起 fold，本插件跳过 ord ≤ 此值；uint64(-1) = 无）。
    std::uint64_t replay_gate_ = static_cast<std::uint64_t>(-1);
    DeltaLog delta_;
    // S32-M1 同款：自 base 以来入窗向量数（恢复链重放代价的直接度量）。
    std::uint64_t vec_docs_since_base_ = 0;
    ChainState chain_{};
    std::string dir_;
    plugin::PluginHost* host_ = nullptr;
    std::uint64_t watermark_ = 0;
};

// ---- 实现 ----

template <typename SealedT>
std::shared_ptr<HnswIndex>
SealedSegmentVectorPlugin<SealedT>::make_window() const {
    if (config_.dim == 0) return nullptr;
    HnswConfig hc;
    hc.dim = config_.dim;
    hc.metric = HnswMetric::kDot;
    hc.inmem_int8 = true;  // 窗口恒 int8-only（分数与 sealed 可比）
    if (config_.hnsw_m != 0) hc.M = config_.hnsw_m;
    if (config_.hnsw_ef_construction != 0) {
        hc.ef_construction = config_.hnsw_ef_construction;
    }
    return std::make_shared<HnswIndex>(hc);
}

template <typename SealedT>
std::expected<std::span<const float>, const char*>
SealedSegmentVectorPlugin<SealedT>::normalize_for_write(
    std::span<const float> input, std::vector<float>& norm_buf) const {
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

template <typename SealedT>
void SealedSegmentVectorPlugin<SealedT>::insert(std::uint64_t ord,
                                                std::span<const float> v) {
    auto w = window_.load(std::memory_order_acquire);
    if (!w || v.size() != config_.dim) return;
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

template <typename SealedT>
std::expected<std::vector<search::SearchHit>, search::SearchError>
SealedSegmentVectorPlugin<SealedT>::search(std::span<const float> query,
                                           std::size_t k, std::size_t ef,
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

    // 双路：sealed（ef 由派生类映射为 nprobe / beam-L）+ window（HNSW）。
    std::vector<std::pair<float, std::uint64_t>> merged;
    if (seg && seg->opened() && seg->size() > 0) {
        for (auto& h : search_sealed(*seg, q, k, ef, &live)) {
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
                  return a.second < b.second;
              });

    std::vector<search::SearchHit> hits;
    hits.reserve(k);
    std::uint64_t prev_ord = static_cast<std::uint64_t>(-1);
    for (const auto& [score, ord] : merged) {
        if (ord == prev_ord) continue;
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

template <typename SealedT>
void SealedSegmentVectorPlugin<SealedT>::rebuild() {
    dirty_.store(true, std::memory_order_relaxed);
    rebase_needed_.store(true, std::memory_order_relaxed);
}

template <typename SealedT>
void SealedSegmentVectorPlugin<SealedT>::on_merge_commit(
    const plugin::MergeCommitEvent&) {
    if (!enabled()) return;
    if (host_ != nullptr) {
        host_->run_serialized([this] { rebuild(); });
    } else {
        rebuild();
    }
}

template <typename SealedT>
std::size_t SealedSegmentVectorPlugin<SealedT>::sealed_size() const {
    auto seg = sealed_.load(std::memory_order_acquire);
    return seg && seg->opened() ? static_cast<std::size_t>(seg->size()) : 0;
}

template <typename SealedT>
std::size_t SealedSegmentVectorPlugin<SealedT>::window_size() const {
    auto w = window_.load(std::memory_order_acquire);
    return w ? w->size() : 0;
}

template <typename SealedT>
bool SealedSegmentVectorPlugin<SealedT>::apply_delta_log(
    std::span<const std::byte> payload) {
    auto w = window_.load(std::memory_order_acquire);
    const std::size_t before = w ? w->size() : 0;
    const bool ok = DeltaLog::parse(
        payload, config_.dim,
        [&](std::uint64_t ord, std::span<const float> v) {
            if (w) w->insert(ord, v);
        });
    if (w) vec_docs_since_base_ += w->size() - before;
    return ok;
}

template <typename SealedT>
bool SealedSegmentVectorPlugin<SealedT>::save_component_base(
    std::string_view dir, std::uint64_t watermark) {
    namespace sc = bitcask::search;
    const std::string fp = comp_path(dir);
    if (config_.dim == 0) {
        std::error_code ec;
        std::filesystem::remove(fp, ec);
        std::filesystem::remove(fp + ".prev", ec);
        std::filesystem::remove(segment_path_ext(fp), ec);
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

    const std::uint64_t gen = make_gen(watermark ^ entries.size());
    const std::uint16_t dim = config_.dim;
    IvfBuildSource src;
    src.count = static_cast<std::uint32_t>(entries.size());
    src.get = [&](std::uint32_t i, std::uint64_t& ord, const float*& vec) {
        const Entry e = entries[i];
        if (e.from_window) {
            ord = w->node_ord(e.idx);
            vec = w->node_vec(e.idx).data();
            return;
        }
        const auto r = seg->record_at(e.idx);
        ord = r.ord;
        thread_local std::vector<float> buf;
        buf.resize(dim);
        const float factor = r.scale / 127.0f;
        for (std::uint16_t j = 0; j < dim; ++j) {
            buf[j] = static_cast<float>(r.codes[j]) * factor;
        }
        vec = buf.data();
    };
    const std::string seg_path = segment_path_ext(fp);
    if (!build_sealed(seg_path, dim, src, gen)) {
        return false;
    }
    auto fresh = open_sealed(seg_path, dim, gen, /*verify_crc=*/false);
    if (!fresh) {
        return false;
    }

    // ckpt：rename → .prev + 引擎段（gen 守卫同 kHnsw payload 语义）。
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
        write_blob_tail(b, *fresh);
        sw.add(section_type(), std::move(b));
    }
    if (!sc::SearchCheckpoint::write(fp, watermark, sw.sections())) {
        return false;  // 旧内存态继续服务；新段配旧 ckpt 由 gen 守卫拒载
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

template <typename SealedT>
typename SealedSegmentVectorPlugin<SealedT>::DeltaSaveResult
SealedSegmentVectorPlugin<SealedT>::save_component_delta(
    std::string_view dir, std::uint64_t watermark) {
    namespace sc = bitcask::search;
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

template <typename SealedT>
typename SealedSegmentVectorPlugin<SealedT>::LoadResult
SealedSegmentVectorPlugin<SealedT>::load_component(
    std::string_view dir, std::uint64_t expected_base_wm,
    std::uint32_t chain_seq) {
    namespace sc = bitcask::search;
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

    const auto want_type = static_cast<std::uint16_t>(section_type());
    bool segments_ok = true;
    bool any = false;
    for (const auto& ls : lc->sections) {
        if (!ls.crc_ok) { segments_ok = false; continue; }
        if (ls.type != want_type) continue;
        if (ls.payload.size() != blob_size()) {
            segments_ok = false;
            continue;
        }
        const auto* p = ls.payload.data();
        const std::uint64_t gen   = sc::detail::get_u64(p); p += 8;
        const std::uint64_t count = sc::detail::get_u64(p); p += 8;
        p += 8;  // max_ord：信息留档（段头亦有）
        const std::uint16_t dim   = sc::detail::get_u16(p); p += 2;
        p += 4;  // nlist / R：信息留档
        if (dim != config_.dim) { segments_ok = false; continue; }
        auto sopen = open_sealed(segment_path_ext(fp), config_.dim, gen,
                                 /*verify_crc=*/true);
        if (sopen && sopen->size() == count) {
            sealed_.store(std::move(sopen), std::memory_order_release);
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

template <typename SealedT>
plugin::PluginStatus
SealedSegmentVectorPlugin<SealedT>::open(const plugin::OpenContext& ctx) {
    dir_.assign(ctx.dir);
    host_ = ctx.host;
    auto r = load_component(dir_, ctx.committed_base_watermark,
                            ctx.committed_chain_seq);
    watermark_ = r.loaded ? r.watermark : 0;
    replay_gate_ = r.loaded ? r.watermark : static_cast<std::uint64_t>(-1);
    rebase_needed_.store(!r.loaded, std::memory_order_relaxed);
    return plugin::PluginStatus::kOk;
}

template <typename SealedT>
plugin::FlushResult
SealedSegmentVectorPlugin<SealedT>::flush(const plugin::FlushRequest& req) {
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

}  // namespace bitcask::vec::detail