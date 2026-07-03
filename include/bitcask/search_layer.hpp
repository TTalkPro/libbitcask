// SearchLayer — Index + InvertedIndex + Analyzer 封装层。
//
// 将 BM25 集成逻辑抽取为可复用组件，供 Phase 4 集成使用。
// SearchLayer 是独立模块，不依赖 Collection/Cask/KeyDir 等上层组件。
//
// === 数据流 ===
//   写入：on_write(key, ord, text, ...) → analyze_with_positions → index_.put_doc + inverted_->add_doc
//   删除：on_delete(key) → index_.get → inverted_->remove_doc → index_.remove
//   查询：search_text(query, k) → analyzer_->analyze → inverted_->search → ord_to_ext → SearchHit
//
// === 约束(线程模型,C1 修订)===
//   - 生产形态:单写者(IndexPool worker 串行消费 on_write/on_delete)
//     + 多读者(查询线程)。曾声明"非线程安全,caller 串行化",与实际
//     使用不符——TSan 全插桩后实测修复了 fields_ map 并发 emplace/find
//     与 DocTextLru 并发 put/get 两处真竞态;现 fields_(shared_mutex)、
//     doc_texts_(内置 mutex)、cache_(shared_mutex)、index_/InvertedIndex
//     (自带锁/分片)在该模型下安全。**多写者仍未支持**。
//   - ord 唯一且单调分配，不复用
//   - analyzer_ 在构造时创建，失败则整个 SearchLayer 创建失败

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "bitcask/analyzer.hpp"
#include "bitcask/highlighter.hpp"
#include "bitcask/hnsw.hpp"
#include "bitcask/index.hpp"
#include "bitcask/index_manifest.hpp"  // S17-2:per-component manifest
#include "bitcask/search_checkpoint.hpp"  // S14-4
#include "bitcask/inverted.hpp"
#include "bitcask/meta_file.hpp"
#include "bitcask/meta_filter.hpp"  // V5：filter 表达树 + MetaOp/MetaCondition
#include "bitcask/search_cache.hpp"
#include "bitcask/string_hash.hpp"  // StringHash（透明 hash，fields_/field_names_intern_ 共用）
#include "bitcask/synonym_map.hpp"

namespace bitcask::search {

// 默认字段名（S8.6）：旧单 text 文档 / 无字段限定查询都映射到此字段，
// 使新旧路径收敛。用不可见控制字符前缀避免与用户字段名冲突。
// 默认字段哨兵。历史：原意为 `\x01` + "default"，但 `\x` 转义会贪婪吞掉后续 hex 位
// （"defa" 全是 hex）→ 实际编译为单字节 0xFA + "ult"（4 字节）。GCC 静默截断、clang
// 直接报「hex escape out of range」。此值已作 fields_ 键 / 可能入 checkpoint，故**保留
// 完全相同的 4 字节**（0xFA 'u' 'l' 't'），仅改写法让 clang 也能编译（相邻字面量断开转义）。
inline constexpr std::string_view kDefaultField = "\xfa" "ult";

// SearchLayer 配置。
struct SearchLayerConfig {
    text::AnalyzerConfig analyzer_config;
    bm25::Bm25Params     bm25_params;
    std::size_t          cache_max_entries = 256;  // 缓存最大条目数，0 禁用
    // 高亮原文 LRU 上限（S9.3）：只缓存最近写入/查询的文档原文，避免全文常驻。
    // 0 表示不缓存（高亮恒拿不到原文 → 降级为无片段），默认 1024 篇。
    std::size_t          doc_text_cache_max = 1024;
    // 是否索引词位置（S10.10）。默认 true。置 false 时倒排不存 positions，
    // 大幅省内存——代价：search_phrase / search_near 失效（无位置可匹配，返回空）。
    // 仅做 search_text/bool/fuzzy/wildcard 的部署可关闭。
    bool                 index_positions = true;
    // V3.3:向量配置(Cask::open 从 meta 透传)。dim>0 时构造 HnswIndex;
    // metric 映射:kCosineNormalized/kDot → HnswMetric::kDot(cosine 已在
    // 写入端归一化),kL2 → kL2。
    std::uint16_t        vector_dim = 0;
    meta::VectorMetric   vector_metric = meta::VectorMetric::kNone;
    // S13-D11：HNSW 建图参数透传（0 = 用 HnswConfig 默认：M=16、
    // ef_construction=200）。高召回部署调大、低内存部署调小。注意：已有
    // checkpoint 的图按建图时参数持久化——改参数影响新插入与 merge 期
    // rebuild 出的图，不追溯改写既有 checkpoint（不入 meta 校验，属调优
    // 参数而非格式参数）。
    std::uint32_t        hnsw_m = 0;
    std::uint32_t        hnsw_ef_construction = 0;
    // P5b:HNSW int8-only 内存模式(Cask::open 从 meta 透传)。仅 kDot。
    bool                 vector_inmem_int8 = false;
    // V6.2:WAL 批量刷新阈值。1 = 即时模式(默认,与旧版行为完全一致)。
    // >1 时积攒 entries 缓冲后单次 fwrite+fflush,减少 sync 调用次数。
    std::size_t          wal_batch_size = 1;
    // S12-2：后台自动 compaction 的 per-list 死占比阈值。
    //   0（默认）  → **关**：行为与旧版完全一致，索引流水线零开销（仅一次 double 比较）；
    //   (0,1]      → **开**：在写入流水线的 reducer 线程内，累计退休文档达节流阈值
    //                （max(1024, live/2)）时对死占比 ≥ 本值的 posting list 触发一次
    //                compact()。与 add_doc/put_doc 同线程串行 → 无并发窗口（见 S12-2）。
    // 效果：posting list 内存随 churn 有界，不再依赖 merge 才回收。代价：触发时 reducer
    // 短暂扫描压实，延迟后续文档的**索引可见性**（非 durability——数据已落 data file）。
    double               auto_compact_dead_ratio = 0.0;
    // 同义词词典（open-time，不可变）。由 Cask::open 从 CaskOptions::synonym_map
    // 透传进来（同 vector_dim 的注入方式）。构造后只读 → 并发查询安全，无需锁。
    // 空 = 不展开同义词。
    std::shared_ptr<const text::SynonymMap> synonym_map;
    // S14-5：delta 链长上限。链达到该长度后下次 save 强制全量 base（链坍缩
    // 回收 delta 文件）——否则不 merge 的纯追加负载（无删除 ⇒ needs_merge
    // 不触发）链随写入量线性堆积、永不回收（向量库尤甚：每个 delta 内联
    // f32 向量）。上限权衡：小 → base 重序列化更频繁（∝ 索引总量）；
    // 大 → 崩溃恢复要重放更长的链 + 磁盘冗余更多。0 = 不设限（不建议）。
    std::uint32_t max_delta_chain = 64;
};

// 搜索结果条目。
struct SearchHit {
    std::string   key;   // 外部 key（由 caller 通过 index_.ord_to_ext 翻译）
    std::uint64_t ord;   // 文档 ord
    double        score; // BM25 分数
};

// S9-P2-d：搜索层错误类型。此前各搜索方法返回 `expected<…, std::string>`，
// 把错误语义压成自由文本，迫使 Cask 边界**静态猜** CaskError 种类（leaky
// abstraction）。改为强类型枚举：搜索层只表达「发生了哪类错误」，由 Cask 边界
// （cask.cpp `search_fault`）翻译成 CaskFault（kind + 人类可读 detail）。
// 全部三种当前都映射到 CaskError::kInvalidOption（见 search_fault）。
enum class SearchError {
    kNoVectorIndex,       // 无向量索引配置（hnsw_ 为空）
    kVectorDimMismatch,   // 查询向量维度与配置不符
    kEmptyHybridQuery,    // hybrid 两路皆空（无文本、无向量）
};

// 带高亮的搜索结果。
struct SearchHitEx {
    std::string              key;
    std::uint64_t            ord;
    double                   score;
    std::vector<Snippet>     highlights;
};

// S6-P0: map_analyze 的产出 / reduce_apply 的输入。
// 把「纯函数 analyze + catch-all 合并」的结果封装为一个 owning 结构，
// 供 reduce_apply 在锁下逐字段 apply。P0 阶段 map/reduce 仍在同线程
// 顺序调用；P2+ 将跨线程传递此结构。
struct ReduceJob {
    std::string          key;           // owning key (reduce_apply 要用)
    std::uint64_t        ord = 0;

