// === 算法参考文献 ===
// BM25 排序函数：Robertson & Sparck Jones 1976, "Relevance weighting of search terms".
//   IDF（Lucene 标准公式）：log(1 + (N - df + 0.5) / (df + 0.5))
//   TF 归一化：tf * (k1+1) / (tf + k1 * (1 - b + b * dl/avgdl))
//
// BM25+ 扩展（δ 参数）：Lv & Zhai 2011, "Lower-bounding term frequencies".
//   在 TF 归一化项上加 δ = 1.0，缓解标准 BM25 对长文档的过度惩罚。
//
// Block-Max WAND： Ding & Suel 2011, "Faster Top-k Document Retrieval Using Block-Max Indexes".
//   在每个 posting 块维护 max_tf，用上界剪枝跳过无望文档。
//
// Document-at-a-time（DAAT）评分：标准 IR 评估模型，对每个文档累加所有查询词的 BM25 分。
//
// === BM25 倒排索引（内存工作副本）。

// InvertedIndex 维护 term → PostingList[(ord, tf)] 的内存映射，
// 以及 BM25 所需的全局统计（N / sum_doc_len / avgdl）。
//
// === 数据流 ===
//   写入：analyzer 切词 → add_doc(ord, term_freqs) → 每个 term 追加 posting
//   删除：remove_doc(ord, term_freqs) → posting 标记删除（V2 靠 live 过滤）
//   查询：search(terms, k, live_checker) → DAAT 累加 BM25 → top-k 堆
//
// === 锁模型（§4） ===
//   写入按 term hash 分片，tbb::concurrent_hash_map 提供桶级锁。
//   查询（search）无锁读——concurrent_hash_map 支持并发迭代。
//   全局统计（live_doc_count_ / sum_doc_len_）用 atomic（S10.1，去锁）。
//
// === df 漂移 ===
//   V2 查询时过滤 live=0 的 ord，接受 df 轻微偏大。merge 时重算 df。

#pragma once

#include <oneapi/tbb/concurrent_hash_map.h>

#include "bitcask/index_ids.hpp"  // S27-1：Lsn/DocId 角色别名

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bitcask/bm25_params.hpp"   // S20-4：Bm25Params 抽出的轻量头
#include "bitcask/fuzzy_matcher.hpp"
#include "bitcask/term_index.hpp"    // S30-P1 Slice 4：段查询接口
#include "bitcask/live_checker.hpp"
#include "bitcask/query.hpp"
#include "bitcask/vbyte.hpp"

namespace bitcask::bm25 {

// Posting 分块元数据（Block-Max WAND 跳跃索引）。
struct PostingBlock {
    std::uint64_t base_ord;
    std::uint64_t end_ord;
    std::uint32_t max_tf;
    // v5 impacts:块内最小 doc_len(索引时值;文档 dl 不可变)。
    // 分数上界用 min_dl 替代 dl=1 假设,消除 ~25%/词 的固有松弛
    // (B1 实测剪枝不触发的根因,doc/kway-blockmax-bmw-zh.md §6.1)。
    // 1 = 旧快照/dl 未知时的 admissible 回退(等价旧行为)。
    std::uint32_t min_dl = 1;
    std::size_t   start_idx;
    std::size_t   count;
};

// S22-M6：posting 行已 SoA 化（见 PostingList）——原 `Posting{ord,tf,dl,
// positions}` AoS 结构 40B/条（含 24B positions vector 头）+ 每条独立堆块，
// 改平行数组后 16B/条（无位置）/ 24B+紧凑数据（有位置）。

struct FlatPostings;  // 前向声明（定义在 PostingList 之后）

// 一个 term 对应的 posting 列表，按 ord 严格升序排列、同一 ord 不重复。
// 该不变量由 InvertedIndex::add_doc 的水位幂等保护（max_indexed_ord_）维持，
// 是 find 二分 / note_appended 封块 / intersect_u64 求交的共同前提。
//
// S22-M6 SoA 布局：下标 i 对应原 items[i]，ords/tfs/dls 平行；positions
// 扁平化为 pos_data + pos_off（第 i 条的位置 = pos_data[pos_off[i],
// pos_off[i+1])，哨兵尾）。pos_off **惰性物化**：首个非空 positions 追加前
// 恒 empty（index_positions=false 的库 16B/条零 positions 开销），物化后
// size == 条数+1。落盘 v6 格式本就列式（ord 列/tf 列/dl 列分开编码），
// SoA 是其天然内存镜像，字节零变化。
struct PostingList {
    static constexpr std::size_t kBlockSize = 128;

