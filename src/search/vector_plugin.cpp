// VectorPlugin 实现（S18-3）。函数体自 SearchLayer 向量域与
// Cask::prepare_vector 平移——行为与文件格式逐字节不变，只换持有方。

#include "bitcask/vector_plugin.hpp"
#include "bitcask/search_checkpoint.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <system_error>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#endif

namespace bitcask::vec {

namespace sc = bitcask::search;

namespace {

constexpr const char* kVecCkptName = "vec.ckpt";

std::string comp_path(std::string_view dir) {
    return (std::filesystem::path(dir) / kVecCkptName).string();
}

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("avx2,fma")))
inline double sum_sq_avx2(const float* v, std::size_t n) noexcept {
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    std::size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        const __m256 a0 = _mm256_loadu_ps(v + i);
        const __m256 a1 = _mm256_loadu_ps(v + i + 8);
        acc0 = _mm256_fmadd_ps(a0, a0, acc0);
        acc1 = _mm256_fmadd_ps(a1, a1, acc1);
    }
    if (i + 8 <= n) {
        const __m256 a = _mm256_loadu_ps(v + i);
        acc0 = _mm256_fmadd_ps(a, a, acc0);
        i += 8;
    }
    __m256 s = _mm256_add_ps(acc0, acc1);
    __m128 lo = _mm256_castps256_ps128(s);
    __m128 hi = _mm256_extractf128_ps(s, 1);
    __m128 s128 = _mm_add_ps(lo, hi);
    s128 = _mm_hadd_ps(s128, s128);
    s128 = _mm_hadd_ps(s128, s128);
    double sq = static_cast<double>(_mm_cvtss_f32(s128));
    for (; i < n; ++i) sq += static_cast<double>(v[i]) * v[i];
    return sq;
}

__attribute__((target("avx2,fma")))
inline void scale_avx2(float* dst, const float* src, float inv, std::size_t n) noexcept {
    const __m256 vinv = _mm256_set1_ps(inv);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 a = _mm256_loadu_ps(src + i);
        _mm256_storeu_ps(dst + i, _mm256_mul_ps(a, vinv));
    }
    for (; i < n; ++i) dst[i] = src[i] * inv;
}

__attribute__((target("avx512f")))
inline double sum_sq_avx512(const float* v, std::size_t n) noexcept {
    __m512 a0 = _mm512_setzero_ps(), a1 = _mm512_setzero_ps();
    std::size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        const __m512 x0 = _mm512_loadu_ps(v + i);
        const __m512 x1 = _mm512_loadu_ps(v + i + 16);
        a0 = _mm512_fmadd_ps(x0, x0, a0);
        a1 = _mm512_fmadd_ps(x1, x1, a1);
    }
    __m512 s = _mm512_add_ps(a0, a1);
    double sq = static_cast<double>(_mm512_reduce_add_ps(s));
    for (; i < n; ++i) sq += static_cast<double>(v[i]) * v[i];
    return sq;
}

__attribute__((target("avx512f")))
inline void scale_avx512(float* dst, const float* src, float inv, std::size_t n) noexcept {
    const __m512 vinv = _mm512_set1_ps(inv);
    std::size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        __m512 a = _mm512_loadu_ps(src + i);
        _mm512_storeu_ps(dst + i, _mm512_mul_ps(a, vinv));
    }
    for (; i < n; ++i) dst[i] = src[i] * inv;
}
#endif

// 归一化两段(SIMD)：sq = Σ v*v 用 double 累加保留标量版精度契约；
// 缩放 v *= inv 用 float 乘。运行时 AVX-512F > AVX2/FMA > 标量三档兜底。
inline double sum_sq(const float* v, std::size_t n) noexcept {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    if (n >= 16 && __builtin_cpu_supports("avx512f")) {
        return sum_sq_avx512(v, n);
    }
    if (n >= 8 && __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
        return sum_sq_avx2(v, n);
    }
#endif
    double sq = 0.0;
    for (std::size_t i = 0; i < n; ++i) sq += static_cast<double>(v[i]) * v[i];
    return sq;
}

