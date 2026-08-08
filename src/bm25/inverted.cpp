#include "bitcask/intersect.hpp"
#include "bitcask/inverted.hpp"
#include "bm25_search_impl.hpp"  // S30-P1：BOW/WAND 共享实现（与 MmapSegment 共用）
#include "bitcask/myers.hpp"
#include "bitcask/term_snapshot_cache.hpp"
#include "bitcask/wildcard_matcher.hpp"
#include "bitcask/bm25_kernels.hpp"
#include "bitcask/detail/file_util.hpp"  // detail::FilePtr（RED-2 归并）
#include "bitcask/detail/cpu_features.hpp"  // S37-4：BITCASK_TSAN_ENABLED

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>       // S7-5：短语候选评分并行
#include <oneapi/tbb/parallel_reduce.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <functional>
#include <limits>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

namespace bitcask::bm25 {

// ===========================================================================
// PostingList
// ===========================================================================

auto PostingList::find(std::uint64_t ord) const -> std::size_t {
    // S22-M6：直接对 SoA 的 ords 数组二分（免逐 Posting 结构跳读）。
    auto it = std::lower_bound(ords.begin(), ords.end(), ord);
    if (it != ords.end() && *it == ord) {
        return static_cast<std::size_t>(it - ords.begin());
    }
    return ords.size();  // not found
}

bool PostingList::has(std::uint64_t ord) const {
    return find(ord) != ords.size();
}

namespace {

// S30-P1：block_for_ord_in / upper_bound_from / ScoredTerm(View) /
// score_bow_topk / search_wand 主体已抽到 bm25_search_impl.hpp（detail::），
// 与 MmapSegment（封口段 mmap reader）共用——「分数位级不变」由同一实现保证。
using detail::block_for_ord_in;
using detail::upper_bound_from;
using detail::ScoredTerm;
using detail::ScoredTermView;
using detail::score_bow_topk;

// 安全收集一个 shard 满足 pred 的 term key 快照（去重升序）。
// 这是遍历 tbb::concurrent_hash_map 的唯一安全原语，把一条实测复现过的
// 并发不变量集中到一处：遍历期间**不可** find（懒 rehash 节点搬迁致迭代器
// 重访/漏访）也**不可**裸读/改 slot 值（shared_ptr 可能被写者 CoW 替换、
// 裸读撕裂）——只读 key（节点 key 稳定）。调用方随后逐 key 经 accessor
// 取值/改值。sort+unique 兜住与单写者并发时 rehash 可能造成的重访去重。
template <typename Pred>
std::vector<std::string> collect_term_keys(
    const InvertedIndex::PostingMap& map, Pred pred) {
    std::vector<std::string> keys;
    for (auto it = map.begin(); it != map.end(); ++it) {
        if (pred(it->first)) keys.push_back(it->first);
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

// S27-3 步骤 5:TSan 注解——mutable_pl 的 CoW 协议(use_count==1 + acquire
// fence,与读者 shared_ptr 析构的 release 递减配对)语义正确,但 TSan 不建模
// atomic_thread_fence → 持引用出锁的读者(S29-1 BOW 快照 / phrase 零拷贝读)
// 与写者原地追加被误报 race。注解把协议边显式告知 TSan,零行为变化。
#if BITCASK_TSAN_ENABLED   // S37-4：见 detail/cpu_features.hpp
extern "C" void __tsan_acquire(void*);
extern "C" void __tsan_release(void*);
#define BITCASK_PL_TSAN_ACQUIRE(p) \
    __tsan_acquire(const_cast<void*>(static_cast<const void*>(p)))
#define BITCASK_PL_TSAN_RELEASE(p) \
    __tsan_release(const_cast<void*>(static_cast<const void*>(p)))
#else
#define BITCASK_PL_TSAN_ACQUIRE(p) (void)(p)
#define BITCASK_PL_TSAN_RELEASE(p) (void)(p)
#endif

// P2-min CoW：返回可安全原地修改的 PostingList。调用方必须持有该桶的写
// accessor。读者只能在桶读锁下取得 shared_ptr 引用（与写 accessor 互斥），
// 因此 use_count()==1 ⟺ 当前无 phrase/near 读者持引用 → 原地改安全；
// >1 则克隆替换，旧版本由读者的引用计数续命（对读者 immutable）。
// use_count() 是 relaxed load：观察到 1 后补 acquire fence，与读者析构
// shared_ptr 的 release 递减配对，确保读者的最后一次数据读 happens-before
// 写者的后续原地修改。
PostingList& mutable_pl(std::shared_ptr<PostingList>& sp) {
    if (!sp) {
        sp = std::make_shared<PostingList>();
    } else if (sp.use_count() > 1) {
        sp = std::make_shared<PostingList>(*sp);
    } else {
        std::atomic_thread_fence(std::memory_order_acquire);
        BITCASK_PL_TSAN_ACQUIRE(sp.get());  // fence 协议对 TSan 显式化
    }
    return *sp;
}

}  // namespace

auto PostingList::block_for_ord(std::uint64_t ord) const -> const PostingBlock* {
    return block_for_ord_in(blocks, ord);
}

auto PostingList::block_upper_bound(float idf, const Bm25Params& params, double avgdl) const -> float {
    if (ords.empty()) return 0.0f;
    // S10.9：直接读缓存的 global max_tf（note_appended 增量维护 / load 后重算），
    // 不再每次重扫全表。
    return upper_bound_from(max_tf, idf, params, avgdl);
}

void PostingList::snapshot_flat(FlatPostings& out) const {
    // S22-M6：SoA 后退化为整列拷贝（assign = memcpy，复用 out 容量）。
    out.ords.assign(ords.begin(), ords.end());
    out.tfs.assign(tfs.begin(), tfs.end());
    out.blocks = blocks;
    out.max_tf = max_tf;
}

auto FlatPostings::block_for_ord(std::uint64_t ord) const -> const PostingBlock* {
    return block_for_ord_in(blocks, ord);
}

auto FlatPostings::block_upper_bound(float idf, const Bm25Params& params, double avgdl) const -> float {
    if (ords.empty()) return 0.0f;
    return upper_bound_from(max_tf, idf, params, avgdl);
}

// ===========================================================================
// InvertedIndex
// ===========================================================================

InvertedIndex::~InvertedIndex() = default;

InvertedIndex::InvertedIndex(Bm25Params params, bool index_positions)
    : params_(params), index_positions_(index_positions) {}

void InvertedIndex::set_topk_use_maxscore(bool on) noexcept {
    detail::g_topk_use_maxscore.store(on ? 1 : 0, std::memory_order_relaxed);
}
bool InvertedIndex::topk_use_maxscore() noexcept {
    return detail::g_topk_use_maxscore.load(std::memory_order_relaxed) != 0;
}

auto InvertedIndex::shard_for(std::string_view term) -> Shard& {
    auto h = std::hash<std::string_view>{}(term);
    return shards_[h % kShardCount];
}

auto InvertedIndex::shard_for(std::string_view term) const -> const Shard& {
    auto h = std::hash<std::string_view>{}(term);
    return shards_[h % kShardCount];
}

// V6.3.1：懒重建排序词典侧表。Fast path 是 acquire-load dirty + shared_lock
// 读 vocab_（常驻基线数据规模下零分配）；dirty 时降级到 unique_lock 写锁下
// 从 shard.inverted 抽 key、sort+unique、装到新 shared_ptr 发布。释放锁前
// release-store false，与后续 add_doc 的 release-store true 形成 release/acquire
// 配对——读者之后 acquire-load 必看到 false → 进入 fast path 时 vocab_ 已含
// 该次 add_doc 新增的 key。
auto InvertedIndex::ensure_vocab(std::size_t shard_idx) const -> VocabView {
    auto& shard = shards_[shard_idx];

    if (!shard.vocab_dirty_.load(std::memory_order_acquire)) {
        std::shared_lock rlock(shard.vocab_mtx_);
        return {shard.vocab_, shard.vocab_extra_};
    }

    std::unique_lock wlock(shard.vocab_mtx_);
    // Double-check：持写锁时已 barrier 此前所有 release-store=true，relaxed
    // load 即可看到最新值；若已被并发线程重建则直接取现成快照。
    if (!shard.vocab_dirty_.load(std::memory_order_relaxed)) {
        return {shard.vocab_, shard.vocab_extra_};
    }

    // S13-F6：不再遍历 shard.inverted（本函数跑在查询线程，与 reducer 的
    // add_doc 插入并发——TBB 明确不支持遍历与插入并发）。key 全集 =
    // vocab_ ∪ vocab_extra_ ∪ vocab_delta_（add_doc/apply_delta/deserialize
    // 对每个新 term 在 vocab_mtx_ 下记账；term 永不从 map 删除）。
    //
    // S24-M9：重建只重排增量层（旧 extra + raw delta，O(extra+delta) 拷贝，
    // 阈值封顶）——base 不再逐串深拷（此前每次新词后的首查询全量 O(V)）。
    // 唯一性：is_new_term 只在 map 首插时为真 ⟹ delta ∩ (base ∪ extra) = ∅，
    // unique() 仅防御。
    std::vector<std::string> ex;
    ex.reserve((shard.vocab_extra_ ? shard.vocab_extra_->size() : 0) +
               shard.vocab_delta_.size());
    if (shard.vocab_extra_) {
        ex.insert(ex.end(), shard.vocab_extra_->begin(),
                  shard.vocab_extra_->end());
    }
    ex.insert(ex.end(),
              std::make_move_iterator(shard.vocab_delta_.begin()),
              std::make_move_iterator(shard.vocab_delta_.end()));
    shard.vocab_delta_.clear();
    std::sort(ex.begin(), ex.end());
    ex.erase(std::unique(ex.begin(), ex.end()), ex.end());

    const std::size_t base_n = shard.vocab_ ? shard.vocab_->size() : 0;
    if (!shard.vocab_ ||
        ex.size() > std::max(kVocabExtraMergeFloor, base_n / 8)) {
        // 无基线（首建/load 后）或增量层超阈：付一次 O(V) 归并成新基线
        // （两有序序列 std::merge，摊还频率 O(V/阈值)）。
        std::vector<std::string> merged;
        merged.reserve(base_n + ex.size());
        if (shard.vocab_) {
            std::merge(shard.vocab_->begin(), shard.vocab_->end(),
                       std::make_move_iterator(ex.begin()),
                       std::make_move_iterator(ex.end()),
                       std::back_inserter(merged));
        } else {
            merged = std::move(ex);
        }
        shard.vocab_ =
            std::make_shared<const std::vector<std::string>>(std::move(merged));
        shard.vocab_extra_.reset();
    } else {
        shard.vocab_extra_ =
            std::make_shared<const std::vector<std::string>>(std::move(ex));
    }
    shard.vocab_dirty_.store(false, std::memory_order_release);
    return {shard.vocab_, shard.vocab_extra_};
}

// ---- 写 ----

void InvertedIndex::add_doc(
    std::uint64_t ord,
    const TermPositions& term_data) {
    // 水位幂等：ord ≤ 已索引最大 ord ⟹ 该文档已在索引里（崩溃恢复时
    // replay_wal 重放快照已含的条目），整文档丢弃，避免 items 重复/乱序。
    // 正常追加 ord 单调递增 > 水位，一次比较即过（max_indexed_ord_ 初值 -1
    // 使首个文档 ord=0 也通过）。
    const std::uint64_t wm = max_indexed_ord_.load(std::memory_order_relaxed);
    if (wm != static_cast<std::uint64_t>(-1) && ord <= wm) {
        return;
    }
    max_indexed_ord_.store(ord, std::memory_order_relaxed);

    // v5 impacts:doc_len 先求和——posting 携带索引时 dl,封块算 min_dl。
    auto doc_len = std::uint32_t{0};
    for (auto& [term, data] : term_data) doc_len += data.first;

    for (auto& [term, data] : term_data) {
        auto& [tf, positions] = data;
        auto& shard = shard_for(term);
        PostingMap::accessor acc;
        const bool is_new_term = shard.inverted.insert(acc, term);  // true = 新 key
        PostingList& pl = mutable_pl(acc->second);  // P2-min：有 phrase 读者持引用时 CoW
        // S10.10：index_positions_=false 时不存 positions（省内存，短语/近邻失效）。
        pl.append(ord, tf, doc_len,
                  index_positions_ ? std::span<const std::uint32_t>(positions)
                                   : std::span<const std::uint32_t>{});
        pl.note_appended();  // S10.6：增量封块，在线索引也吃 WAND 块跳跃
        // V6.3.1：仅当新 key 时标脏——旧 term 的 posting list 增删不影响已排序
        // 的 vocab_ 集合。
        // S13-F6：新 term 同时在 vocab_mtx_ 下记入 delta（ensure_vocab 改为
        // 增量并集，不再遍历 map）。delta push 必须先于 dirty=true 的
        // release-store（同锁内），保证「读者观察到 dirty ⟹ delta 可见」。
        // 仅新 term 付锁开销，稳态（词表收敛后）零额外成本。
        if (is_new_term) {
            std::unique_lock vlock(shard.vocab_mtx_);
            shard.vocab_delta_.push_back(term);
            shard.vocab_dirty_.store(true, std::memory_order_release);
        }
        // S29-6B：posting 变更完成 → 失效查询线程的 term 快照缓存。
        shard.gen_.fetch_add(1, std::memory_order_release);
    }

    live_doc_count_.fetch_add(1, std::memory_order_relaxed);
    sum_doc_len_.fetch_add(doc_len, std::memory_order_relaxed);

}

void InvertedIndex::remove_doc(
    std::uint32_t doc_len,
    const std::unordered_map<std::string, std::uint32_t>& term_freqs) {
    // 写路径 V2 串行，guard 用 load + fetch_sub（reader 侧裸 load 已无 race）。
    if (live_doc_count_.load(std::memory_order_relaxed) > 0) {
        live_doc_count_.fetch_sub(1, std::memory_order_relaxed);
    }
    if (sum_doc_len_.load(std::memory_order_relaxed) >= doc_len) {
        sum_doc_len_.fetch_sub(doc_len, std::memory_order_relaxed);
    }

}

// ---- 查询 ----

namespace {
// S29-4：string_view → PostingMap key 的查找缓冲。tbb find 只收
// const std::string&，每调用 std::string(term) 在跨段×字段×term 聚合循环
// （multi_field_segment_search）下是纯增量堆分配——thread_local 复用容量，
// 稳态零分配。
const std::string& tls_term_key(std::string_view term) {
    static thread_local std::string buf;
    buf.assign(term);
    return buf;
}
}  // namespace

namespace {
// S29-6B：清空缓存条目的 fp（保留 vector 容量复用）。
void clear_fp(FlatPostings& fp) {
    fp.ords.clear();
    fp.tfs.clear();
    fp.blocks.clear();
    fp.max_tf = 0;
}
}  // namespace

// S27-2：term 的 doc frequency（posting list 长度；含未 merge 的已删，
// Lucene-style df，§4 接受该近似）。宿主跨段求和得全局 df。
// S29-6B：快照缓存快路径——gen 相等 ⇒ 自快照以来本 shard 无 posting 变更
// ⇒ 缓存 df 即当前列长，零锁零 RMW（分段查询 stage-1 逐段逐词调用本函数，
// 与 stage-2 search 共享同一条目，同段同词稳态 0 次 find）。
std::uint64_t InvertedIndex::doc_freq(std::string_view term) const {
    auto& shard = shard_for(term);
    if (query_cache_enabled()) {
        auto& cache = TermSnapshotCache::tls_instance();
        const std::uint64_t gen = shard.gen_.load(std::memory_order_acquire);
        if (const auto* e = cache.probe(index_id_, term, gen)) return e->df;
        PostingMap::const_accessor acc;
        const bool found = shard.inverted.find(acc, tls_term_key(term));
        const std::uint64_t df =
            found ? static_cast<std::uint64_t>(acc->second->size()) : 0;
        // 落缓存：present ⇒ df-only（不搬行，大 term 快照不划算；行由 search
        // 标量分支按需补齐）；absent ⇒ 缺席负缓存（has_rows=true + 空 fp）。
        if (auto* e = cache.upsert(index_id_, term, gen)) {
            e->df = df;
            if (!found) {
                clear_fp(e->fp);
                e->has_rows = true;
            }
        }
        return df;
    }
    PostingMap::const_accessor acc;
    if (shard.inverted.find(acc, tls_term_key(term))) {
        return static_cast<std::uint64_t>(acc->second->size());
    }
    return 0;
}

auto InvertedIndex::search(
    const std::vector<std::string>& query_terms,
    std::size_t k,
    const LiveChecker& live_checker,
    const Bm25Params* params_override,
    const ExtStats* ext) const -> std::vector<SearchResult> {
    const Bm25Params& params = params_override ? *params_override : params_;
    const std::uint64_t N =
        ext ? ext->N : live_doc_count_.load(std::memory_order_relaxed);
    const std::uint64_t sum_dl =
        ext ? ext->sum_dl : sum_doc_len_.load(std::memory_order_relaxed);

    // ---- S29-6B Phase 1：thread_local 快照缓存探测（零锁零 RMW） ----
    // 全词命中且 BOW 规模 ⇒ 直接从缓存条目评分：整条路径唯一的共享触点是
    // 每词一次 shard gen_ 的 acquire load（纯读,read-only 稳态下 cacheline
    // 保持 SHARED,线性扩展）——桶锁 RMW 与 shared_ptr 引用计数 RMW 全免
    //（此前每词 4 次共享 cacheline RMW 是 BOW 并发零扩展的根因）。
    // 设计:docs/design/s29-6b-inverted-term-cache.md。
    const bool use_cache = query_cache_enabled();
    auto& cache = TermSnapshotCache::tls_instance();
    static thread_local std::vector<const FlatPostings*> hit_fps;
    static thread_local std::vector<std::uint64_t> term_gens;
    static thread_local std::vector<ScoredTermView> views;
    if (use_cache) {
        cache.begin_query();
        hit_fps.assign(query_terms.size(), nullptr);
        term_gens.resize(query_terms.size());
        bool all_hit = true;
        std::size_t cached_total = 0;
        for (std::size_t i = 0; i < query_terms.size(); ++i) {
            auto& shard = shard_for(query_terms[i]);
            // gen 先于 find 读取(Phase 2 落缓存沿用):快照内容 ≥ gen 时刻
            // 状态,缓存声称的版本恒不晚于实际内容——过保守方向,安全。
            term_gens[i] = shard.gen_.load(std::memory_order_acquire);
            const auto* e = cache.probe(index_id_, query_terms[i], term_gens[i]);
            if (e != nullptr && e->has_rows) {
                hit_fps[i] = &e->fp;
                cached_total += e->fp.size();
            } else {
                all_hit = false;  // miss 或 df-only(行未搬)
            }
        }
        if (all_hit) {
            if (cached_total == 0) return {};  // 全缺席(负缓存)
            if (cached_total < detail::kWandRouteThreshold) {
                views.clear();
                for (std::size_t i = 0; i < query_terms.size(); ++i) {
                    if (hit_fps[i] != nullptr && !hit_fps[i]->empty()) {
                        views.push_back({&query_terms[i], hit_fps[i]});
                    }
                }
                return score_bow_topk(views, k, N, sum_dl, params,
                                      live_checker, ext ? ext->df : nullptr);
            }
            // WAND 规模：评分需 PostingList 指针,落 Phase 2(计算主导,
            // 每词几次 RMW 占比可忽略,有意不为其搬大快照进缓存)。
        }
    }

    // ---- Phase 2：慢路径 ----
    // P1：accessor 下只拷扁平快照（ords/tfs），不再深拷整个 PostingList。
    // S29-1：单趟 find——accessor 下拷出 shared_ptr（P2-min CoW 协议：读者
    // 持引用 → 写者见 use_count>1 时克隆替换，被引用的 PostingList 对本查询
    // 期间 immutable，与 phrase/near 读者同模式）。WAND 路由判定读 size()、
    // 标量/WAND 快照均复用该指针 → 每词桶锁 RMW 从 2 次降到 1 次。查询结束
    // 必须释放引用（guard 兜住所有出口），否则旧版本被长期钉住且迫使写者
    // 恒克隆。
    static thread_local std::vector<std::shared_ptr<const PostingList>> pls_pool;
    pls_pool.clear();
    struct ReleaseRefs {
        std::vector<std::shared_ptr<const PostingList>>& v;
        ~ReleaseRefs() {
            // TSan 注解:最后一次数据读 → release → 写者 fence-acquire 配对。
            for (const auto& p : v) {
                if (p) BITCASK_PL_TSAN_RELEASE(p.get());
            }
            v.clear();
        }
    } release_refs{pls_pool};
    std::size_t total_postings = 0;
    for (auto& term : query_terms) {
        auto& shard = shard_for(term);
        PostingMap::const_accessor acc;
        if (shard.inverted.find(acc, term)) {
            pls_pool.push_back(acc->second);
            total_postings += pls_pool.back()->size();
        } else {
            pls_pool.push_back(nullptr);
        }
    }
    // S29-6B：缺席词落负缓存（含 total==0 早退与 WAND 分支——重复的
    // 全缺席查询下次走零锁快路径）。
    if (use_cache) {
        for (std::size_t i = 0; i < query_terms.size(); ++i) {
            if (pls_pool[i]) continue;
            if (auto* e = cache.upsert(index_id_, query_terms[i],
                                       term_gens[i])) {
                clear_fp(e->fp);
                e->df = 0;
                e->has_rows = true;
            }
        }
    }
    if (total_postings == 0) return {};
    if (total_postings >= detail::kWandRouteThreshold) {
        return search_wand(query_terms, pls_pool, k, live_checker, params, ext);
    }

    // 标量路径：现在才快照（从已持有的指针，免第二趟 find）。
    // S29-6B：快照优先落缓存条目（下次同 gen 查询零锁命中）；upsert 失败
    //（探测窗口全被本查询钉住,罕见）回退 tps_pool 私有槽。
    // S23-M3：tps_pool thread_local 复用,稳态零分配;预 reserve 保证 views
    // 持有的槽地址在本查询内稳定（emplace 不再触发搬移）。
    using TermPostings = ScoredTerm;  // 共用条目（term + 扁平快照）
    static thread_local std::vector<TermPostings> tps_pool;
    tps_pool.reserve(query_terms.size());
    std::size_t n_tps = 0;
    views.clear();
    for (std::size_t i = 0; i < query_terms.size(); ++i) {
        const auto& pl = pls_pool[i];
        if (!pl) continue;
        if (use_cache) {
            if (auto* e = cache.upsert(index_id_, query_terms[i],
                                       term_gens[i])) {
                pl->snapshot_flat(e->fp);
                e->df = e->fp.size();
                e->has_rows = true;
                views.push_back({&query_terms[i], &e->fp});
                continue;
            }
        }
        if (n_tps == tps_pool.size()) tps_pool.emplace_back();
        TermPostings& tp = tps_pool[n_tps];
        tp.term.assign(query_terms[i]);
        pl->snapshot_flat(tp.fp);
        views.push_back({&tp.term, &tp.fp});
        ++n_tps;
    }
    if (views.empty()) return {};

    // bag-of-words 评分 + top-k（共享 kernel score_bow_topk）。
    // S27-2：ext 非空 → 用全局 N/sum_dl/df（G-on-the-fly）。
    return score_bow_topk(views, k, N, sum_dl, params, live_checker,
                          ext ? ext->df : nullptr);
}

// ===========================================================================
// explain —— BM25 评分分项解释（S8.8）
// ===========================================================================

auto InvertedIndex::explain(
    const std::vector<std::string>& query_terms,
    std::uint64_t ord,
    const LiveChecker& live_checker,
    const Bm25Params* params_override) const -> ScoreExplanation {
    const Bm25Params& params = params_override ? *params_override : params_;

    // S30-P1:分项计算主体抽到 detail::explain_impl(与 MmapSegment 共用)。
    // 本函数只采集快照(冷路径,per-term snapshot_flat 可接受)。
    static thread_local std::vector<ScoredTerm> tps_pool;
    static const FlatPostings kEmptyFp;
    // 两趟:先全部快照(emplace 扩容会搬移元素),后取指针建视图。
    std::vector<std::size_t> slot_of_term(query_terms.size(),
                                          static_cast<std::size_t>(-1));
    std::size_t n = 0;
    for (std::size_t qi = 0; qi < query_terms.size(); ++qi) {
        auto& shard = shard_for(query_terms[qi]);
        PostingMap::const_accessor acc;
        if (!shard.inverted.find(acc, query_terms[qi])) continue;
        if (n == tps_pool.size()) tps_pool.emplace_back();
        acc->second->snapshot_flat(tps_pool[n].fp);
        slot_of_term[qi] = n;
        ++n;
    }
    std::vector<ScoredTermView> views;
    views.reserve(query_terms.size());
    for (std::size_t qi = 0; qi < query_terms.size(); ++qi) {
        views.push_back({&query_terms[qi],
                         slot_of_term[qi] == static_cast<std::size_t>(-1)
                             ? &kEmptyFp
                             : &tps_pool[slot_of_term[qi]].fp});
    }
    return detail::explain_impl(
        views, ord, live_checker, params,
        live_doc_count_.load(std::memory_order_relaxed),
        sum_doc_len_.load(std::memory_order_relaxed));
}

// ===========================================================================
// Block-Max WAND
// ===========================================================================

auto InvertedIndex::search_wand(
    const std::vector<std::string>& query_terms,
    const std::vector<std::shared_ptr<const PostingList>>& pls,
    std::size_t k,
    const LiveChecker& live_checker,
    const Bm25Params& params,
    const ExtStats* ext) const -> std::vector<SearchResult> {
    // S30-P1：主体抽到 detail::search_wand_impl（与 MmapSegment 共用,分数
    // 位级不变由同一实现保证）。本包装完成两件原实例态工作:
    // ① S29-1 已持指针的快照(snapshot_flat 进 thread_local 池,免二趟桶锁,
    //   压实空槽);② N/sum_dl 按 ext-or-本地统计解析。
    // 池指针在两趟间稳定:先全部填充,再收集指针(emplace 增长会搬移元素)。
    static thread_local std::vector<FlatPostings> fp_pool;
    static thread_local std::vector<const FlatPostings*> fp_ptrs;
    static thread_local std::vector<std::string_view> term_views;
    std::size_t n = 0;
    for (std::size_t i = 0; i < query_terms.size(); ++i) {
        if (const auto& pl = pls[i]) {
            if (n == fp_pool.size()) fp_pool.emplace_back();
            pl->snapshot_flat(fp_pool[n]);
            ++n;
        }
    }
    fp_ptrs.clear();
    term_views.clear();
    std::size_t j = 0;
    for (std::size_t i = 0; i < query_terms.size(); ++i) {
        if (pls[i]) {
            fp_ptrs.push_back(&fp_pool[j++]);
            term_views.push_back(query_terms[i]);
        }
    }
    const auto N = ext ? ext->N : live_doc_count_.load(std::memory_order_relaxed);
    const auto sum_dl =
        ext ? ext->sum_dl : sum_doc_len_.load(std::memory_order_relaxed);
    return detail::search_topk_impl(term_views, fp_ptrs, k, live_checker,
                                    params, N, sum_dl,
                                    ext ? ext->df : nullptr,
                                    query_terms.size());
}

auto InvertedIndex::search_phrase_impl(
    const std::vector<std::string>& query_terms,
    std::size_t k,
    std::uint32_t slop,
    const LiveChecker& live_checker,
    const Bm25Params* params_override) const -> std::vector<SearchResult> {
    if (query_terms.empty()) return {};
    const Bm25Params& params = params_override ? *params_override : params_;

    // P2-min：持 shared_ptr 引用零拷贝读（原先深拷贝整列表含全部 positions）。
    // 安全性：写者对同 term 追加时经 mutable_pl 做 CoW（见 use_count 协议），
    // 本读者持有的对象自取得引用起不再被修改。
    // S30-P1:匹配/评分主体抽到 detail::phrase_search_impl(与 MmapSegment
    // 共用)——本函数只负责采集引用与统计解析。
    struct TermPostings {
        std::string term;
        std::shared_ptr<const PostingList> pl;
    };
    std::vector<TermPostings> tps;
    struct ReleaseTps {  // TSan 注解(同 search 的 ReleaseRefs)
        std::vector<TermPostings>& v;
        ~ReleaseTps() {
            for (const auto& t : v) {
                if (t.pl) BITCASK_PL_TSAN_RELEASE(t.pl.get());
            }
        }
    } release_tps{tps};
    tps.reserve(query_terms.size());

    for (auto& term : query_terms) {
        auto& shard = shard_for(term);
        PostingMap::const_accessor acc;
        if (!shard.inverted.find(acc, term)) return {};
        tps.push_back({term, acc->second});
    }
    std::vector<const PostingList*> pls;
    pls.reserve(tps.size());
    for (const auto& t : tps) pls.push_back(t.pl.get());

    const auto N = live_doc_count_.load(std::memory_order_relaxed);
    const auto sum_dl = sum_doc_len_.load(std::memory_order_relaxed);
    return detail::phrase_search_impl(pls, k, slop, live_checker, params, N,
                                      sum_dl);
}

auto InvertedIndex::search_phrase(
    const std::vector<std::string>& query_terms,
    std::size_t k,
    const LiveChecker& live_checker,
    const Bm25Params* params_override) const -> std::vector<SearchResult> {
    return search_phrase_impl(query_terms, k, /*slop=*/0, live_checker, params_override);
}

auto InvertedIndex::search_near(
    const std::vector<std::string>& query_terms,
    std::size_t k,
    std::uint32_t slop,
    const LiveChecker& live_checker,
    const Bm25Params* params_override) const -> std::vector<SearchResult> {
    return search_phrase_impl(query_terms, k, slop, live_checker, params_override);
}

auto InvertedIndex::search_wildcard(
    const std::string& pattern,
    std::size_t k,
    const LiveChecker& live_checker,
    const Bm25Params* params_override) const -> std::vector<SearchResult> {
    const Bm25Params& params = params_override ? *params_override : params_;

    using TermPostings = ScoredTerm;  // 共用条目（term + 扁平快照）

    // P2.5：最长字面量预过滤——不含该子串的词必不匹配，免跑回溯匹配器
    // （string_view::find 底层是 SIMD 化的 memchr/memcmp）。
    const std::string_view lit = longest_literal(pattern);

    // V6.3.1：模式有「首段字面量」（首字符非通配符）→ 从该段起跑 binary search
    // 划出候选区间，再在区间内走最长字面量 + wildcard_match 精筛。中缀/后缀
    // 模式（pattern[0]=='*'）走全扫——排序数组虽 cache 友好但 binary search 失效。
    std::string prefix;
    if (!pattern.empty() && pattern[0] != '*') {
        for (char c : pattern) {
            if (c == '*' || c == '?') break;
            prefix.push_back(c);
        }
    }
    // upper_bound 端点用 prefix 的「下一个串」：末字节 +1 即可（例如 "te" → "tf"）。
    // empty prefix 表示走全扫分支。
    std::string prefix_upper;
    if (!prefix.empty()) {
        prefix_upper = prefix;
        prefix_upper.back() = static_cast<char>(static_cast<unsigned char>(prefix_upper.back()) + 1);
    }

    // S10.4：并行扫词表匹配 pattern。按 shard 下标分区，每个 shard 至多被一个任务
    // 遍历（互不重叠），与既有「查询无锁读」模型一致（拷贝 plist 不持桶锁）。
    // V6.3.1：每 shard 取排序 vocab_ 替代 hash_map 全扫 + sort——读路径吃
    // shared_lock（fast path）零分配，binary search 区间再经 lit + wildcard_match
    // 二次过滤。
    std::vector<TermPostings> tps = tbb::parallel_reduce(
        tbb::blocked_range<std::size_t>(0, kShardCount),
        std::vector<TermPostings>{},
        [&](const tbb::blocked_range<std::size_t>& range, std::vector<TermPostings> local) {
            for (std::size_t s = range.begin(); s < range.end(); ++s) {
                // S24-M9：两层视图（base + extra 各自有序）——层间无序不影响
                // 结果（tps 是集合，BM25 按 term 求和顺序无关），逐层独立
                // 二分/扫即可。base ∩ extra = ∅（is_new_term 唯一性）。
                const auto view = ensure_vocab(s);
                for (const auto* vp : {view.base.get(), view.extra.get()}) {
                    if (!vp) continue;
                    const auto& v = *vp;

                    // 候选区间：prefix 模式用 [lower_bound(prefix),
                    // upper_bound(prefix_upper))，其它模式（无 prefix）用全层。
                    auto begin = v.begin();
                    auto end   = v.end();
                    if (!prefix.empty()) {
                        begin = std::lower_bound(v.begin(), v.end(), prefix);
                        end   = std::upper_bound(v.begin(), v.end(),
                                                 prefix_upper);
                    }

                    // 两阶段：先在排序区间内跑 lit 预过滤 + wildcard_match
                    // 收 key；再逐 key 经 const_accessor 取值（并发不变量见
                    // collect_term_keys）。
                    for (auto it = begin; it != end; ++it) {
                        const std::string& t = *it;
                        if (!lit.empty() && t.find(lit) == std::string::npos) {
                            continue;
                        }
                        if (!wildcard_match(pattern, t)) continue;
                        PostingMap::const_accessor acc;
                        if (!shards_[s].inverted.find(acc, t)) continue;
                        TermPostings tp;
                        tp.term = t;
                        acc->second->snapshot_flat(tp.fp);
                        local.push_back(std::move(tp));
                    }
                }
            }
            return local;
        },
        [](std::vector<TermPostings> a, const std::vector<TermPostings>& b) {
            a.insert(a.end(), b.begin(), b.end());
            return a;
        });

    if (tps.empty()) return {};

    // bag-of-words 评分 + top-k（共享 kernel score_bow_topk）。
    // S29-6B：内核收视图——tps 为本函数局部容器,评分期间地址稳定。
    std::vector<ScoredTermView> tv;
    tv.reserve(tps.size());
    for (const auto& tp : tps) tv.push_back({&tp.term, &tp.fp});
    return score_bow_topk(tv, k,
                          live_doc_count_.load(std::memory_order_relaxed),
                          sum_doc_len_.load(std::memory_order_relaxed),
                          params, live_checker);
}

auto InvertedIndex::bool_search(
    const QueryNode& query,
    std::size_t k,
    const LiveChecker& live_checker,
    const Bm25Params* params_override) const -> std::vector<SearchResult> {
    const Bm25Params& params = params_override ? *params_override : params_;
    // S30-P1:主体抽到 detail::bool_search_impl(与 MmapSegment 共用)。
    auto fetch = [this](std::string_view term, FlatPostings& out) {
        auto& shard = shard_for(term);
        PostingMap::const_accessor acc;
        if (!shard.inverted.find(acc, tls_term_key(term))) return false;
        acc->second->snapshot_flat(out);
        return true;
    };
    return detail::bool_search_impl(
        query, k, live_checker, params,
        live_doc_count_.load(std::memory_order_relaxed),
        sum_doc_len_.load(std::memory_order_relaxed), fetch);
}

// S13-D9：树形布尔求值（契约见 inverted.hpp）。
auto InvertedIndex::bool_search_tree(
    const QueryNode& root,
    std::size_t k,
    const LiveChecker& live_checker,
    const Bm25Params* params_override) const -> std::vector<SearchResult> {
    const Bm25Params& params = params_override ? *params_override : params_;
    // S30-P1:主体抽到 detail::bool_tree_impl(与 MmapSegment 共用)。
    auto fetch = [this](std::string_view term, FlatPostings& out) {
        auto& shard = shard_for(term);
        PostingMap::const_accessor acc;
        if (!shard.inverted.find(acc, tls_term_key(term))) return false;
        acc->second->snapshot_flat(out);
        return true;
    };
    auto phrase_fn = [&](const std::vector<std::string>& terms) {
        return search_phrase(terms, std::numeric_limits<std::size_t>::max(),
                             live_checker, params_override);
    };
    return detail::bool_tree_impl(
        root, k, live_checker, params,
        live_doc_count_.load(std::memory_order_relaxed),
        sum_doc_len_.load(std::memory_order_relaxed), fetch, phrase_fn);
}

auto InvertedIndex::search_fuzzy(
    const std::vector<std::string>& query_terms,
    std::size_t k,
    std::uint32_t max_edit_distance,
    const LiveChecker& live_checker,
    const Bm25Params* params_override) const -> std::vector<SearchResult> {
    if (query_terms.empty()) return {};
    const Bm25Params& params = params_override ? *params_override : params_;

    using TermPostings = ScoredTerm;  // 共用条目（term + 扁平快照）
    std::vector<TermPostings> tps;

    // 翻转循环：vocab term 放外层、query term 放内层 + break。
    // ① S10.2：每个 vocab term 至多入 tps 一次——原先 query 多词模糊命中同一 term
    //    会重复 push，导致该 posting list 被评分两遍、IDF 贡献翻倍。
    // ② S10.3：跑 O(n·m) levenshtein 前先按字节长度差剪枝——编辑距离 ≥ |长度差|，
    //    故长度差 > max_edit 时必不匹配，省掉绝大多数 DP（levenshtein 按字节算，用字节长度）。
    // P2.3：每个查询词建一次 Myers matcher（Peq 表摊销于全词典扫描）。
    // 经典 DP O(n·m) + 每次调用两个 vector 堆分配 → 位并行 O(n)、零分配，
    // 原理见 doc/myers-bitparallel-zh.md，对拍见 fuzzy_test.cpp。
    std::vector<MyersMatcher> matchers;
    matchers.reserve(query_terms.size());
    for (auto& q : query_terms) matchers.emplace_back(q);

    // S13-P8.4：并行扫词表（镜像 search_wildcard 的 parallel_reduce 结构——
    // 按 shard 分区互不重叠；此前串行扫全部 64 shard，Myers DP 是纯 CPU 热点）。
    // matchers[].within 需可变内部态？MyersMatcher 是 per-查询词只读 Peq 表 +
    // 局部 DP——within 为 const 纯计算，跨线程并发安全。
    // V6.3.1：排序 vocab_ 线性扫（编辑距离不保序，不能 binary search）。
    tps = tbb::parallel_reduce(
        tbb::blocked_range<std::size_t>(0, kShardCount),
        std::vector<TermPostings>{},
        [&](const tbb::blocked_range<std::size_t>& range,
            std::vector<TermPostings> local) {
            for (std::size_t si = range.begin(); si < range.end(); ++si) {
                // S24-M9：两层线性扫（编辑距离不保序，本就全扫）。
                const auto view = ensure_vocab(si);
                for (const auto* vp : {view.base.get(), view.extra.get()}) {
                if (!vp) continue;
                for (const auto& term : *vp) {
                    bool hit = false;
                    for (std::size_t qi = 0; qi < query_terms.size(); ++qi) {
                        auto& query_term = query_terms[qi];
                        auto len_diff = term.size() > query_term.size()
                                            ? term.size() - query_term.size()
                                            : query_term.size() - term.size();
                        if (len_diff > max_edit_distance) continue;
                        if (matchers[qi].within(term, max_edit_distance)) {
                            hit = true;
                            break;
                        }
                    }
                    if (!hit) continue;
                    PostingMap::const_accessor acc;
                    if (!shards_[si].inverted.find(acc, term)) continue;
                    TermPostings tp;
                    tp.term = term;
                    acc->second->snapshot_flat(tp.fp);
                    local.push_back(std::move(tp));
                }
                }
            }
            return local;
        },
        [](std::vector<TermPostings> a, const std::vector<TermPostings>& b) {
            a.insert(a.end(), b.begin(), b.end());
            return a;
        });
    // 评分一致性：score_bow_topk 对 tps 排序不敏感？——与 wildcard 同一前提
    // （tps 集合相同、每 term 恰一份；BM25 贡献按 term 求和，顺序无关）。

    if (tps.empty()) return {};

    // bag-of-words 评分 + top-k（共享 kernel score_bow_topk）。
    // S29-6B：内核收视图——tps 为本函数局部容器,评分期间地址稳定。
    std::vector<ScoredTermView> tv;
    tv.reserve(tps.size());
    for (const auto& tp : tps) tv.push_back({&tp.term, &tp.fp});
    return score_bow_topk(tv, k,
                          live_doc_count_.load(std::memory_order_relaxed),
                          sum_doc_len_.load(std::memory_order_relaxed),
                          params, live_checker);
}

// ---- 统计 ----

auto InvertedIndex::live_doc_count() const -> std::uint64_t {
    return live_doc_count_.load(std::memory_order_relaxed);
}

auto InvertedIndex::sum_doc_len() const -> std::uint64_t {
    return sum_doc_len_.load(std::memory_order_relaxed);
}

auto InvertedIndex::avg_doc_len() const -> double {
    auto n = live_doc_count_.load(std::memory_order_relaxed);
    if (n == 0) return 0.0;
    return static_cast<double>(sum_doc_len_.load(std::memory_order_relaxed)) / static_cast<double>(n);
}

auto InvertedIndex::df(std::string_view term) const -> std::size_t {
    auto& shard = shard_for(term);
    PostingMap::const_accessor acc;
    if (!shard.inverted.find(acc, tls_term_key(term))) return 0;  // S29-4
    return acc->second->size();
}

auto InvertedIndex::df_live(std::string_view term, const LiveChecker& live_checker) const -> std::size_t {
    auto& shard = shard_for(term);
    PostingMap::const_accessor acc;
    if (!shard.inverted.find(acc, tls_term_key(term))) return 0;  // S29-4
    std::size_t count = 0;
    for (auto ord : acc->second->ords) {
        if (live_checker.is_live(ord)) ++count;
    }
    return count;
}

bool InvertedIndex::snapshot_postings(std::string_view term,
                                      PostingList& out) const {
    const auto& shard = shard_for(term);
    PostingMap::const_accessor acc;
    if (!shard.inverted.find(acc, tls_term_key(term))) return false;
    out = *acc->second;  // 深拷(merge 消费静止段;含 positions)
    return true;
}

void InvertedIndex::visit_postings_sorted(
    const std::function<void(std::string_view term, const PostingList& pl)>& fn)
    const {
    // 全局 term 升序:各 shard key 快照合并后整体排序(shard 划分按 hash,
    // 跨 shard 无序);逐 key 经 const_accessor 取值(不变量见
    // collect_term_keys)。O(T log T),封口点一次性成本。
    std::vector<std::string> keys;
    for (const auto& shard : shards_) {
        auto part = collect_term_keys(shard.inverted,
                                      [](const std::string&) { return true; });
        keys.insert(keys.end(), std::make_move_iterator(part.begin()),
                    std::make_move_iterator(part.end()));
    }
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys) {
        const auto& shard = shard_for(key);
        PostingMap::const_accessor acc;
        if (!shard.inverted.find(acc, key)) continue;
        fn(key, *acc->second);
    }
}

void InvertedIndex::finalize_all_postings() {
    // 先快照全部 key，再逐 key 持写 accessor 经 mutable_pl 修改（迭代器裸改
    // 会绕过 CoW 协议；并发不变量见 collect_term_keys）。
    for (auto& shard : shards_) {
        auto keys = collect_term_keys(shard.inverted,
                                      [](const std::string&) { return true; });
        for (auto& key : keys) {
            PostingMap::accessor acc;
            if (!shard.inverted.find(acc, key)) continue;
            mutable_pl(acc->second).finalize();
        }
        // S13-F6：finalize 不改 key 集合，vocab_ 无需失效（曾保守标脏；
        // 增量 delta 设计下 dirty+空 delta 只会触发一次无谓的全量拷贝重建）。
        // S29-6B：blocks 重建进快照（fp.blocks）→ 失效缓存。
        if (!keys.empty()) {
            shard.gen_.fetch_add(1, std::memory_order_release);
        }
    }
}

std::size_t InvertedIndex::total_postings() const {
    std::size_t n = 0;
    for (const auto& shard : shards_) {
        for (auto it = shard.inverted.begin(); it != shard.inverted.end(); ++it) {
            n += it->second->size();
        }
    }
    return n;
}

auto InvertedIndex::compact(const LiveChecker& live_checker, double dead_ratio_threshold)
    -> std::size_t {
    std::size_t compacted = 0;
    for (auto& shard : shards_) {
        // 先快照 key 列表，再逐 key 持写 accessor 压实：写锁与并发查询的
        // const_accessor 互斥，保证查询不读到半压实状态（不变量见
        // collect_term_keys）。
        auto keys = collect_term_keys(shard.inverted,
                                      [](const std::string&) { return true; });
        std::vector<std::uint64_t> ords_buf;
        std::vector<char> live_buf;
        for (auto& key : keys) {
            PostingMap::accessor acc;
            if (!shard.inverted.find(acc, key)) continue;
            const PostingList& pl = *acc->second;
            if (pl.empty()) continue;

            // P2.4：live 批量取一次——此前死点统计与压实各自逐 posting
            // 一次带锁虚调用（大列表 = 数十万次锁）。
            // S22-M6：整列 assign。
            ords_buf.assign(pl.ords.begin(), pl.ords.end());
            live_buf.resize(ords_buf.size());
            live_checker.fill_is_live(ords_buf, live_buf);

            std::size_t dead = 0;
            for (std::size_t i = 0; i < live_buf.size(); ++i) {
                dead += static_cast<std::size_t>(!live_buf[i]);
            }
            if (dead == 0) continue;
            double ratio = static_cast<double>(dead) / static_cast<double>(pl.size());
            if (ratio < dead_ratio_threshold) continue;

            // mutable_pl 可能因 phrase 读者持引用而克隆——克隆保序保内容，
            // live_buf 与行的下标对齐不受影响。
            if (mutable_pl(acc->second).compact_flags(live_buf)) {
                ++compacted;
                // S29-6B：posting 行删除 → 失效缓存（逐 key bump 而非逐
                // shard 汇总——compact 非热路径,简单优先）。
                shard.gen_.fetch_add(1, std::memory_order_release);
            }
        }
        // V6.3.1：compact 不删 key（保留空 posting list 是有意设计——避免与
        // 写者抢桶锁）。S13-F6：key 集不变 ⇒ vocab_ 无需失效（曾保守标脏；
        // 增量 delta 设计下只会触发无谓的全量拷贝重建）。该「key 永不删除」
        // 不变量是 vocab delta 设计的前提，改动此处需同步重审 ensure_vocab。
    }
    return compacted;
}

// ---- 持久化 ----

static constexpr std::uint32_t kInvMagic   = 0x494E5632;
// v6：ord 改用 FOR(Frame-of-Reference) 块压缩（128/块），TFs/dls 改用 VByte varint
//     整组编码；不再支持 v1..v5（旧快照需先经外部工具迁移或不加载）。
static constexpr std::uint32_t kInvVersion = 6;

// load() 反序列化上限：防止损坏或恶意文件触发 OOM。
// S31:单 term 字节上限——超限跳过插入(容错),不再整段拒收。与
// AnalyzerConfig::max_token_bytes 默认一致(写端过滤 + 读端容错双保险)。
static constexpr std::uint32_t kMaxTermBytes           = 1024;
static constexpr std::uint32_t kMaxPostingsPerTerm     = 1u << 24;  // ~16M
static constexpr std::uint32_t kMaxPositionsPerPosting = 1u << 20;  // ~1M
static constexpr std::uint32_t kMaxBlocksPerTerm       = 1u << 17;  // ~131k

// FOR (Frame of Reference) 块压缩：对一块已排序 ord 序列，按 (frame, bits, packed)
// 三元组编码。frame = 块内最小 ord（升序故 = 第一条），delta[i] = ords[i] - frame；
// bits = ceil(log2(max_delta+1))（max_delta=0 时 bits=0，无 packed 字节）。
// 解码：delta[i] = unpack(packed, i, bits); ords[i] = frame + delta[i]。
namespace {

// 把一个 uint64 值的低 bits 位塞入 dst 的第 bit_pos 位起，返回新 bit_pos。
// 大端序：值的高位先写；单字节内 0 位 = 最高位。
inline std::size_t for_pack_u64(std::uint64_t v, std::uint8_t bits,
                               std::uint8_t* dst, std::size_t bit_pos) {
    for (int b = static_cast<int>(bits) - 1; b >= 0; --b) {
        std::size_t i = bit_pos >> 3;
        std::size_t off = bit_pos & 7;
        std::uint8_t bit = static_cast<std::uint8_t>((v >> b) & 1);
        dst[i] |= static_cast<std::uint8_t>(bit << (7 - off));
        ++bit_pos;
    }
    return bit_pos;
}

// 从 src 第 bit_pos 起读 bits 位，组装为 uint64（值的高位在前）。
inline std::uint64_t for_unpack_u64(const std::uint8_t* src, std::size_t bit_pos,
                                    std::uint8_t bits) {
    std::uint64_t v = 0;
    for (std::uint8_t b = 0; b < bits; ++b) {
        std::size_t i = bit_pos >> 3;
        std::size_t off = bit_pos & 7;
        std::uint8_t bit = static_cast<std::uint8_t>((src[i] >> (7 - off)) & 1);
        v = (v << 1) | bit;
        ++bit_pos;
    }
    return v;
}

inline void for_encode_block(const std::uint64_t* ords, std::size_t count,
                             std::uint64_t& frame, std::uint8_t& bits,
                             std::vector<std::uint8_t>& packed) {
    frame = ords[0];
    if (count == 1) {
        bits = 0;
        packed.clear();
        return;
    }
    std::uint64_t max_delta = ords[count - 1] - frame;
    if (max_delta == 0) {
        bits = 0;
        packed.clear();
        return;
    }
    // ceil(log2(max_delta+1))
    std::uint8_t need = 0;
    std::uint64_t m = max_delta;
    while (m > 0) { ++need; m >>= 1; }
    bits = need;
    std::size_t total_bits = static_cast<std::size_t>(bits) * count;
    std::size_t total_bytes = (total_bits + 7) >> 3;
    packed.assign(total_bytes, 0);
    std::size_t pos = 0;
    for (std::size_t i = 0; i < count; ++i) {
        pos = for_pack_u64(ords[i] - frame, bits, packed.data(), pos);
    }
}

inline void for_decode_block(std::uint64_t frame, std::uint8_t bits,
                             const std::uint8_t* packed,
                             std::size_t count, std::uint64_t* out) {
    if (bits == 0) {
        for (std::size_t i = 0; i < count; ++i) out[i] = frame;
        return;
    }
    // S13-P8：64-bit 窗口批量解包（原逐 bit 循环每值 ~bits 次移位；v6
    // checkpoint 加载对每个 ord 付此代价，大索引启动主导项之一）。流是
    // MSB-first 大端位序：窗口 8 字节大端载入后一次移位+掩码取值，与逐
    // bit 版位级等价。bits>56 时 off+bits 可跨 9 字节 → 整体回退逐 bit
    //（ord delta 位宽 >56 实际不出现，纯正确性兜底）；尾部不足 8 字节的
    // 值同样回退（越界安全）。
    const std::size_t total_bits = static_cast<std::size_t>(bits) * count;
    const std::size_t total_bytes = (total_bits + 7) >> 3;
    std::size_t i = 0;
    if (bits <= 56) {
        const std::uint64_t mask = (1ull << bits) - 1;
        std::size_t pos = 0;
        for (; i < count; ++i, pos += bits) {
            const std::size_t byte_i = pos >> 3;
            if (byte_i + 8 > total_bytes) break;  // 尾部回退逐 bit
            std::uint64_t w;
            std::memcpy(&w, packed + byte_i, 8);
            w = std::byteswap(w);  // LE 主机（codec 已 static_assert）
            const unsigned off = static_cast<unsigned>(pos & 7);
            out[i] = frame + ((w >> (64u - off - bits)) & mask);
        }
    }
    std::size_t pos = static_cast<std::size_t>(bits) * i;
    for (; i < count; ++i, pos += bits) {
        out[i] = frame + for_unpack_u64(packed, pos, bits);
    }
}

}  // namespace

void InvertedIndex::serialize(std::vector<std::byte>& out) const {
    // P14e:I/O 改为追加缓冲(原生小端,字节与旧 FILE 版完全一致);追加不会
    // 失败,故去掉所有 ok/fclose 错误样板。并发安全遍历(collect_term_keys +
    // const_accessor 快照)逐字保留。
    std::uint32_t N = static_cast<std::uint32_t>(live_doc_count_.load(std::memory_order_relaxed));
    std::uint64_t sdl = sum_doc_len_.load(std::memory_order_relaxed);

    auto put = [&](const void* p, std::size_t n) {
        const auto* b = reinterpret_cast<const std::byte*>(p);
        out.insert(out.end(), b, b + n);
    };
    auto write_u32 = [&](std::uint32_t v) { put(&v, 4); };
    auto write_u64 = [&](std::uint64_t v) { put(&v, 8); };
    auto write_u8  = [&](std::uint8_t  v) { put(&v, 1); };

    // positions：沿用 v4+ 的 gap+VByte 压缩（u32 原始个数 + u32 压缩字节数 + 字节流）。
    auto write_positions = [&](std::span<const std::uint32_t> positions) {
        write_u32(static_cast<std::uint32_t>(positions.size()));
        std::vector<std::uint64_t> tmp(positions.begin(), positions.end());
        auto comp = codec::gap_encode(tmp);
        write_u32(static_cast<std::uint32_t>(comp.size()));
        if (!comp.empty()) put(comp.data(), comp.size());
    };

    write_u32(kInvMagic);
    write_u32(kInvVersion);
    write_u32(N);
    write_u64(sdl);

    constexpr std::size_t kBlock = PostingList::kBlockSize;

    for (auto& shard : shards_) {
        // 安全遍历:先快照 key,再逐 key 经 const_accessor 取 shared_ptr。
        // save 在 merge 线程跑,与 put→worker 的 add_doc 并发——裸遍历
        // concurrent_hash_map 会因懒 rehash 重访/漏访,裸读 plsp 还会撞上
        // CoW 替换/撕裂(不变量集中见 collect_term_keys)。const_accessor 持
        // shared_ptr 期间数据 immutable(写者见 use_count>1 则克隆)。
        auto keys = collect_term_keys(shard.inverted,
                                      [](const std::string&) { return true; });
        std::vector<std::pair<const std::string*, std::shared_ptr<PostingList>>> snap;
        snap.reserve(keys.size());
        for (const auto& key : keys) {
            PostingMap::const_accessor acc;
            if (shard.inverted.find(acc, key)) {
                snap.emplace_back(&key, acc->second);
            }
        }

        write_u32(static_cast<std::uint32_t>(snap.size()));

        for (auto& [termp, plsp] : snap) {
            const std::string& term = *termp;
            const PostingList& pl = *plsp;
            auto tlen = static_cast<std::uint32_t>(term.size());
            write_u32(tlen);
            put(term.data(), tlen);

            auto pc = static_cast<std::uint32_t>(pl.size());
            write_u32(pc);

            // v6：ord 改用 FOR 块压缩（128/块）。
            std::size_t ord_block_count = (pc + kBlock - 1) / kBlock;
            write_u32(static_cast<std::uint32_t>(ord_block_count));
            // ⑭ 块间复用缓冲（容量只增）：for_encode_block 内部自 clear/assign。
            // S22-M6：SoA 后 ords 列直接按块喂编码器，免 ords_view 物化拷贝。
            std::vector<std::uint8_t> packed;
            for (std::size_t b = 0; b < ord_block_count; ++b) {
                std::size_t start = b * kBlock;
                std::size_t cnt = std::min(kBlock, static_cast<std::size_t>(pc) - start);
                std::uint64_t frame;
                std::uint8_t  bits;
                for_encode_block(pl.ords.data() + start, cnt, frame, bits, packed);
                auto packed_len = static_cast<std::uint32_t>(packed.size());
                write_u64(frame);
                write_u8(bits);
                write_u32(packed_len);
                if (packed_len > 0) put(packed.data(), packed_len);
            }

            // v6：TFs/dls 改用 VByte varint 整组编码（每个 tf 通常 1-10，占 1B）。
            // 逐项 tf=0 也合法，VByte 对 0 仍编 1B（0x80），正确。
            {
                std::vector<std::uint8_t> tf_buf;
                tf_buf.reserve(pc);
                for (auto tf : pl.tfs) {
                    codec::vbyte_encode(tf, tf_buf);
                }
                write_u32(static_cast<std::uint32_t>(tf_buf.size()));
                if (!tf_buf.empty()) put(tf_buf.data(), tf_buf.size());
            }
            {
                std::vector<std::uint8_t> dl_buf;
                dl_buf.reserve(pc);
                for (auto dl : pl.dls) {
                    codec::vbyte_encode(dl, dl_buf);
                }
                write_u32(static_cast<std::uint32_t>(dl_buf.size()));
                if (!dl_buf.empty()) put(dl_buf.data(), dl_buf.size());
            }

            // positions：保持 v4+ 的逐 posting gap+VByte 格式不变。
            for (std::size_t i = 0; i < pc; ++i) {
                write_positions(pl.positions(i));
            }

            // Block-Max WAND 元数据：保持 v5 结构。
            write_u32(static_cast<std::uint32_t>(pl.blocks.size()));
            for (auto& blk : pl.blocks) {
                write_u64(blk.base_ord);
                write_u64(blk.end_ord);
                write_u32(blk.max_tf);
                write_u32(blk.min_dl);
                write_u32(static_cast<std::uint32_t>(blk.start_idx));
                write_u32(static_cast<std::uint32_t>(blk.count));
            }
        }
    }
}

auto InvertedIndex::save(std::string_view path) const -> bool {
    std::vector<std::byte> buf;
    serialize(buf);
    auto* f = bitcask::detail::fopen_utf8(std::string(path), "wb");
    if (!f) return false;
    const bool wrote =
        buf.empty() || std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    return wrote;
}

auto InvertedIndex::load(std::string_view path) -> bool {
    auto buf = bitcask::detail::read_file_bytes<>(std::string(path));
    if (!buf) return false;
    return deserialize(*buf);
}

auto InvertedIndex::deserialize(std::span<const std::byte> bytes) -> bool {
    // S29-6B：整体重灌 → 出口(含失败半填路径)全量失效 term 快照缓存。
    struct BumpAllGens {
        std::array<Shard, kShardCount>& shards;
        ~BumpAllGens() {
            for (auto& s : shards) {
                s.gen_.fetch_add(1, std::memory_order_release);
            }
        }
    } bump_gens{shards_};

    // P14e:从字节缓冲反序列化,游标带界检查;读越界返回哨兵(同旧 fread 短读
    // 语义,下游既有哨兵判定捕获)。原生小端,字节与 save() 一致。
    const std::byte* d = bytes.data();
    const std::size_t n = bytes.size();
    std::size_t pos = 0;
    // S9-P2-e：读越界哨兵——read_u* 越界时返回全 1，下游 `== kReadFail*` 比较捕获
    // 短读（同旧 fread 短读语义）。正常的 count/len 字段不会取到全 1（更有后续
    // `> kMax…` 上界校验兜底），故全 1 作哨兵无歧义。
    constexpr std::uint32_t kReadFail32 = 0xFFFFFFFFu;
    constexpr std::uint64_t kReadFail64 = 0xFFFFFFFFFFFFFFFFull;
    constexpr std::uint8_t  kReadFail8  = 0xFFu;
    auto read_u32 = [&]() -> std::uint32_t {
        if (pos + 4 > n) return kReadFail32;
        std::uint32_t v; std::memcpy(&v, d + pos, 4); pos += 4; return v;
    };
    auto read_u64 = [&]() -> std::uint64_t {
        if (pos + 8 > n) return kReadFail64;
        std::uint64_t v; std::memcpy(&v, d + pos, 8); pos += 8; return v;
    };
    auto read_u8 = [&]() -> std::uint8_t {
        if (pos + 1 > n) return kReadFail8;
        std::uint8_t v = static_cast<std::uint8_t>(d[pos]); pos += 1; return v;
    };
    // 读 len 字节进 dst;越界返回 false(同旧 fread 短读失败)。
    auto read_bytes = [&](void* dst, std::size_t len) -> bool {
        if (pos + len > n) return false;
        if (len > 0) std::memcpy(dst, d + pos, len);
        pos += len;
        return true;
    };

    auto magic = read_u32();
    auto ver = read_u32();
    if (magic != kInvMagic) {
        return false;
    }
    // v6 不再兼容 v1..v5：项目规则「不考虑向后兼容性」，旧快照直接拒绝。
    if (ver != kInvVersion) {
        return false;
    }

    auto N = read_u32();
    auto sdl = read_u64();

    constexpr std::size_t kBlock = PostingList::kBlockSize;

    for (auto& shard : shards_) {
        auto term_count = read_u32();
        if (term_count == kReadFail32) { return false; }

        for (std::uint32_t t = 0; t < term_count; ++t) {
            auto tlen = read_u32();
            if (tlen == kReadFail32) { return false; }
            // S31(下游反馈 libbitcask.md):写端历史上不限 term 长而读端此处
            // 曾设 1024 硬上限——一个超长 term(zhwiki 实测 jieba 切出 1477B)
            // 使**整段**载入失败,层层上抛成「全库查询静默 0 命中」。修正:
            // ① 超长 term 完整解析后**跳过插入**(容错,不炸段——载荷已经
            //   段级 CRC 背书,tlen 是真实值,损坏由 read_bytes 界检查兜住);
            // ② 计数暴露(load_skipped_oversized_terms);
            // ③ 源头由 AnalyzerConfig::max_token_bytes 过滤,正常流程不再
            //   产生超长 term。v2(mmap)格式读端无此上限,天然免疫。
            const bool oversized = tlen > kMaxTermBytes;

            std::string term(tlen, '\0');
            if (!read_bytes(term.data(), tlen)) return false;

            auto pc = read_u32();
            if (pc == kReadFail32 || pc > kMaxPostingsPerTerm) {
                return false;
            }

            PostingList pl;
            // S22-M6：SoA 三列直接 resize，各列分趟回填（原 items.resize 后
            // 逐字段写，盘格式本就列式）。
            pl.ords.resize(pc);
            pl.tfs.resize(pc);
            pl.dls.resize(pc);

            // v6：ord 走 FOR 块压缩。
            auto ord_block_count = read_u32();
            if (ord_block_count == kReadFail32) { return false; }
            if (ord_block_count != ((pc + kBlock - 1) / kBlock)) {
                return false;
            }
            for (std::uint32_t b = 0; b < ord_block_count; ++b) {
                auto frame = read_u64();
                auto bits  = read_u8();
                auto packed_len = read_u32();
                if (frame == kReadFail64 || packed_len == kReadFail32) {
                    return false;
                }
                std::size_t start = static_cast<std::size_t>(b) * kBlock;
                std::size_t cnt = std::min(kBlock, static_cast<std::size_t>(pc) - start);
                std::vector<std::uint8_t> packed(packed_len);
                if (packed_len > 0 && !read_bytes(packed.data(), packed_len)) {
                    return false;
                }
                // S22-M6：直接解码进 ords 列（免 ords_buf 中转拷贝）。
                for_decode_block(frame, bits, packed.data(), cnt,
                                 pl.ords.data() + start);
            }

            // v6：TFs 整组 VByte 解码。
            {
                auto tf_csize = read_u32();
                if (tf_csize == kReadFail32) { return false; }
                std::vector<std::uint8_t> tf_buf(tf_csize);
                if (tf_csize > 0 && !read_bytes(tf_buf.data(), tf_csize)) {
                    return false;
                }
                std::size_t tf_pos = 0;
                for (std::uint32_t p = 0; p < pc; ++p) {
                    auto [val, np] = codec::vbyte_decode(tf_buf.data(), tf_pos);
                    pl.tfs[p] = static_cast<std::uint32_t>(val);
                    tf_pos = np;
                }
                if (tf_pos != tf_csize) { return false; }
            }

            // v6：dls 整组 VByte 解码。
            {
                auto dl_csize = read_u32();
                if (dl_csize == kReadFail32) { return false; }
                std::vector<std::uint8_t> dl_buf(dl_csize);
                if (dl_csize > 0 && !read_bytes(dl_buf.data(), dl_csize)) {
                    return false;
                }
                std::size_t dl_pos = 0;
                for (std::uint32_t p = 0; p < pc; ++p) {
                    auto [val, np] = codec::vbyte_decode(dl_buf.data(), dl_pos);
                    pl.dls[p] = static_cast<std::uint32_t>(val);
                    dl_pos = np;
                }
                if (dl_pos != dl_csize) { return false; }
            }

            // positions：与 v4+ 同——每 posting (u32 个数 + u32 压缩字节数 + 字节流)。
            // S22-M6：流式灌进扁平 pos_data；pos_off 惰性——首个非空才物化
            // （此前各条起点全 0），保持 append() 同款状态机。
            for (std::uint32_t p = 0; p < pc; ++p) {
                auto posc = read_u32();
                if (posc == kReadFail32 || posc > kMaxPositionsPerPosting) {
                    return false;
                }
                auto csize = read_u32();
                if (csize == kReadFail32) { return false; }
                std::vector<std::uint8_t> comp(csize);
                if (csize > 0 && !read_bytes(comp.data(), csize)) {
                    return false;
                }
                auto vals = codec::gap_decode(comp);
                if (vals.size() != posc) { return false; }
                if (posc > 0 && pl.pos_off.empty()) {
                    pl.pos_off.assign(p + 1, 0);
                }
                if (!pl.pos_off.empty()) {
                    for (std::uint32_t i = 0; i < posc; ++i) {
                        pl.pos_data.push_back(static_cast<std::uint32_t>(vals[i]));
                    }
                    pl.pos_off.push_back(pl.pos_data.size());
                }
            }

            // Block-Max WAND 元数据：保持 v5 结构。
            auto block_count = read_u32();
            if (block_count == kReadFail32 || block_count > kMaxBlocksPerTerm) {
                return false;
            }
            pl.blocks.resize(block_count);
            for (std::uint32_t b = 0; b < block_count; ++b) {
                pl.blocks[b].base_ord = read_u64();
                pl.blocks[b].end_ord = read_u64();
                pl.blocks[b].max_tf = read_u32();
                pl.blocks[b].min_dl = read_u32();
                pl.blocks[b].start_idx = read_u32();
                pl.blocks[b].count = read_u32();
            }

            // S10.9：load 后重算缓存的 global max_tf（落盘格式不含此字段，派生量）。
            // 同时重建 add_doc 水位 = 全局最大 ord（落盘亦不含，派生量）；
            // 用 -1 哨兵区分「未索引任何」与「ord=0」。
            for (std::size_t i = 0; i < pl.size(); ++i) {
                if (pl.tfs[i] > pl.max_tf) pl.max_tf = pl.tfs[i];
                // load 单线程,relaxed 足够。
                const std::uint64_t wm = max_indexed_ord_.load(std::memory_order_relaxed);
                if (wm == static_cast<std::uint64_t>(-1) || pl.ords[i] > wm) {
                    max_indexed_ord_.store(pl.ords[i], std::memory_order_relaxed);
                }
            }
            // S31:超长 term 已完整解析(游标推进正确),仅跳过插入。
            if (oversized) {
                load_skipped_oversized_terms_.fetch_add(
                    1, std::memory_order_relaxed);
                continue;
            }
            // S13-F6：load 单线程（reducer 车道尚未注册），但 ensure_vocab
            // 已改为「vocab_ ∪ delta」增量重建、不再遍历 map——load 直填的
            // key 必须同步记入 delta，否则首次 wildcard/fuzzy 查询拿到空词典。
            shard.vocab_delta_.push_back(term);
            shard.inverted.emplace(std::move(term), std::make_shared<PostingList>(std::move(pl)));
        }
    }

    live_doc_count_.store(N, std::memory_order_relaxed);
    sum_doc_len_.store(sdl, std::memory_order_relaxed);

    // V6.3.1：load 期间各 shard 走 emplace 填入；shards_ 默认构造的 vocab_dirty_
    // 已为 true，但保险起见显式置一次，覆盖将来构造路径变更。
    for (auto& shard : shards_) {
        shard.vocab_dirty_.store(true, std::memory_order_release);
    }

    return true;
}

// ---- S14-4：ord-delta 增量序列化 ----------------------------------------
//
// 格式（"BIVD" v1，LE）：
//   magic u32 | ver u32=1 | N u32 | sdl u64 | from_ord u64 | term_count u64
//   每 term：tlen u32 | term | item_count u32
//     每 item：ord u64 | tf u32 | dl u32
//              | positions（u32 个数 + u32 压缩字节数 + gap+VByte 流）
// 与 base 的列式 FOR 编码刻意不同：delta 是窗口小量（∝ 增量 tokens），
// 逐条编码换取 apply 的直接尾部追加（无需块重排）；WAND 块由
// note_appended 在 apply 侧增量重建。
namespace {
constexpr std::uint32_t kInvDeltaMagic   = 0x44564942;  // "BIVD"
constexpr std::uint32_t kInvDeltaVersion = 1;
}  // namespace

void InvertedIndex::serialize_delta(std::vector<std::byte>& out,
                                    std::uint64_t from_ord) const {
    auto put = [&](const void* p, std::size_t n) {
        const auto* b = reinterpret_cast<const std::byte*>(p);
        out.insert(out.end(), b, b + n);
    };
    auto write_u32 = [&](std::uint32_t v) { put(&v, 4); };
    auto write_u64 = [&](std::uint64_t v) { put(&v, 8); };
    auto write_positions = [&](std::span<const std::uint32_t> positions) {
        write_u32(static_cast<std::uint32_t>(positions.size()));
        std::vector<std::uint64_t> tmp(positions.begin(), positions.end());
        auto comp = codec::gap_encode(tmp);
        write_u32(static_cast<std::uint32_t>(comp.size()));
        if (!comp.empty()) put(comp.data(), comp.size());
    };

    write_u32(kInvDeltaMagic);
    write_u32(kInvDeltaVersion);
    write_u32(static_cast<std::uint32_t>(
        live_doc_count_.load(std::memory_order_relaxed)));
    write_u64(sum_doc_len_.load(std::memory_order_relaxed));
    write_u64(from_ord);
    // term_count 占位，遍历后回填。
    const std::size_t cnt_pos = out.size();
    write_u64(0);
    std::uint64_t term_cnt = 0;

    for (auto& shard : shards_) {
        // 并发安全遍历（与 serialize 相同，不变量见 collect_term_keys）。
        auto keys = collect_term_keys(shard.inverted,
                                      [](const std::string&) { return true; });
        for (const auto& key : keys) {
            PostingMap::const_accessor acc;
            if (!shard.inverted.find(acc, key)) continue;
            std::shared_ptr<PostingList> plsp = acc->second;
            const PostingList& pl = *plsp;
            if (pl.empty() || pl.ords.back() < from_ord) continue;
            // 后缀起点：第一个 ord ≥ from_ord（ords 按升序）。
            // S22-M6：直接对 ords 列二分。
            auto it = std::lower_bound(pl.ords.begin(), pl.ords.end(),
                                       from_ord);
            const auto start =
                static_cast<std::size_t>(it - pl.ords.begin());
            const auto n = pl.size() - start;
            if (n == 0) continue;
            ++term_cnt;
            write_u32(static_cast<std::uint32_t>(key.size()));
            put(key.data(), key.size());
            write_u32(static_cast<std::uint32_t>(n));
            for (std::size_t i = start; i < pl.size(); ++i) {
                write_u64(pl.ords[i]);
                write_u32(pl.tfs[i]);
                write_u32(pl.dls[i]);
                write_positions(pl.positions(i));
            }
        }
    }
    std::memcpy(out.data() + cnt_pos, &term_cnt, 8);
}

bool InvertedIndex::apply_delta(std::span<const std::byte> bytes) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const auto* end = p + bytes.size();
    auto need = [&](std::size_t n) {
        return static_cast<std::size_t>(end - p) >= n;
    };
    auto rd_u32 = [&](std::uint32_t& v) {
        if (!need(4)) return false;
        std::memcpy(&v, p, 4);
        p += 4;
        return true;
    };
    auto rd_u64 = [&](std::uint64_t& v) {
        if (!need(8)) return false;
        std::memcpy(&v, p, 8);
        p += 8;
        return true;
    };