    std::vector<std::uint64_t> ords;  // 严格升序无重复
    std::vector<std::uint32_t> tfs;
    // 索引时 doc_len(v5 impacts)。0 = 未知(旧快照载入)——封块求 min 时
    // 跳过,全 0 回退 min_dl=1。
    std::vector<std::uint32_t> dls;
    // positions 扁平存储（pos_off 用 u64：2^24 posting × 2^20 pos 理论
    // 总量可超 u32；8B/条仍远小于原 24B vector 头 + 独立堆块）。
    std::vector<std::uint32_t> pos_data;
    std::vector<std::uint64_t> pos_off;

    // Block-Max WAND 跳跃索引（增量 seal_full_blocks + finalize 补尾块）。
    // 注：O3 后 ords[] 恒为 ord 的唯一事实来源；VByte 压缩只在落盘格式
    // 里现场编码（save），内存不再常驻压缩副本。start_idx/count 是行下标
    // 区间，对平行数组同样成立。
    std::vector<PostingBlock> blocks;

    // 全局最大 tf 缓存（S10.9）：block_upper_bound 此前每次重扫全表求最大 tf；
    // 改为增量维护（note_appended 追加时更新，load 后重算），查询直接读。
    std::uint32_t max_tf = 0;

    [[nodiscard]] std::size_t size() const noexcept { return ords.size(); }
    [[nodiscard]] bool empty() const noexcept { return ords.empty(); }

    // 第 i 条 posting 的位置区间（未物化/无位置 → 空 span）。CoW 冻结语义
    // （读者持 shared_ptr<const PostingList>）保证 span 生命周期安全。
    [[nodiscard]] std::span<const std::uint32_t>
    positions(std::size_t i) const noexcept {
        if (pos_off.empty()) return {};
        return {pos_data.data() + pos_off[i],
                static_cast<std::size_t>(pos_off[i + 1] - pos_off[i])};
    }

    // 唯一追加入口（收敛 add_doc/apply_delta，防平行数组漏列错位）。
    // caller 随后照旧调 note_appended()。
    void append(std::uint64_t ord, std::uint32_t tf, std::uint32_t dl,
                std::span<const std::uint32_t> pos) {
        const std::size_t idx = ords.size();
        ords.push_back(ord);
        tfs.push_back(tf);
        dls.push_back(dl);
        // 惰性物化：首个非空 positions 才建 pos_off（此前各条起点全 0）。
        if (!pos.empty() && pos_off.empty()) pos_off.assign(idx + 1, 0);
        if (!pos_off.empty()) {
            pos_data.insert(pos_data.end(), pos.begin(), pos.end());
            pos_off.push_back(pos_data.size());
        }
    }

    // 计算块元数据（含部分尾块）。幂等：重复调用重算同一结果。
    void finalize() {
        if (ords.empty()) return;

        // 计算 Block-Max WAND 元数据。S10.6：先 clear——增量封块（seal_full_blocks）
        // 可能已建若干满块，这里重建为含「部分尾块」的规范集（覆盖之），避免重复追加。
        blocks.clear();
        if (ords.size() >= kBlockSize) {
            std::size_t n = ords.size();
            std::size_t block_count = (n + kBlockSize - 1) / kBlockSize;
            blocks.reserve(block_count);
            for (std::size_t b = 0; b < block_count; ++b) {
                std::size_t start = b * kBlockSize;
                std::size_t end = std::min(start + kBlockSize, n);
                std::uint64_t base = ords[start];
                std::uint64_t last = ords[end - 1];
                std::uint32_t blk_max_tf = 0;
                std::uint32_t min_dl = 0xFFFFFFFF;
                for (std::size_t i = start; i < end; ++i) {
                    if (tfs[i] > blk_max_tf) blk_max_tf = tfs[i];
                    if (dls[i] > 0 && dls[i] < min_dl) {
                        min_dl = dls[i];
                    }
                }
                if (min_dl == 0xFFFFFFFF) min_dl = 1;  // dl 全未知 → 回退
                blocks.push_back({base, last, blk_max_tf, min_dl, start, end - start});
            }
        }
    }