    // 每字段的分词结果（field_name 已映射：空名 → kDefaultField）。
    // terms 可能为空（该字段无有效 token）→ reduce_apply 跳过 add_doc。
    struct FieldResult {
        std::string                   field_name;
        text::TermPositionsMap        terms;
        std::uint32_t                 doc_len = 0;  // Σ tf
    };
    std::vector<FieldResult> fields;

    std::uint32_t        total_doc_len = 0;

    // catch-all 合并结果（非默认字段词项合并到默认字段，使 search_text 能命中多字段文档）
    text::TermPositionsMap ca_data;      // 空 = 无需 catch-all add_doc
    std::uint32_t        ca_len = 0;
    bool                 wrote_default = false;  // 有字段直接写了默认字段 → 跳过 catch-all

    // 高亮原文缓存（默认取第一个字段的 text，与现有 on_write_fields line 403 一致）
    std::string          doc_text;

    // DocSlot 定位数据
    std::uint32_t        file_id = 0;
    std::uint64_t        offset = 0;
    std::uint32_t        total_sz = 0;
    std::uint32_t        tstamp = 0;
};

class SearchLayer {
public:
    // 构造 analyzer（可能因无效配置失败）。caller 应检查返回值。
    // S16-1：docmap 尾置注入参——nullptr = 自持（standalone/测试路径，行为
    // 与旧版完全一致）；非空 = 借用宿主（Cask）持有的实例（设计
    // doc/plugin-arch-split-design-zh.md §4：DocMap 是宿主服务，SearchLayer
    // 是消费者）。生命周期：shared_ptr 共持，析构序无关。
    explicit SearchLayer(const SearchLayerConfig& config,
                         std::shared_ptr<index::Index> docmap = nullptr);

    // S10-A1: 测试专用构造函数——注入自定义 analyzer（如计数 wrapper）。
    // 生产代码用上面的构造函数。injected_analyzer 为 nullptr 时退化为默认。
    SearchLayer(const SearchLayerConfig& config,
                std::unique_ptr<text::Analyzer> injected_analyzer);

    // 禁止拷贝（Index 内部含共享状态）。
    SearchLayer(const SearchLayer&) = delete;
    SearchLayer& operator=(const SearchLayer&) = delete;

    // analyzer 是否构造成功。analyzer_config 无效 / 分词器未注册 /
    // 词典加载失败时为 false——caller（Cask::open）必须检查并拒绝打开，
    // 否则首次带 text 的 put 会解空指针段错误。
    [[nodiscard]] bool has_analyzer() const noexcept { return analyzer_ != nullptr; }