    std::uint32_t magic = 0, ver = 0, n_docs = 0;
    std::uint64_t sdl = 0, from_ord = 0, term_cnt = 0;
    if (!rd_u32(magic) || magic != kInvDeltaMagic) return false;
    if (!rd_u32(ver) || ver != kInvDeltaVersion) return false;
    if (!rd_u32(n_docs) || !rd_u64(sdl) || !rd_u64(from_ord) ||
        !rd_u64(term_cnt)) {
        return false;
    }

    // 契约（承重，勿绕过）：调用方必须按 from_ord 单调、经 walk_chain 链接校验
    // （kDeltaInfo 的 prev_wm == 当前累计 coverage、seq 严格连续、base_gen 匹配）
    // 的顺序喂入 delta。本函数**无法验证完整性**：下方 per-term 守卫
    // （ord ≤ 列尾 → continue）为了幂等/防重叠会**静默丢弃**在范围条目——这对
    // 「边界 ord 重复导出 / 崩溃后重放同段」是正确且必需的，但若 delta 被乱序或
    // 跳段应用，被跳过区间的 ord 会**永久丢失且无任何报错**。因此绝不能绕过
    // walk_chain 直接喂 apply_delta（手工重放 / 测试 helper / 新插件路径同理）。
    //
    // S26-B：链完整性由调用方的 walk_chain 独家保证（kDeltaInfo 三元组）。本层
    // **不能**再用 `from_ord ≤ max_indexed_ord_ + 1` 做本地跳段断言：from_ord 是
    // **全局链 coverage 水位**，max_indexed_ord_ 是**本字段实际 posting 的最大
    // ord**——二者差着「不产生本字段 posting 的 ord」数（checkpoint/skip 的 RunFn
    // ord、删除墓碑、向量-only 文档、稀疏命名字段），正常负载下必然发散。该断言
    // （f2f56d3 引入）对合法链恢复恒误报（如 base 覆盖到 ord59、下一 delta from=61
    // 因 ord60 被 checkpoint RunFn 吃掉），已移除。

