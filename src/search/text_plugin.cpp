// TextPlugin 实现（S18-4）。函数体自 SearchLayer 文本域平移——行为与文件
// 格式逐字节不变，只换持有方：index_ 的读面 → docs_（DocTable）、doc_len
// 回填 → doc_len_writer_（S18-1 窄接口）、压实统计 → stats_（S18-4 窄接口）。

#include "bitcask/text_plugin.hpp"
#include "bitcask/search_checkpoint.hpp"
#include "bitcask/text_utils.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <system_error>

namespace bitcask::text {

namespace sc = bitcask::search;
using search::kDefaultField;
using search::CacheKey;
using search::ReduceJob;
using search::SearchError;
using search::SearchHit;
using search::SearchHitEx;
using search::HighlightOptions;
using search::Snippet;

namespace {

constexpr const char* kBm25CkptName = "bm25.ckpt";

std::string comp_path(std::string_view dir) {
    return (std::filesystem::path(dir) / kBm25CkptName).string();
}

// S12-2：自动压实的节流下限（与原 SearchLayer 常量一致）。
constexpr std::uint64_t kAutoCompactMinDeaths = 1024;

}  // namespace

TextPlugin::TextPlugin(const TextPluginConfig& config,
                       const bm25::DocTable& docs,
                       bm25::DocLenWriter& doc_len,
                       bm25::CompactionStats& stats)
    : config_(config)
    , docs_(docs)
    , doc_len_writer_(doc_len)
    , stats_(stats)
    , analyzer_(AnalyzerFactory::create(config.analyzer_config))
    , cache_(config.cache_max_entries)
    , doc_texts_(config.doc_text_cache_max)
    , synonym_map_(config.synonym_map) {}

TextPlugin::TextPlugin(const TextPluginConfig& config,
                       const bm25::DocTable& docs,
                       bm25::DocLenWriter& doc_len,
                       bm25::CompactionStats& stats,
                       std::unique_ptr<Analyzer> injected_analyzer)
    : TextPlugin(config, docs, doc_len, stats) {
    if (injected_analyzer) analyzer_ = std::move(injected_analyzer);
}

bm25::InvertedIndex& TextPlugin::field_index(std::string_view field) {
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
                             std::make_unique<bm25::InvertedIndex>(
                                 config_.bm25_params,
                                 config_.index_positions)).first;
    }
    return *it->second;
}

const bm25::InvertedIndex* TextPlugin::field_index(std::string_view field) const {
    std::shared_lock lk(fields_mu_);
    auto it = fields_.find(field);
    return it == fields_.end() ? nullptr : it->second.get();
}

std::string_view TextPlugin::intern_field_name(std::string_view name) {
    // S10-A4:双检。常态(字段名已 intern)只共享锁;首次出现升级独占 emplace。
    {
        std::shared_lock lk(field_names_intern_mu_);
        auto it = field_names_intern_.find(name);
        if (it != field_names_intern_.end()) return std::string_view(*it);
    }
    std::unique_lock lk(field_names_intern_mu_);
    auto [it, _] = field_names_intern_.emplace(name);
    return std::string_view(*it);
}

void TextPlugin::apply_text(std::string_view key, std::uint64_t ord,
                            std::string_view text) {
    (void)key;
    // docmap 脏位由 Index 自记账（宿主 put_doc/set_doc_len 内部置位）。
    // 默认域倒排仅在真有词项时标脏（向量-only 文档 text 为空，不碰 bm25）。
    auto term_data = analyzer_->analyze_with_positions(text);

    std::uint32_t doc_len = 0;
    std::vector<std::string> changed_terms;
    changed_terms.reserve(term_data.size());
    for (auto& [term, data] : term_data) {
        doc_len += data.first;
        changed_terms.push_back(term);
    }
    doc_len_writer_.set_doc_len(ord, doc_len);  // S18-1：经窄接口回填

    if (!term_data.empty()) {
        dirty_default_.store(true, std::memory_order_relaxed);  // S14-3
        field_index(kDefaultField).add_doc(ord, term_data);
    }
    doc_texts_.put(ord, std::string(text));
    // S9.2：只失效查询词与本文档词集有交集的缓存条目。
    cache_.invalidate_terms(changed_terms);
    maybe_auto_compact();  // S12-2
}