    // ---- 文档写入：建立索引 ----
    // key: 外部 key, ord: 文档序号, text: 文档文本,
    // file_id/offset/total_sz: 存储定位, tstamp: 时间戳
    // S16-2：legacy/standalone 入口——自写 docmap 行 + apply_text。
    // 流水线路径**不走本方法**（docmap 行由宿主先落，adapter 调 apply_text）。
    void on_write(std::string_view key, std::uint64_t ord,
                  std::string_view text,
                  std::uint32_t file_id, std::uint64_t offset,
                  std::uint32_t total_sz, std::uint32_t tstamp);

    // S16-2：单文本写入核心（流水线版）。前置条件：docmap 行已就位（宿主
    // put_doc）。分析 text、回填 doc_len（set_doc_len）、默认域倒排、高亮
    // 原文、缓存失效——**不写 docmap 行**。reducer 单写者上下文。
    void apply_text(std::string_view key, std::uint64_t ord,
                    std::string_view text);

    // ---- 多字段写入（S8.6）----
    // fields: (字段名, 文本) 列表，每字段独立分词建索引。空字段名映射到默认字段。
    void on_write_fields(std::string_view key, std::uint64_t ord,
                         const std::vector<std::pair<std::string, std::string>>& fields,
                         std::uint32_t file_id, std::uint64_t offset,
                         std::uint32_t total_sz, std::uint32_t tstamp);

    // S6-P0: 纯函数阶段 — analyze 各字段 + catch-all 合并，产 ReduceJob。
    // 无锁、无共享状态变更（analyzer_ 是 const 线程安全）。
    // S10-A5:fields 改 string_view 借用（IndexTask 打包进 fields_store；同步 caller 借 caller 的 string）。
    // S15-3:参数放宽为 span——插件 adapter 的 DocView::fields（同元素类型）
    // 可直传；既有 vector 调用点隐式转换，零行为变化。
    [[nodiscard]] ReduceJob map_analyze(
        std::string_view key, std::uint64_t ord,
        std::span<const std::pair<std::string_view, std::string_view>> fields,
        std::uint32_t file_id, std::uint64_t offset,
        std::uint32_t total_sz, std::uint32_t tstamp) const;

    // S6-P0: 状态变更阶段 — 把 ReduceJob apply 到索引（add_doc/set_doc_len/
    // on_vector/cache invalidate）。S16-2 写路径反转：**不再写 docmap 行/meta**
    // ——前置条件：docmap 行已就位（流水线=宿主 put_doc+set_meta；standalone/
    // recover=caller put_doc）；本函数只回填 doc_len（分析产物，宿主拿不到）。
    void reduce_apply(const ReduceJob& job, std::span<const float> vec);

    // ---- 文档删除：移除索引 ----
    // key: 要删除的 key
    // tomb_ord: 墓碑 record 的 ord（用于 index_.remove）
    // 返回被删除文档的 ord（用于 caller 跟踪）；key 不存在返回 nullopt。
    std::optional<std::uint64_t> on_delete(std::string_view key, std::uint64_t tomb_ord);

    // S16-2：流水线版删除。前置条件：宿主已捕获 prior_ord 并完成 docmap
    // remove；本方法只做 BM25 统计扣减 / 高亮 LRU / 缓存失效 / delta 记账
    // ——不查不删 docmap。prior_ord 必须有效（宿主对不存在的 key 不广播删
    // 除语义由 adapter 的 kNoPriorOrd 哨兵守卫）。
    void on_delete(std::string_view key, std::uint64_t tomb_ord,
                   std::uint64_t prior_ord);

    // ---- merge 后重定位：ord 不变，只更新存储定位 ----
    void on_relocate(std::string_view key, std::uint64_t ord,
                     std::uint32_t new_file_id, std::uint64_t new_offset,
                     std::uint32_t new_total_sz);

    // ---- 搜索（词袋模式）----
    // params_override 非空时按查询覆盖默认 BM25 k1/b（S8.5）。
    // V5:filter 非空时从倒排 overfetch K'=max(k×4, 64) 再过滤截断到 k;
    // 因为 BM25 评分的得分排序在 filter 之前,需要更多候选弥补过滤损耗。
    [[nodiscard]] std::expected<std::vector<SearchHit>, SearchError>
    search_text(std::string_view query, std::size_t k,
                const bm25::Bm25Params* params_override = nullptr,
                const meta::MetaFilter* filter = nullptr) const;

    // ---- 搜索（短语模式）----
    [[nodiscard]] std::expected<std::vector<SearchHit>, SearchError>
    search_phrase(std::string_view query, std::size_t k,
                  const bm25::Bm25Params* params_override = nullptr) const;

    // ---- 搜索（近邻模式，S8.7）----
    // term 按 query 词序出现且相邻间隙 ≤ slop。slop=0 等价短语。
    [[nodiscard]] std::expected<std::vector<SearchHit>, SearchError>
    search_near(std::string_view query, std::uint32_t slop, std::size_t k,
                const bm25::Bm25Params* params_override = nullptr) const;

    [[nodiscard]] std::expected<std::vector<SearchHit>, SearchError>
    bool_search(std::string_view query, std::size_t k,
                const bm25::Bm25Params* params_override = nullptr) const;

    [[nodiscard]] std::expected<std::vector<SearchHit>, SearchError>
    search_fuzzy(std::string_view query, std::size_t k, std::uint32_t max_edit_distance,
                 const bm25::Bm25Params* params_override = nullptr) const;

