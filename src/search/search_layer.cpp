#include "bitcask/search_layer.hpp"
#include "bitcask/search_checkpoint.hpp"
#include "bitcask/text_utils.hpp"
#include "bitcask/codec.hpp"
#include "bitcask/highlighter.hpp"

#include <oneapi/tbb/parallel_for.h>      // S3:恢复期批量并行 analyze
#include <oneapi/tbb/task_arena.h>        // S7:共享有界 Search 池（inter-query）
#include <thread>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <utility>

namespace bitcask::search {

namespace {
// S7: 进程级**共享**的「有界 Search 池」。所有 Cask / SearchLayer 共用这一个
// task_arena —— **不是每 Cask 一个**（与 S6 索引池 registry 共享同思路）。
// 用途 = **inter-query 并发**：多条独立查询提交进池并发跑（稳赚，无单查询
// 两路并行的均衡/唤醒摊销问题）。并发上限由 TBB market 封顶（≈hardware_
// concurrency），与索引/恢复期 TBB 工作隔离。
//
// 故意泄漏（never-destroyed）：规避静态析构与 TbbLifetime::finalize 的顺序坑
// （进程退出/.so 卸载时 arena 不主动析构，交给 OS 回收）。task_arena 仅是
// 调度上下文、不持有线程（线程来自全局 market），泄漏成本可忽略。
//
tbb::task_arena& search_arena() {
    static tbb::task_arena* arena = [] {
        unsigned hc = std::thread::hardware_concurrency();
        int slots = static_cast<int>(hc > 1 ? hc : 2);
        return new tbb::task_arena(slots);
    }();
    return *arena;
}
}  // namespace

// S7-4: inter-query 并发入口。n 条独立查询并发跑共享有界 Search 池。
void parallel_for_queries(std::size_t n,
                          const std::function<void(std::size_t)>& body) {
    if (n == 0) return;
    if (n == 1) { body(0); return; }  // 单条直跑，不进池（零开销快路径）
    // grainsize=1 在此正确：每 item 是一条完整重查询（与 BOW 的小 posting 不同）。
    search_arena().execute([&] {
        tbb::parallel_for(std::size_t{0}, n,
                          [&](std::size_t i) { body(i); });
    });
}

SearchLayer::SearchLayer(const SearchLayerConfig& config)
    : config_(config)
    , index_()
    , analyzer_(text::AnalyzerFactory::create(config.analyzer_config))
    , cache_(config.cache_max_entries)
    , doc_texts_(config.doc_text_cache_max)
    , synonym_map_(config.synonym_map)   // S11：open-time 不可变同义词词典
{
    // V3.3:向量配置存在时创建 HNSW。metric 映射:cosine 已在写入端
    // 归一化 → kDot;kDot → kDot;kL2 → kL2。
    if (config.vector_dim > 0) {
        vec::HnswConfig hc;
        hc.dim = config.vector_dim;
        hc.metric = config.vector_metric == meta::VectorMetric::kL2
                        ? vec::HnswMetric::kL2
                        : vec::HnswMetric::kDot;
        hc.inmem_int8 = config.vector_inmem_int8;  // P5b
        hnsw_.store(std::make_shared<vec::HnswIndex>(hc),
                    std::memory_order_release);
    }
}

SearchLayer::SearchLayer(const SearchLayerConfig& config,
                         std::unique_ptr<text::Analyzer> injected_analyzer)
    : SearchLayer(config) {
    if (injected_analyzer) analyzer_ = std::move(injected_analyzer);
}

void SearchLayer::on_vector(std::uint64_t ord, std::span<const float> vec) {
    // 防御:无 HNSW 配置 / dim 不符的向量直接忽略(不崩)。正常路径
    // put_doc 已在写入端校验过 dim。V3.5:经 atomic load 取图快照——
    // worker 是唯一写者,但指针可能被本线程稍早的 rebuild_hnsw 换过。
    auto hnsw = hnsw_.load(std::memory_order_acquire);
    if (!hnsw || vec.size() != config_.vector_dim) return;
    hnsw->insert(ord, vec);
}

std::size_t SearchLayer::hnsw_size() const {
    auto hnsw = hnsw_.load(std::memory_order_acquire);
    return hnsw ? hnsw->size() : 0;
}

void SearchLayer::rebuild_hnsw() {
    auto old = hnsw_.load(std::memory_order_acquire);
    if (!old) return;
    auto fresh = std::make_shared<vec::HnswIndex>(old->config());
    const auto n = static_cast<std::uint32_t>(old->size());
    for (std::uint32_t id = 0; id < n; ++id) {
        const std::uint64_t ord = old->node_ord(id);
        if (!index_.is_live(ord)) continue;
        fresh->insert(ord, old->node_vec(id));
    }
    hnsw_.store(std::move(fresh), std::memory_order_release);
}

namespace {

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

// 查询向量归一化两段(SIMD):sq = Σ v*v 用 double 累加保留标量版精度契约;
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

// D2：bm25 结果集 → SearchHit 物化骨架（5+ 处共用）。filter 非空时按 meta_blob
// 后过滤（空 blob 不通过）；k>0 时截断到 k（text 的 overfetch 路径用之，其余
// bm25 内核已返回 top-k 的路径传 0 不截断）。
std::vector<SearchHit> SearchLayer::materialize_hits(
    const std::vector<bm25::SearchResult>& results,
    const meta::MetaFilter* filter, std::size_t k) const {
    std::vector<SearchHit> hits;
    hits.reserve(results.size());
    for (auto& r : results) {
        if (filter) {
            auto blob = index_.meta_blob(r.ord);
            if (blob.empty() || !filter->evaluate(blob)) continue;
        }
        auto ext_id = index_.ord_to_ext(r.ord);
        if (!ext_id) continue;
        hits.push_back(SearchHit{std::move(*ext_id), r.ord, r.score});
    }
    if (k > 0 && hits.size() > k) hits.resize(k);
    return hits;
}

// D2：phrase/near 共用——analyze_with_positions 还原 query 词序。
std::vector<std::string> SearchLayer::ordered_query_terms(
    std::string_view query) const {
    auto tpm = analyzer_->analyze_with_positions(query);
    std::vector<std::pair<std::uint32_t, std::string>> ordered;  // (position, term)
    for (auto& [term, data] : tpm) {
        for (auto pos : data.second) ordered.push_back({pos, term});
    }
    std::sort(ordered.begin(), ordered.end());
    std::vector<std::string> terms;
    terms.reserve(ordered.size());
    for (auto& [_, term] : ordered) terms.push_back(std::move(term));
    return terms;
}

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::search_vector(std::span<const float> query, std::size_t k,
                           std::size_t ef,
                           const meta::MetaFilter* filter) const {
    // V3.5:查询开头取一次图快照指针——与 merge 重建的换指针并发安全
    // (旧图被换出后由本地 shared_ptr 引用计数续命到查询结束)。
    auto hnsw = hnsw_.load(std::memory_order_acquire);
    if (!hnsw) {
        return std::unexpected(SearchError::kNoVectorIndex);
    }
    if (query.size() != config_.vector_dim) {
        return std::unexpected(SearchError::kVectorDimMismatch);
    }
    // cosine:查询向量同样入口归一化(hnsw-design §1);零向量无方向,
    // 返回空结果(写入端零向量被拒,查询端宽容)。
    std::vector<float> qn;
    std::span<const float> q = query;
    if (config_.vector_metric == meta::VectorMetric::kCosineNormalized) {
        const double sq = sum_sq(query.data(), query.size());
        if (sq <= 0.0) return std::vector<SearchHit>{};
        const auto inv = static_cast<float>(1.0 / std::sqrt(sq));
        qn.resize(query.size());
        scale_query(qn.data(), query.data(), inv, query.size());
        q = qn;
    }
    if (ef == 0) ef = std::max<std::size_t>(k, 64);

    // V5:filter 与 is_live 组合为 HNSW live callback——被拒节点从图遍历
    // 源头就不入候选集,无需 overfetch(k 直接交给 HNSW)。空 meta blob
    // 的文档一律不通过(无 meta → 视为「不在 filter 集合」)。
    std::function<bool(std::uint64_t)> live;
    if (filter) {
        live = [this, filter](std::uint64_t ord) -> bool {
            if (!index_.is_live(ord)) return false;
            auto blob = index_.meta_blob(ord);
            if (blob.empty()) return false;
            return filter->evaluate(blob);
        };
    } else {
        live = [this](std::uint64_t ord) -> bool {
            return index_.is_live(ord);
        };
    }
    auto raw = hnsw->search(q, k, ef, &live);

    std::vector<SearchHit> hits;
    hits.reserve(raw.size());
    for (auto& h : raw) {
        auto ext_id = index_.ord_to_ext(h.ord);
        if (!ext_id) continue;
        hits.push_back(SearchHit{std::move(*ext_id), h.ord,
                                 static_cast<double>(h.score)});
    }
    return hits;
}

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::search_hybrid(std::string_view text_query,
                           std::span<const float> vec_query,
                           std::size_t k,
                           const meta::MetaFilter* filter) const {
    // 两路都空才报错;单路空 = 退化为另一路的 RRF 重打分(hnsw-design §4)。
    if (text_query.empty() && vec_query.empty()) {
        return std::unexpected(SearchError::kEmptyHybridQuery);
    }
    const std::size_t kp = std::max<std::size_t>(k * 4, 64);  // K'

    // V5:filter 独立走两条路(text 后过滤 + vec 折 live callback)——只有
    // 同时通过两路 filter 的文档才进 RRF 融合,符合「filter 收紧 live」语义。
    //
    // S7【单查询两路：串行】决策（2026-06-24，实测定）。两路独立、纯读，理论上
    // 可并行，但实测（见 TASK.md S7-3）盲目并行常见情形不赢：
    //   ① text 路缓存命中（生产 cache 开的常态）→ 近 0，并行白付 worker 唤醒
    //      ~10–13µs → 0.66× 变慢；② 两路常严重不对称 → 并行≈max≈大路，无收益。
    // 只有「≥10 万文档 + 未命中缓存 + 两路成本同数量级」的甜区才赢 ~1.5–1.8×。
    // 故单查询两路保持串行（零开销，对常见情形最优）。线程池的稳赚用途是
    // **inter-query**（多条独立查询并发，见 search_arena() + 待落地的并发入口）。
    std::vector<SearchHit> text_hits;
    if (!text_query.empty()) {
        auto t = search_text(text_query, kp, nullptr, filter);
        if (!t) return std::unexpected(std::move(t.error()));
        text_hits = std::move(*t);
    }
    std::vector<SearchHit> vec_hits;
    if (!vec_query.empty()) {
        // ef=0 + filter → vec 路本身已在 live callback 里过滤;无需 overfetch。
        auto v = search_vector(vec_query, kp, 0, filter);
        if (!v) return std::unexpected(std::move(v.error()));  // 维度不符等
        vec_hits = std::move(*v);
    }

    // RRF(k=60):按 ord 并桶,逐路累加 1/(60+rank),rank 从 1 起。
    //
    // Reciprocal Rank Fusion (RRF): Cormack, Clarke, Buettcher 2009,
    //   "Reciprocal Rank Fusion outperforms Condorcet and individual Rank Learning Methods".
    //   公式：score = Σ 1/(k + rank_i)，其中 k=60 为经验常数（论文建议值）。
    struct Fused {
        SearchHit hit;
        double score = 0.0;
    };
    std::unordered_map<std::uint64_t, Fused> acc;
    acc.reserve(text_hits.size() + vec_hits.size());
    auto fold_leg = [&acc](std::vector<SearchHit>& leg) {
        for (std::size_t i = 0; i < leg.size(); ++i) {
            auto [it, fresh] = acc.try_emplace(leg[i].ord);
            if (fresh) it->second.hit = std::move(leg[i]);
            it->second.score += 1.0 / (60.0 + static_cast<double>(i + 1));
        }
    };
    fold_leg(text_hits);
    fold_leg(vec_hits);

    std::vector<SearchHit> fused;
    fused.reserve(acc.size());
    for (auto& [ord, f] : acc) {
        f.hit.score = f.score;  // score = RRF 分(替换掉单路原始分)
        fused.push_back(std::move(f.hit));
    }
    // 确定性平局序:RRF 分相等 → ord 小者在前(测试锁此行为)。
    std::sort(fused.begin(), fused.end(),
              [](const SearchHit& a, const SearchHit& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.ord < b.ord;
              });
    if (fused.size() > k) fused.resize(k);
    return fused;
}

bm25::InvertedIndex& SearchLayer::field_index(std::string_view field) {
    // 双检:常态(字段已存在)只拿共享锁;首次出现的字段才升级独占建索引。
    {
        std::shared_lock lk(fields_mu_);
        auto it = fields_.find(field);
        if (it != fields_.end()) return *it->second;
    }
    std::unique_lock lk(fields_mu_);
    auto it = fields_.find(field);
    if (it == fields_.end()) {
        it = fields_.emplace(std::string(field),
                             std::make_unique<bm25::InvertedIndex>(config_.bm25_params, config_.index_positions)).first;
    }
    return *it->second;
}

const bm25::InvertedIndex* SearchLayer::field_index(std::string_view field) const {
    std::shared_lock lk(fields_mu_);
    auto it = fields_.find(field);
    return it == fields_.end() ? nullptr : it->second.get();
}

std::string_view SearchLayer::intern_field_name(std::string_view name) {
    // S10-A4:双检。常容(字段名已 intern)只共享锁;首次出现升级独占 emplace。
    {
        std::shared_lock lk(field_names_intern_mu_);
        auto it = field_names_intern_.find(name);
        if (it != field_names_intern_.end()) return std::string_view(*it);
    }
    std::unique_lock lk(field_names_intern_mu_);
    auto [it, _] = field_names_intern_.emplace(name);
    return std::string_view(*it);
}

void SearchLayer::on_write(std::string_view key, std::uint64_t ord,
                           std::string_view text,
                           std::uint32_t file_id, std::uint64_t offset,
                           std::uint32_t total_sz, std::uint32_t tstamp) {
    auto term_data = analyzer_->analyze_with_positions(text);

    std::uint32_t doc_len = 0;
    std::vector<std::string> changed_terms;
    changed_terms.reserve(term_data.size());
    for (auto& [term, data] : term_data) {
        doc_len += data.first;
        changed_terms.push_back(term);
    }

    index_.put_doc(key, ord,
                   index::DocSlot{
                       index::DocLoc{file_id, offset, total_sz},
                       tstamp,
                       doc_len});

    if (!term_data.empty()) {
        field_index(kDefaultField).add_doc(ord, term_data);
    }
    doc_texts_.put(ord, std::string(text));
    // S9.2：只失效查询词与本文档词集有交集的缓存条目。
    cache_.invalidate_terms(changed_terms);
}

// S6 索引流水线的 **Map 阶段**（设计稿 §3）。**纯 const 函数**：只读
// analyzer_（const 配置态，cppjieba Cut 亦 const 线程安全），对每个字段跑
// NFKC + 分词 + 位置，**不碰任何共享可变态** → 可在 N 个 map worker 上对不同
// 文档**并发**调用（F7 不变量，TSan 已验证）。产出 owning `ReduceJob`（自带
// terms/positions/catch-all/DocSlot），由 reducer 线程按 ord 序串行 apply。
//
// catch-all：把非默认字段的分词结果合并进默认字段（让只查默认字段的
// search_text/phrase/near 也能命中多字段文档），见下方注释。
ReduceJob SearchLayer::map_analyze(
    std::string_view key, std::uint64_t ord,
    const std::vector<std::pair<std::string_view, std::string_view>>& fields,
    std::uint32_t file_id, std::uint64_t offset,
    std::uint32_t total_sz, std::uint32_t tstamp) const {
    ReduceJob job;
    job.key      = std::string(key);
    job.ord      = ord;
    job.file_id  = file_id;
    job.offset   = offset;
    job.total_sz = total_sz;
    job.tstamp   = tstamp;

    // catch-all（S8.6 修复 + O5 合并优化）：把非默认字段词项合并进默认字段，
    // 使 search_text/phrase/near（只查默认字段）也能命中多字段文档。
    // O5：此前是拼接原文后整体重新分词（NFKC + 分词全部重跑一遍）；改为直接
    // 合并各字段的分词结果，position 按字段顺序平移 ca_pos_base。字段内
    // 相对位置不变（phrase/near 字段内语义不变）；跨字段间隔取「字段最大
    // position + 1」，与拼接版仅在字段尾部存在被丢短词时差极小的 slop。
    text::TermPositionsMap ca_data;
    std::uint32_t ca_pos_base = 0;

    for (auto& [fname, ftext] : fields) {
        const std::string_view field = fname.empty() ? kDefaultField : fname;
        auto term_data = analyzer_->analyze_with_positions(ftext);
        std::uint32_t flen = 0;
        for (auto& [_, data] : term_data) flen += data.first;

        if (field == kDefaultField) {
            job.wrote_default = true;
        } else if (!term_data.empty()) {
            std::uint32_t field_max_pos = 0;
            for (auto& [term, data] : term_data) {
                auto& [tf, positions] = data;
                auto& [ca_tf, ca_positions] = ca_data[term];
                ca_tf += tf;
                for (auto p : positions) {
                    ca_positions.push_back(p + ca_pos_base);
                    if (p > field_max_pos) field_max_pos = p;
                }
            }
            job.ca_len += flen;
            ca_pos_base += field_max_pos + 1;
        }

        job.fields.push_back(ReduceJob::FieldResult{
            std::string(field), std::move(term_data), flen});
        job.total_doc_len += flen;
    }

    job.ca_data = std::move(ca_data);
    // 高亮：默认字段原文（多字段高亮的精细化留待后续）。
    job.doc_text = fields.empty() ? std::string{}
                                  : std::string(fields.front().second);
    return job;
}

// S6 索引流水线的 **Reduce 阶段**（设计稿 §3）。与 map_analyze 相反：**改共享
// 索引/HNSW/缓存，必须串行**。reducer 线程对每条 lane 按 `next_apply_ord` 严格
// **ord 序**调用本函数（reorder buffer 把并行 map 的乱序结果拗回 ord 序）——
// 这是「到达序 LWW 等价 ord 序 LWW」正确性的关键（否则被删 key 复活，§3 F4∧F5）。
// 锁序：fields_mu_ → index_.mutex_（类级不变量，无死锁）。
// 步骤：① 侧表 ord_field_lens_ 记字段长 ② 各字段 add_doc 进倒排
//       ③ catch-all 合并默认字段 ④ index_.put_doc 落 DocSlot ⑤ 高亮原文 / meta /
//       向量（on_vector → HNSW，单写者=本 reducer）⑥ 失效查询缓存。
void SearchLayer::reduce_apply(const ReduceJob& job,
                               std::span<const std::byte> meta,
                               std::span<const float> vec) {
    // S6-P2: 空 job 守卫（map_fn_ 抛异常时 reducer 收到空 ReduceEntry）。
    // key+fields 都空 = map_analyze 未产出，跳过 apply；reducer 仍推进 ord。
    if (job.key.empty() && job.fields.empty()) return;
    auto& field_lens = ord_field_lens_[job.ord];
    field_lens.reserve(job.fields.size() + 1);
    for (const auto& f : job.fields) {
        // S10-A4:intern 取稳定 string_view，免 owning string 分配。
        field_lens.emplace_back(intern_field_name(f.field_name), f.doc_len);
    }

    for (const auto& f : job.fields) {
        if (!f.terms.empty()) {
            field_index(f.field_name).add_doc(job.ord, f.terms);
        }
    }

    // 若已有字段直接写默认字段，则不重复合并（避免双写）。
    if (!job.wrote_default && !job.ca_data.empty()) {
        field_index(kDefaultField).add_doc(job.ord, job.ca_data);
        field_lens.emplace_back(intern_field_name(kDefaultField), job.ca_len);
    }

    index_.put_doc(job.key, job.ord,
                   index::DocSlot{
                       index::DocLoc{job.file_id, job.offset, job.total_sz},
                       job.tstamp,
                       job.total_doc_len});
    if (!job.doc_text.empty()) {
        doc_texts_.put(job.ord, job.doc_text);
    }
    if (!meta.empty()) {
        index_.set_meta(job.ord, meta);
    }
    if (!vec.empty()) {
        on_vector(job.ord, vec);
    }
    cache_.invalidate();
}

void SearchLayer::on_write_fields(
    std::string_view key, std::uint64_t ord,
    const std::vector<std::pair<std::string, std::string>>& fields,
    std::uint32_t file_id, std::uint64_t offset,
    std::uint32_t total_sz, std::uint32_t tstamp) {
    // S10-A5:同步路径—fields 借 caller 的 string 构造 views（无堆分配）。
    std::vector<std::pair<std::string_view, std::string_view>> fvs;
    fvs.reserve(fields.size());
    for (const auto& [name, text] : fields) {
        fvs.emplace_back(name, text);
    }
    auto job = map_analyze(key, ord, fvs, file_id, offset, total_sz, tstamp);
    reduce_apply(job, {}, {});
}

std::optional<std::uint64_t> SearchLayer::on_delete(std::string_view key, std::uint64_t tomb_ord) {
    auto slot = index_.get(key);
    if (!slot) return std::nullopt;

    // S9.2：取被删文档词集做选择性失效。原文 LRU 命中则精确 analyze；
    // miss（冷文档被挤出）则降级为整缓存失效（安全但粗粒度）。
    auto text = doc_texts_.get(slot->ord);  // 拷贝(C1:并发安全,见 DocTextLru)
    std::vector<std::string> changed_terms;
    if (text) {
        auto tf = analyzer_->analyze(*text);
        changed_terms.reserve(tf.size());
        for (auto& [term, _] : tf) changed_terms.push_back(term);
    }

    // 删除该文档在各字段的统计。多字段路径用 ord_field_lens_ 精确扣减各字段
    // doc_len（R3）；单 text 路径无此表，按默认字段用 slot->doc_len。
    if (auto it = ord_field_lens_.find(slot->ord); it != ord_field_lens_.end()) {
        for (auto& [field, flen] : it->second) {
            field_index(field).remove_doc(flen, {});
        }
        ord_field_lens_.erase(it);
    } else {
        std::shared_lock lk(fields_mu_);  // 只读 map 结构;remove_doc 自带并发
        for (auto& [_, inv] : fields_) {
            inv->remove_doc(slot->doc_len, {});
        }
    }
    index_.remove(key, tomb_ord);
    doc_texts_.erase(slot->ord);
    if (text) {
        cache_.invalidate_terms(changed_terms);
    } else {
        cache_.invalidate();
    }
    return tomb_ord;
}

void SearchLayer::on_relocate(std::string_view key, std::uint64_t ord,
                              std::uint32_t new_file_id, std::uint64_t new_offset,
                              std::uint32_t new_total_sz) {
    auto slot = index_.get(key);
    if (!slot) return;

    index_.put_doc(key, ord,
                   index::DocSlot{
                       index::DocLoc{new_file_id, new_offset, new_total_sz},
                       slot->tstamp,
                       slot->doc_len});
}

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::search_text(std::string_view query, std::size_t k,
                         const bm25::Bm25Params* params_override,
                         const meta::MetaFilter* filter) const {
    // S10-A1:缓存检查前置以跳过 ~20µs NLP analyze（详见 TASK.md）。
    // 安全前提:CacheKey 仅依赖 (query_type, query, k_req),不依赖 analyze 结果。
    if (query.empty()) return std::vector<SearchHit>{};

    // V5:filter 非空时 overfetch K'=max(k×4, 64)——BM25 评分排序在
    // filter 之前,过严 filter 命中数 < k 时需更多候选弥补损耗。无 filter
    // 仍按 k 请求(避免无谓放大,保持兼容)。
    const std::size_t k_req = filter ? std::max<std::size_t>(k * 4, 64) : k;

    auto cache_key = CacheKey::make("text", query, k_req);
    auto cached = params_override
                      ? std::optional<std::vector<bm25::SearchResult>>{}
                      : cache_.get(cache_key);

    std::vector<bm25::SearchResult> results;
    if (cached) {
        results = std::move(*cached);
    } else {
        auto term_freqs = analyzer_->analyze(query);
        if (term_freqs.empty()) return std::vector<SearchHit>{};

        std::vector<std::string> terms;
        terms.reserve(term_freqs.size());
        for (auto& [term, _] : term_freqs) {
            terms.push_back(term);
        }
        if (synonym_map_) {
            terms = synonym_map_->expand_terms(terms);
        }

        const auto* inv = field_index(kDefaultField);
        if (inv) results = inv->search(terms, k_req, index_, params_override);
        if (!params_override) cache_.put(cache_key, results, terms);
    }

    // D2：filter 后过滤（空 meta 不通过）+ overfetch 后截断到 k。
    return materialize_hits(results, filter, k);
}

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::search_phrase(std::string_view query, std::size_t k,
                           const bm25::Bm25Params* params_override) const {
    // S10-A1:缓存检查前置（同 search_text）。
    if (query.empty()) return std::vector<SearchHit>{};

    auto cache_key = CacheKey::make("phrase", query, k);
    auto cached = params_override
                      ? std::optional<std::vector<bm25::SearchResult>>{}
                      : cache_.get(cache_key);

    std::vector<bm25::SearchResult> results;
    if (cached) {
        results = std::move(*cached);
    } else {
        // S9.28：短语匹配依赖查询词序——用 analyze_with_positions 还原（D2 helper）。
        auto terms = ordered_query_terms(query);
        if (terms.empty()) return std::vector<SearchHit>{};

        const auto* inv = field_index(kDefaultField);
        if (inv) results = inv->search_phrase(terms, k, index_, params_override);
        if (!params_override) cache_.put(cache_key, results, terms);
    }

    return materialize_hits(results);
}

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::search_near(std::string_view query, std::uint32_t slop, std::size_t k,
                         const bm25::Bm25Params* params_override) const {
    // 近邻依赖查询词序——用 analyze_with_positions 还原（D2 helper，同 phrase）。
    auto terms = ordered_query_terms(query);
    if (terms.empty()) return std::vector<SearchHit>{};

    std::vector<bm25::SearchResult> results;
    const auto* inv = field_index(kDefaultField);
    if (inv) results = inv->search_near(terms, k, slop, index_, params_override);

    return materialize_hits(results);
}

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::search_fuzzy(std::string_view query, std::size_t k, std::uint32_t max_edit_distance,
                          const bm25::Bm25Params* params_override) const {
    auto term_freqs = analyzer_->analyze(query);
    if (term_freqs.empty()) return std::vector<SearchHit>{};

    std::vector<std::string> terms;
    terms.reserve(term_freqs.size());
    for (auto& [term, _] : term_freqs) {
        terms.push_back(term);
    }

    std::vector<bm25::SearchResult> results;
    const auto* inv = field_index(kDefaultField);
    if (inv) results = inv->search_fuzzy(terms, k, max_edit_distance, index_, params_override);

    return materialize_hits(results);
}

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::bool_search(std::string_view query, std::size_t k,
                         const bm25::Bm25Params* params_override) const {
    // S10-A1:缓存检查前置（同 search_text）。
    if (query.empty()) return std::vector<SearchHit>{};

    auto cache_key = CacheKey::make("bool", query, k);
    auto cached = params_override
                      ? std::optional<std::vector<bm25::SearchResult>>{}
                      : cache_.get(cache_key);

    std::vector<bm25::SearchResult> results;
    if (cached) {
        results = std::move(*cached);
    } else {
        auto query_node = bitcask::bm25::parse_query(query);
        if (query_node.term.empty() && query_node.children.empty()) {
            return std::vector<SearchHit>{};
        }

        const auto* inv = field_index(kDefaultField);
        if (inv) results = inv->bool_search(query_node, k, index_, params_override);
        if (!params_override && !results.empty()) {
            // 收集 MUST/SHOULD/MUST_NOT 全部叶子词，作为该缓存条目的词集。
            std::vector<std::string> must, should, must_not;
            bm25::collect_terms(query_node, must, should, must_not);
            std::vector<std::string> terms = std::move(must);
            terms.insert(terms.end(), should.begin(), should.end());
            terms.insert(terms.end(), must_not.begin(), must_not.end());
            cache_.put(cache_key, results, std::move(terms));
        }
    }

    return materialize_hits(results);
}

std::optional<bm25::ScoreExplanation>
SearchLayer::explain(std::string_view query, std::string_view key,
                     const bm25::Bm25Params* params_override) const {
    auto slot = index_.get(key);
    if (!slot) return std::nullopt;

    auto term_freqs = analyzer_->analyze(query);
    std::vector<std::string> terms;
    terms.reserve(term_freqs.size());
    for (auto& [term, _] : term_freqs) terms.push_back(term);

    const auto* inv = field_index(kDefaultField);
    if (!inv) return bm25::ScoreExplanation{};
    return inv->explain(terms, slot->ord, index_, params_override);
}

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::search_wildcard(std::string_view pattern, std::size_t k,
                             const bm25::Bm25Params* params_override) const {
    std::vector<bm25::SearchResult> results;
    const auto* inv = field_index(kDefaultField);
    if (inv) results = inv->search_wildcard(std::string(pattern), k, index_, params_override);

    return materialize_hits(results);
}

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::search_fields(std::string_view query, std::size_t k,
                           const bm25::Bm25Params* params_override) const {
    auto qnode = bitcask::bm25::parse_query(query);

    std::vector<const bm25::QueryNode*> leaves;
    std::function<void(const bm25::QueryNode&)> walk = [&](const bm25::QueryNode& n) {
        if (!n.term.empty()) { leaves.push_back(&n); return; }
        for (auto& c : n.children) walk(c);
    };
    walk(qnode);
    if (leaves.empty()) return std::vector<SearchHit>{};

    struct FieldQuery { std::vector<std::string> terms; float boost; };
    std::unordered_map<std::string, std::vector<std::pair<std::string,float>>> by_field;
    for (auto* leaf : leaves) {
        std::string field = leaf->field.empty() ? std::string(kDefaultField) : leaf->field;
        auto tf = analyzer_->analyze(leaf->term);
        for (auto& [norm_term, _] : tf) {
            by_field[field].push_back({norm_term, leaf->boost});
        }
    }

    std::unordered_map<std::uint64_t, double> acc;
    for (auto& [field, term_boosts] : by_field) {
        const auto* inv = field_index(field);
        if (!inv) continue;
        std::vector<std::string> terms;
        terms.reserve(term_boosts.size());
        for (auto& [t, _] : term_boosts) terms.push_back(t);
        if (synonym_map_) {
            terms = synonym_map_->expand_terms(terms);
        }
        for (auto& [t, boost] : term_boosts) {
            auto expanded = synonym_map_ ? synonym_map_->expand(t) : std::span<const std::string>{};
            if (expanded.empty()) expanded = {&t, 1};
            for (const auto& et : expanded) {
                auto res = inv->search({et}, k, index_, params_override);
                for (auto& r : res) acc[r.ord] += static_cast<double>(r.score) * boost;
            }
        }
    }

    std::vector<std::pair<std::uint64_t,double>> ranked(acc.begin(), acc.end());
    std::partial_sort(ranked.begin(),
                      ranked.begin() + std::min(k, ranked.size()),
                      ranked.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
    if (ranked.size() > k) ranked.resize(k);

    std::vector<SearchHit> hits;
    hits.reserve(ranked.size());
    for (auto& [ord, score] : ranked) {
        auto ext_id = index_.ord_to_ext(ord);
        if (!ext_id) continue;
        hits.push_back(SearchHit{std::move(*ext_id), ord, score});
    }
    return hits;
}

void SearchLayer::recover_doc(std::string_view key, std::uint64_t ord,
                              std::string_view text,
                              std::uint32_t file_id, std::uint64_t offset,
                              std::uint32_t total_sz, std::uint32_t tstamp,
                              std::span<const float> vector) {
    // S6-P0:单字段(kDefaultField) 恢复——map_analyze + reduce_apply 复用。
    // map_analyze 在 default_field 上写出 → wrote_default=true,触发不到
    // catch-all 路径,语义与原版逐条 recover_doc 完全一致。
    std::vector<std::pair<std::string_view, std::string_view>> fields;
    fields.emplace_back(kDefaultField, text);
    auto job = map_analyze(key, ord, fields, file_id, offset, total_sz, tstamp);
    reduce_apply(job, {}, vector);
}

// S3:批量恢复——并行 analyze + 串行有序插入（见头文件注释的正确性论证）。
// 与逐条 recover_doc 字节等价：同一 fold 序插入、同一单字段路径、HNSW 串行。
void SearchLayer::recover_doc_batch(std::vector<RecoverDoc>& batch) {
    if (batch.empty()) return;
    const std::size_t n = batch.size();

    // 阶段一：并行 map_analyze（analyzer_ const 无可变态；写 jobs[i] 互不相交，
    // 无共享可变状态）。map_analyze 自身 const、纯函数，并行调用安全。TBB
    // 全局线程池，无 per-batch 线程创建。
    std::vector<ReduceJob> jobs(n);
    tbb::parallel_for(std::size_t{0}, n, [&](std::size_t i) {
        const auto& d = batch[i];
        std::vector<std::pair<std::string_view, std::string_view>> fields;
        fields.emplace_back(kDefaultField, d.text);
        jobs[i] = map_analyze(d.key, d.ord, fields,
                              d.file_id, d.offset, d.total_sz, d.tstamp);
    });

    // 阶段二：按 batch 序串行 reduce_apply（= fold 序）。HNSW 单写者 = 本线程。
    // reduce_apply 内部逐条 cache_.invalidate()：恢复期无查询，性能影响可忽略；
    // 最终状态与旧版完全一致。
    for (std::size_t i = 0; i < n; ++i) {
        const auto& d = batch[i];
        reduce_apply(jobs[i], {}, d.vector);
    }
}

void SearchLayer::recover_tomb(std::string_view key, std::uint64_t ord) {
    index_.remove(key, ord);
}


namespace {
constexpr std::uint32_t kSidecarMagic   = 0x42434953;  // "BCIS"
constexpr std::uint32_t kSidecarVersion = 1;
void sc_put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + 4);
}
void sc_put64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + 8);
}
}  // namespace

