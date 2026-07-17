// SearchLayer — Index + InvertedIndex + Analyzer 的封装层。
//
// 把 BM25 集成逻辑抽成可复用组件（Phase 4 集成用），本层不依赖
// Collection/Cask/KeyDir 等上层组件。
//
// === 数据流 ===
//   写入：on_write(key, ord, text, ...) → analyze_with_positions → index_.put_doc + inverted_->add_doc
//   删除：on_delete(key) → index_.get → inverted_->remove_doc → index_.remove
//   查询：search_text(query, k) → analyzer_->analyze → inverted_->search → ord_to_ext → SearchHit
//
// === 线程模型（C1）===
//   单写者（IndexPool worker 串行消费 on_write/on_delete）+ 多读者（查询线程）。
//   该模型下并发安全的成员：fields_（shared_mutex）、doc_texts_（内置 mutex）、
//   cache_（shared_mutex）、index_/InvertedIndex（各自带锁/分片）。**不支持多写者**。
//   ord 单调唯一分配、不复用；analyzer_ 构造失败则整个 SearchLayer 构造失败。

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
#include "bitcask/hybrid_searcher.hpp" // S18-9：RRF 融合器
#include "bitcask/search_arena.hpp"    // S19-1：inter-query 并发入口
#include "bitcask/search_config.hpp"   // S19-2：配置面独立头
#include "bitcask/text_plugin.hpp"    // S18-4：文本域插件
#include "bitcask/vector_plugin.hpp"  // S18-3：向量域插件
#include "bitcask/index.hpp"
#include "bitcask/index_manifest.hpp"  // S17-2:per-component manifest
#include "bitcask/search_checkpoint.hpp"  // S14-4
#include "bitcask/inverted.hpp"
#include "bitcask/meta_file.hpp"
#include "bitcask/meta_filter.hpp"  // V5：filter 表达树 + MetaOp/MetaCondition
#include "bitcask/search_cache.hpp"
#include "bitcask/search_types.hpp"   // S18-3：SearchHit/SearchError 共享类型
#include "bitcask/string_hash.hpp"  // StringHash（透明 hash，fields_/field_names_intern_ 共用）
#include "bitcask/synonym_map.hpp"

namespace bitcask::search {

// kDefaultField / ReduceJob / SearchHitEx：见 search_types.hpp（S18-4 迁出共享）。

// SearchLayerConfig：见 search_config.hpp（S19-2 迁出公开配置面）。

// SearchHit / SearchError：见 search_types.hpp（S18-3 抽出共享）。



class SearchLayer {
public:
    // 构造（analyzer 可能因无效配置失败，caller 须查 has_analyzer()）。
    // S16-1：docmap 注入——nullptr = 自持（standalone/测试）；非空 = 借用宿主
    // （Cask）持有的实例（DocMap 是宿主服务、SearchLayer 是消费者，见
    // doc/plugin-arch-split-design-zh.md §4）。shared_ptr 共持，析构序无关。
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
    [[nodiscard]] bool has_analyzer() const noexcept {
        return text_.has_analyzer();  // S18-4：analyzer 归 TextPlugin
    }

    // ---- 文档写入：建立索引 ----
    // key: 外部 key, ord: 文档序号, text: 文档文本,
    // file_id/offset/total_sz: 存储定位, tstamp: 时间戳
    // S16-2：legacy/standalone 入口——自写 docmap 行 + apply_text。
    // 流水线路径**不走本方法**（docmap 行由宿主先落，adapter 调 apply_text）。
    void on_write(std::string_view key, std::uint64_t ord,
                  std::string_view text,
                  std::uint32_t file_id, std::uint64_t offset,
                  std::uint32_t total_sz, std::uint64_t tstamp);

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
                         std::uint32_t total_sz, std::uint64_t tstamp);

