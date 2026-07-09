// TextPlugin — BM25 文本域插件（S18-4，设计 doc/plugin-arch-split-design-zh.md §6）。
//
// 自 SearchLayer 抽出的文本子系统：per-field 倒排（InvertedIndex）+
// analyzer + 查询缓存 + 高亮原文 LRU + 同义词 + bm25 组件 checkpoint
//（bm25.ckpt 文件族 + delta 链）。S18-5 起实现 plugin::CaskPlugin
//（prepare/on_put/flush/open）；S19 起由 Cask 直持（S18 期经 SearchLayer
// 委托，shim 已降级为测试夹具）——外部 API 面零变化。
//
// === 线程模型（与原 SearchLayer 文本域一致，C1）===
//   单写者（reducer：apply_text/apply_job/on_delete/组件 save/load）+
//   多读者（查询线程）。fields_（shared_mutex 管 map 结构，InvertedIndex
//   本体自带分片并发）、doc_texts_（内置 mutex）、cache_（自带锁）。
//
// === 依赖注入 ===
//   const DocTable&：查询面只读身份表（live/翻译/meta 过滤）。
//   DocLenWriter&：doc_len 回填通道（S16-2/S18-1，reducer 单写者可调）。
//   CompactionStats&：S12-2 自动压实的节流统计（宿主 DocMap 的写路径计数）。

#pragma once

#include "bitcask/analyzer.hpp"
#include "bitcask/component_ckpt.hpp"     // S20-1 R6：共用链状态/载入结果类型
#include "bitcask/doc_table.hpp"
#include "bitcask/highlighter.hpp"
#include "bitcask/text_plugin_config.hpp"  // S20-4：TextPluginConfig（轻量头）
#include "bitcask/inverted.hpp"
#include "bitcask/plugin_api.hpp"    // S18-5：实现 CaskPlugin
#include "bitcask/search_cache.hpp"
#include "bitcask/search_types.hpp"
#include "bitcask/segment.hpp"       // S27-3 Slice B1：SealedSegment 镜像
#include "bitcask/segment_set.hpp"   // S27-3 Slice B1：SegmentSet 段集
#include "bitcask/string_hash.hpp"
#include "bitcask/synonym_map.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bitcask::text {

// TextPluginConfig 定义已迁 text_plugin_config.hpp（S20-4）。

// prepare 相产物：map_analyze 的 ReduceJob（类型擦除跨线程移交；原
// SearchLayerAdapter::SearchPrepared，S18-5 随 adapter 退役迁入）。
struct TextPrepared final : plugin::Prepared {
    search::ReduceJob job;
};

class TextPlugin final : public plugin::CaskPlugin {
public:
    // 构造 analyzer（可能因无效配置失败，caller 须查 has_analyzer()）。
    TextPlugin(const TextPluginConfig& config, const bm25::DocTable& docs,
               bm25::DocLenWriter& doc_len, bm25::CompactionStats& stats);
    // S10-A1：测试专用——注入自定义 analyzer（如计数 wrapper）。
    TextPlugin(const TextPluginConfig& config, const bm25::DocTable& docs,
               bm25::DocLenWriter& doc_len, bm25::CompactionStats& stats,
               std::unique_ptr<Analyzer> injected_analyzer);

    TextPlugin(const TextPlugin&) = delete;
    TextPlugin& operator=(const TextPlugin&) = delete;

    [[nodiscard]] bool has_analyzer() const noexcept {
        return analyzer_ != nullptr;
    }
    // 测试注入通道（SearchLayer 兼容构造用）。
    void replace_analyzer(std::unique_ptr<Analyzer> a) {
        if (a) analyzer_ = std::move(a);
    }