// docmap sidecar（"BCIS"）序列化：把 index_ 的 ord → (ext_id, DocSlot) 活映射
// 落进 checkpoint，避免冷启动靠全量 fold 重建 docmap。`covers_next_ord` 记录
// 快照覆盖到的 ord 水位——load 后只需 fold 该水位之后的增量（checkpoint 与
// 后续 WAL/data 的衔接点）。布局：header(magic+version+covers_next_ord+行数) +
// 每活文档一行（见下方固定段 34B + 变长 ext_id）。仅遍历 live 文档（死文档
// 由 keydir LWW 过滤，不入快照）。
bool SearchLayer::serialize_docmap(std::vector<std::uint8_t>& buf,
                                   std::uint64_t covers_next_ord) const {
    buf.clear();
    // S4:预留容量。无 reserve 时从零起几何增长，GB 级 docmap 累计搬运 ~2×
    // 终态字节。每行固定 34B（ord8+klen2+file_id4+offset8+total_sz4+tstamp4
    // +doc_len4）+ 变长 ext_id；按 live_docs 预留固定段 + 48B/行 ext 估值，
    // 常态零 realloc（偏小由几何增长兜底，reserve 仅设容量、绝不溢出）。
    const std::uint64_t live = index_.info().live_docs;
    buf.reserve(28 + static_cast<std::size_t>(live) * (34 + 48));
    sc_put32(buf, kSidecarMagic);
    sc_put32(buf, kSidecarVersion);
    sc_put64(buf, covers_next_ord);
    // 行数占位,回填。
    const std::size_t cnt_pos = buf.size();
    sc_put64(buf, 0);
    std::uint64_t rows = 0;
    bool ok = true;
    index_.for_each_live([&](std::uint64_t ord, const std::string& ext,
                             const index::DocSlot& slot) {
        if (ext.size() > 0xFFFF) { ok = false; return; }
        sc_put64(buf, ord);
        const auto klen = static_cast<std::uint16_t>(ext.size());
        const auto* kp = reinterpret_cast<const std::uint8_t*>(&klen);
        buf.insert(buf.end(), kp, kp + 2);
        const auto* kd = reinterpret_cast<const std::uint8_t*>(ext.data());
        buf.insert(buf.end(), kd, kd + ext.size());
        sc_put32(buf, slot.loc.file_id);
        sc_put64(buf, slot.loc.offset);
        sc_put32(buf, slot.loc.total_sz);
        sc_put32(buf, slot.tstamp);
        sc_put32(buf, slot.doc_len);
        ++rows;
    });
    if (!ok) return false;
    std::memcpy(buf.data() + cnt_pos, &rows, 8);
    const std::uint32_t crc = bitcask::codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(buf.data() + 8), buf.size() - 8));
    sc_put32(buf, crc);
    return true;
}