    // 增量封块（S10.6）：把已攒满 kBlockSize 的整块封进 blocks，尾部不足一块不封。
    // ord 单调递增（alloc_ord 全局递增）→ 新 posting 必落在末尾，O(1) 摊还。
    // 不变量：增量阶段 blocks 仅含满块（count==kBlockSize）；部分尾块只由 finalize 产生。
    void seal_full_blocks() {
        std::size_t sealed = blocks.size() * kBlockSize;
        while (ords.size() - sealed >= kBlockSize) {
            std::size_t start = sealed;
            std::size_t end = start + kBlockSize;
            std::uint32_t blk_max_tf = 0;
            std::uint32_t min_dl = 0xFFFFFFFF;
            for (std::size_t i = start; i < end; ++i) {
                if (tfs[i] > blk_max_tf) blk_max_tf = tfs[i];
                if (dls[i] > 0 && dls[i] < min_dl) {
                    min_dl = dls[i];
                }
            }
            if (min_dl == 0xFFFFFFFF) min_dl = 1;
            blocks.push_back({ords[start], ords[end - 1], blk_max_tf,
                              min_dl, start, kBlockSize});
            sealed += kBlockSize;
        }
    }

    // add_doc 追加一条 posting 后调用（S10.6）：让在线索引也具备 WAND 块跳跃。
    void note_appended() {
        // S10.9：增量维护全局 max_tf（新 posting 必在末尾）。
        if (!tfs.empty() && tfs.back() > max_tf) max_tf = tfs.back();
        // finalize 可能留下不满的尾块；增量封块要求 blocks 仅含满块，先弹掉它。
        if (!blocks.empty() && blocks.back().count < kBlockSize) {
            blocks.pop_back();
        }
        seal_full_blocks();
    }

    // 死点压实（S10.11）：删除 live 标志为 0 的 posting，重建派生态。
    // 行原本按 ord 升序，过滤保序 → 压实后仍有序。返回是否实际删了。
    // 分数无关：live_df/idf/avgdl 都只数 live，压实只是不再扫死点。
    // P2.4：flags 版本——live 与行按下标对齐（批量 fill_is_live 产物，
    // 免每 posting 一次带锁虚调用）。
    // S22-M6：SoA 原地双指针压实；positions 显式搬移 + pos_off 重写（每轮
    // 迭代先读 pos_off[i]/[i+1] 再写 pos_off[w]，w ≤ i 保证读写不冲突）。
    bool compact_flags(std::span<const char> live) {
        const std::size_t n = ords.size();
        const bool have_pos = !pos_off.empty();
        std::size_t w = 0;
        std::uint64_t wpos = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (!live[i]) continue;
            if (have_pos) {
                const std::uint64_t b = pos_off[i], e = pos_off[i + 1];
                if (wpos != b) {
                    std::copy(pos_data.begin() + static_cast<std::ptrdiff_t>(b),
                              pos_data.begin() + static_cast<std::ptrdiff_t>(e),
                              pos_data.begin() + static_cast<std::ptrdiff_t>(wpos));
                }
                pos_off[w] = wpos;
                wpos += e - b;
            }
            if (w != i) {
                ords[w] = ords[i];
                tfs[w]  = tfs[i];
                dls[w]  = dls[i];
            }
            ++w;
        }
        if (w == n) return false;  // 无死点，不动
        ords.resize(w);
        tfs.resize(w);
        dls.resize(w);
        if (have_pos) {
            pos_off[w] = wpos;  // 哨兵
            pos_off.resize(w + 1);
            pos_data.resize(static_cast<std::size_t>(wpos));
        }
        // 重建派生态（blocks/max_tf）。
        blocks.clear();
        max_tf = 0;
        for (auto tf : tfs) {
            if (tf > max_tf) max_tf = tf;
        }
        seal_full_blocks();  // 仅封满块（与增量一致，尾部留给后续 finalize）
        return true;
    }

    // 按 ord 查找（二分，用于 add_doc 去重 / remove_doc 定位）。
    [[nodiscard]] auto find(std::uint64_t ord) const -> std::size_t;
    [[nodiscard]] bool has(std::uint64_t ord) const;

    // 返回包含指定 ord 的块（binary search）。
    [[nodiscard]] auto block_for_ord(std::uint64_t ord) const -> const PostingBlock*;

    // 计算该 posting list 的全局上界分数（用于 WAND剪枝）。
    [[nodiscard]] auto block_upper_bound(float idf, const Bm25Params& params, double avgdl) const -> float;

    // P1：在 caller 持桶锁（accessor）期间拷出查询评分所需的扁平快照。
    // 只拷 (ord, tf) 双数组 + WAND 元数据——positions 评分用不到，不拷。
    // 相比整列表深拷贝：分配 N+1 次 → 2 次，拷贝 ~40B+positions 堆块 → 12B/posting。
    void snapshot_flat(FlatPostings& out) const;
};

