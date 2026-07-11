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

}  // namespace bitcask::bm25::detail