    // S6-P0: 纯函数阶段 — analyze 各字段 + catch-all 合并，产 ReduceJob。
    // 无锁、无共享状态变更（analyzer_ const 线程安全）。fields 用 string_view 借用
    // （S10-A5），参数为 span（S15-3：插件 adapter 的 DocView::fields 可直传，
    // vector 调用点隐式转换）。
    [[nodiscard]] ReduceJob map_analyze(
        std::string_view key, std::uint64_t ord,
        std::span<const std::pair<std::string_view, std::string_view>> fields,
        std::uint32_t file_id, std::uint64_t offset,
        std::uint32_t total_sz, std::uint64_t tstamp) const;

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
    [[nodiscard]] std::size_t cache_entries() const { return text_.cache_entries(); }
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
    // 两路各取 K'=max(k×4, 64)：文本走 search_text 内核、向量走 search_vector 内核
    // （归一化/live 过滤/ord 翻译全复用）。融合 score(doc)=Σ_路 1/(60+rank_路)，rank
    // 从 1 起，单路文档只累加该路项（无需分数归一化）。平局：RRF 分相等取 ord 小者。
    // 一路空 → 退化为纯另一路，两路皆空或 vec 维度不符 → 错误。
    // V5：filter 同时作用两路（text 路 overfetch 后过滤、vec 路折进 HNSW live callback），
    // 只有两路都通过的文档进融合。线程安全同两条内核。
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
                     std::uint32_t total_sz, std::uint64_t tstamp,
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
        std::uint64_t      tstamp = 0;
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
            index_.dirty();  // S18-2：docmap 脏位由 Index 自记账
        m[static_cast<std::size_t>(bitcask::ComponentId::kBm25)] =
            text_.dirty();  // S18-4：bm25 脏位由 TextPlugin 自记账
        m[static_cast<std::size_t>(bitcask::ComponentId::kVec)] =
            vec_.dirty();  // S18-3：vec 脏位由 VectorPlugin 自记账
        return m;
    }

    // S14-4：强制下次 save 写全量 base（链坍缩）。close 前调用——干净关闭
    // 收敛为单一 base 文件：.prev 代际随之刷新，链不跨干净重启累积
    // （delta 链的预期存续范围 = 两次 base 之间的运行期窗口）。
    void force_ckpt_rebase() {
        ckpt_rebase_needed_.store(true, std::memory_order_relaxed);
        // S18-6：插件自持 rebase 标志联动（close 收链语义覆盖全组件）。
        text_.force_rebase();
        vec_.force_rebase();
    }
    // S18-6：恢复载入全组件健康后清 legacy 全局 rebase（宿主 docmap 决策
    // 与 legacy 路径消费；细粒度标志由各插件自管）。
    void clear_ckpt_rebase() {
        ckpt_rebase_needed_.store(false, std::memory_order_relaxed);
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
        // S18-2：docmap 脏位由 Index::compact_chunks 自记账。
        return index_.compact_chunks();
    }

    // ---- 访问内部组件（Phase 4 集成用）----
    // S18-3/4：插件句柄（Cask 归一化下沉调用 / S18-5 起注册进 plugins_）。
    [[nodiscard]] vec::VectorPlugin&        vector_plugin()       { return vec_; }
    [[nodiscard]] const vec::VectorPlugin&  vector_plugin() const { return vec_; }
    [[nodiscard]] text::TextPlugin&         text_plugin()         { return text_; }
    [[nodiscard]] const text::TextPlugin&   text_plugin() const   { return text_; }
    [[nodiscard]] const HybridSearcher&     hybrid_searcher() const {
        return hybrid_;  // S19-1：Cask 门面直调融合器
    }
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
    // S18-4：DocTextLru 已迁 TextPlugin。

    // S18-4：field_index/intern/auto-compact/物化骨架等私有 helper 已迁 TextPlugin。









    SearchLayerConfig  config_;
    // S16-1：docmap 实体经 shared_ptr 持有（自持或宿主注入），index_ 是其
    // 引用别名——两个大实现体的既有 `index_.` 用法零改动。声明序：holder
    // 必须先于引用初始化。
    std::shared_ptr<index::Index> index_holder_;
    index::Index&     index_;
    // S18-1 的 doc_len_writer_ 引用成员已随 S18-4 移除——回填通道整体
    // 迁入 TextPlugin（构造注入，见 doc_table.hpp DocLenWriter 契约）。
    // S18-4：文本域整体抽出为 TextPlugin（倒排/analyzer/缓存/高亮 LRU/
    // bm25 组件 ckpt）。声明序：需 index_holder_ 先初始化。
    text::TextPlugin text_;
    // S18-3：向量域整体抽出为 VectorPlugin（HNSW/插入日志/vec 组件 ckpt/
    // 归一化）。声明序：需 index_holder_ 先初始化（注入 DocTable&）。
    vec::VectorPlugin vec_;
    // S18-9：RRF 融合器（持两插件引用；声明序：text_/vec_ 之后）。
    HybridSearcher hybrid_{text_, vec_};
    // S18-4：fields_/ord_field_lens_/intern 池/analyzer/缓存等文本域成员已迁 TextPlugin。



    // 注：查询并行用的「有界 Search 池」是**进程级共享**的（非 per-Cask），
    // 定义在 search_layer.cpp（search_arena()）。见 S7-2。

    // S18-4：bm25 段级脏位已迁 TextPlugin（S14-3 语义随迁）。
    // S18-3：vec 脏位已迁 VectorPlugin（自记账）。

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
    // S18-2：docmap 窗口删除日志已迁 index::Index（remove 自记账）。
    // S18-3：向量插入日志已迁 VectorPlugin。

    // S18-2/3/4：per-component 链状态归各持有方（docmap=宿主、bm25=TextPlugin、
    // vec=VectorPlugin）；本层仅存 legacy 单链三元组（上方 ckpt_*）。

    // delta 保存/应用（save_search_ckpt / load_search_ckpt 内部）。
    [[nodiscard]] bool save_delta_ckpt(const std::string& base_path,
                                       std::uint64_t watermark,
                                       std::span<const std::byte> keydir_delta);
    [[nodiscard]] bool apply_delta_file(
        const std::vector<bitcask::search::LoadedSection>& sections,
        const DeltaReplayHook& hook);
};

// S19-1：parallel_for_queries 已迁 search_arena.hpp（本头 include 保兼容）。

}  // namespace bitcask::search