// S6 索引流水线的 **Map 阶段**（设计稿 §3）。**纯 const 函数**：只读
// analyzer_（const 配置态，cppjieba Cut 亦 const 线程安全），对每个字段跑
// NFKC + 分词 + 位置，**不碰任何共享可变态** → 可在 N 个 map worker 上对不同
// 文档**并发**调用（F7 不变量，TSan 已验证）。产出 owning `ReduceJob`，由
// reducer 线程按 ord 序串行 apply。catch-all 合并见下方注释。
ReduceJob TextPlugin::map_analyze(
    std::string_view key, std::uint64_t ord,
    std::span<const std::pair<std::string_view, std::string_view>> fields,
    std::uint32_t file_id, std::uint64_t offset,
    std::uint32_t total_sz, std::uint32_t tstamp) const {
    ReduceJob job;
    job.key      = std::string(key);
    job.ord      = ord;
    job.file_id  = file_id;
    job.offset   = offset;
    job.total_sz = total_sz;
    job.tstamp   = tstamp;

    // catch-all（S8.6 + O5）：把非默认字段词项合并进默认字段，使
    // search_text/phrase/near（只查默认字段）也能命中多字段文档。O5：直接
    // 合并各字段分词结果（免整体重新分词），position 按字段顺序平移
    // ca_pos_base。字段内相对位置不变（phrase/near 字段内语义不变）；跨字段
    // 间隔取「字段最大 position + 1」。
    TermPositionsMap ca_data;
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

// Reduce 阶段的 BM25 半边（原 reduce_apply 去掉 on_vector；向量半边归
// VectorPlugin，由 SearchLayer shim / S18-5 起的宿主扇出编排）。reducer
// 单写者、严格 ord 序（reorder buffer 保证）。
// 步骤：① 侧表 ord_field_lens_ 记字段长 ② 各字段 add_doc 进倒排 ③ catch-all
// 合并默认字段 ④ doc_len 回填（docmap 行本体与 meta 由宿主先落）⑤ 高亮原文
// ⑥ 失效查询缓存。
void TextPlugin::apply_job(const ReduceJob& job) {
    // S6-P2: 空 job 守卫（prepare 抛异常时 adapter 送来空 job）。
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
            if (f.field_name == kDefaultField) {
                dirty_default_.store(true, std::memory_order_relaxed);
            } else {
                dirty_fields_.store(true, std::memory_order_relaxed);
            }
            field_index(f.field_name).add_doc(job.ord, f.terms);
        }
    }

    // 若已有字段直接写默认字段，则不重复合并（避免双写）。
    if (!job.wrote_default && !job.ca_data.empty()) {
        dirty_default_.store(true, std::memory_order_relaxed);  // S14-3
        field_index(kDefaultField).add_doc(job.ord, job.ca_data);
        field_lens.emplace_back(intern_field_name(kDefaultField), job.ca_len);
    }

    // S16-2：docmap 行与 meta 由宿主（流水线）/caller（standalone・recover）
    // 先落；分析产物 doc_len 在此回填（宿主不做分析拿不到 token 数）。
    doc_len_writer_.set_doc_len(job.ord, job.total_doc_len);
    if (!job.doc_text.empty()) {
        doc_texts_.put(job.ord, job.doc_text);
    }
    // S13-P1：S9.2 选择性失效——job 已物化本文档全部词集（各字段 terms +
    // catch-all）。BM25 全局统计漂移是 S9.2 已接受的 near-real-time 近似。
    std::vector<std::string> changed_terms;
    {
        std::size_t est = job.ca_data.size();
        for (const auto& f : job.fields) est += f.terms.size();
        changed_terms.reserve(est);
    }
    for (const auto& f : job.fields) {
        for (const auto& [term, data] : f.terms) changed_terms.push_back(term);
    }
    for (const auto& [term, data] : job.ca_data) changed_terms.push_back(term);
    cache_.invalidate_terms(changed_terms);
    maybe_auto_compact();  // S12-2：本 reducer 线程内，达阈值则压实死 posting
}

void TextPlugin::on_delete(std::string_view key, std::uint64_t tomb_ord,
                           std::uint64_t prior_ord) {
    (void)key;
    (void)tomb_ord;
    // 前置条件：docmap 行已删（宿主或 SearchLayer 二参版）。doc_len 经
    // prior_ord 从 SoA 读取——Index::remove 只翻 live/ext2ord，不清 SoA，
    // 删除后仍可读（S16-2 侦查坐实）。
    const std::uint32_t prior_doc_len = docs_.doc_len(prior_ord);
    // S14-3：删除触全部 bm25 段（remove_doc 调整各域 N/sum_doc_len 全局
    // 统计）。docmap 脏位/删除日志由宿主 Index::remove 自记账（S18-2）。
    dirty_default_.store(true, std::memory_order_relaxed);
    dirty_fields_.store(true, std::memory_order_relaxed);

    // S9.2：取被删文档词集做选择性失效。原文 LRU 命中则精确 analyze；
    // miss（冷文档被挤出）则降级为整缓存失效（安全但粗粒度）。
    // S13-P8.5：缓存本就为空时跳过整个「LRU 拷原文 + NFKC + 分词」。
    std::vector<std::string> changed_terms;
    bool have_terms = false;
    if (cache_.size() > 0) {
        auto text = doc_texts_.get(prior_ord);  // 拷贝(C1:并发安全)
        if (text) {
            auto tf = analyzer_->analyze(*text);
            changed_terms.reserve(tf.size());
            for (auto& [term, _] : tf) changed_terms.push_back(term);
            have_terms = true;
        }
    } else {
        have_terms = true;  // 空缓存：invalidate_terms(空) 即 no-op，语义等价
    }

    // 删除该文档在各字段的统计。多字段路径用 ord_field_lens_ 精确扣减各字段
    // doc_len（R3）；单 text 路径无此表，按默认字段用 prior_doc_len。
    if (auto it = ord_field_lens_.find(prior_ord); it != ord_field_lens_.end()) {
        for (auto& [field, flen] : it->second) {
            field_index(field).remove_doc(flen, {});
        }
        ord_field_lens_.erase(it);
    } else {
        std::shared_lock lk(fields_mu_);  // 只读 map 结构;remove_doc 自带并发
        for (auto& [_, inv] : fields_) {
            inv->remove_doc(prior_doc_len, {});
        }
    }
    doc_texts_.erase(prior_ord);
    if (have_terms) {
        cache_.invalidate_terms(changed_terms);  // 空缓存时为 no-op
    } else {
        cache_.invalidate();  // 原文 LRU miss：降级整缓存失效（S9.2）
    }
    maybe_auto_compact();  // S12-2
}