    // ---- plugin::CaskPlugin（S18-5：写路径直连；flush/open 实装在 S18-6）----
    //
    // 语义决定（属于本插件，非宿主约定；原 SearchLayerAdapter 逐条移入）：
    //   - doc == nullptr（纯 KV put）时 text := value——「纯 put 也入全文索引」。
    //   - 多字段路径（doc.fields 非空）走 prepare（map_analyze 纯函数并行）；
    //     单文本路径 prepare 返回 nullptr，分析在 on_put（reducer）内进行。
    [[nodiscard]] std::string_view name() const override { return "bm25"; }
    // S18-6：载入 bm25 组件（base+链，manifest 提示经 ctx）。损坏/缺失自行
    // 降级：watermark() 报 0 → 宿主全量重放重建（接口契约）。恒返回 kOk。
    plugin::PluginStatus open(const plugin::OpenContext& ctx) override;
    [[nodiscard]] std::uint64_t watermark() const override {
        return watermark_;
    }
    plugin::PluginStatus close() override { return plugin::PluginStatus::kOk; }
    [[nodiscard]] bool wants_prepare() const override { return true; }
    [[nodiscard]] plugin::PreparedPtr
    prepare(const plugin::PutEvent& e) const override {
        if (!e.doc || e.doc->fields.empty()) {
            // 单文本：活写路径在 reducer 内分析（S15-2 路由镜像）。
            // S18-8：**重放批**让单文本也走 prepare 并行分析——镜像原
            // recover_doc 的 kDefaultField 包装（S3 恢复期并行语义）。
            if (!e.replay) return nullptr;
            const std::string_view t = e.doc ? e.doc->text : e.value;
            const std::pair<std::string_view, std::string_view> f{
                search::kDefaultField, t};
            auto p = std::make_unique<TextPrepared>();
            p->job = map_analyze(e.key, e.ord, {&f, 1}, e.loc.file_id,
                                 e.loc.offset, e.loc.total_sz, e.tstamp);
            return p;
        }
        auto p = std::make_unique<TextPrepared>();
        p->job = map_analyze(e.key, e.ord, e.doc->fields, e.loc.file_id,
                             e.loc.offset, e.loc.total_sz, e.tstamp);
        return p;
    }
    // S16-2 前置条件：宿主已先 apply docmap——本插件只做 BM25 侧。
    void on_put(const plugin::PutEvent& e, plugin::PreparedPtr prep) override {
        if (auto* sp = static_cast<TextPrepared*>(prep.get())) {
            // prepare 相产物（多字段活写 / 重放批含单文本）→ 直接 apply。
            apply_job(sp->job);
            return;
        }
        if (e.doc && !e.doc->fields.empty()) {
            // 多字段但 prep 为空 = prepare 抛过异常（宿主已计数）→ 空 job
            // 降级，apply_job 空 job 守卫兜底。
            const search::ReduceJob empty{};
            apply_job(empty);
            return;
        }
        // 单文本（原 OnWriteEntry 语义）：分析在 reducer 内进行。
        const std::string_view t = e.doc ? e.doc->text : e.value;
        apply_text(e.key, e.ord, t);
    }
    void on_delete(const plugin::DeleteEvent& e) override {
        // kNoPriorOrd = 删不存在的 key：宿主未动 docmap，历史语义为 no-op。
        if (e.prior_ord == plugin::kNoPriorOrd) return;
        on_delete(e.key, e.ord, e.prior_ord);
    }
    // S18-6：落盘到 req.watermark。base/delta 决策下沉：force_rebase ‖ 自身
    // rebase 标志（compact/rebuild_index 置位）‖ 链长达上限 → base；否则
    // 脏才写 delta（干净 = no-op，covered_ord 报当前链水位不推进 manifest）。
    plugin::FlushResult flush(const plugin::FlushRequest& req) override;
    // S18-7（设计 §3.9）：merge 收尾——死 posting 压实经 run_serialized 投递
    // reducer 静止点（先于宿主随后的成对保存点 RunFn，FIFO 顺序涌现 = 旧
    // 「compact 在前、ckpt 在后」硬编码序）。P4 保持无条件提交（阈值 0.2
    // 与旧 kMergeCompactDeadRatio 一致；dead_ratio 决策下沉留 P5）。
    void on_merge_commit(const plugin::MergeCommitEvent&) override;
    // S14-4：强制下次 flush 写全量 base（链坍缩；close/compact/rebuild 后）。
    void force_rebase() noexcept {
        rebase_needed_.store(true, std::memory_order_relaxed);
    }
    [[nodiscard]] bool rebase_needed() const noexcept {
        return rebase_needed_.load(std::memory_order_relaxed);
    }

