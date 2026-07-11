#include "bitcask/intersect.hpp"
#include "bitcask/inverted.hpp"
#include "bitcask/myers.hpp"
#include "bitcask/term_snapshot_cache.hpp"
#include "bitcask/wildcard_matcher.hpp"
#include "bitcask/bm25_kernels.hpp"

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

// block_for_ord / block_upper_bound 的共享实现——PostingList 与
// FlatPostings（P1 查询快照）语义必须一致，逻辑只写一份。
const PostingBlock* block_for_ord_in(const std::vector<PostingBlock>& blocks,
                                     std::uint64_t ord) {
    if (blocks.empty()) return nullptr;
    auto it = std::lower_bound(blocks.begin(), blocks.end(), ord,
                               [](const PostingBlock& block, std::uint64_t o) {
                                   return block.end_ord < o;
                               });
    if (it != blocks.end() && it->base_ord <= ord) {
        return &(*it);
    }
    if (it != blocks.begin()) {
        --it;
        if (it->base_ord <= ord && it->end_ord >= ord) {
            return &(*it);
        }
    }
    return nullptr;
}

// min_dl:分母的 doc_len 下界。默认 1 = 最松 admissible(旧行为);
// v5 块级 impacts 传块内真实最小 dl,上界收紧 ~25%/词(§6.1)。
float upper_bound_from(std::uint32_t global_max_tf, float idf,
                       const Bm25Params& params, double avgdl,
                       std::uint32_t min_dl = 1) {
    float tf_norm = static_cast<float>(global_max_tf) * (params.k1 + 1.0f) /
                    (static_cast<float>(global_max_tf) + params.k1 *
                     (1.0f - params.b + params.b * static_cast<float>(min_dl) /
                      static_cast<float>(avgdl)));
    // BM25+：上界含 δ 下界项，与实际评分一致，避免 WAND 剪枝漏结果（S8.10）。
    return idf * (tf_norm + params.delta);
}

// search / search_wildcard / search_fuzzy 共用的查询词条目：term + 扁平快照。
// （wand/bool 的词条目更富——带 live/dls/cursor/idf——仍各自局部定义。）
struct ScoredTerm {
    std::string  term;
    FlatPostings fp;
};

// S29-6B：score_bow_topk 改收视图——search 命中路径的 fp 常驻 thread_local
// TermSnapshotCache 条目（零拷贝消费），wildcard/fuzzy 套一层视图数组。
// 指向物在评分期间稳定：缓存条目被 use_seq 钉住（同查询不淘汰），
// tps_pool/tps 为调用方栈上/线程私有容器。
struct ScoredTermView {
    const std::string*  term;
    const FlatPostings* fp;
};