    // ---- 多字段搜索（S8.6）----
    // 解析 `field:term^boost` 语法：有字段限定的词查对应字段索引，无限定的词
    // 查默认字段；各词得分 × boost，同一文档跨字段累加；返回 top-k。
    // 不含字段语法时等价于在默认字段做词袋搜索。
    [[nodiscard]] std::expected<std::vector<SearchHit>, SearchError>
    search_fields(std::string_view query, std::size_t k,
                  const bm25::Bm25Params* params_override = nullptr) const;

    [[nodiscard]] std::expected<std::vector<SearchHit>, SearchError>
    search_wildcard(std::string_view pattern, std::size_t k,
                    const bm25::Bm25Params* params_override = nullptr) const;

    // ---- 评分解释（S8.8，调试/调优）----
    // 解释 query 对外部 key 文档的 BM25 评分分项。key 不存在返回 nullopt。
    [[nodiscard]] std::optional<bm25::ScoreExplanation>
    explain(std::string_view query, std::string_view key,
            const bm25::Bm25Params* params_override = nullptr) const;

    // ---- 搜索（带高亮）----
    [[nodiscard]] std::expected<std::vector<SearchHitEx>, SearchError>
    search_text_highlight(std::string_view query, std::size_t k,
                          const HighlightOptions& opts = {}) const;


    // ---- V3.3:向量写入(IndexPool worker 线程,单写者)----
    // hnsw_ 存在且 vec.size()==配置 dim 才 insert;不符直接忽略(防御,
    // 不崩)。水位幂等由 HnswIndex 保证(回放重叠区安全)。
    void on_vector(std::uint64_t ord, std::span<const float> vec);

    // ---- HNSW 大小 + merge 重建 ----
    // 图节点数(含软删死节点;测试/观测用)。无向量配置 = 0。
    [[nodiscard]] std::size_t hnsw_size() const;
    // S13-D8：查询缓存当前条目数（观测用；SearchCache 自带锁，线程安全）。
    [[nodiscard]] std::size_t cache_entries() const { return cache_.size(); }
    // merge 重建(物理清除死节点)。**只能由 IndexPool worker 执行**
    // (与 on_vector 同线程 → 维持 HNSW 单写者约束):新建同 config 图,
    // 遍历旧图节点,跳过 !index_.is_live(ord),重插活节点,完毕原子换
    // 指针。重建期间查询走旧图(含死节点,结果语义不变);换入后旧图由
    // 在途读者的 shared_ptr 引用计数续命。
    void rebuild_hnsw();

    // ---- V3.3:向量查询(线程安全)----
    // cosine 配置时内部归一化查询向量(零向量返回空);ef=0 → max(k,64)。
    // 结果经 index_.is_live 过滤死文档,翻译为 SearchHit{key,ord,score}。
    // V5:filter 非空时与 is_live 组合为 HNSW live callback — 拒节点从
    // 图遍历源头就不入候选集,无需 overfetch。结果可能少于 k(filter
    // 通过率低时),符合「filter 收紧 live」语义。
    [[nodiscard]] std::expected<std::vector<SearchHit>, SearchError>
    search_vector(std::span<const float> query, std::size_t k,
                  std::size_t ef = 0,
                  const meta::MetaFilter* filter = nullptr) const;

    // ---- V3.6:RRF 混合检索(hnsw-design §4)----
    // 两路各取 K' = max(k×4, 64):BM25 词袋走 search_text 内核,向量走
    // search_vector 内核(查询归一化/live 过滤/ord 翻译全部复用)。融合:
    //   score(doc) = Σ_路 1/(60 + rank_路),rank 从 1 起;
    // 单路出现的文档照常只累加该路项,无需分数归一化。**确定性平局序**:
    // RRF 分相等 → ord 小者在前。text_query 空 → 纯向量(BM25 路空);
    // vec_query 空 → 纯文本(RRF 重打分);两路都空 → 错误。vec 维度
    // 不符 → 错误(经 search_vector)。返回 score = RRF 分。
    // V5:filter 同时作用于两条路(text 路 overfetch 后过滤;vec 路
    // 折进 HNSW live callback);只有同时通过两路 filter 的文档进 RRF 融合。
    // 线程安全:同两条内核(text 路同 search_text,vec 路同 search_vector)。
    [[nodiscard]] std::expected<std::vector<SearchHit>, SearchError>
    search_hybrid(std::string_view text_query,
                  std::span<const float> vec_query, std::size_t k,
                  const meta::MetaFilter* filter = nullptr) const;

    // ---- 恢复：从磁盘 record 重放活文档 ----
    // 恢复文档到索引（全量 analyze + add_doc）。
    // V3.3:vector 非空时顺路重建 HNSW(on_vector;水位幂等保证重放安全)。
    void recover_doc(std::string_view key, std::uint64_t ord,
                     std::string_view text,
                     std::uint32_t file_id, std::uint64_t offset,
                     std::uint32_t total_sz, std::uint32_t tstamp,
                     std::span<const float> vector = {});

    // S3:批量恢复。一批文档的 analyze 在 TBB 线程池并行（analyzer 无可变
    // 状态，纯函数线程安全），随后**按 batch 序串行插入**索引/HNSW——插入序
    // == 逐条 recover_doc 的 fold 序，结果与串行完全一致（HNSW 单写者亦保持）。
    // caller 必须在任何 recover_tomb 之前 flush 本批，以保持「文档↔墓碑」相对序。
    struct RecoverDoc {
        std::string        key;
        std::uint64_t      ord = 0;
        std::string        text;
        std::uint32_t      file_id = 0;
        std::uint64_t      offset = 0;
        std::uint32_t      total_sz = 0;
        std::uint32_t      tstamp = 0;
        std::vector<float> vector;  // 空 = 无向量
        // S14-6：命名字段（名字已由 caller 经 field.schema 还原；owning
        // 拷贝——fold 缓冲复用）。非空时镜像活写路径语义：map 只喂 fields、
        // text 不参与索引（与 put_doc 的 task.fields 装配一致），per-field
        // 词表 + catch-all 合并在 map_analyze 内自然复原。空 = 单默认字段。
        std::vector<std::pair<std::string, std::string>> fields;
    };
    void recover_doc_batch(std::vector<RecoverDoc>& batch);

