// bitcask/term_index.hpp — 段内单字段倒排的纯查询接口(S30-P1 Slice 4)。
//
// 动机:S27 的段查询视图(SegmentView/FieldSegmentView)原来直接持
// `const InvertedIndex*`——封口段 mmap 化(S30)后,段字段的查询实现有两个:
// 内存 InvertedIndex(building/legacy)与 MmapSegment 按需解码(封口 v2)。
// 本接口把「查询面」从「存储形态」解耦:视图持 `const TermIndex*`,消费方
// (multi_segment_search / multi_field_segment_search / TextPlugin 逐段
// 查询)一行不改。
//
// 实现方:
//   - bm25::InvertedIndex(直接继承——方法签名逐一相同,零适配层);
//   - search::MmapFieldIndex(segment_v2.hpp,per-field 薄适配,委托
//     MmapSegment::*(field, ...))。
// 两侧算法共享 src/bm25/bm25_search_impl.hpp(分数位级一致由同一实现保证)。
//
// 契约:
//   - 全部方法 const、线程安全(并发查询);
//   - docid 语义 = 段内本地(与 LiveChecker 一致);
//   - 默认实参必须与实现方声明**逐一相同**(经接口指针调用取接口默认值,
//     经具体类型调用取其自身默认值——两处不一致会造成同名调用行为分叉)。
//
// 有意不入接口:写路径(add_doc/…)、持久化(serialize/…)、内省
// (total_postings/df/df_live)——它们是存储形态专属,消费方按具体类型调用。

#pragma once

#include "bitcask/bm25_params.hpp"
#include "bitcask/live_checker.hpp"
#include "bitcask/query.hpp"  // QueryNode

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bitcask::bm25 {

struct SearchResult;      // inverted.hpp
struct ExtStats;          // inverted.hpp
struct ScoreExplanation;  // inverted.hpp

class TermIndex {
public:
    virtual ~TermIndex() = default;

    // ---- 统计(G-on-the-fly 聚合用) ----
    [[nodiscard]] virtual std::uint64_t live_doc_count() const = 0;
    [[nodiscard]] virtual std::uint64_t sum_doc_len() const = 0;
    [[nodiscard]] virtual std::uint64_t doc_freq(std::string_view term) const = 0;

    // ---- 查询面(语义详见 InvertedIndex 同名方法) ----
    [[nodiscard]] virtual auto search(
        const std::vector<std::string>& query_terms,
        std::size_t k,
        const LiveChecker& live_checker,
        const Bm25Params* params_override = nullptr,
        const ExtStats* ext = nullptr) const -> std::vector<SearchResult> = 0;

    [[nodiscard]] virtual auto search_phrase(
        const std::vector<std::string>& query_terms,
        std::size_t k,
        const LiveChecker& live_checker,
        const Bm25Params* params_override = nullptr) const
        -> std::vector<SearchResult> = 0;

    [[nodiscard]] virtual auto search_near(
        const std::vector<std::string>& query_terms,
        std::size_t k,
        std::uint32_t slop,
        const LiveChecker& live_checker,
        const Bm25Params* params_override = nullptr) const
        -> std::vector<SearchResult> = 0;

    [[nodiscard]] virtual auto bool_search(
        const QueryNode& query,
        std::size_t k,
        const LiveChecker& live_checker,
        const Bm25Params* params_override = nullptr) const
        -> std::vector<SearchResult> = 0;

    [[nodiscard]] virtual auto bool_search_tree(
        const QueryNode& root,
        std::size_t k,
        const LiveChecker& live_checker,
        const Bm25Params* params_override = nullptr) const
        -> std::vector<SearchResult> = 0;

    [[nodiscard]] virtual auto search_fuzzy(
        const std::vector<std::string>& query_terms,
        std::size_t k,
        std::uint32_t max_edit_distance,
        const LiveChecker& live_checker,
        const Bm25Params* params_override = nullptr) const
        -> std::vector<SearchResult> = 0;

    [[nodiscard]] virtual auto search_wildcard(
        const std::string& pattern,
        std::size_t k,
        const LiveChecker& live_checker,
        const Bm25Params* params_override = nullptr) const
        -> std::vector<SearchResult> = 0;

    [[nodiscard]] virtual auto explain(
        const std::vector<std::string>& query_terms,
        std::uint64_t ord,
        const LiveChecker& live_checker,
        const Bm25Params* params_override = nullptr) const
        -> ScoreExplanation = 0;
};

}  // namespace bitcask::bm25