inline void scale_query(float* dst, const float* src, float inv, std::size_t n) noexcept {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    if (n >= 16 && __builtin_cpu_supports("avx512f")) {
        scale_avx512(dst, src, inv, n);
        return;
    }
    if (n >= 8 && __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
        scale_avx2(dst, src, inv, n);
        return;
    }
#endif
    for (std::size_t i = 0; i < n; ++i) dst[i] = src[i] * inv;
}

}  // namespace

VectorPlugin::VectorPlugin(const VectorPluginConfig& config,
                           const bm25::DocTable& docs)
    : config_(config), docs_(docs) {
    // V3.3：向量配置存在时创建 HNSW。metric 映射：cosine 已在写入端
    // 归一化 → kDot；kDot → kDot；kL2 → kL2。
    if (config_.dim > 0) {
        HnswConfig hc;
        hc.dim = config_.dim;
        hc.metric = config_.metric == meta::VectorMetric::kL2
                        ? HnswMetric::kL2
                        : HnswMetric::kDot;
        hc.inmem_int8 = config_.inmem_int8;  // P5b
        // S13-D11：建图参数透传（0 = 保持 HnswConfig 默认）。rebuild
        // 复用 old->config() → 本处设置对重建图同样生效。
        if (config_.hnsw_m > 0) hc.M = config_.hnsw_m;
        if (config_.hnsw_ef_construction > 0) {
            hc.ef_construction = config_.hnsw_ef_construction;
        }
        hnsw_.store(std::make_shared<HnswIndex>(hc),
                    std::memory_order_release);
    }
}

std::expected<std::span<const float>, const char*>
VectorPlugin::normalize_for_write(std::span<const float> input,
                                  std::vector<float>& norm_buf) const {
    // 原 Cask::prepare_vector（V3.1：存储即归一化，merge/恢复不再重算）。
    if (input.empty()) return std::span<const float>{};
    if (config_.dim == 0) {
        return std::unexpected("collection has no vector config");
    }
    if (input.size() != config_.dim) {
        return std::unexpected("vector dim mismatch");
    }
    if (config_.metric == meta::VectorMetric::kCosineNormalized) {
        const double sq = sum_sq(input.data(), input.size());
        if (sq <= 0.0) {
            return std::unexpected("zero vector not allowed under cosine metric");
        }
        const auto inv = static_cast<float>(1.0 / std::sqrt(sq));
        norm_buf.resize(input.size());
        scale_query(norm_buf.data(), input.data(), inv, input.size());
        return std::span<const float>(norm_buf);
    }
    return input;
}

void VectorPlugin::insert(std::uint64_t ord, std::span<const float> v) {
    // 防御：无图 / dim 不符直接忽略（不崩）。正常路径写入端已校验 dim。
    auto hnsw = hnsw_.load(std::memory_order_acquire);
    if (!hnsw || v.size() != config_.dim) return;
    dirty_.store(true, std::memory_order_relaxed);  // S14-3
    // S14-4：窗口插入日志（图无不可变旧段，delta 用插入日志重放）。只记
    // 窗口水位之上的（fold 重叠区的重放向量已在链里，S18-1 门限语义）。
    if (ord >= delta_window_wm_) {
        delta_vecs_.emplace_back(ord, std::vector<float>(v.begin(), v.end()));
    }
    hnsw->insert(ord, v);
}