std::optional<std::uint64_t>
SearchLayer::deserialize_docmap(std::span<const std::uint8_t> buf) {
    if (buf.size() < 28) return std::nullopt;
    auto rd32 = [&](std::size_t off) {
        std::uint32_t v; std::memcpy(&v, buf.data() + off, 4); return v;
    };
    if (rd32(0) != kSidecarMagic || rd32(4) != kSidecarVersion) {
        return std::nullopt;
    }
    std::uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, buf.data() + buf.size() - 4, 4);
    const std::uint32_t crc = bitcask::codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(buf.data() + 8), buf.size() - 12));
    if (crc != stored_crc) return std::nullopt;

    const std::uint8_t* p = buf.data() + 8;
    const std::uint8_t* end = buf.data() + buf.size() - 4;
    auto need = [&](std::size_t n) {
        return static_cast<std::size_t>(end - p) >= n;
    };
    std::uint64_t covers = 0, rows = 0;
    std::memcpy(&covers, p, 8); p += 8;
    std::memcpy(&rows, p, 8); p += 8;
    if (rows > (1ull << 40)) return std::nullopt;
    for (std::uint64_t i = 0; i < rows; ++i) {
        if (!need(10)) return std::nullopt;
        std::uint64_t ord; std::memcpy(&ord, p, 8); p += 8;
        std::uint16_t klen; std::memcpy(&klen, p, 2); p += 2;
        if (!need(static_cast<std::size_t>(klen) + 20)) return std::nullopt;
        std::string ext(reinterpret_cast<const char*>(p), klen); p += klen;
        index::DocSlot slot;
        std::memcpy(&slot.loc.file_id, p, 4); p += 4;
        std::memcpy(&slot.loc.offset, p, 8); p += 8;
        std::memcpy(&slot.loc.total_sz, p, 4); p += 4;
        std::memcpy(&slot.tstamp, p, 4); p += 4;
        std::memcpy(&slot.doc_len, p, 4); p += 4;
        index_.put_doc(ext, ord, slot);  // 重建 ext2ord/live/doc_lens/水位
    }
    if (p != end) return std::nullopt;
    return covers;
}