    // ---- 恢复：从磁盘 record 重放墓碑 ----
    void recover_tomb(std::string_view key, std::uint64_t ord);

    // S14-7：delta 链重放钩子——每应用一个 delta 文件，把解析好的 docmap
    // 行/删除 + kKeydirDelta 段原始字节回调给上层（Cask 用它同步推进
    // keydir：行 → put、删除 → remove_if_older、meta → apply_meta_delta）。
    // SearchLayer 不依赖 KeyDir，只透传；keydir_meta 可为空（该 delta 无
    // 元数据段 → 字节水位不推进，方向安全）。
    struct DeltaDocRow {
        std::uint64_t  ord = 0;
        std::string    ext;
        index::DocSlot slot;
    };
    struct DeltaRemoval {
        std::uint64_t tomb = 0;
        std::string   key;
    };
    using DeltaReplayHook = std::function<void(
        const std::vector<DeltaDocRow>&, const std::vector<DeltaRemoval>&,
        std::span<const std::byte> keydir_meta)>;

    // P14e:docmap 序列化到/自字节缓冲(供 search.ckpt 分段)。
    // serialize 返回 false 仅当某 ext 超 64KiB;
    // deserialize 校验失败返回 nullopt,成功返回 covers。
    [[nodiscard]] bool serialize_docmap(std::vector<std::uint8_t>& out,
                                        std::uint64_t covers_next_ord) const;
    [[nodiscard]] std::optional<std::uint64_t>
    deserialize_docmap(std::span<const std::uint8_t> bytes);

    // ---- P14e:统一分段 search.ckpt 持久化 ----
    // save_search_ckpt: 序列化所有索引段（docmap/bm25.default/bm25.fields/
    // hnsw）写入单个 search.ckpt，并做 .prev 代际回退。watermark = 保存时
    // 的 next_ord（覆盖上界）。caller 须先排干 IndexPool（写者静止点）。
    // 返回 false = 序列化或写入失败（best-effort，caller 不阻断）。
    [[nodiscard]] bool save_search_ckpt(
        std::string_view path, std::uint64_t watermark,
        std::span<const std::byte> keydir_delta = {},
        bool* wrote_base = nullptr);

    // load_search_ckpt 结果。
    struct CkptLoadResult {
        bool loaded         = false;  // search.ckpt（或 .prev）结构完整
        std::uint64_t watermark = 0;   // 快照覆盖的 next_ord 上界（S14-4：含 delta 链）
        bool all_segments_ok = false;  // 全段 CRC 通过（段级健康）
        // S14-4：本次载入落到了 .prev 旧代。段级可能完全健康，但磁盘上的
        // keydir 快照可能与坏掉的新代成对（水位超前于 prev 覆盖）——caller
        // （Cask::load_recovery_snapshots）必须据此放弃字节水位快路径，
        // 退全量 fold（自门跳过已载入区）。
        bool from_prev = false;
    };
    // 读 search.ckpt → 逐段校验 CRC → 分发到各反序列化器。
    // 结构损坏 → 尝试 .prev；都失败 → loaded=false（全量 fold 兜底）。
    // 段 CRC 失败 → 该段内存为空（fold 时重建），其余段照常载入。
    // S14-7：hook 见 DeltaReplayHook；空 = 不透传（SearchLayer 单元测试）。
    [[nodiscard]] CkptLoadResult load_search_ckpt(
        std::string_view path, const DeltaReplayHook& hook = {});

    // ---- S17-2:per-component 持久化（P3 checkpoint 拆分）----
    // 把上面 save/load 的单文件契约展开为 3 个组件文件（docmap.ckpt /
    // bm25.ckpt / vec.ckpt）+ 配套 .prev 代际 + 链（.d<seq>），由 Cask 侧
    // 写入 manifest 作为 commit point。SearchLayer 自身不维护 manifest——
    // 这是 P3 设计：SearchLayer 只认组件文件，Cask 维护跨组件的 commit 簿。
    //
    // dirty_mask: 哪些组件脏（需要写）。bit 0=docmap, 1=bm25, 2=vec。
    // 脏位只在 save 时清零（与 S14-3 既有契约一致）。
    struct ComponentSaveResult {
        std::array<bool, kComponentCount> wrote_base{};
        // base 路径下哪个组件没写出有效段（结构/序列化失败或被跳过的
        // ——如 vec 缺配置）。Cask 用此决定 manifest 入口的 chain_watermark
        // 推进与否。
    };
    // Base 写：每个组件内部走「rename old → .prev, 写新 base」。dir 是
    // 组件文件所在目录。watermark 是统一的提交水位。
    [[nodiscard]] ComponentSaveResult save_components_base(
        std::string_view dir, std::uint64_t watermark,
        std::array<bool, kComponentCount> dirty_mask);
    // Delta 写：只写脏组件的 .d<seq>，每个组件独立推进 chain_seq。
    // chain_seqs 初值 = 各自当前 seq（首次写 → 1）；返回每组件写入是否成功
    // ——失败时 caller 回退到 base。
    struct ComponentDeltaResult {
        std::array<bool, kComponentCount> wrote{};
        std::array<std::uint32_t, kComponentCount> new_seqs{};
    };
    [[nodiscard]] ComponentDeltaResult save_components_delta(
        std::string_view dir, std::uint64_t watermark,
        std::array<bool, kComponentCount> dirty_mask,
        std::span<const std::byte> keydir_delta);