// P1：查询路径的 PostingList 扁平快照（见 doc/posting-zero-copy-design-zh.md）。
// 6 条查询路径中 5 条只需要 (ord, tf)（search/wand/bool/fuzzy/wildcard），
// 由本结构承载；phrase/near 需要 positions，仍走 PostingList 深拷贝。
struct FlatPostings {
    std::vector<std::uint64_t> ords;    // 与 tfs 平行，按 ord 升序
    std::vector<std::uint32_t> tfs;
    std::vector<PostingBlock>  blocks;  // WAND 跳跃索引（量 = N/128，浅拷）
    std::uint32_t              max_tf = 0;

    [[nodiscard]] std::size_t size() const noexcept { return ords.size(); }
    [[nodiscard]] bool empty() const noexcept { return ords.empty(); }

    // 与 PostingList 同名方法语义一致（共享实现，见 inverted.cpp）。
    [[nodiscard]] auto block_for_ord(std::uint64_t ord) const -> const PostingBlock*;
    [[nodiscard]] auto block_upper_bound(float idf, const Bm25Params& params, double avgdl) const -> float;
};

using TermPositions = std::unordered_map<std::string, std::pair<std::uint32_t, std::vector<std::uint32_t>>>;

// 搜索结果条目。
struct SearchResult {
    Lsn           ord;   // S27-1：段内命中当前 == DocId==LSN
    float         score;
};

// S27-2：G-on-the-fly 外部 collection 统计注入（分段查询用）。
// nullptr（search 默认）= 用本索引本地统计——**现行为，零变更**。
// 分段查询时宿主先跨段聚合出全局 N/sum_dl + per-term 全局 df，令每段用**同一
// idf/avgdl** 打分（对标 ES 段级，见 doc/segment-index-design-zh.md §4）。
// df 用「跨段 doc_freq 求和」预建好的列表；某 term 缺失 → 回退本段本地 live_df。
// S29-5：df 从 unordered_map 改扁平 pair 列表——查询词个位数，消费侧线性
// 扫描优于 hash find，生产侧免每查询 map 节点分配（可 thread_local 槽位
// 复用）。契约：term 无重复（重复时取首个匹配）。
struct ExtStats {
    std::uint64_t N      = 0;   // 全局文档数
    std::uint64_t sum_dl = 0;   // 全局 Σdoc_len（→ 全局 avgdl）
    const std::vector<std::pair<std::string, std::uint64_t>>* df = nullptr;  // term→全局 df
};

// BM25 评分解释的单 term 分项（S8.8）。
struct TermScore {
    std::string   term;
    std::size_t   df        = 0;   // live document frequency
    double        idf       = 0.0; // log(1 + (N - df + 0.5)/(df + 0.5))
    std::uint32_t tf        = 0;   // 该 term 在目标文档中的词频（不在文档则 0）
    float         tf_norm   = 0.0F;// tf 长度归一化项
    float         contribution = 0.0F; // idf * tf_norm，该 term 对总分的贡献
};

// explain() 的返回：各 term 分项 + 总分。
struct ScoreExplanation {
    std::vector<TermScore> terms;
    float                  total = 0.0F;
};

// live 文档检查器接口（由 Index 侧表提供）。
// search() 调用它跳过已删除的 ord。
//
// 倒排索引。S30-P1 Slice 4:实现 TermIndex(段查询接口,SegmentView 经
// `const TermIndex*` 同时指内存段与 mmap 段;查询面方法即接口 override,
// 默认实参与接口逐一相同——契约见 term_index.hpp)。
class InvertedIndex : public TermIndex {
public:
    InvertedIndex() = default;
    ~InvertedIndex() override;
    // index_positions=false 时不存 positions（S10.10，省内存，短语/近邻失效）。
    explicit InvertedIndex(Bm25Params params, bool index_positions = true);

    [[nodiscard]] bool index_positions() const { return index_positions_; }
    // A4-P2:已索引最大 ord 水位(u64(-1)=尚无文档)。快照成对性门用。
    // S27-1：这是 **LSN 幂等水位**（拒绝重放旧序）；posting 存的是 DocId，
    // 当前 docid==lsn 故用同一值比较，分段化后水位归 Lsn、posting 键归 DocId。
    [[nodiscard]] Lsn max_indexed_ord() const {
        return max_indexed_ord_.load(std::memory_order_relaxed);
    }