void SearchLayer::rebuild_index(DocReader doc_reader) {
    // 阶段2a：仍按默认字段重建（多字段从 DocValue 取字段在阶段4打通）。
    auto new_inv = std::make_unique<bm25::InvertedIndex>(config_.bm25_params, config_.index_positions);
    doc_texts_.clear();
    ord_field_lens_.clear();  // 否则旧 ord 的多字段统计跨重建残留→无界增长。

    index_.for_each_live([&](std::uint64_t ord,
                              const std::string& /*ext_id*/,
                              const index::DocSlot& slot) {
        auto text = doc_reader(slot.loc.file_id, slot.loc.offset, slot.loc.total_sz);
        if (!text) return;

        auto term_data = analyzer_->analyze_with_positions(*text);
        if (term_data.empty()) return;

        new_inv->add_doc(ord, term_data);
        doc_texts_.put(ord, *text);
    });

    new_inv->finalize_all_postings();

    fields_.clear();
    fields_.emplace(kDefaultField, std::move(new_inv));

    cache_.invalidate();
}

std::size_t SearchLayer::compact(double dead_ratio_threshold) {
    std::size_t total = 0;
    for (auto& [field, inv] : fields_) {
        total += inv->compact(index_, dead_ratio_threshold);
    }
    if (total > 0) cache_.invalidate();  // posting 行变了，缓存可能含陈旧结果
    return total;
}

