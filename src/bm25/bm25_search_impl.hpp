// bm25_search_impl.hpp — BOW/WAND 评分共享实现（内部头,S30-P1）。
//
// 从 inverted.cpp 匿名命名空间抽出:BM25 公式与「分数位级不变 / 无分支可
// 向量化」不变量只此一处——InvertedIndex 与 MmapSegment(S30 封口段 mmap
// reader)两个消费方共用,mmap 段查询与内存段**逐位一致**由「同一实现」
// 保证而非对拍维持。
//
// 契约:
// - score_bow_topk / search_wand_impl 的评分数学与调用序不得分叉;改公式
//   必须两个消费方一起过等价性测试(tests/segment_v2_test.cpp round-trip)。
// - search_wand_impl 收**已快照**的 FlatPostings 指针(平行 terms,已压实
//   无空槽);N/sum_dl 由 caller 按 ext-or-本地统计解析后传入(原
//   InvertedIndex::search_wand 唯二实例状态)。指向物须在调用期间稳定
//   (caller 的 thread_local 池/mmap 解码 scratch)。
//
// 线程安全:纯函数 + 函数内 thread_local 工作区(全程串行,无 TBB spawn)。

#pragma once

#include "bitcask/bm25_kernels.hpp"
#include "bitcask/inverted.hpp"

#include "bitcask/intersect.hpp"

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>  // phrase 候选评分并行

#include <functional>
#include <limits>
#include <unordered_map>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bitcask::bm25::detail {

// BOW/WAND 路由阈值:总 posting 数 ≥ 此值走 Block-Max WAND,否则标量 BOW。
// InvertedIndex::search 与 MmapSegment::search 必须共用(路由分叉 = 同一
// 查询在内存段与 mmap 段可能走不同算法——结果仍等价,但违反位级一致契约)。
inline constexpr std::size_t kWandRouteThreshold = 1024;

// S7-5：短语/近邻查询候选数（驱动词 posting 数）≥ 此阈值才并行评分。
// 甜区是大候选集（热词短语，~8.7ms）；小候选集并行 task spawn 开销 > 收益，
// 走串行（同 S7-1 BOW 串行化的教训）。
inline constexpr std::size_t kPhraseParallelThreshold = 2048;