    // 组件载入结果。
    struct ComponentLoadResult {
        bool loaded = false;           // 文件结构完整 + header.watermark == expected
        std::uint64_t watermark = 0;   // 实际 header.watermark
        bool from_prev = false;        // 落在 .prev
        bool all_segments_ok = false;  // 段级 CRC 全过（delta 链分段的健康判断）
    };
    // 从 dir/comp.ckpt（或 .prev）载入一个组件，校验 header.watermark 与
    // expected_base_wm。base 异常时 fold 阶段从 0 重放（comp watermark=0）。
    // 链（.d1..d{chain_seq}）在组件内连续重放，hook 透传到 keydir。
    [[nodiscard]] ComponentLoadResult load_component(
        bitcask::ComponentId comp, std::string_view dir,
        std::uint64_t expected_base_wm, std::uint32_t chain_seq,
        const DeltaReplayHook& hook);

    // 链状态 setter（Cask 维护 manifest 时同步）——SearchLayer 不再自管链。
    // 主要在 load 后回写「本组件当前已 commit 的链状态」。
    struct ComponentCkptState {
        std::uint64_t base_gen = 0;
        std::uint64_t chain_wm = 0;
        std::uint32_t next_seq = 1;
        bool rebase_needed = false;
    };
    void set_component_state(bitcask::ComponentId comp,
                             const ComponentCkptState& st);
    [[nodiscard]] ComponentCkptState
    get_component_state(bitcask::ComponentId comp) const;

    // 是否需要 rebase（close/merge/compact 后强制走 base 路径）。Cask
    // 在 save_checkpoint_paired 决策 base vs delta 时调用。
    [[nodiscard]] bool needs_ckpt_rebase() const {
        return ckpt_rebase_needed_.load(std::memory_order_relaxed);
    }

    // 当前各组件的脏位掩码（true = 该组件自上次 save 以来有变化）。
    // Cask 决策每个组件是 base 还是 delta 时调用。
    [[nodiscard]] std::array<bool, kComponentCount> dirty_mask() const {
        std::array<bool, kComponentCount> m{};
        m[static_cast<std::size_t>(bitcask::ComponentId::kDocmap)] =
            dirty_docmap_.load(std::memory_order_relaxed);
        m[static_cast<std::size_t>(bitcask::ComponentId::kBm25)] =
            dirty_bm25_default_.load(std::memory_order_relaxed) ||
            dirty_bm25_fields_.load(std::memory_order_relaxed);
        m[static_cast<std::size_t>(bitcask::ComponentId::kVec)] =
            dirty_hnsw_.load(std::memory_order_relaxed);
        return m;
    }

    // S14-4：强制下次 save 写全量 base（链坍缩）。close 前调用——干净关闭
    // 收敛为单一 base 文件：.prev 代际随之刷新，链不跨干净重启累积
    // （delta 链的预期存续范围 = 两次 base 之间的运行期窗口）。
    void force_ckpt_rebase() {
        ckpt_rebase_needed_.store(true, std::memory_order_relaxed);
    }

    // 从磁盘重建倒排索引：遍历 Index 中所有 live 文档，通过 doc_reader 回调读取文本，
    // 重新分词并构建全新的 InvertedIndex，原子替换旧的。
    // doc_reader(file_id, offset, total_sz) → 返回文档文本，失败返回 std::nullopt。
    using DocReader = std::function<std::optional<std::string>(
        std::uint32_t, std::uint64_t, std::uint32_t)>;
    void rebuild_index(DocReader doc_reader);

    // 死点压实（S10.11）：对各字段倒排里死点占比 ≥ threshold 的 posting list，
    // 用 index_（LiveChecker）重建只留 live ord，回收高 churn 累积的死点。
    // 比 rebuild_index 轻（不重读磁盘、不重新分词）；分数无关。返回压实的 list 数。
    std::size_t compact(double dead_ratio_threshold = 0.5);

    std::uint64_t compact_index_chunks() {
        dirty_docmap_.store(true, std::memory_order_relaxed);  // S14-3
        return index_.compact_chunks();
    }

    // ---- 访问内部组件（Phase 4 集成用）----
    [[nodiscard]] index::Index&       index()       { return index_; }
    [[nodiscard]] const index::Index& index() const { return index_; }
    // S16-3：查询面只读身份表视图（Index IS-A DocTable）。查询代码经此消费
    // docmap，不直摸 index_ 的具体类型——P4 双插件拆分的前置。
    [[nodiscard]] const bm25::DocTable& doc_table() const noexcept { return index_; }

    // V4:Index 概要(totlive / total_ords),Cask::needs_merge 据此算
    // dead_doc_rate。无索引时 = IndexInfo 零值。
    [[nodiscard]] index::IndexInfo index_info() const { return index_.info(); }

    // S12-2：所有字段所有 posting list 的 items 总数（含未压实死点）。内省/测试用。
    // 非并发安全——须在静止时调用。
    [[nodiscard]] std::size_t total_postings() const;

private:
    // 高亮原文 LRU（S9.3）：ord → 原文，带容量上限。只为高亮路径服务；
    // 冷文档被挤出后高亮降级为无片段，不影响 BM25 检索本身。
    // C1:内置 mutex——IndexPool 工作线程 put 与查询线程 get(高亮)并发,
    // TSan 降噪后实测捕获竞态;原"caller 串行化"假设与生产线程模型不符。
    // get 返回拷贝而非内部指针:旧接口指针在锁外可被并发淘汰释放(UAF 窗口)。
    class DocTextLru {
    public:
        explicit DocTextLru(std::size_t cap) : cap_(cap) {}