    std::uint64_t max_ord_seen = 0;
    bool any_item = false;
    for (std::uint64_t t = 0; t < term_cnt; ++t) {
        std::uint32_t tlen = 0;
        if (!rd_u32(tlen) || !need(tlen)) return false;
        std::string term(reinterpret_cast<const char*>(p), tlen);
        p += tlen;
        std::uint32_t item_cnt = 0;
        if (!rd_u32(item_cnt)) return false;

        auto& shard = shard_for(term);
        PostingMap::accessor acc;
        const bool is_new_term = shard.inverted.insert(acc, term);
        PostingList& pl = mutable_pl(acc->second);
        for (std::uint32_t i = 0; i < item_cnt; ++i) {
            std::uint64_t ord = 0;
            std::uint32_t tf = 0, dl = 0, posc = 0, csize = 0;
            if (!rd_u64(ord) || !rd_u32(tf) || !rd_u32(dl) ||
                !rd_u32(posc) || posc > kMaxPositionsPerPosting ||
                !rd_u32(csize) || !need(csize)) {
                return false;
            }
            std::vector<std::uint32_t> positions;
            if (index_positions_ && posc > 0) {
                std::vector<std::uint8_t> comp(p, p + csize);
                auto vals = codec::gap_decode(comp);
                if (vals.size() != posc) return false;
                positions.resize(posc);
                for (std::uint32_t k = 0; k < posc; ++k) {
                    positions[k] = static_cast<std::uint32_t>(vals[k]);
                }
            }
            p += csize;
            // 幂等守卫：陈旧/重叠条目（ord ≤ 列尾）拒绝——维持「按 ord
            // 升序、同 ord 不重复」不变量（陈旧 delta 误 apply 的防线）。
            if (!pl.empty() && ord <= pl.ords.back()) continue;
            pl.append(ord, tf, dl, positions);
            pl.note_appended();
            if (ord > max_ord_seen) max_ord_seen = ord;
            any_item = true;
        }
        // 新 term 记账（镜像 add_doc 的 S13-F6 增量 vocab 协议）。
        if (is_new_term) {
            std::unique_lock vlock(shard.vocab_mtx_);
            shard.vocab_delta_.push_back(term);
            shard.vocab_dirty_.store(true, std::memory_order_release);
        }
        // S29-6B：posting 变更 → 失效查询线程的 term 快照缓存（镜像 add_doc）。
        shard.gen_.fetch_add(1, std::memory_order_release);
    }
    if (p != end) return false;

    // 全局统计取绝对值（删除的统计效果由此覆盖，无需删除日志）。
    live_doc_count_.store(n_docs, std::memory_order_relaxed);
    sum_doc_len_.store(sdl, std::memory_order_relaxed);
    if (any_item) {
        const std::uint64_t wm =
            max_indexed_ord_.load(std::memory_order_relaxed);
        if (wm == static_cast<std::uint64_t>(-1) || max_ord_seen > wm) {
            max_indexed_ord_.store(max_ord_seen, std::memory_order_relaxed);
        }
    }
    return true;
}

}  // namespace bitcask::bm25