void TextPlugin::rebuild_index(
    const std::function<void(
        const std::function<void(std::uint64_t, const std::string&)>&)>&
        for_each_doc) {
    // 阶段2a：仍按默认字段重建。caller（SearchLayer/宿主）负责 legacy
    // rebase 标志与 docmap 遍历 + 磁盘读回（emit 回调喂 (ord, text)）。
    rebase_needed_.store(true, std::memory_order_relaxed);  // S18-6/S14-4
    auto new_inv = std::make_unique<bm25::InvertedIndex>(
        config_.bm25_params, config_.index_positions);
    doc_texts_.clear();
    ord_field_lens_.clear();  // 否则旧 ord 的多字段统计跨重建残留→无界增长。

    for_each_doc([&](std::uint64_t ord, const std::string& text) {
        auto term_data = analyzer_->analyze_with_positions(text);
        if (term_data.empty()) return;
        new_inv->add_doc(ord, term_data);
        doc_texts_.put(ord, text);
    });

    new_inv->finalize_all_postings();

    fields_.clear();
    fields_.emplace(kDefaultField, std::move(new_inv));

    cache_.invalidate();
}

std::size_t TextPlugin::compact(double dead_ratio_threshold) {
    // S14-3：压实物理重排 posting，两个 bm25 段序列化字节都会变。
    // S14-4：压实破坏 base+delta 可重构性 → 自身 rebase（legacy 全局标志
    // 仍由 SearchLayer shim 维护）。无 fields_mu_ 锁——reducer 单写者上下文，
    // map 结构在本调用期间稳定（与原 SearchLayer::compact 一致）。
    rebase_needed_.store(true, std::memory_order_relaxed);  // S18-6
    dirty_default_.store(true, std::memory_order_relaxed);
    dirty_fields_.store(true, std::memory_order_relaxed);
    std::size_t total = 0;
    for (auto& [field, inv] : fields_) {
        total += inv->compact(docs_, dead_ratio_threshold);
    }
    if (total > 0) cache_.invalidate();  // posting 行变了，缓存可能含陈旧结果
    return total;
}

std::size_t TextPlugin::total_postings() const {
    std::shared_lock lk(fields_mu_);
    std::size_t n = 0;
    for (const auto& [field, inv] : fields_) n += inv->total_postings();
    return n;
}

void TextPlugin::maybe_auto_compact() {
    const double thr = config_.auto_compact_dead_ratio;
    if (thr <= 0.0) return;  // opt-in 关（默认）→ 零开销

    const std::uint64_t retired = stats_.retired_since_compact();
    if (retired < kAutoCompactMinDeaths) return;      // 常态早退
    // 节流阈值随 live 规模缩放：大库摊薄扫描成本，小库用下限。
    if (retired < stats_.live_docs() / 2) return;

    compact(thr);  // reducer 线程内串行 → 与 add_doc/put_doc 无并发窗口
    stats_.reset_retired_since_compact();
}

// ---- 查询面（自 SearchLayer 平移，S18-4）----