std::expected<std::vector<SearchHitEx>, SearchError>
SearchLayer::search_text_highlight(std::string_view query, std::size_t k,
                                   const HighlightOptions& opts) const {
    auto term_freqs = analyzer_->analyze(query);
    if (term_freqs.empty()) return std::vector<SearchHitEx>{};

    auto cache_key = CacheKey::make("highlight", query, k);
    auto cached = cache_.get(cache_key);

    std::vector<bm25::SearchResult> results;
    if (cached) {
        results = std::move(*cached);
    } else {
        std::vector<std::string> terms;
        terms.reserve(term_freqs.size());
        for (auto& [term, _] : term_freqs) {
            terms.push_back(term);
        }

        const auto* inv = field_index(kDefaultField);
        if (inv) results = inv->search(terms, k, index_);
        cache_.put(cache_key, results, terms);
    }

    std::vector<SearchHitEx> hits;
    hits.reserve(results.size());
    for (auto& r : results) {
        auto ext_id = index_.ord_to_ext(r.ord);
        if (!ext_id) continue;

        // S9.3：原文 LRU 命中才生成高亮片段；冷文档被挤出（miss）时降级为
        // 无片段的 hit，而非整条丢弃——保证结果集不因 LRU 容量而缩水。
        auto doc_text = doc_texts_.get(r.ord);  // 拷贝(C1)
        std::vector<Snippet> snippets;
        if (doc_text) {
            // S9.19：analyze_with_offsets 产出的 byte offset 相对「归一化文本」，
            // 故 highlight 也必须在归一化文本上切片，否则非规范文本（全角/组合
            // 字符等）会因坐标系不一致切出乱码。NFKC 幂等，传 norm 再归一化无害。
            std::string norm = text::detail::nfkc_fold(*doc_text);
            auto token_offsets = analyzer_->analyze_with_offsets(norm);
            std::unordered_map<std::string, std::vector<text::TokenInfo>> query_token_offsets;
            for (auto& [term, _] : term_freqs) {
                auto it_token = token_offsets.find(term);
                if (it_token != token_offsets.end()) {
                    query_token_offsets[term] = it_token->second;
                }
            }
            auto hl_result = highlight(norm, query_token_offsets, opts);
            snippets = std::move(hl_result.snippets);
        }

        hits.push_back(SearchHitEx{
            std::move(*ext_id),
            r.ord,
            r.score,
            std::move(snippets)
        });
    }
    return hits;
}

