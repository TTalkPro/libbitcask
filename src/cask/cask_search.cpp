// cask_search.cpp — Cask 搜索门面：错误翻译、查询读屏障、单条/批量查询
// 骨架与全部 search_* 入口。S21-3 B1：从 cask.cpp 纯物理平移拆出（函数体
// 不变），先例同 meta_file.cpp / legacy_ckpt.cpp。
#include "bitcask/cask.hpp"

#include "bitcask/search_arena.hpp"  // S19-2：批量查询并发入口（原经 shim 头）

#include "cask_internal.hpp"  // err

namespace bitcask {

// S9-P2-d: 搜索层 SearchError → CaskFault 边界翻译（消除 expected<,string> 的
// leaky abstraction）。当前三种都映射 kInvalidOption，但语义集中在此一处、
// detail 文案由枚举确定性派生——新增搜索错误时只改这里，不必各 caller 猜 kind。
// S19-1：提为 Cask 静态成员（Searcher 门面共享同一翻译）。
CaskFault Cask::search_error_fault(search::SearchError e) {
    switch (e) {
        case search::SearchError::kNoVectorIndex:
            return err(CaskError::kInvalidOption, "no vector index configured");
        case search::SearchError::kVectorDimMismatch:
            return err(CaskError::kInvalidOption, "query vector dim mismatch");
        case search::SearchError::kEmptyHybridQuery:
            return err(CaskError::kInvalidOption,
                       "hybrid query empty (no text, no vector)");
    }
    return err(CaskError::kInvalidOption, "unknown search error");
}

static CaskFault search_fault(search::SearchError e) {
    return Cask::search_error_fault(e);
}

// S14-4/S19-2：merge/close 收链入口（原 SearchLayer::force_ckpt_rebase）。
void Cask::force_ckpt_rebase() {
    ckpt_rebase_needed_.store(true, std::memory_order_relaxed);
    if (text_) text_->force_rebase();
    if (vec_plugin_) vec_plugin_->force_rebase();
}

// S19-1：查询读屏障公开化（Searcher 门面消费；语义 = closed 检查 +
// prepare_search 的 flush 读屏障）。
std::expected<void, CaskFault> Cask::drain_plugins() {
    if (is_closed()) {
        return std::unexpected(err(CaskError::kClosed, "cask is closed"));
    }
    return prepare_search();
}

// S8-R3: 单条搜索公共骨架。flush → 可选 vector 校验 → 跑内核 → 包错误/结果。
std::expected<TextSearchResult, CaskFault>
Cask::run_search_one(
    bool require_vector,
    const std::function<
        std::expected<std::vector<search::SearchHit>, search::SearchError>()>& run) {
    if (is_closed()) return std::unexpected(err(CaskError::kClosed, "cask is closed"));  // S11-W3
    if (auto g = prepare_search(); !g) return std::unexpected(g.error());
    if (require_vector && meta_config_.vector_dim == 0) {
        return std::unexpected(err(CaskError::kInvalidOption,
            "collection has no vector config"));
    }
    auto hits = run();
    if (!hits) return std::unexpected(search_fault(hits.error()));
    return TextSearchResult{std::move(*hits)};
}

// search_vector：HNSW 向量检索(V3.3)。归一化/live 过滤/ord 翻译都在
// VectorPlugin（live/ord 经 DocTable）。
std::expected<TextSearchResult, CaskFault>
Cask::search_vector(std::span<const float> query, std::size_t k,
                     std::size_t ef, const meta::MetaFilter* filter) {
    return run_search_one(/*require_vector=*/true,
        [&] { return vec_plugin_->search(query, k, ef, filter); });
}

// search_hybrid:RRF 混合检索(V3.6)。两路检索与 RRF 融合在
// search::HybridSearcher（Cask 门面只做校验 + 委托）。
std::expected<TextSearchResult, CaskFault>
Cask::search_hybrid(std::string_view text_query,
                     std::span<const float> vec_query, std::size_t k,
                     const meta::MetaFilter* filter) {
    return run_search_one(/*require_vector=*/true,
        [&] { return hybrid_->search(text_query, vec_query, k, filter); });
}

// search_text：BM25 词袋模式搜索。
std::expected<TextSearchResult, CaskFault>
Cask::search_text(std::string_view query, std::size_t k,
                  const meta::MetaFilter* filter, std::size_t offset) {
    return run_search_one(/*require_vector=*/false,
        [&] {
            auto hits = text_->search_text(query, k + offset, nullptr, filter);
            if (hits && offset > 0) {  // S13-D10：overfetch 后丢前 offset 条
                if (hits->size() > offset) {
                    hits->erase(hits->begin(),
                                hits->begin() + static_cast<std::ptrdiff_t>(offset));
                } else {
                    hits->clear();
                }
            }
            return hits;
        });
}

// S13-D3：带高亮搜索——补门面缺口（README 宣称有而 Cask 无）。骨架与
// run_search_one 相同（closed fail-fast → prepare_search flush → 内核 →
// 错误翻译），仅命中类型是 SearchHitEx、无法复用泛型骨架。
std::expected<Cask::HighlightSearchResult, CaskFault>
Cask::search_text_highlight(std::string_view query, std::size_t k,
                            const search::HighlightOptions& opts) {
    if (is_closed()) return std::unexpected(err(CaskError::kClosed, "cask is closed"));  // S11-W3
    if (auto g = prepare_search(); !g) return std::unexpected(g.error());
    auto hits = text_->search_text_highlight(query, k, opts);
    if (!hits) return std::unexpected(search_fault(hits.error()));
    return HighlightSearchResult{std::move(*hits)};
}

// S7-4: 批量搜索公共骨架。空批早退 → 一次 prepare_search（flush 覆盖全批）→
// 可选向量配置校验 → N 条查询并发跑共享 Search 池，保序写各自结果槽。
std::vector<std::expected<TextSearchResult, CaskFault>>
Cask::run_search_batch(
    std::size_t n, bool require_vector,
    const std::function<
        std::expected<TextSearchResult, CaskFault>(std::size_t)>& run_one) {
    std::vector<std::expected<TextSearchResult, CaskFault>> out(n);
    if (n == 0) return out;
    if (is_closed()) {  // S11-W3：全槽同 closed 错误
        for (auto& o : out)
            o = std::unexpected(err(CaskError::kClosed, "cask is closed"));
        return out;
    }
    // 前置校验一次覆盖全批（所有查询共享同一 search_/lane）；失败 → 全槽同错。
    if (auto g = prepare_search(); !g) {
        for (auto& o : out) o = std::unexpected(g.error());
        return out;
    }
    if (require_vector && meta_config_.vector_dim == 0) {
        for (auto& o : out)
            o = std::unexpected(err(CaskError::kInvalidOption,
                                    "collection has no vector config"));
        return out;
    }
    // 各槽独立、互不重叠 → 无需锁。grainsize=1：每 item 是一条完整重查询。
    search::parallel_for_queries(n, [&](std::size_t i) { out[i] = run_one(i); });
    return out;
}

// S7-4: 批量文本搜索——K 条独立查询并发跑共享 Search 池，保序返回。
std::vector<std::expected<TextSearchResult, CaskFault>>
Cask::search_text_batch(std::span<const std::string_view> queries,
                        std::size_t k, const meta::MetaFilter* filter) {
    return run_search_batch(queries.size(), /*require_vector=*/false,
        [&](std::size_t i) -> std::expected<TextSearchResult, CaskFault> {
            auto hits = text_->search_text(queries[i], k, nullptr, filter);
            if (!hits) return std::unexpected(search_fault(hits.error()));
            return TextSearchResult{std::move(*hits)};
        });
}

// S7-4: 批量向量检索——K 条独立查询并发跑共享 Search 池，保序返回。
std::vector<std::expected<TextSearchResult, CaskFault>>
Cask::search_vector_batch(std::span<const std::span<const float>> queries,
                          std::size_t k, std::size_t ef,
                          const meta::MetaFilter* filter) {
    return run_search_batch(queries.size(), /*require_vector=*/true,
        [&](std::size_t i) -> std::expected<TextSearchResult, CaskFault> {
            auto hits = vec_plugin_->search(queries[i], k, ef, filter);
            if (!hits) return std::unexpected(search_fault(hits.error()));
            return TextSearchResult{std::move(*hits)};
        });
}

// S7-4: 批量 hybrid 检索——K 条独立 (text,vec) 查询并发跑共享 Search 池。
std::vector<std::expected<TextSearchResult, CaskFault>>
Cask::search_hybrid_batch(std::span<const HybridQuery> queries,
                          std::size_t k, const meta::MetaFilter* filter) {
    return run_search_batch(queries.size(), /*require_vector=*/true,
        [&](std::size_t i) -> std::expected<TextSearchResult, CaskFault> {
            auto hits = hybrid_->search(queries[i].text, queries[i].vec, k, filter);
            if (!hits) return std::unexpected(search_fault(hits.error()));
            return TextSearchResult{std::move(*hits)};
        });
}

// search_phrase：BM25 短语模式搜索。
std::expected<TextSearchResult, CaskFault>
Cask::search_phrase(std::string_view query, std::size_t k,
                    std::size_t offset) {
    return run_search_one(/*require_vector=*/false,
        [&] {
            auto hits = text_->search_phrase(query, k + offset);
            if (hits && offset > 0) {  // S13-D10
                if (hits->size() > offset) {
                    hits->erase(hits->begin(),
                                hits->begin() + static_cast<std::ptrdiff_t>(offset));
                } else {
                    hits->clear();
                }
            }
            return hits;
        });
}

// search_fields：BM25 多字段搜索（S8.6），支持 field:term^boost。
std::expected<TextSearchResult, CaskFault>
Cask::search_fields(std::string_view query, std::size_t k) {
    return run_search_one(/*require_vector=*/false,
        [&] { return text_->search_fields(query, k); });
}

// search_near：BM25 近邻搜索（S8.7）。
std::expected<TextSearchResult, CaskFault>
Cask::search_near(std::string_view query, std::uint32_t slop, std::size_t k) {
    return run_search_one(/*require_vector=*/false,
        [&] { return text_->search_near(query, slop, k); });
}

// bool_search：BM25 布尔搜索（AND/OR/NOT）。
std::expected<TextSearchResult, CaskFault>
Cask::bool_search(std::string_view query, std::size_t k,
                  std::size_t offset) {
    return run_search_one(/*require_vector=*/false,
        [&] {
            auto hits = text_->bool_search(query, k + offset);
            if (hits && offset > 0) {  // S13-D10
                if (hits->size() > offset) {
                    hits->erase(hits->begin(),
                                hits->begin() + static_cast<std::ptrdiff_t>(offset));
                } else {
                    hits->clear();
                }
            }
            return hits;
        });
}

// S8.3：模糊搜索（Levenshtein 编辑距离匹配）。
std::expected<TextSearchResult, CaskFault>
Cask::search_fuzzy(std::string_view query, std::size_t k, std::uint32_t max_edit_distance) {
    return run_search_one(/*require_vector=*/false,
        [&] { return text_->search_fuzzy(query, k, max_edit_distance); });
}

// S8.4：通配符搜索（* / ? 模式匹配）。
std::expected<TextSearchResult, CaskFault>
Cask::search_wildcard(std::string_view pattern, std::size_t k) {
    return run_search_one(/*require_vector=*/false,
        [&] { return text_->search_wildcard(pattern, k); });
}

}  // namespace bitcask