std::expected<std::vector<search::SearchHit>, search::SearchError>
VectorPlugin::search(std::span<const float> query, std::size_t k,
                     std::size_t ef, const meta::MetaFilter* filter) const {
    // V3.5：查询开头取一次图快照指针——与 rebuild 的换指针并发安全。
    auto hnsw = hnsw_.load(std::memory_order_acquire);
    if (!hnsw) {
        return std::unexpected(search::SearchError::kNoVectorIndex);
    }
    if (query.size() != config_.dim) {
        return std::unexpected(search::SearchError::kVectorDimMismatch);
    }
    // cosine：查询向量同样入口归一化（hnsw-design §1）；零向量无方向，
    // 返回空结果（写入端零向量被拒，查询端宽容）。
    std::vector<float> qn;
    std::span<const float> q = query;
    if (config_.metric == meta::VectorMetric::kCosineNormalized) {
        const double sq = sum_sq(query.data(), query.size());
        if (sq <= 0.0) return std::vector<search::SearchHit>{};
        const auto inv = static_cast<float>(1.0 / std::sqrt(sq));
        qn.resize(query.size());
        scale_query(qn.data(), query.data(), inv, query.size());
        q = qn;
    }
    if (ef == 0) ef = std::max<std::size_t>(k, 64);

    // V5：filter 与 is_live 组合为 HNSW live callback——被拒节点从图遍历
    // 源头就不入候选集。空 meta blob 一律不通过。
    const bm25::DocTable& dt = docs_;
    std::function<bool(std::uint64_t)> live;
    if (filter) {
        live = [&dt, filter](std::uint64_t ord) -> bool {
            if (!dt.is_live(ord)) return false;
            return dt.eval_meta(ord, *filter);  // S13-P8：锁内求值
        };
    } else {
        live = [&dt](std::uint64_t ord) -> bool {
            return dt.is_live(ord);
        };
    }
    auto raw = hnsw->search(q, k, ef, &live);

    std::vector<search::SearchHit> hits;
    hits.reserve(raw.size());
    for (auto& h : raw) {
        auto ext_id = dt.ord_to_ext(h.ord);
        if (!ext_id) continue;
        hits.push_back(search::SearchHit{std::move(*ext_id), h.ord,
                                         static_cast<double>(h.score)});
    }
    return hits;
}

void VectorPlugin::rebuild() {
    auto old = hnsw_.load(std::memory_order_acquire);
    if (!old) return;
    dirty_.store(true, std::memory_order_relaxed);  // S14-3
    // S14-4/S18-6：图重建后旧链插入日志语义不再成立 → 自身 rebase。
    rebase_needed_.store(true, std::memory_order_relaxed);
    // S13-P8：clone_live 结构化拷贝替代从零重插——O(节点+边)，无距离计算。
    auto fresh = old->clone_live(
        [this](std::uint64_t ord) { return docs_.is_live(ord); });
    hnsw_.store(std::move(fresh), std::memory_order_release);
}

std::size_t VectorPlugin::size() const {
    auto hnsw = hnsw_.load(std::memory_order_acquire);
    return hnsw ? hnsw->size() : 0;
}

void VectorPlugin::serialize_delta_log(std::vector<std::byte>& out) const {
    sc::detail::put_u64(out, static_cast<std::uint64_t>(delta_vecs_.size()));
    sc::detail::put_u16(out, config_.dim);
    for (const auto& [ord, v] : delta_vecs_) {
        sc::detail::put_u64(out, ord);
        out.insert(out.end(),
            reinterpret_cast<const std::byte*>(v.data()),
            reinterpret_cast<const std::byte*>(v.data()) +
                v.size() * sizeof(float));
    }
}

bool VectorPlugin::apply_delta_log(std::span<const std::byte> payload) {
    const auto* p = payload.data();
    const auto* end = p + payload.size();
    if (end - p < 10) return false;
    std::uint64_t cnt = sc::detail::get_u64(p); p += 8;
    std::uint16_t dim = sc::detail::get_u16(p); p += 2;
    if (dim != config_.dim) return false;
    auto hnsw = hnsw_.load(std::memory_order_acquire);
    const std::size_t vb = static_cast<std::size_t>(dim) * sizeof(float);
    std::vector<float> v(dim);
    for (std::uint64_t i = 0; i < cnt; ++i) {
        if (end - p < static_cast<std::ptrdiff_t>(8 + vb)) {
            return false;
        }
        const std::uint64_t ord = sc::detail::get_u64(p); p += 8;
        std::memcpy(v.data(), p, vb);
        p += vb;
        // 直插（不入窗口日志、不标脏——链内容本就已持久化）。insert 自带
        // ord 水位幂等门。
        if (hnsw) {
            hnsw->insert(ord, std::span<const float>(v.data(), dim));
        }
    }
    return p == end;
}