        void put(std::uint64_t ord, std::string text) {
            if (cap_ == 0) return;
            std::lock_guard<std::mutex> lk(mu_);
            if (auto it = map_.find(ord); it != map_.end()) {
                it->second->second = std::move(text);
                lru_.splice(lru_.begin(), lru_, it->second);
                return;
            }
            lru_.emplace_front(ord, std::move(text));
            map_[ord] = lru_.begin();
            while (lru_.size() > cap_) {
                map_.erase(lru_.back().first);
                lru_.pop_back();
            }
        }

        // 命中返回原文拷贝并提升为最近使用；未命中返回 nullopt。
        std::optional<std::string> get(std::uint64_t ord) {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = map_.find(ord);
            if (it == map_.end()) return std::nullopt;
            lru_.splice(lru_.begin(), lru_, it->second);
            return it->second->second;
        }

        void erase(std::uint64_t ord) {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = map_.find(ord);
            if (it == map_.end()) return;
            lru_.erase(it->second);
            map_.erase(it);
        }

        void clear() {
            std::lock_guard<std::mutex> lk(mu_);
            lru_.clear();
            map_.clear();
        }

    private:
        std::size_t cap_;
        std::list<std::pair<std::uint64_t, std::string>> lru_;  // front=最近
        std::unordered_map<std::uint64_t,
            std::list<std::pair<std::uint64_t, std::string>>::iterator> map_;
        std::mutex mu_;
    };

    // 取或建某字段的 InvertedIndex（S8.6 阶段2）。
    bm25::InvertedIndex& field_index(std::string_view field);
    // 取某字段的 InvertedIndex（只读，不存在返回 nullptr）。
    const bm25::InvertedIndex* field_index(std::string_view field) const;

    // S10-A4：把字段名 intern 进 field_names_intern_，返回稳定 string_view（node 不失效）。
    std::string_view intern_field_name(std::string_view name);

    // S12-2：写路径末尾（reducer 线程）的自动 compaction 触发。config_.auto_compact_dead_ratio
    // <=0 时是单次 double 比较即返回（默认关，零开销）。开启后累计退休文档达节流阈值才
    // 在本线程内 compact()——与 add_doc/put_doc 同线程，无并发窗口。由 reduce_apply /
    // on_write / on_delete 末尾调用。
    void maybe_auto_compact();

    // D2：抽出 5+ 处 search_* 共有的「bm25 结果集物化为 SearchHit」骨架：
    // 逐条 ord→ext 翻译（翻译失败跳过）+ 可选 MetaFilter 后过滤（空 meta 不通过）
    // + 可选截断到 k（k==0 不截断，bm25 内核已 top-k 的路径用之）。
    // S16-3：经 const DocTable& 形参消费 docmap（不直摸 index_ 具体类型）。
    [[nodiscard]] std::vector<SearchHit> materialize_hits(
        const std::vector<bm25::SearchResult>& results,
        const bm25::DocTable& doc_table,
        const meta::MetaFilter* filter = nullptr,
        std::size_t k = 0) const;

    // D2：抽出 search_phrase/search_near 共有的「按 position 还原 query 词序」：
    // analyze_with_positions → 展开 (position, term) → 按位置排序 → terms 向量
    // （phrase/near 依赖词序，analyze() 的 map 无序不可直接用）。空查询 → 空向量。
    [[nodiscard]] std::vector<std::string> ordered_query_terms(
        std::string_view query) const;

    SearchLayerConfig  config_;
    // S16-1：docmap 实体经 shared_ptr 持有（自持或宿主注入），index_ 是其
    // 引用别名——两个大实现体的既有 `index_.` 用法零改动。声明序：holder
    // 必须先于引用初始化。
    std::shared_ptr<index::Index> index_holder_;
    index::Index&     index_;
    // S8.6：每字段一个 InvertedIndex（字段间 avgdl/idf 隔离）。
    // 旧单 text 文档与无字段限定查询都走 kDefaultField。
    // O8：透明 hash——field_index 查找直接吃 string_view，免临时 string。
    // C1:fields_mu_ 保护 map 结构——IndexPool 工作线程首次写入新字段会
    // emplace,与查询线程的 find 并发(TSan 降噪后实测捕获的真竞态)。
    // InvertedIndex 本体地址稳定(unique_ptr)且内部自带分片并发,
    // 锁只管 map;引用/指针可出锁使用。
    mutable std::shared_mutex fields_mu_;
    std::unordered_map<std::string, std::unique_ptr<bm25::InvertedIndex>,
                       StringHash, std::equal_to<>> fields_;
    // R3：ord → (字段名 → 该字段 doc_len)，供 on_delete 按字段精确扣减统计。
    // 仅多字段路径填充；单 text 路径用 index_ 的 doc_len 即可（默认字段）。
    // S10-A4：字段名借自 field_names_intern_，消除每文档每字段一次 owning string 分配。
    std::unordered_map<std::uint64_t,
                       std::vector<std::pair<std::string_view, std::uint32_t>>> ord_field_lens_;
    // S10-A4:字段名 intern 池。unordered_set node 在 insert 后稳定 → string_view 安全。
    // 透明 hash（StringHash）让 find 直接吃 string_view，免临时 string（与 fields_ 同）。
    std::unordered_set<std::string, StringHash, std::equal_to<>> field_names_intern_;
    mutable std::shared_mutex field_names_intern_mu_;
    std::unique_ptr<text::Analyzer>      analyzer_;
    // V3.3:HNSW 向量索引(config.vector_dim>0 时创建)。单写者
    // (IndexPool worker 的 on_vector/recover_doc/rebuild_hnsw)+ 多读者
    // (search_vector)并发安全,协议见 hnsw.hpp。
    // V3.5:atomic<shared_ptr>——merge 重建以"新图旁路构建 + 原子换指针"
    // 实现,读者每次操作开头 load 一次快照指针,旧图由引用计数续命;
    // 写路径(worker 单线程)同样经 load 取图。指针仅在构造与
    // rebuild_hnsw 的换入点变更。
    std::atomic<std::shared_ptr<vec::HnswIndex>> hnsw_;
    mutable SearchCache cache_;
    mutable DocTextLru  doc_texts_;
    // S11：open-time 注入的不可变同义词词典（来自 SearchLayerConfig::synonym_map）。
    // shared_ptr<const> → 构造后只读，并发查询安全；移除了运行期 set_synonym_map
    // setter（曾是配置项里唯一的 reader-vs-writer 竞态源）。
    std::shared_ptr<const text::SynonymMap> synonym_map_;
    // 注：查询并行用的「有界 Search 池」是**进程级共享**的（非 per-Cask），
    // 定义在 search_layer.cpp（search_arena()）。见 S7-2。