// block_for_ord / block_upper_bound 的共享实现——PostingList 与
// FlatPostings（P1 查询快照）语义必须一致，逻辑只写一份。
inline const PostingBlock* block_for_ord_in(
    const std::vector<PostingBlock>& blocks, std::uint64_t ord) {
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
inline float upper_bound_from(std::uint32_t global_max_tf, float idf,
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
inline std::vector<SearchResult> score_bow_topk(
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
            bm25_score_dispatch(
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

// Block-Max WAND 主体（原 InvertedIndex::search_wand 逐字搬移,S30-P1 抽出
// 供 MmapSegment 复用）。参数化差异:
// - terms/fps 平行且已压实(无空槽;caller 完成 snapshot_flat/解码);
// - N/sum_dl 已由 caller 按 ext-or-本地统计解析(原唯二实例状态);
// - query_term_count = 原始查询词数(含未命中词)——「单词查询恒全量填充
//   dls」的判定沿用原语义(按查询词数而非命中词数)。
inline std::vector<SearchResult> search_wand_impl(
    std::span<const std::string_view> terms,
    std::span<const FlatPostings* const> fps,
    std::size_t k,
    const LiveChecker& live_checker,
    const Bm25Params& params,
    std::uint64_t N,
    std::uint64_t sum_dl,
    const std::vector<std::pair<std::string, std::uint64_t>>* global_df,
    std::size_t query_term_count) {
    struct TermPostings {
        std::string_view term;
        const FlatPostings* fp = nullptr;  // caller 持有的快照（评分期间稳定）
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
    // S23-M3：thread_local 池复用（每 term 4 个内层 vector：live/dls/
    // dls_filled/block_upper_bounds，原每查询全新分配）。
    // WAND 主循环只经 order 索引数组排序、从不搬移/增删 tps 元素，池槽稳定；
    // 全程串行无 TBB spawn（无 work-stealing 重入窗口）。标量字段逐一重置，
    // block_upper_bounds 为 push_back 增长必须 clear；live/dls/dls_filled
    // 由下方 resize+fill/assign 整段覆盖。
    static thread_local std::vector<TermPostings> tps_pool;
    std::size_t n_tps = 0;
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (n_tps == tps_pool.size()) tps_pool.emplace_back();
        TermPostings& tp = tps_pool[n_tps];
        tp.term = terms[i];
        tp.fp = fps[i];
        tp.dls_all = false;
        tp.cursor = 0;
        tp.idf = 0.0f;
        tp.list_upper_bound = 0.0f;
        tp.block_upper_bounds.clear();
        ++n_tps;
    }
    if (n_tps == 0) return {};
    const std::span<TermPostings> tps(tps_pool.data(), n_tps);

    auto avgdl = N > 0 ? static_cast<double>(sum_dl) / static_cast<double>(N) : 1.0;

    // 计算每个 term 的 IDF 和上界分数。
    // P2.1：live/doc_len 批量取一次（Index 侧各一次锁）存进 tp——
    // DAAT 循环每 pivot 的 is_live/doc_len 改读数组，全程零虚调用零锁。
    constexpr std::size_t kB = PostingList::kBlockSize;
    for (auto& tp : tps) {
        tp.live.resize(tp.fp->size());
        live_checker.fill_is_live(tp.fp->ords, tp.live);
        // S13-P4：dls 惰性按块填充（见 ensure_dls）。中小列表（≤32 块 =
        // 4K posting）直接全量填充——批量 gather 本就便宜，惰性簿记
        // （per-pivot 分支 + 位图）反而更贵（实测 4K 档 +13%~28%）；
        // 大列表才吃 WAND 块跳跃的省填充收益。
        tp.dls.resize(tp.fp->size());
        // 单词查询恒全量：无其它词可比 → WAND 不可能跳块，惰性纯开销。
        if (query_term_count == 1 || tp.fp->size() <= 32 * kB) {
            live_checker.fill_doc_lens(tp.fp->ords, tp.dls);
            tp.dls_filled.assign(1, 1);  // 单标记=全满（ensure_dls 兼容见下）
            tp.dls_all = true;
        } else {
            tp.dls_filled.assign((tp.fp->size() + kB - 1) / kB, 0);
        }
        std::size_t live_df = 0;
        for (std::size_t i = 0; i < tp.live.size(); ++i) {
            live_df += static_cast<std::size_t>(tp.live[i]);
        }
        if (live_df == 0) {
            tp.idf = 0.0f;
            tp.list_upper_bound = 0.0f;
            // S30-P2 修复(既存潜伏 bug,S10-A2 起):全死词此前留空
            // block_upper_bounds 便 continue,但其游标仍参与下方块跳跃判定
            // → 空数组按 block_idx 越界读(触发条件:≥kBlockSize posting 的
            // 词全部死亡 + WAND 档查询;ASan 于 mmap 段恢复场景实测抓获)。
            // 填 0 与「idf=0 时逐块计算」位级一致(上界 = idf×… = 0)。
            tp.block_upper_bounds.assign(tp.fp->blocks.size(), 0.0F);
            continue;
        }
        // S27-2：idf 的 df——全局注入优先，回退本段 live_df（同 score_bow_topk）。
        // idf 一致地用于块上界与实际打分 → WAND 剪枝仍正确。
        double df_idf = static_cast<double>(live_df);
        if (global_df) {
            for (const auto& [t, v] : *global_df) {  // S29-5：扁平列表线性扫
                if (t == tp.term) {
                    if (v > 0) df_idf = static_cast<double>(v);
                    break;
                }
            }
        }
        tp.idf = static_cast<float>(std::log(1.0 + (static_cast<double>(N) - df_idf + 0.5) /
                                             (df_idf + 0.5)));
        tp.list_upper_bound = tp.fp->block_upper_bound(tp.idf, params, avgdl);
        // S10-A2:per-block 上界一次算好，WAND 内层循环免每次 pivot 重算。
        tp.block_upper_bounds.reserve(tp.fp->blocks.size());
        for (const auto& blk : tp.fp->blocks) {
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
        const std::size_t cnt = std::min(kB, tp.fp->size() - start);
        live_checker.fill_doc_lens(
            std::span<const std::uint64_t>(tp.fp->ords.data() + start, cnt),
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
    // 循环遍历全部匹配词，块跳跃仅 admissible 跳过。
    auto wand_less = [&tps](std::size_t a, std::size_t b) {
        const auto& ta = tps[a];
        const auto& tb = tps[b];
        bool a_ex = ta.cursor >= ta.fp->ords.size();
        bool b_ex = tb.cursor >= tb.fp->ords.size();
        if (a_ex && b_ex) return false;
        if (a_ex) return true;
        if (b_ex) return false;
        return ta.fp->ords[ta.cursor] < tb.fp->ords[tb.cursor];
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
            if (tp.cursor >= tp.fp->ords.size()) continue;
            acc_score += tp.list_upper_bound;
            if (acc_score >= threshold) {
                pivot_pos = i;
                pivot_found = true;
                break;
            }
        }
        if (!pivot_found) break;

        auto& pivot_tp = tps[order[pivot_pos]];
        auto pivot_ord = pivot_tp.fp->ords[pivot_tp.cursor];

        // C4 修复(既存正确性 bug,S8/S10 era 起潜伏):规范 WAND 要求评分前
        // **对齐**——排序在 pivot 之前、docid < pivot_ord 的列表必须先推进到
        // ≥ pivot_ord 并重选 pivot。原实现缺此步:落后列表被跳过评分后又被
        // 尾部推进越过 pivot ⟹ ① 命中文档以**部分分数**进堆;② 更糟,
        // 「部分分 < θ ≤ 真分」的文档被整个丢弃(top-k 集合错误)。三方对拍
        // (穷举参照)实测抓获:ord 252 丢失 hot 贡献。galloping 推进与
        // bool_search BMW 的 advance 同型。
        {
            bool lagged = false;
            for (std::size_t i = 0; i < pivot_pos; ++i) {
                auto& tp = tps[order[i]];
                if (tp.cursor >= tp.fp->ords.size()) continue;
                if (tp.fp->ords[tp.cursor] >= pivot_ord) continue;
                const auto* o = tp.fp->ords.data();
                const std::size_t n = tp.fp->size();
                std::size_t lo = tp.cursor;
                std::size_t step = 1;
                std::size_t hi = lo + 1;
                while (hi < n && o[hi] < pivot_ord) {
                    lo = hi;
                    hi += step;
                    step <<= 1;
                }
                if (hi > n) hi = n;
                tp.cursor = static_cast<std::size_t>(
                    std::lower_bound(o + lo + 1, o + hi, pivot_ord) - o);
                lagged = true;
            }
            if (lagged) continue;  // 游标变了:重排序、重选 pivot(不评分)
        }

        // A1:非耗尽词的列表上界总和——块跳跃判定的保守"其余词"上界
        // (含 pivot 之后 cursor 恰为 pivot_ord 的词,admissible)。
        float total_ub = 0.0f;
        for (auto& t : tps) {
            if (t.cursor < t.fp->ords.size()) total_ub += t.list_upper_bound;
        }

        bool any_skipped = false;
        for (std::size_t i = 0; i <= pivot_pos; ++i) {
            auto& tp = tps[order[i]];
            if (tp.cursor >= tp.fp->ords.size()) continue;
            if (tp.fp->ords[tp.cursor] != pivot_ord) continue;

            const auto* block = tp.fp->block_for_ord(pivot_ord);
            if (block != nullptr) {
                // S10-A2:读初始化阶段算好的 per-block 上界（免每次 pivot 重算 6 FMA+1 div）。
                const std::size_t block_idx =
                    static_cast<std::size_t>(block - tp.fp->blocks.data());
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
                    if (next_start >= tp.fp->ords.size()) {
                        tp.cursor = tp.fp->ords.size();
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
                if (tps[i].cursor >= tps[i].fp->ords.size()) continue;
                if (tps[i].fp->ords[tps[i].cursor] != pivot_ord) continue;

                auto tf_norm = static_cast<float>(tps[i].fp->tfs[tps[i].cursor]) *
                               (params.k1 + 1.0f) /
                               (static_cast<float>(tps[i].fp->tfs[tps[i].cursor]) + params.k1 *
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
            while (tps[i].cursor < tps[i].fp->ords.size() && tps[i].fp->ords[tps[i].cursor] <= pivot_ord) {
                ++tps[i].cursor;
            }
        }

        bool any_exhausted = false;
        for (auto& tp : tps) {
            if (tp.cursor >= tp.fp->ords.size()) any_exhausted = true;
        }
        if (any_exhausted) {
            bool all_exhausted = true;
            for (auto& tp : tps) {
                if (tp.cursor < tp.fp->ords.size()) {
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

// 短语/近邻匹配 + 评分主体(原 InvertedIndex::search_phrase_impl 采集后
// 半段逐字搬移,S30-P1 抽出供 MmapSegment 复用)。
// 契约:pls 与查询词序平行且**全部命中**(任一词缺席由 caller 直接返回空);
// 指向的 PostingList 在调用期间稳定且含 positions(ords/positions/find 被
// 消费;tf 列不参与——短语分数用 phrase_tf)。slop=0 严格相邻。
inline std::vector<SearchResult> phrase_search_impl(
    std::span<const PostingList* const> pls,
    std::size_t k,
    std::uint32_t slop,
    const LiveChecker& live_checker,
    const Bm25Params& params,
    std::uint64_t N,
    std::uint64_t sum_dl) {
    if (pls.empty()) return {};
    auto avgdl = N > 0 ? static_cast<double>(sum_dl) / static_cast<double>(N) : 1.0;

    auto& first_pl = *pls[0];

    // S13-P8.2：候选枚举改由**最稀有词**驱动（此前恒 tps[0]——"the quantum"
    // 会遍历 "the" 的大表）。候选集 = 全词交集不变；两列表都按 ord 升序 ⟹
    // (score, ord) 推入序一致，top-k 含平分决策**逐字节同果**。idf 语义仍取
    // first term 的 live_df（评分公式不变）。
    std::size_t drv = 0;
    for (std::size_t t = 1; t < pls.size(); ++t) {
        if (pls[t]->size() < pls[drv]->size()) drv = t;
    }
    auto& cand_pl = *pls[drv];
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

    // S7-5：单候选评分——纯函数，仅读 pls/first_*/params（const）并写自己的返回值，
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
        other_pos.assign(pls.size(), {});
        // 链式匹配从 term 0 的 positions 起步（驱动词只负责候选枚举）。
        std::span<const std::uint32_t> anchor;
        if (drv == 0) {
            anchor = cand_pl.positions(i);
        } else {
            auto idx0 = first_pl.find(posting_ord);
            if (idx0 >= first_pl.size()) return 0.0F;
            anchor = first_pl.positions(idx0);
        }
        for (std::size_t t = 1; t < pls.size(); ++t) {
            if (t == drv) {
                other_pos[t] = cand_pl.positions(i);
                continue;
            }
            auto& other_pl = *pls[t];
            auto idx = other_pl.find(posting_ord);
            if (idx >= other_pl.size()) return 0.0F;
            other_pos[t] = other_pl.positions(idx);
        }

        std::uint32_t phrase_tf = 0;
        for (auto start_pos : anchor) {
            // 有序匹配：term t 必须在 (prev, prev+1+slop] 内出现（slop=0 即精确相邻）。
            bool match = true;
            std::uint32_t prev = start_pos;
            for (std::size_t t = 1; t < pls.size(); ++t) {
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

// BM25 评分分项解释主体(原 InvertedIndex::explain 逐字搬移,S30-P1 抽出
// 供 MmapSegment 复用)。tps 与查询词序平行(缺席词 fp 指向空 FlatPostings,
// 输出仍记录 df=0 条目以示未命中);公式与 search() 完全一致。
inline ScoreExplanation explain_impl(std::span<const ScoredTermView> tps,
                                     std::uint64_t ord,
                                     const LiveChecker& live_checker,
                                     const Bm25Params& params,
                                     std::uint64_t N,
                                     std::uint64_t sum_dl) {
    ScoreExplanation out;
    out.terms.reserve(tps.size());
    const double avgdl =
        N > 0 ? static_cast<double>(sum_dl) / static_cast<double>(N) : 1.0;
    const auto dl = live_checker.doc_len(ord);

    for (const auto& tv : tps) {
        TermScore ts;
        ts.term = *tv.term;
        const FlatPostings& fp = *tv.fp;
        if (fp.empty()) {
            // term 不在索引：df=0、各项 0，仍记录以示「未命中」。
            out.terms.push_back(std::move(ts));
            continue;
        }

        // 与 search() 一致地算 live df（O3：直接读 ords[]，免物化拷贝）。
        std::size_t live_df = 0;
        for (std::size_t i = 0; i < fp.size(); ++i) {
            if (live_checker.is_live(fp.ords[i])) ++live_df;
        }
        ts.df = live_df;
        if (live_df == 0) {
            out.terms.push_back(std::move(ts));
            continue;
        }

        ts.idf = std::log(1.0 + (static_cast<double>(N) - static_cast<double>(live_df) + 0.5) /
                                (static_cast<double>(live_df) + 0.5));

        // 找该 ord 的 posting 取 tf（不在该文档则 tf=0，贡献 0;与
        // PostingList::find 同为 lower_bound 二分）。
        const auto it = std::lower_bound(fp.ords.begin(), fp.ords.end(), ord);
        if (it != fp.ords.end() && *it == ord) {
            const auto idx = static_cast<std::size_t>(it - fp.ords.begin());
            ts.tf = fp.tfs[idx];
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

// 扁平布尔查询主体(原 InvertedIndex::bool_search 逐字搬移,S30-P1 抽出
// 供 MmapSegment 复用)。fetch(term, out):拷出该 term 的扁平快照,缺席返回
// false——采集是两个消费方唯一的差异点。N/sum_dl 由 caller 解析。
inline std::vector<SearchResult> bool_search_impl(
    const QueryNode& query,
    std::size_t k,
    const LiveChecker& live_checker,
    const Bm25Params& params,
    std::uint64_t N,
    std::uint64_t sum_dl,
    const std::function<bool(std::string_view, FlatPostings&)>& fetch) {
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
        if (n == pool.size()) pool.emplace_back();
        TermPostings& tp = pool[n];
        if (!fetch(term, tp.fp)) return;  // 缺席:槽位留待复用
        tp.term.assign(term);
        tp.is_must = is_must;
        ++n;
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
        // 任一 MUST 词缺席 → 空集(collect 只收命中词,计数比对即全查)。
        if (must_tps.size() != must_terms.size()) {
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


// 集合式：每叶产出 live ord 升序集，组内交/并/差后按全部正向词打分取 top-k。
// 树形布尔求值主体(原 InvertedIndex::bool_search_tree 逐字搬移,S30-P1
// 抽出供 MmapSegment 复用)。fetch 同 bool_search_impl;phrase_fn(terms):
// 返回该短语的全部命中(k=∞,各消费方绑定自己的 search_phrase)。
inline std::vector<SearchResult> bool_tree_impl(
    const QueryNode& root,
    std::size_t k,
    const LiveChecker& live_checker,
    const Bm25Params& params,
    std::uint64_t N,
    std::uint64_t sum_dl,
    const std::function<bool(std::string_view, FlatPostings&)>& fetch,
    const std::function<std::vector<SearchResult>(
        const std::vector<std::string>&)>& phrase_fn) {
    // term 叶 → live ord 升序集（posting ords 本就 ord 升序）。
    auto term_ords = [&](const std::string& term) {
        std::vector<std::uint64_t> out;
        FlatPostings fp;
        if (!fetch(term, fp)) return out;
        std::vector<char> live(fp.size());
        live_checker.fill_is_live(fp.ords, live);
        out.reserve(fp.size());
        for (std::size_t i = 0; i < fp.size(); ++i) {
            if (live[i]) out.push_back(fp.ords[i]);
        }
        return out;
    };
    // 短语叶 → 匹配 ord 升序集（复用 search_phrase 内核取全部命中）。
    auto phrase_ords = [&](const std::vector<std::string>& terms) {
        std::vector<std::uint64_t> out;
        if (terms.empty()) return out;
        auto hits = phrase_fn(terms);
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

    const double avgdl =
        N > 0 ? static_cast<double>(sum_dl) / static_cast<double>(N) : 1.0;

    // 候选平行分数数组 + 每词双指针归并（同扁平 bool_search 的评分形态）。
    std::vector<float> scores(candidates.size(), 0.0F);
    FlatPostings fp_buf;
    std::vector<char> live_buf;
    std::vector<std::uint32_t> dls_buf;
    for (const auto& st : sterms) {
        if (!fetch(st.term, fp_buf)) continue;
        const auto& ords_buf = fp_buf.ords;
        const auto& tfs_buf = fp_buf.tfs;
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


}  // namespace bitcask::bm25::detail