    // ---- 写（reducer 单写者；docmap 行由宿主先落，本层只写倒排/回填）----
    // 单文本核心：分析 + doc_len 回填 + 默认域倒排 + 高亮原文 + 缓存失效。
    void apply_text(std::string_view key, std::uint64_t ord,
                    std::string_view text);
    // Map 阶段（纯 const 函数，可并发）：analyze 各字段 + catch-all 合并。
    [[nodiscard]] search::ReduceJob map_analyze(
        std::string_view key, std::uint64_t ord,
        std::span<const std::pair<std::string_view, std::string_view>> fields,
        std::uint32_t file_id, std::uint64_t offset,
        std::uint32_t total_sz, std::uint32_t tstamp) const;
    // Reduce 阶段（原 reduce_apply 的 BM25 半边；向量半边归 VectorPlugin）。
    // S23-M4：非 const 版 move doc_text 进原文 LRU（生产流水线路径）；
    // const 版兼容 shim/降级路径（仅 doc_text 多一次拷贝）。
    void apply_job(search::ReduceJob& job);
    void apply_job(const search::ReduceJob& job);
    // 删除的 BM25 统计扣减半边（docmap 删除/日志由宿主 Index 自记账）。
    void on_delete(std::string_view key, std::uint64_t tomb_ord,
                   std::uint64_t prior_ord);

