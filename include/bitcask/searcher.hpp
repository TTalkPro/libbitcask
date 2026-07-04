// Searcher 门面 — 类型化查询入口（S19-1，设计 doc/plugin-arch-split-design-zh.md §3.5/§7）。
//
// 「调用方自持插件对象，查询走类型化门面」的落地：`text::Searcher` /
// `vec::Searcher` / `search::CaskHybridSearcher` 持 Cask& + 对应插件引用，
// 每次查询先经 `cask.drain_plugins()` 读屏障（read-your-writes：submitted ⇒
// applied），再直调插件内核，错误经 `Cask::search_error_fault` 统一翻译。
//
// Cask 的 search_* 门面方法保留为薄委托（源兼容，deprecated——P6 删），
// 新代码推荐本门面：
//   auto* tp = cask.text_plugin();
//   text::Searcher ts(cask, *tp);
//   auto hits = ts.search_text("query", 10);
//
// 线程安全：与各插件查询内核一致（多读者并发安全）；批量入口经进程级
// 共享 Search 池（search_arena.hpp）。

#pragma once

#include "bitcask/cask.hpp"
#include "bitcask/hybrid_searcher.hpp"
#include "bitcask/search_arena.hpp"
#include "bitcask/text_plugin.hpp"
#include "bitcask/vector_plugin.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace bitcask::search::detail {

// 三门面（text/vec/hybrid）单查/批量共用骨架（S20-3 R5）。
//
// 单查询：读屏障（drain_plugins）→ 调内核 fn() → 错误经 search_error_fault
// 翻译 → 命中包装 TextSearchResult。fn 返回内核的 expected<vector<SearchHit>,
// SearchError>。
template <typename Fn>
[[nodiscard]] std::expected<TextSearchResult, CaskFault>
run_query(Cask& cask, const Fn& fn) {
    if (auto g = cask.drain_plugins(); !g) {
        return std::unexpected(g.error());
    }
    auto hits = fn();
    if (!hits) {
        return std::unexpected(Cask::search_error_fault(hits.error()));
    }
    return TextSearchResult{std::move(*hits)};
}

// 批量：一次读屏障覆盖全批 → n 条独立查询并发跑共享 Search 池（保序）→
// 逐槽翻译。fn(i) 返回第 i 条查询内核的 expected。
template <typename Fn>
[[nodiscard]] std::vector<std::expected<TextSearchResult, CaskFault>>
run_batch(Cask& cask, std::size_t n, const Fn& fn) {
    std::vector<std::expected<TextSearchResult, CaskFault>> out;
    if (n == 0) return out;
    if (auto g = cask.drain_plugins(); !g) {
        out.assign(n, std::unexpected(g.error()));
        return out;
    }
    out.resize(n, std::unexpected(CaskFault{}));  // 槽位占位，下方覆写
    parallel_for_queries(n, [&](std::size_t i) {
        auto hits = fn(i);
        if (hits) {
            out[i] = TextSearchResult{std::move(*hits)};
        } else {
            out[i] = std::unexpected(Cask::search_error_fault(hits.error()));
        }
    });
    return out;
}

}  // namespace bitcask::search::detail

namespace bitcask::text {

// BM25 文本查询门面。
class Searcher {
public:
    Searcher(Cask& cask, const TextPlugin& plugin)
        : cask_(cask), plugin_(plugin) {}

    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    search_text(std::string_view query, std::size_t k,
                const meta::MetaFilter* filter = nullptr) const {
        return search::detail::run_query(cask_, [&] {
            return plugin_.search_text(query, k, nullptr, filter);
        });
    }
    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    search_phrase(std::string_view query, std::size_t k) const {
        return search::detail::run_query(
            cask_, [&] { return plugin_.search_phrase(query, k); });
    }
    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    search_near(std::string_view query, std::uint32_t slop,
                std::size_t k) const {
        return search::detail::run_query(
            cask_, [&] { return plugin_.search_near(query, slop, k); });
    }
    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    bool_search(std::string_view query, std::size_t k) const {
        return search::detail::run_query(
            cask_, [&] { return plugin_.bool_search(query, k); });
    }
    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    search_fields(std::string_view query, std::size_t k) const {
        return search::detail::run_query(
            cask_, [&] { return plugin_.search_fields(query, k); });
    }
    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    search_fuzzy(std::string_view query, std::size_t k,
                 std::uint32_t max_edit_distance) const {
        return search::detail::run_query(cask_, [&] {
            return plugin_.search_fuzzy(query, k, max_edit_distance);
        });
    }
    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    search_wildcard(std::string_view pattern, std::size_t k) const {
        return search::detail::run_query(
            cask_, [&] { return plugin_.search_wildcard(pattern, k); });
    }
    // 高亮：返回类型为 SearchHitEx 向量（非 TextSearchResult），不套用共用
    // 骨架——单独物化。
    [[nodiscard]] std::expected<std::vector<search::SearchHitEx>, CaskFault>
    search_text_highlight(std::string_view query, std::size_t k,
                          const search::HighlightOptions& opts = {}) const {
        if (auto g = cask_.drain_plugins(); !g) {
            return std::unexpected(g.error());
        }
        auto hits = plugin_.search_text_highlight(query, k, opts);
        if (!hits) {
            return std::unexpected(Cask::search_error_fault(hits.error()));
        }
        return std::move(*hits);
    }
    // 批量：一次读屏障覆盖全批，N 条独立查询并发跑共享 Search 池（保序）。
    [[nodiscard]] std::vector<std::expected<TextSearchResult, CaskFault>>
    search_text_batch(std::span<const std::string_view> queries, std::size_t k,
                      const meta::MetaFilter* filter = nullptr) const {
        return search::detail::run_batch(
            cask_, queries.size(), [&](std::size_t i) {
                return plugin_.search_text(queries[i], k, nullptr, filter);
            });
    }

private:
    Cask&             cask_;
    const TextPlugin& plugin_;
};

}  // namespace bitcask::text

namespace bitcask::vec {

// HNSW 向量查询门面。
class Searcher {
public:
    Searcher(Cask& cask, const VectorPlugin& plugin)
        : cask_(cask), plugin_(plugin) {}

    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    search(std::span<const float> query, std::size_t k, std::size_t ef = 0,
           const meta::MetaFilter* filter = nullptr) const {
        return search::detail::run_query(
            cask_, [&] { return plugin_.search(query, k, ef, filter); });
    }
    [[nodiscard]] std::vector<std::expected<TextSearchResult, CaskFault>>
    search_batch(std::span<const std::span<const float>> queries,
                 std::size_t k, std::size_t ef = 0,
                 const meta::MetaFilter* filter = nullptr) const {
        return search::detail::run_batch(
            cask_, queries.size(), [&](std::size_t i) {
                return plugin_.search(queries[i], k, ef, filter);
            });
    }

private:
    Cask&               cask_;
    const VectorPlugin& plugin_;
};

}  // namespace bitcask::vec

namespace bitcask::search {

// RRF 混合查询门面（HybridSearcher 融合内核 + Cask 读屏障）。
class CaskHybridSearcher {
public:
    CaskHybridSearcher(Cask& cask, const HybridSearcher& hybrid)
        : cask_(cask), hybrid_(hybrid) {}

    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    search(std::string_view text_query, std::span<const float> vec_query,
           std::size_t k, const meta::MetaFilter* filter = nullptr) const {
        return search::detail::run_query(cask_, [&] {
            return hybrid_.search(text_query, vec_query, k, filter);
        });
    }

private:
    Cask&                 cask_;
    const HybridSearcher& hybrid_;
};

}  // namespace bitcask::search