// D2：bm25 结果集 → SearchHit 物化骨架（5+ 处共用）。filter 非空时按 meta_blob
// 后过滤（空 blob 不通过）；k>0 时截断到 k（text 的 overfetch 路径用之，其余
// bm25 内核已返回 top-k 的路径传 0 不截断）。
std::vector<SearchHit> TextPlugin::materialize_hits(
    const std::vector<bm25::SearchResult>& results,
    const bm25::DocTable& doc_table,
    const meta::MetaFilter* filter, std::size_t k) const {
    std::vector<SearchHit> hits;
    hits.reserve(results.size());
    for (auto& r : results) {
        if (filter) {
            // S13-P8：锁内求值，免每候选一次 blob 堆拷贝。
            if (!doc_table.eval_meta(r.ord, *filter)) continue;
        }
        auto ext_id = doc_table.ord_to_ext(r.ord);
        if (!ext_id) continue;
        hits.push_back(SearchHit{std::move(*ext_id), r.ord, r.score});
    }
    if (k > 0 && hits.size() > k) hits.resize(k);
    return hits;
}

// D2：phrase/near 共用——analyze_with_positions 还原 query 词序。
std::vector<std::string> TextPlugin::ordered_query_terms(
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
TextPlugin::search_text(std::string_view query, std::size_t k,
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
        if (inv) results = inv->search(terms, k_req, docs_, params_override);
        if (!params_override) cache_.put(cache_key, results, terms);
    }

    // D2：filter 后过滤（空 meta 不通过）+ overfetch 后截断到 k。
    return materialize_hits(results, docs_, filter, k);
}

std::expected<std::vector<SearchHit>, SearchError>
TextPlugin::search_phrase(std::string_view query, std::size_t k,
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
        if (inv) results = inv->search_phrase(terms, k, docs_, params_override);
        if (!params_override) cache_.put(cache_key, results, terms);
    }

    return materialize_hits(results, docs_);
}

std::expected<std::vector<SearchHit>, SearchError>
TextPlugin::search_near(std::string_view query, std::uint32_t slop, std::size_t k,
                         const bm25::Bm25Params* params_override) const {
    // 近邻依赖查询词序——用 analyze_with_positions 还原（D2 helper，同 phrase）。
    auto terms = ordered_query_terms(query);
    if (terms.empty()) return std::vector<SearchHit>{};

    std::vector<bm25::SearchResult> results;
    const auto* inv = field_index(kDefaultField);
    if (inv) results = inv->search_near(terms, k, slop, docs_, params_override);

    return materialize_hits(results, docs_);
}

std::expected<std::vector<SearchHit>, SearchError>
TextPlugin::search_fuzzy(std::string_view query, std::size_t k, std::uint32_t max_edit_distance,
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
    if (inv) results = inv->search_fuzzy(terms, k, max_edit_distance, docs_, params_override);

    return materialize_hits(results, docs_);
}

std::expected<std::vector<SearchHit>, SearchError>
TextPlugin::bool_search(std::string_view query, std::size_t k,
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
        // S13-D9：含 '(' 或 '"' 的查询走树路径（递归下降 + 集合求值）；
        // 其余仍走扁平路径——既有查询行为位级不变。
        const bool tree_syntax =
            query.find('(') != std::string_view::npos ||
            query.find('"') != std::string_view::npos;
        auto query_node = tree_syntax ? bitcask::bm25::parse_query_tree(query)
                                      : bitcask::bm25::parse_query(query);
        if (query_node.term.empty() && query_node.children.empty()) {
            return std::vector<SearchHit>{};
        }
        if (tree_syntax) {
            // 短语叶子：analyzer 切词填 phrase_terms（有序）。
            std::function<void(bm25::QueryNode&)> fill =
                [&](bm25::QueryNode& node) {
                if (node.is_phrase) {
                    node.phrase_terms = ordered_query_terms(node.term);
                    return;
                }
                for (auto& c : node.children) fill(c);
            };
            fill(query_node);
        }

        const auto* inv = field_index(kDefaultField);
        if (inv) {
            results = tree_syntax
                          ? inv->bool_search_tree(query_node, k, docs_,
                                                  params_override)
                          : inv->bool_search(query_node, k, docs_,
                                             params_override);
        }
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

    return materialize_hits(results, docs_);
}

std::optional<bm25::ScoreExplanation>
TextPlugin::explain(std::string_view query, std::string_view key,
                     const bm25::Bm25Params* params_override) const {
    auto ord = docs_.ord_of(key);
    if (!ord) return std::nullopt;

    auto term_freqs = analyzer_->analyze(query);
    std::vector<std::string> terms;
    terms.reserve(term_freqs.size());
    for (auto& [term, _] : term_freqs) terms.push_back(term);

    const auto* inv = field_index(kDefaultField);
    if (!inv) return bm25::ScoreExplanation{};
    return inv->explain(terms, *ord, docs_, params_override);
}

std::expected<std::vector<SearchHit>, SearchError>
TextPlugin::search_wildcard(std::string_view pattern, std::size_t k,
                             const bm25::Bm25Params* params_override) const {
    std::vector<bm25::SearchResult> results;
    const auto* inv = field_index(kDefaultField);
    if (inv) results = inv->search_wildcard(std::string(pattern), k, docs_, params_override);

    return materialize_hits(results, docs_);
}