// ---- P14e:统一分段 search.ckpt 持久化 ----

namespace {
// type 3 (bm25.fields) 辅助:把多个非默认字段序列化为一个段 payload。
// 格式:u32 fieldCount; 每字段 [u16 nameLen][name][u64 invLen][inv bytes]。
// 使用与 search_checkpoint.hpp 相同的小端编码。
void put_u16_byte(std::vector<std::byte>& b, std::uint16_t v) {
    b.push_back(static_cast<std::byte>(v & 0xFF));
    b.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
}
void put_u32_byte(std::vector<std::byte>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
void put_u64_byte(std::vector<std::byte>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
std::uint16_t get_u16_byte(const std::byte* p) {
    return static_cast<std::uint16_t>(p[0]) |
           (static_cast<std::uint16_t>(p[1]) << 8);
}
std::uint32_t get_u32_byte(const std::byte* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t get_u64_byte(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}
}  // namespace

bool SearchLayer::save_search_ckpt(std::string_view path,
                                   std::uint64_t watermark) {
    namespace sc = bitcask::search;
    const std::string fp(path);

    std::vector<sc::CkptSection> secs;
    // 段 payload 缓冲区须活到 write() 完成——span 是非 owning 视图。
    std::vector<std::vector<std::byte>> byte_bufs;
    std::vector<std::vector<std::uint8_t>> u8_bufs;
    auto add_byte_sec = [&](std::uint16_t type,
                            std::vector<std::byte> buf) {
        byte_bufs.push_back(std::move(buf));
        secs.push_back(sc::CkptSection{
            type, 0,
            std::span<const std::byte>(byte_bufs.back().data(),
                                        byte_bufs.back().size())});
    };
    auto add_u8_sec = [&](std::uint16_t type,
                          std::vector<std::uint8_t> buf) {
        u8_bufs.push_back(std::move(buf));
        secs.push_back(sc::CkptSection{
            type, 0,
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(u8_bufs.back().data()),
                u8_bufs.back().size())});
    };

    // 段 1: docmap (type 1)。
    {
        std::vector<std::uint8_t> buf;
        if (serialize_docmap(buf, watermark)) {
            add_u8_sec(static_cast<std::uint16_t>(sc::CkptSectionType::kDocmap),
                       std::move(buf));
        }
    }

    // 段 2 + 3: bm25.default + bm25.fields。
    {
        std::shared_lock lk(fields_mu_);
        auto dit = fields_.find(kDefaultField);
        if (dit != fields_.end()) {
            std::vector<std::byte> buf;
            dit->second->serialize(buf);
            add_byte_sec(
                static_cast<std::uint16_t>(sc::CkptSectionType::kBm25Default),
                std::move(buf));
        }
        std::uint32_t other_count = 0;
        for (auto& [field, inv] : fields_) {
            if (field == kDefaultField) continue;
            ++other_count;
        }
        if (other_count > 0) {
            std::vector<std::byte> fbuf;
            put_u32_byte(fbuf, other_count);
            for (auto& [field, inv] : fields_) {
                if (field == kDefaultField) continue;
                put_u16_byte(fbuf, static_cast<std::uint16_t>(field.size()));
                fbuf.insert(fbuf.end(),
                    reinterpret_cast<const std::byte*>(field.data()),
                    reinterpret_cast<const std::byte*>(field.data()) +
                        field.size());
                std::uint64_t pos = fbuf.size();
                put_u64_byte(fbuf, 0);  // invLen 占位
                inv->serialize(fbuf);
                std::uint64_t inv_len = fbuf.size() - pos - 8;
                std::memcpy(fbuf.data() + pos, &inv_len, 8);
            }
            add_byte_sec(
                static_cast<std::uint16_t>(sc::CkptSectionType::kBm25Fields),
                std::move(fbuf));
        }
    }

    // 段 4: hnsw (type 4)。V7:BCVS v2 双文件——vecs_ 先落 search.vec
    // (save_vec_payload, atomic tmp+rename),再 serialize header 入 ckpt 段。
    if (config_.vector_dim > 0) {
        auto hnsw = hnsw_.load(std::memory_order_acquire);
        if (hnsw) {
            const std::string vec_path =
                std::filesystem::path(fp).replace_extension(".vec").string();
            if (hnsw->save_vec_payload(vec_path)) {
                std::vector<std::uint8_t> buf;
                if (hnsw->serialize(buf)) {
                    add_u8_sec(
                        static_cast<std::uint16_t>(sc::CkptSectionType::kHnsw),
                        std::move(buf));
                }
            }
        }
    }

    // 代际回退:把现有 search.ckpt 重命名为 search.ckpt.prev。
    {
        const std::string prev = fp + ".prev";
        std::error_code ec;
        if (std::filesystem::exists(fp, ec)) {
            std::filesystem::rename(fp, prev, ec);
        }
    }

    if (!sc::SearchCheckpoint::write(fp, watermark, secs)) return false;

    // 保存成功后截断 WAL（与旧 save_snapshot 行为一致）。
    {
        std::shared_lock lk(fields_mu_);
        for (auto& [_, inv] : fields_) {
            inv->truncate_wal();
        }
    }
    return true;
}

SearchLayer::CkptLoadResult
SearchLayer::load_search_ckpt(std::string_view path) {
    namespace sc = bitcask::search;
    const std::string fp(path);
    const std::string prev = fp + ".prev";
    // V7:BCVS v2 vecs_ payload 路径(与 ckpt 同目录,.vec 扩展名)。
    const std::string vec_path =
        std::filesystem::path(fp).replace_extension(".vec").string();

    auto try_load = [&](std::string_view p) -> std::optional<sc::LoadedCheckpoint> {
        return sc::SearchCheckpoint::read(p);
    };

    auto lc = try_load(fp);
    bool from_prev = false;
    if (!lc) {
        lc = try_load(prev);
        if (!lc) return {};
        from_prev = true;
    }

    CkptLoadResult result;
    result.loaded = true;
    result.watermark = lc->watermark;
    result.all_segments_ok = true;

    // 逐段分发到反序列化器。
    bool bm25_loaded = false;
    bool docmap_loaded = false;
    bool hnsw_loaded = false;

    for (auto& ls : lc->sections) {
        auto st = static_cast<sc::CkptSectionType>(ls.type);
        if (!ls.crc_ok) {
            result.all_segments_ok = false;
            continue;
        }
        switch (st) {
        case sc::CkptSectionType::kBm25Default: {
            std::unique_lock lk(fields_mu_);
            auto it = fields_.find(std::string(kDefaultField));
            if (it == fields_.end()) {
                auto inv = std::make_unique<bm25::InvertedIndex>(
                    config_.bm25_params, config_.index_positions);
                if (inv->deserialize(
                        std::span<const std::byte>(ls.payload.data(),
                                                    ls.payload.size()))) {
                    fields_.emplace(kDefaultField,
                                     std::move(inv));
                    bm25_loaded = true;
                } else {
                    result.all_segments_ok = false;
                }
            } else {
                bm25_loaded = it->second->deserialize(
                    std::span<const std::byte>(ls.payload.data(),
                                                ls.payload.size()));
                if (!bm25_loaded) result.all_segments_ok = false;
            }
            break;
        }
        case sc::CkptSectionType::kBm25Fields: {
            // 解析 u32 count; 每字段 [u16 nameLen][name][u64 invLen][inv]。
            const auto* p = ls.payload.data();
            const auto* end = p + ls.payload.size();
            if (end - p < 4) { result.all_segments_ok = false; break; }
            std::uint32_t cnt = get_u32_byte(p); p += 4;
            std::unique_lock lk(fields_mu_);
            for (std::uint32_t i = 0; i < cnt; ++i) {
                if (end - p < 2) { result.all_segments_ok = false; break; }
                std::uint16_t nlen = get_u16_byte(p); p += 2;
                if (end - p < nlen + 8) { result.all_segments_ok = false; break; }
                std::string name(reinterpret_cast<const char*>(p), nlen);
                p += nlen;
                std::uint64_t ilen = get_u64_byte(p); p += 8;
                if (end - p < static_cast<std::ptrdiff_t>(ilen)) {
                    result.all_segments_ok = false; break;
                }
                auto inv = std::make_unique<bm25::InvertedIndex>(
                    config_.bm25_params, config_.index_positions);
                if (inv->deserialize(std::span<const std::byte>(p, ilen))) {
                    fields_.emplace(std::move(name), std::move(inv));
                    bm25_loaded = true;
                } else {
                    result.all_segments_ok = false;
                }
                p += ilen;
            }
            break;
        }
        case sc::CkptSectionType::kDocmap: {
            auto covers = deserialize_docmap(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(ls.payload.data()),
                ls.payload.size()));
            if (covers) {
                docmap_loaded = true;
            } else {
                result.all_segments_ok = false;
            }
            break;
        }
        case sc::CkptSectionType::kHnsw: {
            auto cur = hnsw_.load(std::memory_order_acquire);
            if (cur) {
                auto fresh = std::make_shared<vec::HnswIndex>(cur->config());
                auto* raw = reinterpret_cast<const std::uint8_t*>(ls.payload.data());
                if (fresh->deserialize({raw, ls.payload.size()})) {
                    // V7:mmap vec payload(search.vec)。inmem_int8 无 payload
                    // (has_payload=false);否则 mmap,失败则段坏 → fold 重建。
                    if (fresh->config().inmem_int8 ||
                        fresh->load_vec_payload(vec_path)) {
                        hnsw_.store(std::move(fresh),
                                    std::memory_order_release);
                        hnsw_loaded = true;
                    } else {
                        result.all_segments_ok = false;
                    }
                } else {
                    result.all_segments_ok = false;
                }
            }
            break;
        }
        default:
            break;  // 未知段类型（meta/terms 等）忽略。
        }
    }

    // 如果 docmap 未载入,标记 all_segments_ok=false（需要 fold 补全 Index 侧表）。
    if (!docmap_loaded) result.all_segments_ok = false;
    // bm25 至少要有一个字段载入才算成功。
    if (!bm25_loaded) result.all_segments_ok = false;
    // 有向量配置但 hnsw 未载入 → 需要重建。
    if (config_.vector_dim > 0 && !hnsw_loaded) result.all_segments_ok = false;

    (void)from_prev;  // 仅调试用
    return result;
}

}  // namespace bitcask::search