    // S14-3：段级 dirty-bit（路线 A §5）。写路径置位，save_search_ckpt
    // 消费：干净段从现有 search.ckpt 原字节前移（免重序列化，无向量写
    // 周期 hnsw 段零 CPU；纯向量负载 bm25 段零 CPU），只重序列化脏段；
    // 保存成功后清零，load 成功载入的段亦清零（此刻内存 == 文件）。
    // 初值 true：未知状态一律重序列化。relaxed 原子：全部写点与 save 点
    // 在现有路径中已被 reducer / 静止点串行化，原子仅为同步旁路（无池
    // 模式的 on_write/on_delete）的形式安全。
    std::atomic<bool> dirty_docmap_{true};
    std::atomic<bool> dirty_bm25_default_{true};
    std::atomic<bool> dirty_bm25_fields_{true};
    std::atomic<bool> dirty_hnsw_{true};

    // ---- S14-4：ord-delta 链状态（base + search.ckpt.d<seq> 文件链）----
    // 全部只在 save/load/reducer 上下文访问（现有路径已串行化）。
    //
    // rebase 标志：compact/rebuild_index 物理重排 posting、破坏 base+delta
    // 的可重构性 → 置位后下次 save 写全量 base 并清链。初值 true（未知
    // 状态一律全量），base 成功保存/载入后清。
    std::atomic<bool> ckpt_rebase_needed_{true};
    std::uint64_t ckpt_base_gen_ = 0;   // 当前 base 的 watermark（代 id）
    std::uint64_t ckpt_chain_wm_ = 0;   // base+链的覆盖水位（下个 delta 的 from）
    std::uint32_t ckpt_next_seq_ = 1;   // 下个 delta 文件序号
    // 窗口日志（自上次 save 起，save 成功即清）：
    // 删除 (key, tomb_ord)——docmap delta 的 remove 半边；bm25 统计效果由
    // delta 头的绝对 N/sdl 覆盖，无需入日志。只记 tomb_ord ≥ chain_wm 的
    // （fold 重叠区的旧墓碑不入——其目标可能已被链内更新的 put 复活）。
    std::vector<std::pair<std::string, std::uint64_t>> delta_removals_;
    // 向量插入 (ord, f32)——hnsw 无不可变旧段（插入改写旧邻接），delta 用
    // 插入日志重放（insert 有 ord 水位自门）。只记 ord ≥ chain_wm 的。
    std::vector<std::pair<std::uint64_t, std::vector<float>>> delta_vecs_;

    // S17-2:per-component 链状态——与 Cask 侧 manifest 同步。索引同
    // ComponentId（0=docmap, 1=bm25, 2=vec）。ckpt_rebase_needed_ 为
    // 全局开关（旧路径遗留），仍被 save_search_ckpt 的 rebase 路径读。
    std::array<std::uint64_t, kComponentCount> comp_base_gen_{};
    std::array<std::uint64_t, kComponentCount> comp_chain_wm_{};
    std::array<std::uint32_t, kComponentCount> comp_next_seq_{};
    static_assert(kComponentCount == 3, "comp_*_ size must match ComponentId");

    // delta 保存/应用（save_search_ckpt / load_search_ckpt 内部）。
    [[nodiscard]] bool save_delta_ckpt(const std::string& base_path,
                                       std::uint64_t watermark,
                                       std::span<const std::byte> keydir_delta);
    [[nodiscard]] bool apply_delta_file(
        const std::vector<bitcask::search::LoadedSection>& sections,
        const DeltaReplayHook& hook);
};

// S7-4: 把 [0, n) 并发跑在进程级共享「有界 Search 池」上（inter-query 并发）。
// body(i) 执行第 i 条**独立**查询，写各自结果槽（槽间不重叠 → 无需锁）。每条
// 查询内部仍串行；并发发生在查询**之间**（多条独立重查询，总功/核数，无单查询
// 两路并行的均衡/唤醒摊销问题）。n<=1 直跑（零池开销）。
// 要求：body 之间不共享可变态（查询纯读各索引 shared_lock，安全）。
void parallel_for_queries(std::size_t n,
                          const std::function<void(std::size_t)>& body);

}  // namespace bitcask::search