    // ---- 写 ----

    // 添加一篇文档的 posting。term_freqs 来自 analyzer。
    // 线程安全：按 term hash 分片锁。S27-1：posting 存 DocId（分段化后段内本地）。
    void add_doc(DocId docid, const TermPositions& term_data);

    // 删除一篇文档的 posting。V2 实际不删除 posting 行（靠 live 过滤），
    // 但减少 live_doc_count_ / sum_doc_len_ 以保持统计准确。
    void remove_doc(std::uint32_t doc_len,
                    const std::unordered_map<std::string, std::uint32_t>& term_freqs);

    // ---- 查询 ----

    // BM25 搜索：对 query terms 做 DAAT 累加，返回 top-k 结果。
    // live_checker 用于跳过已删文档并获取 doc_len。
    // 线程安全：持所有分片 shared_lock。
    // params_override 非空时覆盖默认 Bm25Params（查询期 k1/b 调参，S8.5）；
    // 为空则用构造时的 params_。WAND 上界估算也用同一组参数，保证剪枝正确。
    // S27-2：某 term 的 doc frequency（= posting list 长度，**含未 merge 的已删**，
    // Lucene-style df；§4 接受该近似，merge 自愈）。宿主用它跨段求和得全局 df。
    // 不存在 → 0。线程安全：term 分片 shared_lock。
    [[nodiscard]] std::uint64_t doc_freq(std::string_view term) const override;

    // ext 非空时走 G-on-the-fly：用 ext->N/sum_dl 定 avgdl、ext->df 定 idf 的 df
    // （回退本地 live_df）；nullptr = 本地统计（现行为）。见 ExtStats。
    [[nodiscard]] auto search(
        const std::vector<std::string>& query_terms,
        std::size_t k,
        const LiveChecker& live_checker,
        const Bm25Params* params_override = nullptr,
        const ExtStats* ext = nullptr) const -> std::vector<SearchResult> override;

    [[nodiscard]] auto search_phrase(
        const std::vector<std::string>& query_terms,
        std::size_t k,
        const LiveChecker& live_checker,
        const Bm25Params* params_override = nullptr) const
        -> std::vector<SearchResult> override;

    // 近邻搜索（S8.7）：term 按查询顺序出现，相邻 term 间隙 ≤ slop。
    // slop=0 等价于 search_phrase（严格相邻）。复用 positions。
    [[nodiscard]] auto search_near(
        const std::vector<std::string>& query_terms,
        std::size_t k,
        std::uint32_t slop,
        const LiveChecker& live_checker,
        const Bm25Params* params_override = nullptr) const
        -> std::vector<SearchResult> override;

    [[nodiscard]] auto bool_search(
        const QueryNode& query,
        std::size_t k,
        const LiveChecker& live_checker,
        const Bm25Params* params_override = nullptr) const
        -> std::vector<SearchResult> override;

    // S13-D9：树形布尔求值（括号嵌套 + 引号短语）。语义：
    //   组内 MUST 子项交集为基集（无 MUST 则 SHOULD 并集），MUST_NOT 差集；
    //   SHOULD 在有 MUST 时只参与打分不扩候选（与扁平 bool_search 一致）。
    //   短语叶子按 phrase_terms 的 positions 匹配（需 index_positions）。
    // 评分：候选按全部正向词（term 叶 + 正向短语成分词）的 BM25 贡献求和
    //   （term 叶乘 boost），top-k。集合式求值 O(Σ posting)——无新语法的
    //   查询由 TextPlugin 路由到扁平 bool_search，性能不受影响。
    [[nodiscard]] auto bool_search_tree(
        const QueryNode& root,
        std::size_t k,
        const LiveChecker& live_checker,
        const Bm25Params* params_override = nullptr) const
        -> std::vector<SearchResult> override;

    [[nodiscard]] auto search_fuzzy(
        const std::vector<std::string>& query_terms,
        std::size_t k,
        std::uint32_t max_edit_distance,
        const LiveChecker& live_checker,
        const Bm25Params* params_override = nullptr) const
        -> std::vector<SearchResult> override;

    [[nodiscard]] auto search_wildcard(
        const std::string& pattern,
        std::size_t k,
        const LiveChecker& live_checker,
        const Bm25Params* params_override = nullptr) const
        -> std::vector<SearchResult> override;