std::expected<std::vector<SearchHit>, SearchError>
TextPlugin::search_fields(std::string_view query, std::size_t k,
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
        // S13-P8：按 boost 分组，同 boost 词（含同义词扩展）合并为一次
        // search（BM25 逐词贡献求和公式不变），组级 top-k 截断，内核调用数
        // O(boost 组)。避免逐词各自截断到 k 时丢失「单词排名 >k 但跨词组合
        // 分高」的文档。
        std::map<float, std::vector<std::string>> boost_groups;
        for (auto& [t, boost] : term_boosts) {
            auto expanded = synonym_map_ ? synonym_map_->expand(t)
                                         : std::span<const std::string>{};
            if (expanded.empty()) expanded = {&t, 1};
            auto& g = boost_groups[boost];
            g.insert(g.end(), expanded.begin(), expanded.end());
        }
        for (auto& [boost, gterms] : boost_groups) {
            auto res = inv->search(gterms, k, docs_, params_override);
            for (auto& r : res) {
                acc[r.ord] += static_cast<double>(r.score) * boost;
            }
        }
    }

    std::vector<std::pair<std::uint64_t,double>> ranked(acc.begin(), acc.end());
    std::partial_sort(ranked.begin(),
                      ranked.begin() +
                          static_cast<std::ptrdiff_t>(std::min(k, ranked.size())),
                      ranked.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
    if (ranked.size() > k) ranked.resize(k);

    std::vector<SearchHit> hits;
    hits.reserve(ranked.size());
    for (auto& [ord, score] : ranked) {
        auto ext_id = docs_.ord_to_ext(ord);
        if (!ext_id) continue;
        hits.push_back(SearchHit{std::move(*ext_id), ord, score});
    }
    return hits;
}