    // ---- 查询面（线程安全，语义与原 SearchLayer 相同）----
    [[nodiscard]] std::expected<std::vector<search::SearchHit>,
                                search::SearchError>
    search_text(std::string_view query, std::size_t k,
                const bm25::Bm25Params* params_override = nullptr,
                const meta::MetaFilter* filter = nullptr) const;
    [[nodiscard]] std::expected<std::vector<search::SearchHit>,
                                search::SearchError>
    search_phrase(std::string_view query, std::size_t k,
                  const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::expected<std::vector<search::SearchHit>,
                                search::SearchError>
    search_near(std::string_view query, std::uint32_t slop, std::size_t k,
                const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::expected<std::vector<search::SearchHit>,
                                search::SearchError>
    bool_search(std::string_view query, std::size_t k,
                const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::expected<std::vector<search::SearchHit>,
                                search::SearchError>
    search_fuzzy(std::string_view query, std::size_t k,
                 std::uint32_t max_edit_distance,
                 const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::expected<std::vector<search::SearchHit>,
                                search::SearchError>
    search_fields(std::string_view query, std::size_t k,
                  const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::expected<std::vector<search::SearchHit>,
                                search::SearchError>
    search_wildcard(std::string_view pattern, std::size_t k,
                    const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::optional<bm25::ScoreExplanation>
    explain(std::string_view query, std::string_view key,
            const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::expected<std::vector<search::SearchHitEx>,
                                search::SearchError>
    search_text_highlight(std::string_view query, std::size_t k,
                          const search::HighlightOptions& opts = {}) const;

    // ---- 维护 ----
    // 死点压实（S10.11）。caller 负责 legacy rebase 标志。
    std::size_t compact(double dead_ratio_threshold = 0.5);
    // 从头重建倒排：for_each_doc 回调枚举 (ord, text) 对（宿主提供 docmap
    // 遍历 + 磁盘读回），本层重新分词构建全新倒排并原子替换。
    void rebuild_index(
        const std::function<void(
            const std::function<void(std::uint64_t, const std::string&)>&)>&
            for_each_doc);
    [[nodiscard]] std::size_t total_postings() const;
    [[nodiscard]] std::size_t cache_entries() const { return cache_.size(); }

    // ---- Slice B1 testing-only flush 入口 ----
    // B1 阶段 flush_building 仅由 64K 阈值触发，单测不便写满 64K。
    // 这里暴露显式入口供 Slice B1 单测用；生产路径不会调（仍走阈值）。
    void flush_building_now() { flush_building(); }

    // ---- Slice B1 测试 / 内省访问器 ----
    // building_/segment_set_ 是 fields_ 的**镜像**（B1 阶段查询仍走 fields_，
    // B2 才切换）。测试用只读视图验证镜像一致性、flush 触发、段级删除。
    [[nodiscard]] const search::SealedSegment* building_segment() const {
        return building_.get();
    }
    [[nodiscard]] const search::SegmentSet* segment_set() const {
        return segment_set_.get();
    }

    // ---- 记账（S14-3 语义；default/fields 两段独立脏位）----
    [[nodiscard]] bool dirty() const noexcept {
        return dirty_default_.load(std::memory_order_relaxed) ||
               dirty_fields_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool dirty_default() const noexcept {
        return dirty_default_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool dirty_fields() const noexcept {
        return dirty_fields_.load(std::memory_order_relaxed);
    }
    void clear_dirty() noexcept {
        dirty_default_.store(false, std::memory_order_relaxed);
        dirty_fields_.store(false, std::memory_order_relaxed);
    }
    void clear_dirty_default() noexcept {
        dirty_default_.store(false, std::memory_order_relaxed);
    }
    void clear_dirty_fields() noexcept {
        dirty_fields_.store(false, std::memory_order_relaxed);
    }

    // ---- legacy 统一 ckpt 容器原语（SearchLayer 的 search.ckpt 路径消费；
    //      P5 随 legacy 收编后删除）----
    // base 段序列化：段不存在/为空返回 false（caller 省段）。
    [[nodiscard]] bool serialize_default(std::vector<std::byte>& out) const;
    [[nodiscard]] bool serialize_fields(std::vector<std::byte>& out) const;
    // delta 段序列化（窗口 [from, ...)）。
    [[nodiscard]] bool serialize_default_delta(std::vector<std::byte>& out,
                                               std::uint64_t from) const;
    [[nodiscard]] bool serialize_fields_delta(std::vector<std::byte>& out,
                                              std::uint64_t from) const;
    // base 段反序列化（成功清对应脏位）。
    [[nodiscard]] bool deserialize_default(std::span<const std::byte> payload);
    [[nodiscard]] bool deserialize_fields(std::span<const std::byte> payload);
    // delta 段重放。
    [[nodiscard]] bool apply_default_delta(std::span<const std::byte> payload);
    [[nodiscard]] bool apply_fields_delta(std::span<const std::byte> payload);

    // ---- bm25 组件 checkpoint（bm25.ckpt 文件族；S18-6 收进 flush/open）----
    [[nodiscard]] bool save_component_base(std::string_view dir,
                                           std::uint64_t watermark);
    // S23-M4：apply_job 双入口的共享实现（doc_text 所有权经右值参数注入）。
    void apply_job_impl(const search::ReduceJob& job, std::string&& doc_text);
    // 三组件同构，收敛至 ckpt:: 共用类型（S20-1 R6）。
    using DeltaSaveResult = ckpt::DeltaSaveResult;
    [[nodiscard]] DeltaSaveResult save_component_delta(std::string_view dir,
                                                       std::uint64_t watermark);
    using LoadResult = ckpt::LoadResult;
    [[nodiscard]] LoadResult load_component(std::string_view dir,
                                            std::uint64_t expected_base_wm,
                                            std::uint32_t chain_seq);

    // 链状态（Cask 转发同步）。
    using ChainState = ckpt::ChainState;
    [[nodiscard]] ChainState chain_state() const { return chain_; }
    void set_chain_state(const ChainState& st) { chain_ = st; }

private:
    // 高亮原文 LRU（S9.3）：ord → 原文，带容量上限。只服务高亮路径；冷文档
    // 挤出后高亮降级为无片段。C1：内置 mutex（写线程 put 与查询线程 get
    // 并发）；get 返回拷贝而非内部指针，避免锁外被并发淘汰释放（UAF）。
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

    // 取或建某字段的 InvertedIndex（S8.6）。
    bm25::InvertedIndex& field_index(std::string_view field);
    const bm25::InvertedIndex* field_index(std::string_view field) const;
    // S10-A4：字段名 intern（node 稳定 → string_view 安全）。
    std::string_view intern_field_name(std::string_view name);
    // S12-2：写路径末尾的自动 compaction 触发（reducer 线程内）。
    void maybe_auto_compact();
    // S27-3 Slice B1：Building 段阈值封口。空段 / 无 segment_set_ 直接 no-op。
    void flush_building();
    // D2：bm25 结果集 → SearchHit 物化骨架。
    [[nodiscard]] std::vector<search::SearchHit> materialize_hits(
        const std::vector<bm25::SearchResult>& results,
        const bm25::DocTable& doc_table,
        const meta::MetaFilter* filter = nullptr,
        std::size_t k = 0) const;
    // D2：phrase/near 共用——按 position 还原 query 词序。
    [[nodiscard]] std::vector<std::string> ordered_query_terms(
        std::string_view query) const;
    // S27-3 Slice B2a：收集 [SegmentSet 全段 + Building 段] 的默认字段
    // SegmentView 列表，供 search_text/phrase 等走 multi_segment_search。
    [[nodiscard]] std::vector<search::SegmentView>
    collect_default_segment_views() const;
    [[nodiscard]] std::vector<search::MultiFieldSegmentView>
    collect_multi_field_segment_views() const;

    TextPluginConfig       config_;
    const bm25::DocTable&  docs_;
    bm25::DocLenWriter&    doc_len_writer_;
    bm25::CompactionStats& stats_;
    // S8.6：每字段一个 InvertedIndex（字段间 avgdl/idf 隔离）。O8：透明
    // hash。C1：fields_mu_ 只保护 map 结构（写线程首次 emplace vs 查询
    // find）；InvertedIndex 本体地址稳定且自带分片并发 → 引用可出锁用。
    mutable std::shared_mutex fields_mu_;
    std::unordered_map<std::string, std::unique_ptr<bm25::InvertedIndex>,
                       StringHash, std::equal_to<>> fields_;
    // R3：ord → (字段名 → 该字段 doc_len)，on_delete 按字段精确扣减。
    std::unordered_map<std::uint64_t,
                       std::vector<std::pair<std::string_view, std::uint32_t>>>
        ord_field_lens_;
    // S10-A4：字段名 intern 池（unordered_set node 稳定）。
    std::unordered_set<std::string, StringHash, std::equal_to<>>
        field_names_intern_;
    mutable std::shared_mutex field_names_intern_mu_;
    std::unique_ptr<Analyzer> analyzer_;
    mutable search::SearchCache cache_;
    mutable DocTextLru doc_texts_;
    std::shared_ptr<const SynonymMap> synonym_map_;
    // S14-3：段级 dirty-bit（写路径置位，save/load 成功清零；初值 true）。
    std::atomic<bool> dirty_default_{true};
    std::atomic<bool> dirty_fields_{true};
    ChainState chain_{};
    // S18-6：flush/open 自治状态。
    std::string dir_;               // open 时记录（flush 复用）
    plugin::PluginHost* host_ = nullptr;  // open 时注入（S18-7 merge 收尾用）
    std::uint64_t watermark_ = 0;   // open 后的覆盖水位（宿主定恢复起点）
    std::atomic<bool> rebase_needed_{true};  // 初值 true：未知状态一律 base

    // ---- Slice B1：Building 段镜像（为 B2 切换查询路径做准备）----
    // building_/segment_set_ 是 fields_ 的**影子**：apply_* 同时写 fields_
    // （权威）+ building_（影子）；查询仍走 fields_（B1 行为零变化），B2 才
    // 切到 [SegmentSet + Building] 归并查询（设计 §3.5）。
    //
    // key_to_location_ 用于 on_delete 段级删除定位（段内 docid 是本地序号，
    // 删除要查到该 key 当前所在的段 + 段内 docid）。在 building_ 内 →
    // building_->mark_dead；已封口入 segment_set_ → segment_set_->segment(...)->
    // mark_dead（设计 §3.4 接受 df 高估，merge 自愈）。
    static constexpr std::size_t kBuildingFlushDocThreshold = 65536;  // 可调

    struct KeyLocation {
        bool          in_building = true;  // true: 在 building_；false: 已封口
        std::uint64_t seg_id      = 0;     // 仅 in_building=false 时有效
        DocId         docid       = 0;     // 段内本地 docid
    };

    std::unique_ptr<search::SealedSegment>   building_;     // 当前 Building 段
    std::unique_ptr<search::SegmentSet>      segment_set_;  // 已封口活跃段集
    std::unordered_map<std::string, KeyLocation,
                       StringHash, std::equal_to<>> key_to_location_;
};

}  // namespace bitcask::text