    // 解释 query_terms 对文档 ord 的 BM25 评分（S8.8，调试/调优用）。
    // 用与 search() 完全相同的 idf/tf_norm 公式，逐 term 给出分项。
    // 与 search 一致：参数可被 params_override 覆盖。
    [[nodiscard]] auto explain(
        const std::vector<std::string>& query_terms,
        std::uint64_t ord,
        const LiveChecker& live_checker,
        const Bm25Params* params_override = nullptr) const
        -> ScoreExplanation override;

    auto save(std::string_view path) const -> bool;
    auto load(std::string_view path) -> bool;
    // P14e:序列化到字节缓冲(供 search.ckpt 分段嵌入)。盘字节与 save() 一致
    // (自带 INV 框架、原生小端)。serialize 仅追加缓冲、不会失败。
    void serialize(std::vector<std::byte>& out) const;

    // S30-P1:导出遍历——按 term 字节序升序逐个访问 posting list(v2 段
    // writer 的词典/posting 流式导出原语;S27-4 era 挂账的「词表遍历原语」)。
    // 并发安全同 serialize(key 快照 + 逐 key const_accessor);设计上在
    // 静止(封口)索引上调用。
    void visit_postings_sorted(
        const std::function<void(std::string_view term, const PostingList& pl)>&
            fn) const;

    // S30-P3:单 term posting 深拷(含 positions/dls;段合并的 v1 输入按需
    // 取数)。不存在返回 false。线程安全:const_accessor 读锁下整体拷贝。
    [[nodiscard]] bool snapshot_postings(std::string_view term,
                                         PostingList& out) const;

    // S14-4：增量（delta）序列化——只导出 ord ≥ from_ord 的 posting 后缀
    // （items 按 ord 升序不变量 → 后缀连续，见 PostingList 头注释）+ 绝对
    // 全局统计（N/sdl：删除只改统计不碰 posting，随 delta 整体覆盖）。
    // 与 serialize 相同的并发安全遍历（collect_term_keys + const_accessor）。
    void serialize_delta(std::vector<std::byte>& out,
                         std::uint64_t from_ord) const;
    // 应用 delta：对每 term 尾部追加（per-item「ord > 列尾」守卫幂等拒绝
    // 陈旧/重叠条目），note_appended 增量封块；新 term 走 add_doc 同款
    // vocab_delta_ 记账；全局统计取 delta 内绝对值。解析失败（截断/魔数
    // 不符）返回 false，调用方视作坏 delta 终止链。
    //
    // 【调用契约，承重】必须经 walk_chain 按 from_ord 单调、链接校验
    // （prev_wm==coverage / seq 连续 / base_gen 匹配）的顺序喂入。守卫只保证
    // PostingList 不变量与幂等，**不保证完整性**：乱序/跳段应用会让被跳过的
    // ord 被静默丢弃且无报错。切勿绕过 walk_chain 直接调用。完整性只能由
    // walk_chain 在调用方保证——本层无法本地断言（from_ord 是全局链 coverage，
    // 与本字段 posting 水位天然发散，详见 inverted.cpp 中 apply_delta 注释）。
    [[nodiscard]] bool apply_delta(std::span<const std::byte> bytes);
    // 从字节缓冲反序列化(search.ckpt 段)。语义同 load:任何越界/校验违例
    // 整体拒绝返回 false。
    [[nodiscard]] auto deserialize(std::span<const std::byte> bytes) -> bool;