bool VectorPlugin::load_graph_section(std::span<const std::byte> payload,
                                      const std::string& vec_path,
                                      const std::string& qc_path) {
    auto cur = hnsw_.load(std::memory_order_acquire);
    if (!cur) return false;
    auto fresh = std::make_shared<HnswIndex>(cur->config());
    const auto* raw = reinterpret_cast<const std::uint8_t*>(payload.data());
    if (!fresh->deserialize({raw, payload.size()})) return false;
    // V7：mmap vec payload；inmem_int8 无 payload。S14-8：qc8 码字自门。
    if ((fresh->config().inmem_int8 || fresh->load_vec_payload(vec_path)) &&
        fresh->load_qc_payload(qc_path)) {
        hnsw_.store(std::move(fresh), std::memory_order_release);
        return true;
    }
    return false;
}

bool VectorPlugin::save_component_base(std::string_view dir,
                                       std::uint64_t watermark) {
    const std::string fp = comp_path(dir);
    if (config_.dim == 0) {
        // 无 hnsw 配置：清掉残留（防御性，不阻断）。脏位照清——否则 vec
        // 恒脏使 any_dirty 永真，静止后的收尾 ckpt 永远走不到 base 路径
        // （旧 save_components_base 尾部即无条件清）。
        std::error_code ec;
        std::filesystem::remove(fp, ec);
        std::filesystem::remove(fp + ".prev", ec);
        std::filesystem::remove(
            std::filesystem::path(fp).replace_extension(".vec"), ec);
        std::filesystem::remove(
            std::filesystem::path(fp).replace_extension(".qc8"), ec);
        dirty_.store(false, std::memory_order_relaxed);
        return false;
    }
    const std::string prev = fp + ".prev";
    std::error_code ec;
    if (std::filesystem::exists(fp, ec)) {
        std::filesystem::rename(fp, prev, ec);
    }
    std::vector<sc::CkptSection> secs;
    std::vector<std::uint8_t> buf;
    auto hnsw = hnsw_.load(std::memory_order_acquire);
    if (hnsw) {
        const std::string vec_path =
            std::filesystem::path(fp).replace_extension(".vec").string();
        const std::string qc_path =
            std::filesystem::path(fp).replace_extension(".qc8").string();
        if (hnsw->save_vec_payload(vec_path) &&
            hnsw->save_qc_payload(qc_path)) {
            if (hnsw->serialize(buf)) {
                secs.push_back(sc::CkptSection{
                    static_cast<std::uint16_t>(sc::CkptSectionType::kHnsw), 0,
                    std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(buf.data()),
                        buf.size())});
            }
        }
    }
    const bool ok = !secs.empty() &&
        sc::SearchCheckpoint::write(fp, watermark, secs);
    if (!ok) return false;
    // 链坍缩：清 .d 链（连续序号 + 8 空洞 orphan 扫尾），重置链状态。
    std::uint32_t misses = 0;
    for (std::uint32_t i = 1; misses < 8; ++i) {
        std::error_code ec2;
        if (std::filesystem::remove(fp + ".d" + std::to_string(i), ec2)) {
            misses = 0;
        } else {
            ++misses;
        }
    }
    chain_ = ChainState{watermark, watermark, 1};
    // S18-1：base 落成——窗口日志全量被 base 覆盖 → 清空；窗口推进。
    delta_window_wm_ = watermark;
    delta_vecs_.clear();
    dirty_.store(false, std::memory_order_relaxed);
    return true;
}

