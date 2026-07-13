// VectorPlugin 实现（S18-3）。函数体自 SearchLayer 向量域与
// Cask::prepare_vector 平移——行为与文件格式逐字节不变，只换持有方。

#include "bitcask/vector_plugin.hpp"
#include "bitcask/ckpt_chain.hpp"       // S20-2：walk_chain / remove_chain_files
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
    delta_.record(ord, v);  // 窗口门内建（S32-M0b）
    // S32-M1：按图节点数增量计入 base 窗口（幂等门丢弃的重放不计）。
    const std::size_t before = hnsw->size();
    hnsw->insert(ord, v);
    vec_docs_since_base_ += hnsw->size() - before;
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
    // S32-M2b：payload 外溢——活集 f32/码字直接流式写 .vec/.qc8（新 gen）
    // 并 mmap attach，重建期堆峰值不再翻倍。旧图并发读者读旧 mmap（inode
    // 由旧 fd 续命，rename 不影响）；crash 窗口 = 新 payload 已落而新 ckpt
    // 未落 → gen 守卫拒载 → fold 重建（与旧「rebuild 后 flush 全量重写」
    // 同窗口；宿主在 merge 收尾后 FIFO 紧跟成对保存点）。dir_ 未知
    // （standalone 测试未 open）时退堆拷贝旧行为。
    std::string vec_path, qc_path;
    if (!dir_.empty()) {
        const std::string fp = comp_path(dir_);
        vec_path =
            std::filesystem::path(fp).replace_extension(".vec").string();
        qc_path =
            std::filesystem::path(fp).replace_extension(".qc8").string();
    }
    auto fresh = old->clone_live(
        [this](std::uint64_t ord) { return docs_.is_live(ord); },
        vec_path, qc_path);
    hnsw_.store(std::move(fresh), std::memory_order_release);
}

std::size_t VectorPlugin::size() const {
    auto hnsw = hnsw_.load(std::memory_order_acquire);
    return hnsw ? hnsw->size() : 0;
}

void VectorPlugin::serialize_delta_log(std::vector<std::byte>& out) const {
    delta_.serialize(config_.dim, out);  // S32-M0b：格式单一真源
}

bool VectorPlugin::apply_delta_log(std::span<const std::byte> payload) {
    auto hnsw = hnsw_.load(std::memory_order_acquire);
    // S32-M1：链重放同样计入 base 窗口——重放条数就是崩溃恢复的重建图
    // 代价，载入后窗口已"欠账"多少一目了然（继续写入会尽早触发 base）。
    const std::size_t before = hnsw ? hnsw->size() : 0;
    // 直插（不入窗口日志、不标脏——链内容本就已持久化）。insert 自带
    // ord 水位幂等门。解析走 DeltaLog::parse（S32-M0b 格式单一真源）。
    const bool ok = DeltaLog::parse(
        payload, config_.dim,
        [&](std::uint64_t ord, std::span<const float> v) {
            if (hnsw) hnsw->insert(ord, v);
        });
    if (hnsw) vec_docs_since_base_ += hnsw->size() - before;
    return ok;
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
    sc::remove_chain_files(fp);  // 链坍缩（S20-2 R8）
    chain_ = ChainState{watermark, watermark, 1};
    // S18-1：base 落成——窗口日志全量被 base 覆盖 → 清空；窗口推进。
    delta_.set_window(watermark);
    clear_delta_log();
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
    if (delta_.empty()) {
        dirty_.store(false, std::memory_order_relaxed);
        return result;
    }
    const std::string fp = comp_path(dir);
    const std::uint32_t seq = chain_.next_seq;
    const std::string dpath = fp + ".d" + std::to_string(seq);
    sc::SectionWriter sw;  // S20-1 R4
    // kDeltaInfo：链校验三元组。
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
    clear_delta_log();        // S18-1：写入成功才清
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
        delta_.set_window(0);
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
    // 链重放（.prev 回退 = 链不可信，与 SearchLayer 版语义一致）。S20-2 R2：
    // 走读收敛至 sc::walk_chain，仅「应用 kHnswDelta 段」定制。
    std::uint64_t coverage = lc->watermark;
    std::uint32_t next_seq = 1;
    bool chain_ok = true;
    if (segments_ok && !from_prev) {
        const auto w = sc::walk_chain(
            fp, /*base_gen=*/coverage, /*base_coverage=*/coverage, chain_seq,
            /*unbounded=*/false,
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
        coverage = w.coverage;
        next_seq = w.next_seq;
        chain_ok = w.ok;
    } else {
        chain_ok = false;
    }
    result.loaded = segments_ok && chain_ok;
    result.watermark = coverage;
    result.from_prev = from_prev;
    result.all_segments_ok = segments_ok && chain_ok;
    if (result.loaded) {
        chain_ = ChainState{expected_base_wm, coverage, next_seq};
        delta_.set_window(coverage);
        clear_delta_log();
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
    // S32-M1：窗口门——自 base 以来入图向量数达阈值即收链（恢复链重放 =
    // 重新建图，仅靠链长门最坏重放 max_delta_chain × auto 阈值条）。
    const bool window_hit = config_.rebase_min_docs > 0 &&
                            vec_docs_since_base_ >= config_.rebase_min_docs;
    const bool want_base = req.force_rebase ||
                           rebase_needed_.load(std::memory_order_relaxed) ||
                           cap_hit || window_hit;
    if (want_base) {
        const bool ok = save_component_base(dir_, req.watermark);
        if (config_.dim == 0) {
            // 无向量配置：清残留 + no-op 成功（宿主不为空组件记账——
            // covered_ord 停在链水位，与 req.watermark 不等则 entry 不更新）。
            rebase_needed_.store(false, std::memory_order_relaxed);
            res.covered_ord = chain_.chain_wm;
        } else if (ok) {
            rebase_needed_.store(false, std::memory_order_relaxed);
            vec_docs_since_base_ = 0;  // S32-M1：窗口自 base 重新起算
            res.covered_ord = req.watermark;
        } else {
            res.status = plugin::PluginStatus::kFailed;
            res.covered_ord = chain_.chain_wm;
        }
    } else if (!dirty() || delta_.empty()) {
        // 干净或空插入日志：no-op（脏位照清——镜像旧「save 时无条件清」）。
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
    // S20-3 B-B2：链回执从 chain_（save 后已更新）统一回传。
    res.generation = chain_.base_gen;
    res.chain_seq  = chain_.next_seq - 1;
    res.chain_wm   = chain_.chain_wm;
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