    // ---- 统计 ----
    // S31:v1 载入时跳过的超长 term 数(0 = 干净;>0 提示历史坏库,建议
    // 重建索引以物理清除盘上超长 term)。
    [[nodiscard]] std::uint64_t load_skipped_oversized_terms() const {
        return load_skipped_oversized_terms_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto live_doc_count() const -> std::uint64_t override;
    [[nodiscard]] auto sum_doc_len() const -> std::uint64_t override;
    [[nodiscard]] auto avg_doc_len() const -> double;
    // 所有 posting list 的 items 总数（含尚未压实的死点）。内省/测试用：
    // 观测 compaction 效果与 posting 膨胀。非并发安全遍历——须在静止时调用。
    [[nodiscard]] std::size_t total_postings() const;

    // 调试：返回 term 的 df（posting list 长度，含死点）。
    [[nodiscard]] auto df(std::string_view term) const -> std::size_t;
    [[nodiscard]] auto df_live(std::string_view term, const LiveChecker& live_checker) const -> std::size_t;

    // 压缩所有 posting list 的 ord 为 VByte gap 编码。
    void finalize_all_postings();

    // 死点压实（S10.11）：对死点占比 ≥ dead_ratio_threshold 的 posting list，
    // 用 live_checker 重建只留 live ord。高 churn 下死 ord 长期累积、每查询都扫，
    // 此操作回收之。非查询热路径（持每 key 写锁，与查询互斥）；分数无关。
    // 返回被压实的 posting list 数。
    auto compact(const LiveChecker& live_checker, double dead_ratio_threshold = 0.5)
        -> std::size_t;

    // 内部分片结构（公开用于测试）。
    // P2-min：map 值为 shared_ptr<PostingList>（CoW 发布，见 inverted.cpp
    // mutable_pl）。phrase/near 读者持引用零拷贝读；写者 use_count==1 时
    // 原地改（常态），>1（有 phrase 读者在持）才克隆替换。
    using PostingMap = tbb::concurrent_hash_map<std::string, std::shared_ptr<PostingList>>;
    struct Shard {
        PostingMap inverted;

        // V6.3.1：排序词典侧表——替代每次查询时的 hash_map 全扫 + sort。
        // vocab_dirty_ 由 add_doc 置 true（release）；首次搜索检测到 dirty 时
        // 在 vocab_mtx_ 写锁下重建 vocab_、清 dirty。
        // 非脏路径：shared_lock 读 vocab_ → 无重建开销。
        // shard.inverted 与 vocab_ 的不一致窗口由 vocab_dirty_ 兜住：
        //   写者 add_doc 后 release-store true；
        //   读者 ensure_vocab 入口 acquire-load，true 才付写锁重建。
        //
        // S13-F6：重建不再遍历 inverted（tbb::concurrent_hash_map 的遍历与
        // 并发插入不兼容，rehash 可致迭代器失效——而 ensure_vocab 跑在查询
        // 线程、与 reducer 的 add_doc 插入并发）。改为增量：add_doc 插入新
        // term 时在 vocab_mtx_ 下 push 进 vocab_delta_，重建 = vocab_ ∪ delta
        // （term 一经插入永不从 map 删除——compact 有意保留空 posting list，
        // 该不变量是本设计的前提）。
        mutable std::shared_mutex vocab_mtx_;
        mutable std::shared_ptr<const std::vector<std::string>> vocab_;
        // S24-M9：排序增量层（base 之上、raw delta 之下）——重建时只重排
        // 这一层，base 不再逐串深拷。发布协议同 vocab_（shared_ptr 换指针）。
        mutable std::shared_ptr<const std::vector<std::string>> vocab_extra_;
        mutable std::vector<std::string> vocab_delta_;  // 由 vocab_mtx_ 保护
        mutable std::atomic<bool> vocab_dirty_{true};

        // S29-6B:分片 posting 世代——写者对本 shard 任一 posting list 可见
        // 变更(add_doc/apply_delta/compact/finalize/deserialize)完成后
        // fetch_add(release);查询线程 acquire load 后用于 TermSnapshotCache
        // 命中判据(entry.gen == gen_)。remove_doc 不 bump(只改全局统计,
        // 不碰 posting;删除由 live_checker 查询期过滤,与缓存正交)。
        // 独占 cacheline:写者 bump 不得连带失效读者正在读的邻接字段。
        alignas(64) std::atomic<std::uint64_t> gen_{0};
    };

    // 获取内部 shard（用于测试）。
    [[nodiscard]] auto shard_for(std::string_view term) -> Shard&;
    [[nodiscard]] auto shard_for(std::string_view term) const -> const Shard&;

    // S29-6B:本索引实例 id(进程级单调分配,永不复用)——TermSnapshotCache
    // 的 key 成分,保证指向已析构索引的残留缓存条目永不假命中。
    [[nodiscard]] std::uint64_t index_id() const noexcept { return index_id_; }

    // S29-6B:查询快照缓存运行期开关(进程级,默认开)。关闭 ⇒ probe 恒
    // miss、不产生条目 ⇒ 字节级回到无缓存路径(线上出问题免重编译回退)。
    static void set_query_cache_enabled(bool on) noexcept {
        query_cache_enabled_.store(on, std::memory_order_relaxed);
    }
    [[nodiscard]] static bool query_cache_enabled() noexcept {
        return query_cache_enabled_.load(std::memory_order_relaxed);
    }

    // S29-6B/S30-P5:TermIndex 实例 id 分配(进程级单调,永不复用)——
    // TermSnapshotCache 的 key 成分。InvertedIndex 与 MmapSegment 字段
    // **必须共用同一序列**(各自计数会撞 id → 跨索引缓存串味)。
    [[nodiscard]] static std::uint64_t next_index_id() noexcept {
        static std::atomic<std::uint64_t> counter{1};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t kShardCount = 64;
    // S30-P1:BOW/WAND 路由阈值移至 bm25_search_impl.hpp
    // (detail::kWandRouteThreshold)——MmapSegment 路由必须与本类一致。
    // S30-P1:kPhraseParallelThreshold 移至 bm25_search_impl.hpp(detail::)。

    inline static std::atomic<bool> query_cache_enabled_{true};

    std::array<Shard, kShardCount> shards_;
    const std::uint64_t index_id_ = next_index_id();
    Bm25Params params_;
    bool index_positions_ = true;  // S10.10：false 时 add_doc 丢弃 positions

    // 全局统计（S10.1）：改用 atomic 去掉 stats_mutex_。
    // 此前 search()/explain()/wand 等查询路径裸读这两个字段而写路径持锁，
    // 并发查询+写=data race（UB）。atomic 既消 race 又免锁（写路径 V2 串行，
    // remove_doc 的 guard 用 load+fetch_sub 即可）。
    std::atomic<std::uint64_t> live_doc_count_{0};
    std::atomic<std::uint64_t> sum_doc_len_{0};

    // S31:v1 载入时因超长(>1024B)被跳过的 term 数(容错可见性;
    // 详见 inverted.cpp deserialize 注)。
    std::atomic<std::uint64_t> load_skipped_oversized_terms_{0};

    // 已索引文档的最大 ord 水位（add_doc 幂等保护）。ord 由引擎单调分配、
    // add_doc 调用序保持单调（IndexPool 单消费者 + 恢复按 ord 序回放），故
    // 正常追加恒满足 ord > 水位。崩溃恢复时 save/truncate_wal 非原子窗口会让
    // load 后的 replay_wal 重放已在快照里的 (ord, term)；用水位把 ord ≤ 水位的
    // 重放整文档丢弃，保证 PostingList::items 严格升序无重复（intersect_u64 /
    // find 二分 / note_appended 封块都依赖该不变量）。-1 = 尚未索引任何文档。
    // atomic:worker 线程写,搜索线程经 max_indexed_ord() 读,跨线程访问。
    std::atomic<std::uint64_t> max_indexed_ord_{static_cast<std::uint64_t>(-1)};

    // Block-Max WAND 算法。S27-2：ext 非空走 G-on-the-fly（同 search）。
    // S29-1：pls 与 query_terms 按下标平行（未命中词为 nullptr），由 search()
    // 单趟 find 预取——本函数不再触碰桶锁。
    auto search_wand(
        const std::vector<std::string>& query_terms,
        const std::vector<std::shared_ptr<const PostingList>>& pls,
        std::size_t k,
        const LiveChecker& live_checker,
        const Bm25Params& params,
        const ExtStats* ext = nullptr) const -> std::vector<SearchResult>;

    // search_phrase / search_near 的共同实现（S8.7）：slop=0 为严格短语，
    // slop>0 允许相邻 term 间隙 ≤ slop（有序近邻）。
    auto search_phrase_impl(
        const std::vector<std::string>& query_terms,
        std::size_t k,
        std::uint32_t slop,
        const LiveChecker& live_checker,
        const Bm25Params* params_override) const -> std::vector<SearchResult>;

    // V6.3.1：确保指定 shard 的排序词典可用。脏则重建（写锁），否则直接返回快照（读锁）。
    // S24-M9：两层视图——base（大基线，重建时**不再拷贝**）+ extra（排序
    // 增量层，重建只拷 O(extra+delta)，阈值封顶）。base ∪ extra = key 全集
    // （集合语义；wildcard/fuzzy 消费顺序无关，两层各自独立扫/二分）。
    struct VocabView {
        std::shared_ptr<const std::vector<std::string>> base;   // 可空
        std::shared_ptr<const std::vector<std::string>> extra;  // 可空
    };
    auto ensure_vocab(std::size_t shard_idx) const -> VocabView;

private:
    // S24-M9：增量层并入基线的阈值——extra 超过 max(1024, base/8) 时付一次
    // O(V) 合并（摊还频率 O(V/阈值)）；此前**每次**有新词后的首查询都全量
    // 深拷 vocab_（高写入负载下反复 O(V) string 拷贝）。
    static constexpr std::size_t kVocabExtraMergeFloor = 1024;
};

}  // namespace bitcask::bm25