std::expected<std::vector<SearchHitEx>, SearchError>
TextPlugin::search_text_highlight(std::string_view query, std::size_t k,
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
        if (inv) results = inv->search(terms, k, docs_);
        cache_.put(cache_key, results, terms);
    }

    std::vector<SearchHitEx> hits;
    hits.reserve(results.size());
    for (auto& r : results) {
        auto ext_id = docs_.ord_to_ext(r.ord);
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


// ---- ckpt 原语（legacy 统一容器与 bm25 组件共用；S18-4）----

namespace {
// bm25.fields 段辅助（小端；与 search_checkpoint.hpp 编码一致）。
void put_u16_b(std::vector<std::byte>& b, std::uint16_t v) {
    sc::detail::put_u16(b, v);
}
void put_u32_b(std::vector<std::byte>& b, std::uint32_t v) {
    sc::detail::put_u32(b, v);
}
void put_u64_b(std::vector<std::byte>& b, std::uint64_t v) {
    sc::detail::put_u64(b, v);
}
}  // namespace

bool TextPlugin::serialize_default(std::vector<std::byte>& out) const {
    std::shared_lock lk(fields_mu_);
    auto dit = fields_.find(kDefaultField);
    if (dit == fields_.end()) return false;
    dit->second->serialize(out);
    return true;
}

bool TextPlugin::serialize_fields(std::vector<std::byte>& out) const {
    // 格式:u32 fieldCount; 每字段 [u16 nameLen][name][u64 invLen][inv bytes]。
    std::shared_lock lk(fields_mu_);
    std::uint32_t other_count = 0;
    for (auto& [field, inv] : fields_) {
        if (field == kDefaultField) continue;
        ++other_count;
    }
    if (other_count == 0) return false;
    put_u32_b(out, other_count);
    for (auto& [field, inv] : fields_) {
        if (field == kDefaultField) continue;
        put_u16_b(out, static_cast<std::uint16_t>(field.size()));
        out.insert(out.end(),
            reinterpret_cast<const std::byte*>(field.data()),
            reinterpret_cast<const std::byte*>(field.data()) + field.size());
        std::uint64_t pos = out.size();
        put_u64_b(out, 0);  // invLen 占位
        inv->serialize(out);
        std::uint64_t inv_len = out.size() - pos - 8;
        std::memcpy(out.data() + pos, &inv_len, 8);
    }
    return true;
}

bool TextPlugin::serialize_default_delta(std::vector<std::byte>& out,
                                         std::uint64_t from) const {
    std::shared_lock lk(fields_mu_);
    auto dit = fields_.find(kDefaultField);
    if (dit == fields_.end()) return false;
    dit->second->serialize_delta(out, from);
    return true;
}

bool TextPlugin::serialize_fields_delta(std::vector<std::byte>& out,
                                        std::uint64_t from) const {
    std::shared_lock lk(fields_mu_);
    std::uint32_t other = 0;
    for (auto& [field, inv] : fields_) {
        if (field != kDefaultField) ++other;
    }
    if (other == 0) return false;
    put_u32_b(out, other);
    for (auto& [field, inv] : fields_) {
        if (field == kDefaultField) continue;
        put_u16_b(out, static_cast<std::uint16_t>(field.size()));
        out.insert(out.end(),
            reinterpret_cast<const std::byte*>(field.data()),
            reinterpret_cast<const std::byte*>(field.data()) + field.size());
        const std::uint64_t pos = out.size();
        put_u64_b(out, 0);  // len 占位
        inv->serialize_delta(out, from);
        const std::uint64_t len = out.size() - pos - 8;
        std::memcpy(out.data() + pos, &len, 8);
    }
    return true;
}

bool TextPlugin::deserialize_default(std::span<const std::byte> payload) {
    std::unique_lock lk(fields_mu_);
    auto it = fields_.find(std::string(kDefaultField));
    std::unique_ptr<bm25::InvertedIndex> inv;
    if (it == fields_.end()) {
        inv = std::make_unique<bm25::InvertedIndex>(config_.bm25_params,
                                                    config_.index_positions);
        if (inv->deserialize(payload)) {
            fields_.emplace(kDefaultField, std::move(inv));
            dirty_default_.store(false, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
    if (it->second->deserialize(payload)) {
        dirty_default_.store(false, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool TextPlugin::deserialize_fields(std::span<const std::byte> payload) {
    const auto* p = payload.data();
    const auto* end = p + payload.size();
    if (end - p < 4) return false;
    std::uint32_t cnt = sc::detail::get_u32(p); p += 4;
    std::unique_lock lk(fields_mu_);
    bool any = false;
    bool ok = true;
    for (std::uint32_t i = 0; i < cnt; ++i) {
        if (end - p < 2) { ok = false; break; }
        std::uint16_t nlen = sc::detail::get_u16(p); p += 2;
        if (end - p < nlen + 8) { ok = false; break; }
        std::string name(reinterpret_cast<const char*>(p), nlen);
        p += nlen;
        std::uint64_t ilen = sc::detail::get_u64(p); p += 8;
        if (end - p < static_cast<std::ptrdiff_t>(ilen)) { ok = false; break; }
        auto inv = std::make_unique<bm25::InvertedIndex>(
            config_.bm25_params, config_.index_positions);
        if (inv->deserialize(std::span<const std::byte>(p, ilen))) {
            fields_.emplace(std::move(name), std::move(inv));
            any = true;
        } else {
            ok = false;
        }
        p += ilen;
    }
    if (any && ok) {
        dirty_fields_.store(false, std::memory_order_relaxed);
    }
    return any && ok;
}

bool TextPlugin::apply_default_delta(std::span<const std::byte> payload) {
    std::unique_lock lk(fields_mu_);
    auto it = fields_.find(std::string(kDefaultField));
    if (it == fields_.end()) {
        auto inv = std::make_unique<bm25::InvertedIndex>(
            config_.bm25_params, config_.index_positions);
        it = fields_.emplace(kDefaultField, std::move(inv)).first;
    }
    return it->second->apply_delta(payload);
}

bool TextPlugin::apply_fields_delta(std::span<const std::byte> payload) {
    const auto* p = payload.data();
    const auto* end = p + payload.size();
    if (end - p < 4) return false;
    std::uint32_t cnt = sc::detail::get_u32(p); p += 4;
    std::unique_lock lk(fields_mu_);
    for (std::uint32_t i = 0; i < cnt; ++i) {
        if (end - p < 2) return false;
        std::uint16_t nlen = sc::detail::get_u16(p); p += 2;
        if (end - p < nlen + 8) return false;
        std::string name(reinterpret_cast<const char*>(p), nlen);
        p += nlen;
        std::uint64_t ilen = sc::detail::get_u64(p); p += 8;
        if (end - p < static_cast<std::ptrdiff_t>(ilen)) return false;
        auto it = fields_.find(name);
        if (it == fields_.end()) {
            auto inv = std::make_unique<bm25::InvertedIndex>(
                config_.bm25_params, config_.index_positions);
            it = fields_.emplace(std::move(name), std::move(inv)).first;
        }
        if (!it->second->apply_delta(std::span<const std::byte>(p, ilen))) {
            return false;
        }
        p += ilen;
    }
    return true;
}

void TextPlugin::truncate_wal() {
    std::shared_lock lk(fields_mu_);
    for (auto& [_, inv] : fields_) {
        inv->truncate_wal();
    }
}

// ---- bm25 组件 checkpoint（bm25.ckpt 文件族；S17 格式不变）----

bool TextPlugin::save_component_base(std::string_view dir,
                                     std::uint64_t watermark) {
    const std::string fp = comp_path(dir);
    const std::string prev = fp + ".prev";
    std::error_code ec;
    if (std::filesystem::exists(fp, ec)) {
        std::filesystem::rename(fp, prev, ec);
    }
    std::vector<sc::CkptSection> secs;
    std::vector<std::vector<std::byte>> byte_bufs;
    auto add_byte = [&](std::uint16_t type, std::vector<std::byte> buf) {
        byte_bufs.push_back(std::move(buf));
        secs.push_back(sc::CkptSection{
            type, 0,
            std::span<const std::byte>(byte_bufs.back().data(),
                                        byte_bufs.back().size())});
    };
    {
        std::vector<std::byte> buf;
        if (serialize_default(buf)) {
            add_byte(static_cast<std::uint16_t>(
                         sc::CkptSectionType::kBm25Default),
                     std::move(buf));
        }
    }
    {
        std::vector<std::byte> fbuf;
        if (serialize_fields(fbuf)) {
            add_byte(static_cast<std::uint16_t>(
                         sc::CkptSectionType::kBm25Fields),
                     std::move(fbuf));
        }
    }
    const bool ok = !secs.empty() &&
        sc::SearchCheckpoint::write(fp, watermark, secs);
    if (!ok) return false;
    // 链坍缩：清 .d 链（连续序号 + 8 空洞 orphan 扫尾），重置链状态。
    std::uint32_t misses = 0;
    for (std::uint32_t i = 1; misses < 8; ++i) {
        std::error_code ec2;
        if (std::filesystem::remove(fp + ".d" + std::to_string(i), ec2)) {
            misses = 0;
        } else {
            ++misses;
        }
    }
    chain_ = ChainState{watermark, watermark, 1};
    clear_dirty();
    return true;
}

TextPlugin::DeltaSaveResult
TextPlugin::save_component_delta(std::string_view dir,
                                 std::uint64_t watermark) {
    DeltaSaveResult result;
    const std::string fp = comp_path(dir);
    const std::uint32_t seq = chain_.next_seq;
    const std::string dpath = fp + ".d" + std::to_string(seq);
    const std::uint64_t from = chain_.chain_wm;
    std::vector<sc::CkptSection> secs;
    std::vector<std::vector<std::byte>> bufs;
    auto add = [&](sc::CkptSectionType t, std::vector<std::byte> b) {
        bufs.push_back(std::move(b));
        secs.push_back(sc::CkptSection{
            static_cast<std::uint16_t>(t), 0,
            std::span<const std::byte>(bufs.back().data(),
                                       bufs.back().size())});
    };
    // kDeltaInfo：链校验三元组。
    {
        std::vector<std::byte> b;
        put_u64_b(b, chain_.base_gen);
        put_u64_b(b, from);
        put_u32_b(b, seq);
        add(sc::CkptSectionType::kDeltaInfo, std::move(b));
    }
    // bm25 delta：default + fields（组件 delta 恒全量构造，S17 设计要点）。
    {
        std::vector<std::byte> b;
        if (serialize_default_delta(b, from)) {
            add(sc::CkptSectionType::kBm25DefaultDelta, std::move(b));
        }
    }
    {
        std::vector<std::byte> fb;
        if (serialize_fields_delta(fb, from)) {
            add(sc::CkptSectionType::kBm25FieldsDelta, std::move(fb));
        }
    }
    if (!sc::SearchCheckpoint::write(dpath, watermark, secs)) return result;
    chain_.chain_wm = watermark;
    chain_.next_seq = seq + 1;
    clear_dirty();
    result.wrote = true;
    result.new_seq = seq;
    return result;
}

TextPlugin::LoadResult
TextPlugin::load_component(std::string_view dir,
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
        return result;
    };
    if (!lc) return fail();
    // kBm25Default / kBm25Fields 段应用。
    bool segments_ok = true;
    bool any = false;
    for (const auto& ls : lc->sections) {
        if (!ls.crc_ok) { segments_ok = false; continue; }
        auto st = static_cast<sc::CkptSectionType>(ls.type);
        if (st == sc::CkptSectionType::kBm25Default) {
            if (deserialize_default(std::span<const std::byte>(
                    ls.payload.data(), ls.payload.size()))) {
                any = true;
            } else {
                segments_ok = false;
            }
        } else if (st == sc::CkptSectionType::kBm25Fields) {
            if (deserialize_fields(std::span<const std::byte>(
                    ls.payload.data(), ls.payload.size()))) {
                any = true;
            } else {
                segments_ok = false;
            }
        }
    }
    if (!any) segments_ok = false;
    // 链重放（.prev 回退 = 链不可信）。
    std::uint64_t coverage = lc->watermark;
    std::uint32_t next_seq = 1;
    bool chain_ok = true;
    if (segments_ok && !from_prev) {
        const std::uint64_t base_gen_for_chain = coverage;
        for (std::uint32_t seq = 1; seq <= chain_seq; ++seq) {
            const std::string dpath = fp + ".d" + std::to_string(seq);
            std::error_code ec;
            if (!std::filesystem::exists(dpath, ec)) { chain_ok = false; break; }
            auto dc = sc::SearchCheckpoint::read(dpath);
            if (!dc) { chain_ok = false; break; }
            const sc::LoadedSection* info = nullptr;
            for (const auto& dls : dc->sections) {
                if (dls.type ==
                    static_cast<std::uint16_t>(
                        sc::CkptSectionType::kDeltaInfo)) {
                    info = &dls; break;
                }
            }
            if (!info || !info->crc_ok || info->payload.size() != 20) {
                chain_ok = false; break;
            }
            const auto* q = info->payload.data();
            const std::uint64_t gen = sc::detail::get_u64(q); q += 8;
            const std::uint64_t prev_wm = sc::detail::get_u64(q); q += 8;
            const std::uint32_t sq = sc::detail::get_u32(q);
            if (gen != base_gen_for_chain || prev_wm != coverage ||
                sq != seq) {
                chain_ok = false; break;
            }
            // 段级 CRC 预检 + delta 应用。
            bool applied = true;
            for (const auto& dls : dc->sections) {
                if (!dls.crc_ok) { applied = false; break; }
            }
            if (applied) {
                for (const auto& dls : dc->sections) {
                    auto dst = static_cast<sc::CkptSectionType>(dls.type);
                    std::span<const std::byte> pl(dls.payload.data(),
                                                  dls.payload.size());
                    if (dst == sc::CkptSectionType::kBm25DefaultDelta) {
                        if (!apply_default_delta(pl)) { applied = false; break; }
                    } else if (dst == sc::CkptSectionType::kBm25FieldsDelta) {
                        if (!apply_fields_delta(pl)) { applied = false; break; }
                    }
                }
            }
            if (!applied) { chain_ok = false; break; }
            coverage = dc->watermark;
            next_seq = seq + 1;
        }
    } else {
        chain_ok = false;
    }
    result.loaded = segments_ok && chain_ok;
    result.watermark = coverage;
    result.from_prev = from_prev;
    result.all_segments_ok = segments_ok && chain_ok;
    if (result.loaded) {
        chain_ = ChainState{expected_base_wm, coverage, next_seq};
        clear_dirty();
    } else {
        return fail();
    }
    return result;
}


// ---- S18-6：CaskPlugin flush/open 实装 ----

plugin::PluginStatus TextPlugin::open(const plugin::OpenContext& ctx) {
    dir_.assign(ctx.dir);
    host_ = ctx.host;
    auto r = load_component(dir_, ctx.committed_base_watermark,
                            ctx.committed_chain_seq);
    // 损坏/缺失 → 自行降级：watermark 0（宿主全量重放），rebase 置位
    //（下次 flush 全量 base）。loaded → 续链。
    watermark_ = r.loaded ? r.watermark : 0;
    rebase_needed_.store(!r.loaded, std::memory_order_relaxed);
    return plugin::PluginStatus::kOk;
}

plugin::FlushResult TextPlugin::flush(const plugin::FlushRequest& req) {
    plugin::FlushResult res;
    const bool cap_hit = config_.max_delta_chain > 0 &&
                         chain_.next_seq > config_.max_delta_chain;
    const bool want_base = req.force_rebase ||
                           rebase_needed_.load(std::memory_order_relaxed) ||
                           cap_hit;
    if (want_base) {
        if (save_component_base(dir_, req.watermark)) {
            rebase_needed_.store(false, std::memory_order_relaxed);
            res.covered_ord = req.watermark;
            res.generation = chain_.base_gen;
        } else {
            res.status = plugin::PluginStatus::kFailed;
            res.covered_ord = chain_.chain_wm;
        }
        return res;
    }
    if (!dirty()) {
        // 干净：no-op，覆盖水位停在当前链水位（宿主不推进 manifest）。
        res.covered_ord = chain_.chain_wm;
        res.generation = chain_.base_gen;
        return res;
    }
    auto d = save_component_delta(dir_, req.watermark);
    if (d.wrote) {
        res.covered_ord = req.watermark;
    } else {
        res.status = plugin::PluginStatus::kFailed;
        res.covered_ord = chain_.chain_wm;
    }
    res.generation = chain_.base_gen;
    return res;
}


void TextPlugin::on_merge_commit(const plugin::MergeCommitEvent&) {
    // S13-F6：compact 遍历 concurrent_hash_map 必须在 reducer 静止点执行
    // （与 add_doc 并发不安全）——经 run_serialized 投递（原 Cask 硬编码
    // RunFn 的插件自治版）。merge 后压实恒 rebase（compact 内自置标志）。
    constexpr double kMergeCompactDeadRatio = 0.2;
    if (host_) {
        host_->run_serialized([this] { compact(kMergeCompactDeadRatio); });
    } else {
        compact(kMergeCompactDeadRatio);  // 无宿主（standalone 测试）：直跑
    }
}

}  // namespace bitcask::text