VectorPlugin::DeltaSaveResult
VectorPlugin::save_component_delta(std::string_view dir,
                                   std::uint64_t watermark) {
    DeltaSaveResult result;
    // 插入日志为空 → 跳过本组件写入（与旧 save_components_delta 一致）。
    // 脏位照清（旧代码 dirty_mask[2] 为真即清，与是否写入无关）——防
    // 空链的 vec 恒脏卡死 any_dirty 决策。
    if (delta_vecs_.empty()) {
        dirty_.store(false, std::memory_order_relaxed);
        return result;
    }
    const std::string fp = comp_path(dir);
    const std::uint32_t seq = chain_.next_seq;
    const std::string dpath = fp + ".d" + std::to_string(seq);
    std::vector<sc::CkptSection> secs;
    std::vector<std::vector<std::byte>> bufs;
    auto add = [&](sc::CkptSectionType t, std::vector<std::byte> b) {
        bufs.push_back(std::move(b));
        secs.push_back(sc::CkptSection{
            static_cast<std::uint16_t>(t), 0,
            std::span<const std::byte>(bufs.back().data(),
                                       bufs.back().size())});
    };
    // kDeltaInfo：链校验三元组。
    {
        std::vector<std::byte> b;
        sc::detail::put_u64(b, chain_.base_gen);
        sc::detail::put_u64(b, chain_.chain_wm);
        sc::detail::put_u32(b, seq);
        add(sc::CkptSectionType::kDeltaInfo, std::move(b));
    }
    {
        std::vector<std::byte> b;
        serialize_delta_log(b);
        add(sc::CkptSectionType::kHnswDelta, std::move(b));
    }
    if (!sc::SearchCheckpoint::write(dpath, watermark, secs)) return result;
    chain_.chain_wm = watermark;
    chain_.next_seq = seq + 1;
    delta_window_wm_ = watermark;
    delta_vecs_.clear();      // S18-1：写入成功才清
    dirty_.store(false, std::memory_order_relaxed);
    result.wrote = true;
    result.new_seq = seq;
    return result;
}

VectorPlugin::LoadResult
VectorPlugin::load_component(std::string_view dir,
                             std::uint64_t expected_base_wm,
                             std::uint32_t chain_seq) {
    LoadResult result;
    const std::string fp = comp_path(dir);
    const std::string prev_path = fp + ".prev";
    auto lc = sc::SearchCheckpoint::read(fp);
    bool from_prev = false;
    if (lc && lc->watermark != expected_base_wm) lc.reset();
    if (!lc) {
        lc = sc::SearchCheckpoint::read(prev_path);
        if (lc && lc->watermark != expected_base_wm) lc.reset();
        if (lc) from_prev = true;
    }
    auto fail = [&]() {
        result.loaded = false;
        result.watermark = 0;
        result.all_segments_ok = false;
        chain_ = ChainState{};
        delta_window_wm_ = 0;
        return result;
    };
    if (!lc) return fail();
    // kHnsw 段应用。
    bool segments_ok = true;
    bool any = false;
    for (const auto& ls : lc->sections) {
        if (!ls.crc_ok) { segments_ok = false; continue; }
        if (ls.type ==
            static_cast<std::uint16_t>(sc::CkptSectionType::kHnsw)) {
            const std::string vec_path =
                std::filesystem::path(fp).replace_extension(".vec").string();
            const std::string qc_path =
                std::filesystem::path(fp).replace_extension(".qc8").string();
            if (load_graph_section(
                    std::span<const std::byte>(ls.payload.data(),
                                               ls.payload.size()),
                    vec_path, qc_path)) {
                any = true;
                dirty_.store(false, std::memory_order_relaxed);
            } else {
                segments_ok = false;
            }
        }
    }
    if (config_.dim > 0 && !any) segments_ok = false;
    // 链重放（.prev 回退 = 链不可信，与 SearchLayer 版语义一致）。
    std::uint64_t coverage = lc->watermark;
    std::uint32_t next_seq = 1;
    bool chain_ok = true;
    if (segments_ok && !from_prev) {
        const std::uint64_t base_gen_for_chain = coverage;
        for (std::uint32_t s = 1; s <= chain_seq; ++s) {
            const std::string dpath = fp + ".d" + std::to_string(s);
            std::error_code ec;
            if (!std::filesystem::exists(dpath, ec)) { chain_ok = false; break; }
            auto dc = sc::SearchCheckpoint::read(dpath);
            if (!dc) { chain_ok = false; break; }
            const sc::LoadedSection* info = nullptr;
            for (const auto& dls : dc->sections) {
                if (dls.type ==
                    static_cast<std::uint16_t>(
                        sc::CkptSectionType::kDeltaInfo)) {
                    info = &dls; break;
                }
            }
            if (!info || !info->crc_ok || info->payload.size() != 20) {
                chain_ok = false; break;
            }
            const auto* q = info->payload.data();
            const std::uint64_t gen = sc::detail::get_u64(q); q += 8;
            const std::uint64_t prev_wm = sc::detail::get_u64(q); q += 8;
            const std::uint32_t seq = sc::detail::get_u32(q);
            if (gen != base_gen_for_chain || prev_wm != coverage ||
                seq != s) {
                chain_ok = false; break;
            }
            // 段级 CRC 预检 + kHnswDelta 应用。
            bool applied = true;
            for (const auto& dls : dc->sections) {
                if (!dls.crc_ok) { applied = false; break; }
            }
            if (applied) {
                for (const auto& dls : dc->sections) {
                    if (dls.type ==
                        static_cast<std::uint16_t>(
                            sc::CkptSectionType::kHnswDelta)) {
                        if (!apply_delta_log(std::span<const std::byte>(
                                dls.payload.data(), dls.payload.size()))) {
                            applied = false;
                            break;
                        }
                    }
                }
            }
            if (!applied) { chain_ok = false; break; }
            coverage = dc->watermark;
            next_seq = s + 1;
        }
    } else {
        chain_ok = false;
    }
    result.loaded = segments_ok && chain_ok;
    result.watermark = coverage;
    result.from_prev = from_prev;
    result.all_segments_ok = segments_ok && chain_ok;
    if (result.loaded) {
        chain_ = ChainState{expected_base_wm, coverage, next_seq};
        delta_window_wm_ = coverage;
        delta_vecs_.clear();
        dirty_.store(false, std::memory_order_relaxed);
    } else {
        return fail();
    }
    return result;
}