// bag-of-words 评分 + top-k：三条路径（search 标量 / wildcard / fuzzy）此前
// 各自内联一份逐字相同的「批量 live/doc_len + 两阶段评分 parallel_reduce +
// 小顶堆 top-k」。提取单一实现，BM25 公式与「分数位级不变 / 无分支可向量化」
// 两条不变量只此一处（避免改公式时漏改某条低频路径致评分不一致）。
std::vector<SearchResult> score_bow_topk(
    std::span<const ScoredTermView> tps, std::size_t k,
    std::uint64_t N, std::uint64_t sum_dl,
    const Bm25Params& params, const LiveChecker& live_checker,
    const std::vector<std::pair<std::string, std::uint64_t>>* global_df = nullptr) {  // S29-5：扁平化
    // S27-2：global_df 非空 → idf 用全局 df（G-on-the-fly）；N/sum_dl 亦已由
    // caller 传全局值。本地 live_df 仍用于跳空/reserve。
    const double avgdl =
        N > 0 ? static_cast<double>(sum_dl) / static_cast<double>(N) : 1.0;

    // 串行 BM25 评分：按查询词顺序累积扁平 (ord, contrib) 数组。
    //
    // 【T6 决策，2026-06-24】曾用 tbb::parallel_reduce 按词分片并行，但 BOW
    // 路径按定义只在 total_postings < kWandThreshold(1024) 时走 ——评分工作量
    // 恒小，TBB task spawn/steal/join 开销远超收益。实测（8 词 960 posting）：
    // 串行较 grain=1 并行 **单线程快 1.6×、并发下快 1.4–2.4×**（grain=1 拆任务
    // 在高读并发下过度订阅）。大查询走 WAND（串行），BOW 无任何需要并行的区间，
    // 故彻底串行化。附带收益：评分浮点累加序确定（不再分片相关）。
    using Hit = std::pair<std::uint64_t, float>;
    // S23-M3：工作数组 thread_local 复用（原每查询 4 次分配 → 稳态 0）。
    // 安全性：本函数彻底串行（T6 决策，无 TBB spawn → 无 work-stealing
    // 重入窗口）；批量查询按线程隔离（thread_local 每 TBB worker 独立）。
    static thread_local std::vector<Hit> hits;
    hits.clear();
    {
        static thread_local std::vector<char> live;
        static thread_local std::vector<std::uint32_t> dls;
        static thread_local std::vector<float> contrib;
        for (std::size_t ti = 0; ti < tps.size(); ++ti) {
            const auto& fp = *tps[ti].fp;
            const std::size_t n = fp.size();

            // P2.1：live/doc_len 批量取——一次虚调用（Index 侧一次锁）完成
            // 整列，评分浮点循环不再含虚调用，编译器可自动向量化。
            live.resize(n);
            live_checker.fill_is_live(fp.ords, live);
            std::size_t live_df = 0;
            for (std::size_t i = 0; i < n; ++i) {
                live_df += static_cast<std::size_t>(live[i]);
            }
            if (live_df == 0) continue;

            // S27-2：idf 的 df——全局注入优先，回退本段 live_df。
            // S29-5：扁平列表线性扫（词数个位数，快于 hash find）。
            double df_idf = static_cast<double>(live_df);
            if (global_df) {
                for (const auto& [t, v] : *global_df) {
                    if (t == *tps[ti].term) {
                        if (v > 0) df_idf = static_cast<double>(v);
                        break;
                    }
                }
            }
            auto idf = std::log(1.0 + (static_cast<double>(N) - df_idf + 0.5) / (df_idf + 0.5));

            dls.resize(n);
            live_checker.fill_doc_lens(fp.ords, dls);

            // 两阶段评分：① 纯数组浮点（可向量化；死点也算、结果不用，
            // 保持无分支），公式与逐 posting 版逐运算一致（分数位级不变）；
            // ② 标量 append 进扁平数组。
            contrib.resize(n);
            const float fidf = static_cast<float>(idf);
            const float inv_avgdl = 1.0f / static_cast<float>(avgdl);
            detail::bm25_score_dispatch(
                fp.tfs.data(), dls.data(),
                params.k1 + 1.0f,
                params.k1 * (1.0f - params.b),
                params.k1 * params.b,
                params.delta,
                fidf,
                inv_avgdl,
                contrib.data(),
                n);
            hits.reserve(hits.size() + live_df);
            for (std::size_t i = 0; i < n; ++i) {
                if (live[i]) hits.emplace_back(fp.ords[i], contrib[i]);
            }
        }
    }

    // 按 ord 排序 → 同 ord 连续成段 → 归并累加,边累加边喂 top-k 小顶堆。
    // C3（2026-06-24 评估后保留 sort）：micro-bench 证明 hash-aggregate 在 BOW
    // 范围（< 1024 hits）比 sort 慢 25-40%——sort 在 cache-resident 数据上极快，
    // hash map 的 hashing/probing 开销不划算。sort+merge+heap 已是该规模最优。
    std::sort(hits.begin(), hits.end(),
              [](const Hit& x, const Hit& y) { return x.first < y.first; });

    using Entry = std::pair<float, std::uint64_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> heap;
    for (std::size_t i = 0; i < hits.size();) {
        const std::uint64_t ord = hits[i].first;
        float score = 0.0F;
        do {
            score += hits[i].second;
            ++i;
        } while (i < hits.size() && hits[i].first == ord);
        if (heap.size() < k) {
            heap.push({score, ord});
        } else if (score > heap.top().first) {
            heap.pop();
            heap.push({score, ord});
        }
    }
    std::vector<SearchResult> results;
    results.reserve(heap.size());
    while (!heap.empty()) {
        auto& [score, ord] = heap.top();
        results.push_back({ord, score});
        heap.pop();
    }
    std::reverse(results.begin(), results.end());  // 分数降序
    return results;
}

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
#if defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && __has_feature(thread_sanitizer))
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
            if (cached_total < kWandThreshold) {
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
    if (total_postings >= kWandThreshold) {
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

    ScoreExplanation out;
    out.terms.reserve(query_terms.size());

    const auto N = live_doc_count_.load(std::memory_order_relaxed);
    const auto sum_dl = sum_doc_len_.load(std::memory_order_relaxed);
    const double avgdl = N > 0 ? static_cast<double>(sum_dl) / static_cast<double>(N) : 1.0;
    const auto dl = live_checker.doc_len(ord);

    for (const auto& term : query_terms) {
        TermScore ts;
        ts.term = term;

        auto& shard = shard_for(term);
        PostingMap::const_accessor acc;
        if (!shard.inverted.find(acc, term)) {
            // term 不在索引：df=0、各项 0，仍记录以示「未命中」。
            out.terms.push_back(std::move(ts));
            continue;
        }
        const PostingList& pl = *acc->second;

        // 与 search() 一致地算 live df（O3：直接读 ords[]，免物化拷贝）。
        std::size_t live_df = 0;
        for (std::size_t i = 0; i < pl.size(); ++i) {
            if (live_checker.is_live(pl.ords[i])) ++live_df;
        }
        ts.df = live_df;
        if (live_df == 0) { out.terms.push_back(std::move(ts)); continue; }

        ts.idf = std::log(1.0 + (static_cast<double>(N) - static_cast<double>(live_df) + 0.5) /
                                (static_cast<double>(live_df) + 0.5));

        // 找该 ord 的 posting 取 tf（不在该文档则 tf=0，贡献 0）。
        auto idx = pl.find(ord);
        if (idx < pl.size()) {
            ts.tf = pl.tfs[idx];
            ts.tf_norm = static_cast<float>(ts.tf) * (params.k1 + 1.0F) /
                         (static_cast<float>(ts.tf) + params.k1 *
                          (1.0F - params.b + params.b *
                           static_cast<float>(dl) / static_cast<float>(avgdl)));
            ts.contribution = static_cast<float>(ts.idf) * (ts.tf_norm + params.delta);
            out.total += ts.contribution;
        }
        out.terms.push_back(std::move(ts));
    }
    return out;
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
    struct TermPostings {
        std::string term;
        FlatPostings fp;   // P1：扁平快照，ords/tfs 兼任 DAAT 游标数组
        std::vector<char> live;          // P2.1：与 ords 平行，批量取一次
        // S13-P4：dls 改按块惰性填充（沿 bool_search BMW 的 ensure_block
        // 模式）——dl 只在 pivot 评分点读一次，被块跳跃略过的区段永不填充。
        // live 仍需全量：IDF 用 live_df（分数位级不变约定，不能换 raw df）。
        std::vector<std::uint32_t> dls;
        std::vector<char> dls_filled;    // 每 kBlockSize 粒度一位
        bool dls_all = false;            // S13-P4：小列表全量填充标记
        std::size_t cursor = 0;
        float idf = 0.0f;
        float list_upper_bound = 0.0f;
        // S10-A2:per-query per-block 上界缓存。idf/avgdl 查询时常量，
        // max_tf/min_dl 索引时确定 → block_upper 整个查询期间不变，初始化一次算好。
        std::vector<float> block_upper_bounds;
    };
    // S23-M3：thread_local 池复用（每 term 7 个内层 vector：fp.ords/tfs/
    // blocks + live/dls/dls_filled/block_upper_bounds，原每查询全新分配）。
    // WAND 主循环只经 order 索引数组排序、从不搬移/增删 tps 元素，池槽稳定；
    // 全程串行无 TBB spawn（无 work-stealing 重入窗口）。标量字段逐一重置，
    // block_upper_bounds 为 push_back 增长必须 clear；live/dls/dls_filled
    // 由下方 resize+fill/assign 整段覆盖。
    static thread_local std::vector<TermPostings> tps_pool;
    std::size_t n_tps = 0;
    // S29-1：从 search() 单趟 find 已持有的指针快照，免第二趟桶锁。
    for (std::size_t i = 0; i < query_terms.size(); ++i) {
        if (const auto& pl = pls[i]) {
            if (n_tps == tps_pool.size()) tps_pool.emplace_back();
            TermPostings& tp = tps_pool[n_tps];
            tp.term.assign(query_terms[i]);
            pl->snapshot_flat(tp.fp);
            tp.dls_all = false;
            tp.cursor = 0;
            tp.idf = 0.0f;
            tp.list_upper_bound = 0.0f;
            tp.block_upper_bounds.clear();
            ++n_tps;
        }
    }
    if (n_tps == 0) return {};
    const std::span<TermPostings> tps(tps_pool.data(), n_tps);

    // S27-2：ext 非空 → 全局 N/sum_dl（G-on-the-fly）；否则本地统计（现行为）。
    auto N = ext ? ext->N : live_doc_count_.load(std::memory_order_relaxed);
    auto sum_dl = ext ? ext->sum_dl : sum_doc_len_.load(std::memory_order_relaxed);
    auto avgdl = N > 0 ? static_cast<double>(sum_dl) / static_cast<double>(N) : 1.0;

    // 计算每个 term 的 IDF 和上界分数。
    // P2.1：live/doc_len 批量取一次（Index 侧各一次锁）存进 tp——
    // DAAT 循环每 pivot 的 is_live/doc_len 改读数组，全程零虚调用零锁。
    constexpr std::size_t kB = PostingList::kBlockSize;
    for (auto& tp : tps) {
        tp.live.resize(tp.fp.size());
        live_checker.fill_is_live(tp.fp.ords, tp.live);
        // S13-P4：dls 惰性按块填充（见 ensure_dls）。中小列表（≤32 块 =
        // 4K posting）直接全量填充——批量 gather 本就便宜，惰性簿记
        // （per-pivot 分支 + 位图）反而更贵（实测 4K 档 +13%~28%）；
        // 大列表才吃 WAND 块跳跃的省填充收益。
        tp.dls.resize(tp.fp.size());
        // 单词查询恒全量：无其它词可比 → WAND 不可能跳块，惰性纯开销。
        if (query_terms.size() == 1 || tp.fp.size() <= 32 * kB) {
            live_checker.fill_doc_lens(tp.fp.ords, tp.dls);
            tp.dls_filled.assign(1, 1);  // 单标记=全满（ensure_dls 兼容见下）
            tp.dls_all = true;
        } else {
            tp.dls_filled.assign((tp.fp.size() + kB - 1) / kB, 0);
        }
        std::size_t live_df = 0;
        for (std::size_t i = 0; i < tp.live.size(); ++i) {
            live_df += static_cast<std::size_t>(tp.live[i]);
        }
        if (live_df == 0) {
            tp.idf = 0.0f;
            tp.list_upper_bound = 0.0f;
            continue;
        }
        // S27-2：idf 的 df——全局注入优先，回退本段 live_df（同 score_bow_topk）。
        // idf 一致地用于块上界与实际打分 → WAND 剪枝仍正确。
        double df_idf = static_cast<double>(live_df);
        if (ext && ext->df) {
            for (const auto& [t, v] : *ext->df) {  // S29-5：扁平列表线性扫
                if (t == tp.term) {
                    if (v > 0) df_idf = static_cast<double>(v);
                    break;
                }
            }
        }
        tp.idf = static_cast<float>(std::log(1.0 + (static_cast<double>(N) - df_idf + 0.5) /
                                             (df_idf + 0.5)));
        tp.list_upper_bound = tp.fp.block_upper_bound(tp.idf, params, avgdl);
        // S10-A2:per-block 上界一次算好，WAND 内层循环免每次 pivot 重算。
        tp.block_upper_bounds.reserve(tp.fp.blocks.size());
        for (const auto& blk : tp.fp.blocks) {
            tp.block_upper_bounds.push_back(
                upper_bound_from(blk.max_tf, tp.idf, params, avgdl, blk.min_dl));
        }
    }

    // S13-P4：dls 惰性按块填充（每块一次批量 gather；被 WAND 块跳跃略过的
    // 区段永不付 doc_len 查询成本）。dl 值 ord 定后不可变，填充时机无关正确性。
    auto ensure_dls = [&](TermPostings& tp, std::size_t idx) {
        if (tp.dls_all) return;  // 小列表已全量填充
        const std::size_t b = idx / kB;
        if (tp.dls_filled[b]) return;
        const std::size_t start = b * kB;
        const std::size_t cnt = std::min(kB, tp.fp.size() - start);
        live_checker.fill_doc_lens(
            std::span<const std::uint64_t>(tp.fp.ords.data() + start, cnt),
            std::span<std::uint32_t>(tp.dls.data() + start, cnt));
        tp.dls_filled[b] = 1;
    };

    using Entry = std::pair<float, std::uint64_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> heap;
    float threshold = 0.0f;

    // S10.5：每轮只排序索引数组，避免 std::sort 整体搬运含多个 vector 的
    // TermPostings（P1 后为 fp 的 ords/tfs/blocks）。
    // order[i] 给出按当前 ord 升序的第 i 个 term 在 tps 中的下标。
    std::vector<std::size_t> order(tps.size());
    for (std::size_t i = 0; i < tps.size(); ++i) order[i] = i;

    // 比较器语义：耗尽的 term 排前面（会被下方 continue 跳过），其余按当前
    // ord 升序。平局顺序不影响正确性——pivot_ord 由 cursor 推进确定，打分
    // 循环（line 642）遍历全部匹配词，块跳跃仅 admissible 跳过。
    auto wand_less = [&tps](std::size_t a, std::size_t b) {
        const auto& ta = tps[a];
        const auto& tb = tps[b];
        bool a_ex = ta.cursor >= ta.fp.ords.size();
        bool b_ex = tb.cursor >= tb.fp.ords.size();
        if (a_ex && b_ex) return false;
        if (a_ex) return true;
        if (b_ex) return false;
        return ta.fp.ords[ta.cursor] < tb.fp.ords[tb.cursor];
    };

    while (true) {
        // 优化②：order 在迭代间持久，每轮仅少数 cursor 前移 → 近乎有序。
        // 插入排序对近乎有序输入是 O(t+inversions)，替代每轮 std::sort 的
        // O(t log t) 最坏。等价：任一正确排序都满足比较器，平局差异已论证无害。
        for (std::size_t i = 1; i < order.size(); ++i) {
            std::size_t key = order[i];
            std::size_t j = i;
            while (j > 0 && wand_less(key, order[j - 1])) {
                order[j] = order[j - 1];
                --j;
            }
            order[j] = key;
        }

        std::size_t pivot_pos = 0;  // pivot 在排序序列 order 中的位置
        float acc_score = 0.0f;
        bool pivot_found = false;

        for (std::size_t i = 0; i < order.size(); ++i) {
            auto& tp = tps[order[i]];
            if (tp.cursor >= tp.fp.ords.size()) continue;
            acc_score += tp.list_upper_bound;
            if (acc_score >= threshold) {
                pivot_pos = i;
                pivot_found = true;
                break;
            }
        }
        if (!pivot_found) break;

        auto& pivot_tp = tps[order[pivot_pos]];
        auto pivot_ord = pivot_tp.fp.ords[pivot_tp.cursor];

        // A1:非耗尽词的列表上界总和——块跳跃判定的保守"其余词"上界
        // (含 pivot 之后 cursor 恰为 pivot_ord 的词,admissible)。
        float total_ub = 0.0f;
        for (auto& t : tps) {
            if (t.cursor < t.fp.ords.size()) total_ub += t.list_upper_bound;
        }

        bool any_skipped = false;
        for (std::size_t i = 0; i <= pivot_pos; ++i) {
            auto& tp = tps[order[i]];
            if (tp.cursor >= tp.fp.ords.size()) continue;
            if (tp.fp.ords[tp.cursor] != pivot_ord) continue;

            const auto* block = tp.fp.block_for_ord(pivot_ord);
            if (block != nullptr) {
                // S10-A2:读初始化阶段算好的 per-block 上界（免每次 pivot 重算 6 FMA+1 div）。
                const std::size_t block_idx =
                    static_cast<std::size_t>(block - tp.fp.blocks.data());
                const float block_upper = tp.block_upper_bounds[block_idx];
                // A1 修复:原公式 threshold - heap.top() 在 threshold ==
                // heap.top()(下方 θ 更新同源)时恒 ≈1e-6,块跳跃从未
                // 触发过(死代码)。正确判定:本词块上界 + 其余词列表
                // 上界之和 ≤ θ ⟹ pivot 不可能严格超过 θ,整块跳过。
                // 注意不能加绝对 epsilon(如 1e-6):idf 极小时(df≈N)
                // 分数量级 ~1e-4,绝对容差变成巨大相对容差,会把
                // 真 top-k 所在块当"平分"误跳——只认 <=(位级平分才跳,
                // top-k 是严格优于语义,平分块挤不掉现有结果)。
                if (heap.size() >= k &&
                    block_upper <=
                        threshold - (total_ub - tp.list_upper_bound)) {
                    // 跳过到下一个块边界。
                    std::size_t next_start = block->start_idx + block->count;
                    if (next_start >= tp.fp.ords.size()) {
                        tp.cursor = tp.fp.ords.size();
                    } else {
                        tp.cursor = next_start;
                    }
                    any_skipped = true;
                }
            }
        }
        if (any_skipped) continue;

        // 所有 term 在 pivot_ord 处都值得关注，计算实际分数。
        // P2.1：live/dl 读 pivot term 的批量数组（任意在 pivot_ord 处的 term
        // 给出同一 ord 的同一答案，取 pivot_tp 自己游标位置的即可）。
        if (pivot_tp.live[pivot_tp.cursor]) {
            float score = 0.0f;
            // S10.8：dl 只依赖 pivot_ord，提到 term 循环外取一次（原先每个匹配 term 重取）。
            ensure_dls(pivot_tp, pivot_tp.cursor);  // S13-P4：惰性按块填充
            auto dl = pivot_tp.dls[pivot_tp.cursor];
            for (std::size_t i = 0; i < tps.size(); ++i) {
                if (tps[i].cursor >= tps[i].fp.ords.size()) continue;
                if (tps[i].fp.ords[tps[i].cursor] != pivot_ord) continue;

                auto tf_norm = static_cast<float>(tps[i].fp.tfs[tps[i].cursor]) *
                               (params.k1 + 1.0f) /
                               (static_cast<float>(tps[i].fp.tfs[tps[i].cursor]) + params.k1 *
                                (1.0f - params.b + params.b *
                                 static_cast<float>(dl) / static_cast<float>(avgdl)));
                score += tps[i].idf * (tf_norm + params.delta);
            }

            if (score >= threshold) {
                if (heap.size() < k) {
                    heap.push({score, pivot_ord});
                } else if (score > heap.top().first) {
                    heap.pop();
                    heap.push({score, pivot_ord});
                }
                if (heap.size() >= k) {
                    threshold = heap.top().first;
                }
            }
        }

        // 推进所有 cursor <= pivot_ord 的 term。
        for (std::size_t i = 0; i < tps.size(); ++i) {
            while (tps[i].cursor < tps[i].fp.ords.size() && tps[i].fp.ords[tps[i].cursor] <= pivot_ord) {
                ++tps[i].cursor;
            }
        }

        bool any_exhausted = false;
        for (auto& tp : tps) {
            if (tp.cursor >= tp.fp.ords.size()) any_exhausted = true;
        }
        if (any_exhausted) {
            bool all_exhausted = true;
            for (auto& tp : tps) {
                if (tp.cursor < tp.fp.ords.size()) {
                    all_exhausted = false;
                    break;
                }
            }
            if (all_exhausted) break;
        }
    }

    std::vector<SearchResult> results;
    results.reserve(heap.size());
    while (!heap.empty()) {
        auto& [score, ord] = heap.top();
        results.push_back({ord, score});
        heap.pop();
    }
    std::reverse(results.begin(), results.end());
    return results;
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

    auto N = live_doc_count_.load(std::memory_order_relaxed);
    auto sum_dl = sum_doc_len_.load(std::memory_order_relaxed);
    auto avgdl = N > 0 ? static_cast<double>(sum_dl) / static_cast<double>(N) : 1.0;

    auto& first_pl = *tps[0].pl;

    // S13-P8.2：候选枚举改由**最稀有词**驱动（此前恒 tps[0]——"the quantum"
    // 会遍历 "the" 的大表）。候选集 = 全词交集不变；两列表都按 ord 升序 ⟹
    // (score, ord) 推入序一致，top-k 含平分决策**逐字节同果**。idf 语义仍取
    // first term 的 live_df（评分公式不变）。
    std::size_t drv = 0;
    for (std::size_t t = 1; t < tps.size(); ++t) {
        if (tps[t].pl->size() < tps[drv].pl->size()) drv = t;
    }
    auto& cand_pl = *tps[drv].pl;
    const std::size_t n_cand = cand_pl.size();

    // live/doc_len 批量取一次（Index 侧各一次锁），主循环复用（P2.1/S7-5）。
    // S22-M6：SoA 后 ords 列本身即所需数组，直接整列拷贝。
    std::vector<std::uint64_t> cand_ords(cand_pl.ords);
    std::vector<char> cand_live(n_cand);
    live_checker.fill_is_live(cand_ords, cand_live);
    std::vector<std::uint32_t> cand_dls(n_cand);
    live_checker.fill_doc_lens(cand_ords, cand_dls);

    // live_df/idf 只依赖 first term 的 posting list（与候选枚举无关），
    // 提到循环外算一次（S9.7）。drv==0 时复用 cand_live 免二次 gather。
    std::size_t live_df = 0;
    if (drv == 0) {
        for (char c : cand_live) live_df += static_cast<std::size_t>(c);
    } else {
        std::vector<std::uint64_t> first_ords(first_pl.ords);
        std::vector<char> first_live(first_ords.size());
        live_checker.fill_is_live(first_ords, first_live);
        for (char c : first_live) live_df += static_cast<std::size_t>(c);
    }
    auto idf = std::log(1.0 + (static_cast<double>(N) - static_cast<double>(live_df) + 0.5) / (static_cast<double>(live_df) + 0.5));

    // S7-5：单候选评分——纯函数，仅读 tps/first_*/params（const）并写自己的返回值，
    // 无共享可变态 → 串行与并行两路共用。返回 0 表示「该 doc 不构成短语」
    // （phrase_tf==0 / 已删 / 缺词）；idf>0 ∧ tf_norm>0 ∧ delta≥0 ⇒ 真匹配分恒 >0，
    // 故 0 可作哨兵无歧义。
    auto score_one = [&](std::size_t i) -> float {
        if (!cand_live[i]) return 0.0F;
        const auto posting_ord = cand_pl.ords[i];

        // 把「在各 term 的 posting list 里定位本 doc」提到 start_pos 循环外：
        // idx 对固定 (doc, term) 不变（S9.7）。任一 term 在本 doc 不存在 →
        // 整 doc 不可能成短语，直接返回 0。
        // S13-P8.1：other_pos 改 thread_local（此前每候选一次堆分配，且在
        // tbb::parallel_for 内 → 分配器争用）。
        // S22-M6：positions 扁平化后持 span（CoW 冻结语义下读者持
        // shared_ptr<const PostingList>，span 生命周期安全）。
        thread_local std::vector<std::span<const std::uint32_t>> other_pos;
        other_pos.assign(tps.size(), {});
        // 链式匹配从 term 0 的 positions 起步（驱动词只负责候选枚举）。
        std::span<const std::uint32_t> anchor;
        if (drv == 0) {
            anchor = cand_pl.positions(i);
        } else {
            auto idx0 = first_pl.find(posting_ord);
            if (idx0 >= first_pl.size()) return 0.0F;
            anchor = first_pl.positions(idx0);
        }
        for (std::size_t t = 1; t < tps.size(); ++t) {
            if (t == drv) {
                other_pos[t] = cand_pl.positions(i);
                continue;
            }
            auto& other_pl = *tps[t].pl;
            auto idx = other_pl.find(posting_ord);
            if (idx >= other_pl.size()) return 0.0F;
            other_pos[t] = other_pl.positions(idx);
        }

        std::uint32_t phrase_tf = 0;
        for (auto start_pos : anchor) {
            // 有序匹配：term t 必须在 (prev, prev+1+slop] 内出现（slop=0 即精确相邻）。
            bool match = true;
            std::uint32_t prev = start_pos;
            for (std::size_t t = 1; t < tps.size(); ++t) {
                const auto pos_list = other_pos[t];
                const std::uint32_t lo = prev + 1;
                const std::uint32_t hi = prev + 1 + slop;  // 闭区间上界
                // 找 >= lo 的第一个 position。
                auto it = std::lower_bound(pos_list.begin(), pos_list.end(), lo);
                if (it == pos_list.end() || *it > hi) { match = false; break; }
                prev = *it;  // 推进到该 term 的匹配位置（贪心取最早，保证后续窗口最大）
            }
            if (match) ++phrase_tf;
        }

        if (phrase_tf == 0) return 0.0F;
        auto dl = cand_dls[i];
        auto tf_norm = static_cast<float>(phrase_tf) *
                       (params.k1 + 1.0F) /
                       (static_cast<float>(phrase_tf) + params.k1 *
                        (1.0F - params.b + params.b *
                         static_cast<float>(dl) / static_cast<float>(avgdl)));
        return static_cast<float>(idf) * (tf_norm + params.delta);
    };

    // S7-5：候选数过阈才并行（甜区：大候选集短语，~8.7ms）。各候选写自己的
    // cand_scores[i]（不同下标、互不重叠）→ 无锁 data-race-free。候选 ord 互异
    // （posting 每 doc 一条），故末尾按 (score, ord) 选 top-k 与评分顺序无关，
    // 并行/串行**逐字节同果**（确定性）。
    std::vector<float> cand_scores(n_cand);
    if (n_cand >= kPhraseParallelThreshold) {
        tbb::parallel_for(std::size_t{0}, n_cand,
                          [&](std::size_t i) { cand_scores[i] = score_one(i); });
    } else {
        for (std::size_t i = 0; i < n_cand; ++i) cand_scores[i] = score_one(i);
    }

    using Entry = std::pair<float, std::uint64_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> heap;
    for (std::size_t i = 0; i < n_cand; ++i) {
        float score = cand_scores[i];
        if (score <= 0.0F) continue;  // 0 = 非短语（见 score_one 哨兵契约）
        std::uint64_t ord = cand_pl.ords[i];
        if (heap.size() < k) {
            heap.push({score, ord});
        } else if (score > heap.top().first) {
            heap.pop();
            heap.push({score, ord});
        }
    }

    std::vector<SearchResult> results;
    results.reserve(heap.size());
    while (!heap.empty()) {
        auto& [score, ord] = heap.top();
        results.push_back({ord, score});
        heap.pop();
    }
    std::reverse(results.begin(), results.end());
    return results;
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
    std::vector<std::string> must_terms;
    std::vector<std::string> should_terms;
    std::vector<std::string> must_not_terms;
    collect_terms(query, must_terms, should_terms, must_not_terms);

    struct TermPostings {
        std::string term;
        FlatPostings fp;  // P1：扁平快照（S9.6 的 ords 缓存由 fp.ords 取代）
        bool is_must;
        std::vector<char> live;          // P2.1：live 批量取一次，多阶段复用
        std::vector<std::uint32_t> dls;  // P2.1：doc_len 批量取一次，评分循环复用
    };
    // 收集一个 term 的 posting（accessor 下拷扁平快照）。
    // S23-M3：三组 thread_local 池复用（原每查询每 term 5 个内层 vector）。
    // 收集后三向量只读不增删（BMW 持 &must_tps[i] 指针在收集完成后取得，
    // 池不再增长 → 指针稳定）；live/dls 由 fill_live 整段 resize+fill 覆盖
    // （must_not 不填 dls，其池槽陈旧 dls 在 must_not 路径永不被读）。
    // 全程串行（BMW/交并/评分均无 TBB spawn）。
    auto collect = [&](const std::string& term, bool is_must,
                       std::vector<TermPostings>& pool, std::size_t& n) {
        auto& shard = shard_for(term);
        PostingMap::const_accessor acc;
        if (shard.inverted.find(acc, term)) {
            if (n == pool.size()) pool.emplace_back();
            TermPostings& tp = pool[n];
            tp.term.assign(term);
            acc->second->snapshot_flat(tp.fp);
            tp.is_must = is_must;
            ++n;
        }
    };

    static thread_local std::vector<TermPostings> must_pool;
    static thread_local std::vector<TermPostings> should_pool;
    static thread_local std::vector<TermPostings> not_pool;
    std::size_t n_must = 0, n_should = 0, n_not = 0;
    for (auto& term : must_terms) collect(term, true, must_pool, n_must);
    for (auto& term : should_terms) collect(term, false, should_pool, n_should);
    for (auto& term : must_not_terms) collect(term, false, not_pool, n_not);
    const std::span<TermPostings> must_tps(must_pool.data(), n_must);
    const std::span<TermPostings> should_tps(should_pool.data(), n_should);
    const std::span<TermPostings> must_not_tps(not_pool.data(), n_not);

    // ── B1:must-only 合取 Block-Max 剪枝(设计:doc/kway-blockmax-bmw-zh.md §6)
    // top-k 驱动:K1 leapfrog 对齐候选;堆满后用块级分数上界跳过注定
    // 不竞争的整块;live/doc_len 按 128-ord 块懒取(每块一次虚调用+一次锁,
    // 未触达的块零成本)。idf 基于 df(无删除时与原路径位级一致,见 §6)。
    if (!must_terms.empty() && should_terms.empty() && must_not_terms.empty() &&
        k > 0) {
        if (must_tps.size() != must_terms.size()) return {};  // 缺词 → 空集

        const auto N = live_doc_count_.load(std::memory_order_relaxed);
        const auto sum_dl = sum_doc_len_.load(std::memory_order_relaxed);
        const double avgdl =
            N > 0 ? static_cast<double>(sum_dl) / static_cast<double>(N) : 1.0;
        constexpr std::size_t B = PostingList::kBlockSize;

        struct BmwCur {
            TermPostings* tp;
            std::size_t i = 0;               // posting 游标
            float idf = 0.0F;
            std::vector<char> block_filled;  // live/dls 是否已按块填充
            std::vector<float> block_ub;     // 块分数上界缓存
            std::vector<char> ub_done;
        };
        const std::size_t nterms = must_tps.size();
        std::vector<BmwCur> curs(nterms);
        {
            std::vector<std::size_t> order(nterms);
            for (std::size_t i = 0; i < nterms; ++i) order[i] = i;
            std::sort(order.begin(), order.end(),
                      [&](std::size_t a, std::size_t b2) {
                          return must_tps[a].fp.size() <
                                 must_tps[b2].fp.size();
                      });
            for (std::size_t s = 0; s < nterms; ++s) {
                auto& c = curs[s];
                c.tp = &must_tps[order[s]];
                const auto& fp = c.tp->fp;
                if (fp.empty()) return {};
                const auto df = static_cast<double>(fp.size());
                c.idf = static_cast<float>(std::log(
                    1.0 + (static_cast<double>(N) - df + 0.5) / (df + 0.5)));
                const std::size_t nblk = (fp.size() + B - 1) / B;
                c.tp->live.resize(fp.size());
                c.tp->dls.resize(fp.size());
                c.block_filled.assign(nblk, 0);
                c.block_ub.assign(nblk, 0.0F);
                c.ub_done.assign(nblk, 0);
            }
        }

        auto advance = [](BmwCur& c, std::uint64_t target) {
            const auto* o = c.tp->fp.ords.data();
            const std::size_t n = c.tp->fp.size();
            std::size_t lo = c.i;
            if (lo >= n || o[lo] >= target) return;
            std::size_t step = 1;
            std::size_t hi = lo + 1;
            while (hi < n && o[hi] < target) {
                lo = hi;
                hi += step;
                step <<= 1;
            }
            if (hi > n) hi = n;
            c.i = static_cast<std::size_t>(
                std::lower_bound(o + lo + 1, o + hi, target) - o);
        };

        // 懒填充:游标所在块的 live/doc_len 一次批量取(P2.1 的接口,
        // 块粒度复用)。
        auto ensure_block = [&](BmwCur& c) {
            const std::size_t b = c.i / B;
            if (c.block_filled[b]) return;
            auto& fp = c.tp->fp;
            const std::size_t start = b * B;
            const std::size_t cnt = std::min(B, fp.size() - start);
            live_checker.fill_is_live(
                std::span<const std::uint64_t>(fp.ords.data() + start, cnt),
                std::span<char>(c.tp->live.data() + start, cnt));
            live_checker.fill_doc_lens(
                std::span<const std::uint64_t>(fp.ords.data() + start, cnt),
                std::span<std::uint32_t>(c.tp->dls.data() + start, cnt));
            c.block_filled[b] = 1;
        };

        auto block_ub = [&](BmwCur& c) -> float {
            const std::size_t b = c.i / B;
            if (!c.ub_done[b]) {
                const auto& fp = c.tp->fp;
                // 尾块未 seal 无块元数据 → 列表级 max_tf + dl=1 退化(admissible)。
                const bool sealed = b < fp.blocks.size();
                const std::uint32_t mtf = sealed ? fp.blocks[b].max_tf
                                                 : fp.max_tf;
                const std::uint32_t mdl = sealed ? fp.blocks[b].min_dl : 1;
                c.block_ub[b] =
                    upper_bound_from(mtf, c.idf, params, avgdl, mdl);
                c.ub_done[b] = 1;
            }
            return c.block_ub[b];
        };

        auto block_end = [](const BmwCur& c) -> std::uint64_t {
            const std::size_t b = c.i / B;
            const auto& fp = c.tp->fp;
            return b < fp.blocks.size() ? fp.blocks[b].end_ord
                                        : fp.ords.back();
        };

        using Entry = std::pair<float, std::uint64_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<>> heap;

        bool exhausted = false;
        while (!exhausted && curs[0].i < curs[0].tp->fp.size()) {
            const std::uint64_t v = curs[0].tp->fp.ords[curs[0].i];
            std::size_t j = 1;
            for (; j < nterms; ++j) {
                advance(curs[j], v);
                if (curs[j].i == curs[j].tp->fp.size()) {
                    exhausted = true;
                    break;
                }
                if (curs[j].tp->fp.ords[curs[j].i] != v) break;
            }
            if (exhausted) break;
            if (j < nterms) {
                // 被第 j 列表挡住:驱动游标跳到挡路值。
                advance(curs[0], curs[j].tp->fp.ords[curs[j].i]);
                continue;
            }

            if (heap.size() == k) {
                float ub = 0.0F;
                for (auto& c : curs) ub += block_ub(c);
                if (ub <= heap.top().first) {
                    // 当前各块的上界之和够不到 θ:整段跳过,不查 live
                    // 不评分。跳到各块末尾的最小值 +1。
                    std::uint64_t next = block_end(curs[0]);
                    for (std::size_t m = 1; m < nterms; ++m) {
                        next = std::min(next, block_end(curs[m]));
                    }
                    advance(curs[0], next + 1);
                    continue;
                }
            }

            bool all_live = true;
            for (auto& c : curs) {
                ensure_block(c);
                if (!c.tp->live[c.i]) {
                    all_live = false;
                    break;
                }
            }
            if (all_live) {
                float score = 0.0F;
                for (auto& c : curs) {
                    // 公式与原 must 评分循环逐运算一致(分数位级不变约定)。
                    auto tf_norm =
                        static_cast<float>(c.tp->fp.tfs[c.i]) *
                        (params.k1 + 1.0F) /
                        (static_cast<float>(c.tp->fp.tfs[c.i]) +
                         params.k1 *
                             (1.0F - params.b +
                              params.b *
                                  static_cast<float>(c.tp->dls[c.i]) /
                                  static_cast<float>(avgdl)));
                    score += c.idf * (tf_norm + params.delta);
                }
                if (heap.size() < k) {
                    heap.push({score, v});
                } else if (score > heap.top().first) {
                    heap.pop();
                    heap.push({score, v});
                }
            }
            ++curs[0].i;
        }

        std::vector<SearchResult> results;
        results.reserve(heap.size());
        while (!heap.empty()) {
            auto& [score, ord] = heap.top();
            results.push_back({ord, score});
            heap.pop();
        }
        std::reverse(results.begin(), results.end());
        return results;
    }

    // P2.1：每个 term 的 live 批量取一次（此前 must_not/交集/should/idf/评分
    // 五个阶段各自逐 posting 重扫 is_live——既重复又每次一锁）。
    // must/should 进评分循环，需 doc_len 批量（with_dls）；must_not 只用 live
    // 建排除集，免去 doc_len 取数。
    auto fill_live = [&](std::span<TermPostings> v, bool with_dls) {
        for (auto& tp : v) {
            tp.live.resize(tp.fp.size());
            live_checker.fill_is_live(tp.fp.ords, tp.live);
            if (with_dls) {
                tp.dls.resize(tp.fp.size());
                live_checker.fill_doc_lens(tp.fp.ords, tp.dls);
            }
        }
    };
    fill_live(must_tps, /*with_dls=*/true);
    fill_live(should_tps, /*with_dls=*/true);
    fill_live(must_not_tps, /*with_dls=*/false);

    std::vector<std::uint64_t> must_not_ords;
    for (auto& tp : must_not_tps) {
        for (std::size_t i = 0; i < tp.fp.size(); ++i) {
            if (tp.live[i]) {
                must_not_ords.push_back(tp.fp.ords[i]);
            }
        }
    }
    std::sort(must_not_ords.begin(), must_not_ords.end());
    must_not_ords.erase(std::unique(must_not_ords.begin(), must_not_ords.end()), must_not_ords.end());

    if (must_tps.empty() && should_tps.empty()) return {};

    std::vector<std::uint64_t> candidates;

    if (!must_tps.empty()) {
        bool all_terms_found = true;
        for (auto& term : must_terms) {
            auto& shard = shard_for(term);
            PostingMap::const_accessor acc;
            if (!shard.inverted.find(acc, term)) {
                all_terms_found = false;
                break;
            }
        }

        if (!all_terms_found) {
            return {};
        }

        // O4：按 posting 数升序处理 MUST——最短 list 先进交集，accumulator 尽早
        // 缩小；交集一旦为空提前退出。交集与处理顺序无关，结果集语义不变
        // （must_tps 本体不重排，评分用）。
        std::vector<std::size_t> must_order(must_tps.size());
        for (std::size_t i = 0; i < must_order.size(); ++i) must_order[i] = i;
        std::sort(must_order.begin(), must_order.end(),
                  [&](std::size_t a, std::size_t b) {
                      return must_tps[a].fp.size() <
                             must_tps[b].fp.size();
                  });

        // K1:k-way leapfrog 交集(替代 pairwise:k-1 轮中间 vector 物化
        // + 每轮 live 过滤拷贝)。k 个游标在各 posting 数组上同时推进,
        // 最短列表驱动,其余 galloping advance——大小不对称时天然亚线性。
        // 结果谓词与 pairwise 等价:ord ∈ 结果 ⟺ 出现在全部 MUST 列表
        // 且各列表 live 标志全真。这里定下的 advance(target) 形态就是
        // 后续块级元数据 / BMW 的游标接口(doc/kway-blockmax-bmw-zh.md)。
        auto run_must_intersect = [&] {
            std::vector<std::uint64_t> acc;
            const std::size_t mk = must_order.size();

            // 单词退化:live 过滤直拷(与旧实现首词分支等价)。
            if (mk == 1) {
                auto& tp = must_tps[must_order[0]];
                acc.reserve(tp.fp.size());
                for (std::size_t i = 0; i < tp.fp.size(); ++i) {
                    if (tp.live[i]) acc.push_back(tp.fp.ords[i]);
                }
                return acc;
            }

            // k==2 走 SIMD pairwise(intersect_u64:旋转内核 + galloping
            // 分发)。实测两热词形态 leapfrog 比 SIMD 慢 ~10-13%
            // (BoolMustHot 4096:44.3→50.3μs),两次 live 过滤拷贝的代价
            // 小于 SIMD 对标量的优势;k≥3 才轮到 leapfrog(收益来自
            // 消除 k-1 轮物化 + 多列表互相 gallop)。
            if (mk == 2) {
                std::vector<std::uint64_t> a;
                std::vector<std::uint64_t> b;
                auto fill = [&](const TermPostings& tp,
                                std::vector<std::uint64_t>& dst) {
                    dst.reserve(tp.fp.size());
                    for (std::size_t i = 0; i < tp.fp.size(); ++i) {
                        if (tp.live[i]) dst.push_back(tp.fp.ords[i]);
                    }
                };
                fill(must_tps[must_order[0]], a);
                fill(must_tps[must_order[1]], b);
                intersect_u64(a, b, acc);
                return acc;
            }

            struct Cur {
                const std::uint64_t* ords;
                const char* live;
                std::size_t n;
                std::size_t i = 0;
            };
            std::vector<Cur> curs;
            curs.reserve(mk);
            for (auto mi : must_order) {
                auto& tp = must_tps[mi];
                if (tp.fp.size() == 0) return acc;  // 任一列表空 → 交集空
                curs.push_back(Cur{tp.fp.ords.data(), tp.live.data(),
                                   tp.fp.size(), 0});
            }
            acc.reserve(curs[0].n);  // 上界 = 最短列表长度

            // advance:游标推到首个 ords[i] >= target 处(galloping +
            // 二分收尾)。游标只前进不回退——target 跨轮单调不减。
            auto advance = [](Cur& c, std::uint64_t target) {
                std::size_t lo = c.i;
                if (lo >= c.n || c.ords[lo] >= target) return;
                std::size_t step = 1;
                std::size_t hi = lo + 1;
                while (hi < c.n && c.ords[hi] < target) {
                    lo = hi;
                    hi += step;
                    step <<= 1;
                }
                if (hi > c.n) hi = c.n;
                // ords[lo] < target 已知,二分区间 (lo, hi)。
                c.i = static_cast<std::size_t>(
                    std::lower_bound(c.ords + lo + 1, c.ords + hi, target) -
                    c.ords);
            };

            while (curs[0].i < curs[0].n) {
                const std::uint64_t v = curs[0].ords[curs[0].i];
                std::size_t j = 1;
                for (; j < mk; ++j) {
                    advance(curs[j], v);
                    if (curs[j].i == curs[j].n) return acc;  // 耗尽 → 结束
                    if (curs[j].ords[curs[j].i] != v) break; // 被挡住
                }
                if (j == mk) {
                    // 全列表命中:liveness 全检后输出。
                    bool all_live = true;
                    for (std::size_t m = 0; m < mk; ++m) {
                        if (!curs[m].live[curs[m].i]) {
                            all_live = false;
                            break;
                        }
                    }
                    if (all_live) acc.push_back(v);
                    ++curs[0].i;
                } else {
                    // 驱动游标直接跳到挡路值,跳过中间注定不在交集的区段。
                    advance(curs[0], curs[j].ords[curs[j].i]);
                }
            }
            return acc;
        };

        candidates = run_must_intersect();
    } else if (!should_tps.empty()) {
        for (auto& tp : should_tps) {
            for (std::size_t i = 0; i < tp.fp.size(); ++i) {
                if (tp.live[i]) {
                    candidates.push_back(tp.fp.ords[i]);
                }
            }
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    } else {
        return {};
    }

    // 注意：SHOULD 词在 MUST 非空时只参与打分（见下方评分循环），不扩大候选集。
    // 候选集已由上面确定（MUST → 交集；纯 SHOULD → 并集），此处不再追加 SHOULD ords，
    // 否则「只含 should、不含 must」的文档会错误进入结果（违反 MUST 语义）。

    std::vector<std::uint64_t> filtered;
    filtered.reserve(candidates.size());
    for (auto ord : candidates) {
        if (!std::binary_search(must_not_ords.begin(), must_not_ords.end(), ord)) {
            filtered.push_back(ord);
        }
    }
    candidates = std::move(filtered);

    if (candidates.empty()) return {};

    auto N = live_doc_count_.load(std::memory_order_relaxed);
    auto sum_dl = sum_doc_len_.load(std::memory_order_relaxed);
    auto avgdl = N > 0 ? static_cast<double>(sum_dl) / static_cast<double>(N) : 1.0;

    // S13-P3：move 而非拷贝——TermPostings 持整条 posting 扁平快照
    // （ords u64 + tfs u32 + live + dls，热词可达 MB 级），此处之后
    // must_tps/should_tps 不再使用，深拷贝纯属浪费。
    std::vector<TermPostings> all_tps;
    all_tps.reserve(must_tps.size() + should_tps.size());
    all_tps.insert(all_tps.end(),
                   std::make_move_iterator(must_tps.begin()),
                   std::make_move_iterator(must_tps.end()));
    all_tps.insert(all_tps.end(),
                   std::make_move_iterator(should_tps.begin()),
                   std::make_move_iterator(should_tps.end()));

    std::sort(all_tps.begin(), all_tps.end(), [](const auto& a, const auto& b) {
        return a.term < b.term;
    });
    all_tps.erase(std::unique(all_tps.begin(), all_tps.end(), [](const auto& a, const auto& b) {
        return a.term == b.term;
    }), all_tps.end());

    std::unordered_map<std::string, float> term_idf;
    for (auto& tp : all_tps) {
        std::size_t live_df = 0;
        for (std::size_t i = 0; i < tp.fp.size(); ++i) {
            live_df += static_cast<std::size_t>(tp.live[i]);
        }
        if (live_df == 0) continue;
        auto idf = std::log(1.0 + (static_cast<double>(N) - static_cast<double>(live_df) + 0.5) /
                          (static_cast<double>(live_df) + 0.5));
        term_idf[tp.term] = static_cast<float>(idf);
    }

    // 候选集与 posting ords 都是升序去重——评分用「平行分数数组 +
    // 每词双指针归并」O(|posting| + |candidates|)。替代原先的
    // unordered_map 播种:per-candidate 一次 hash 节点分配(实测
    // BoolMust 每查询 ~2 万次 malloc 即来源于此)+ 每 posting 一次
    // hash find,全部消除。
    std::vector<float> scores(candidates.size(), 0.0F);

    for (auto& tp : all_tps) {
        auto idf_it = term_idf.find(tp.term);
        if (idf_it == term_idf.end()) continue;
        auto idf = idf_it->second;

        std::size_t ci = 0;
        for (std::size_t i = 0;
             i < tp.fp.size() && ci < candidates.size(); ++i) {
            const auto posting_ord = tp.fp.ords[i];
            while (ci < candidates.size() && candidates[ci] < posting_ord) {
                ++ci;
            }
            if (ci == candidates.size()) break;
            if (candidates[ci] != posting_ord) continue;
            if (!tp.live[i]) continue;

            // P2.1：doc_len 读批量数组 tp.dls（此前逐 posting 一把 Index
            // shared_lock + 虚调用，大候选集下锁风暴；与其它路径对齐）。
            auto dl = tp.dls[i];
            auto tf_norm = static_cast<float>(tp.fp.tfs[i]) *
                           (params.k1 + 1.0F) /
                           (static_cast<float>(tp.fp.tfs[i]) + params.k1 *
                            (1.0F - params.b + params.b *
                             static_cast<float>(dl) / static_cast<float>(avgdl)));
            scores[ci] += idf * (tf_norm + params.delta);
        }
    }

    using Entry = std::pair<float, std::uint64_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> heap;

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const float score = scores[i];
        if (heap.size() < k) {
            heap.push({score, candidates[i]});
        } else if (score > heap.top().first) {
            heap.pop();
            heap.push({score, candidates[i]});
        }
    }

    std::vector<SearchResult> results;
    results.reserve(heap.size());
    while (!heap.empty()) {
        auto& [score, ord] = heap.top();
        results.push_back({ord, score});
        heap.pop();
    }
    std::reverse(results.begin(), results.end());
    return results;
}

// S13-D9：树形布尔求值（契约见 inverted.hpp）。集合式：每叶产出 live ord
// 升序集，组内交/并/差后按全部正向词打分取 top-k。
auto InvertedIndex::bool_search_tree(
    const QueryNode& root,
    std::size_t k,
    const LiveChecker& live_checker,
    const Bm25Params* params_override) const -> std::vector<SearchResult> {
    const Bm25Params& params = params_override ? *params_override : params_;

    // term 叶 → live ord 升序集（posting ords 本就 ord 升序）。
    auto term_ords = [&](const std::string& term) {
        std::vector<std::uint64_t> out;
        const auto& shard = shard_for(term);
        PostingMap::const_accessor acc;
        if (!shard.inverted.find(acc, term)) return out;
        const PostingList& pl = *acc->second;
        std::vector<std::uint64_t> ords(pl.ords);  // S22-M6：整列拷贝
        std::vector<char> live(ords.size());
        acc.release();
        live_checker.fill_is_live(ords, live);
        out.reserve(ords.size());
        for (std::size_t i = 0; i < ords.size(); ++i) {
            if (live[i]) out.push_back(ords[i]);
        }
        return out;
    };
    // 短语叶 → 匹配 ord 升序集（复用 search_phrase 内核取全部命中）。
    auto phrase_ords = [&](const std::vector<std::string>& terms) {
        std::vector<std::uint64_t> out;
        if (terms.empty()) return out;
        auto hits = search_phrase(terms,
                                  std::numeric_limits<std::size_t>::max(),
                                  live_checker, params_override);
        out.reserve(hits.size());
        for (const auto& h : hits) out.push_back(h.ord);
        std::sort(out.begin(), out.end());
        return out;
    };
    auto intersect = [](std::vector<std::uint64_t>& a,
                        const std::vector<std::uint64_t>& b) {
        std::vector<std::uint64_t> out;
        std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                              std::back_inserter(out));
        a = std::move(out);
    };
    auto unite = [](std::vector<std::uint64_t>& a,
                    const std::vector<std::uint64_t>& b) {
        std::vector<std::uint64_t> out;
        std::set_union(a.begin(), a.end(), b.begin(), b.end(),
                       std::back_inserter(out));
        a = std::move(out);
    };
    auto subtract = [](std::vector<std::uint64_t>& a,
                       const std::vector<std::uint64_t>& b) {
        std::vector<std::uint64_t> out;
        std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                            std::back_inserter(out));
        a = std::move(out);
    };

    // 递归求值。返回该节点的匹配 ord 集（升序）。
    std::function<std::vector<std::uint64_t>(const QueryNode&)> eval =
        [&](const QueryNode& node) -> std::vector<std::uint64_t> {
        if (node.is_phrase) return phrase_ords(node.phrase_terms);
        if (!node.term.empty()) return term_ords(node.term);
        // 组：MUST 交集为基集（无 MUST 则 SHOULD 并集）；MUST_NOT 差集。
        std::vector<std::uint64_t> base;
        bool has_must = false, base_init = false;
        for (const auto& c : node.children) {
            if (c.op != QueryOp::MUST) continue;
            has_must = true;
            auto cs = eval(c);
            if (!base_init) { base = std::move(cs); base_init = true; }
            else intersect(base, cs);
            if (base.empty()) break;
        }
        if (!has_must) {
            for (const auto& c : node.children) {
                if (c.op != QueryOp::SHOULD) continue;
                auto cs = eval(c);
                if (!base_init) { base = std::move(cs); base_init = true; }
                else unite(base, cs);
            }
        }
        if (!base.empty()) {
            for (const auto& c : node.children) {
                if (c.op != QueryOp::MUST_NOT) continue;
                subtract(base, eval(c));
                if (base.empty()) break;
            }
        }
        return base;
    };

    auto candidates = eval(root);
    if (candidates.empty()) return {};

    // 打分词集：全部正向 term 叶（带 boost）+ 正向短语成分词（boost 1）。
    struct ScoringTerm { std::string term; float boost; };
    std::vector<ScoringTerm> sterms;
    std::function<void(const QueryNode&)> collect_pos =
        [&](const QueryNode& node) {
        if (node.op == QueryOp::MUST_NOT) return;
        if (node.is_phrase) {
            for (const auto& t : node.phrase_terms) sterms.push_back({t, 1.0F});
            return;
        }
        if (!node.term.empty()) { sterms.push_back({node.term, node.boost}); return; }
        for (const auto& c : node.children) collect_pos(c);
    };
    collect_pos(root);
    // 同词去重（保留最大 boost，避免重复计分）。
    std::sort(sterms.begin(), sterms.end(),
              [](const auto& a, const auto& b) {
                  return a.term < b.term ||
                         (a.term == b.term && a.boost > b.boost);
              });
    sterms.erase(std::unique(sterms.begin(), sterms.end(),
                             [](const auto& a, const auto& b) {
                                 return a.term == b.term;
                             }),
                 sterms.end());

    const auto N = live_doc_count_.load(std::memory_order_relaxed);
    const auto sum_dl = sum_doc_len_.load(std::memory_order_relaxed);
    const double avgdl =
        N > 0 ? static_cast<double>(sum_dl) / static_cast<double>(N) : 1.0;

    // 候选平行分数数组 + 每词双指针归并（同扁平 bool_search 的评分形态）。
    std::vector<float> scores(candidates.size(), 0.0F);
    std::vector<std::uint64_t> ords_buf;
    std::vector<std::uint32_t> tfs_buf;
    std::vector<char> live_buf;
    std::vector<std::uint32_t> dls_buf;
    for (const auto& st : sterms) {
        const auto& shard = shard_for(st.term);
        PostingMap::const_accessor acc;
        if (!shard.inverted.find(acc, st.term)) continue;
        const PostingList& pl = *acc->second;
        // S22-M6：整列 assign（memcpy，复用 buf 容量）。
        ords_buf.assign(pl.ords.begin(), pl.ords.end());
        tfs_buf.assign(pl.tfs.begin(), pl.tfs.end());
        acc.release();
        live_buf.resize(ords_buf.size());
        live_checker.fill_is_live(ords_buf, live_buf);
        dls_buf.resize(ords_buf.size());
        live_checker.fill_doc_lens(ords_buf, dls_buf);

        std::size_t live_df = 0;
        for (char c : live_buf) live_df += static_cast<std::size_t>(c);
        if (live_df == 0) continue;
        const float idf = static_cast<float>(std::log(
            1.0 + (static_cast<double>(N) - static_cast<double>(live_df) + 0.5) /
                      (static_cast<double>(live_df) + 0.5)));

        std::size_t ci = 0;
        for (std::size_t i = 0;
             i < ords_buf.size() && ci < candidates.size(); ++i) {
            const auto po = ords_buf[i];
            while (ci < candidates.size() && candidates[ci] < po) ++ci;
            if (ci == candidates.size()) break;
            if (candidates[ci] != po || !live_buf[i]) continue;
            const auto dl = dls_buf[i];
            const float tf_norm =
                static_cast<float>(tfs_buf[i]) * (params.k1 + 1.0F) /
                (static_cast<float>(tfs_buf[i]) +
                 params.k1 * (1.0F - params.b +
                              params.b * static_cast<float>(dl) /
                                  static_cast<float>(avgdl)));
            scores[ci] += st.boost * idf * (tf_norm + params.delta);
        }
    }

    using Entry = std::pair<float, std::uint64_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> heap;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (heap.size() < k) {
            heap.push({scores[i], candidates[i]});
        } else if (scores[i] > heap.top().first) {
            heap.pop();
            heap.push({scores[i], candidates[i]});
        }
    }
    std::vector<SearchResult> results;
    results.reserve(heap.size());
    while (!heap.empty()) {
        results.push_back({heap.top().second, heap.top().first});
        heap.pop();
    }
    std::reverse(results.begin(), results.end());
    return results;
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
    auto* f = std::fopen(std::string(path).c_str(), "wb");
    if (!f) return false;
    const bool wrote =
        buf.empty() || std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    return wrote;
}

auto InvertedIndex::load(std::string_view path) -> bool {
    // S13-M3：RAII 持 FILE*——fsz 来自可能损坏的文件，resize 可抛 bad_alloc，
    // 裸 FILE* 在异常路径泄漏。
    struct FileCloser {
        void operator()(std::FILE* fp) const noexcept {
            if (fp) std::fclose(fp);
        }
    };
    std::unique_ptr<std::FILE, FileCloser> f(
        std::fopen(std::string(path).c_str(), "rb"));
    if (!f) return false;
    std::fseek(f.get(), 0, SEEK_END);
    const long fsz = std::ftell(f.get());
    std::fseek(f.get(), 0, SEEK_SET);
    std::vector<std::byte> buf;
    bool rd = (fsz >= 0);
    if (rd) {
        buf.resize(static_cast<std::size_t>(fsz));
        rd = buf.empty() ||
             std::fread(buf.data(), 1, buf.size(), f.get()) == buf.size();
    }
    f.reset();
    if (!rd) return false;
    return deserialize(buf);
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
            if (tlen == kReadFail32 || tlen > 1024) { return false; }

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