// ---- S18-6：CaskPlugin flush/open 实装 ----

plugin::PluginStatus VectorPlugin::open(const plugin::OpenContext& ctx) {
    dir_.assign(ctx.dir);
    host_ = ctx.host;
    auto r = load_component(dir_, ctx.committed_base_watermark,
                            ctx.committed_chain_seq);
    watermark_ = r.loaded ? r.watermark : 0;
    rebase_needed_.store(!r.loaded, std::memory_order_relaxed);
    return plugin::PluginStatus::kOk;
}

plugin::FlushResult VectorPlugin::flush(const plugin::FlushRequest& req) {
    plugin::FlushResult res;
    const bool cap_hit = config_.max_delta_chain > 0 &&
                         chain_.next_seq > config_.max_delta_chain;
    const bool want_base = req.force_rebase ||
                           rebase_needed_.load(std::memory_order_relaxed) ||
                           cap_hit;
    if (want_base) {
        const bool ok = save_component_base(dir_, req.watermark);
        if (config_.dim == 0) {
            // 无向量配置：清残留 + no-op 成功（宿主不为空组件记账）。
            rebase_needed_.store(false, std::memory_order_relaxed);
            res.covered_ord = chain_.chain_wm;
            return res;
        }
        if (ok) {
            rebase_needed_.store(false, std::memory_order_relaxed);
            res.covered_ord = req.watermark;
            res.generation = chain_.base_gen;
        } else {
            res.status = plugin::PluginStatus::kFailed;
            res.covered_ord = chain_.chain_wm;
        }
        return res;
    }
    if (!dirty() || delta_vecs_.empty()) {
        // 干净或空插入日志：no-op（脏位照清——镜像旧「save 时无条件清」）。
        dirty_.store(false, std::memory_order_relaxed);
        res.covered_ord = chain_.chain_wm;
        res.generation = chain_.base_gen;
        return res;
    }
    auto d = save_component_delta(dir_, req.watermark);
    if (d.wrote) {
        res.covered_ord = req.watermark;
    } else {
        res.status = plugin::PluginStatus::kFailed;
        res.covered_ord = chain_.chain_wm;
    }
    res.generation = chain_.base_gen;
    return res;
}


void VectorPlugin::on_merge_commit(const plugin::MergeCommitEvent&) {
    if (!enabled()) return;
    // V4：merge 后同步重建 HNSW（物理清死节点）。重建必须在 reducer 静止
    // 点（单写者）——经 run_serialized 投递（原 Cask 硬编码 RunFn 的插件
    // 自治版）。
    if (host_) {
        host_->run_serialized([this] { rebuild(); });
    } else {
        rebuild();  // 无宿主（standalone 测试）：直跑
    }
}

}  // namespace bitcask::vec
