// 多段查询归并（S27-2 Slice 2）。见 doc/segment-index-design-zh.md §3.5 / §4。
//
// 本头实现「串行逐段 + G-on-the-fly 共享 idf + 大小 k 并集归并」。此阶段只做
// **内存段的只读查询视图**（SegmentView），尚无落盘/段管理器（Slice 3/4）。
// 阈值传播（§3.5：把全局第 k 名分数当下一段 WAND floor）是**纯剪枝优化、
// 不改结果**，留待后续 slice（search_wand 加 floor 参数）——本版逐段各查 k、
// 全量并集，结果与之等价。

#pragma once

#include "bitcask/index_ids.hpp"
#include "bitcask/inverted.hpp"      // InvertedIndex / ExtStats / Bm25Params
#include "bitcask/live_checker.hpp"
#include "bitcask/search_types.hpp"  // SearchHit

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bitcask::search {

// 段的最小只读查询视图。倒排/存活按**段内本地 docid**寻址；两个 hydrate 闭包
// 把本地 docid 翻成外部 key（结果水合）与全局 LSN（版本/RRF 键，见 §3.4）。
// 分段化落地后由段 doc_store 承担这两个映射；此处闭包化以便先驱动查询归并。
struct SegmentView {
    // S30-P1 Slice 4:查询接口指针——内存段(InvertedIndex)与 mmap 段
    // (MmapFieldIndex)同型接入,消费方无感。
    const bm25::TermIndex*            inv;     // 段内倒排查询面（本地 docid）
    const bm25::LiveChecker*          live;    // 段内 live/doc_len（本地 docid）
    std::function<std::string(DocId)> key_of;  // 本地 docid → 外部 key
    std::function<Lsn(DocId)>         lsn_of;  // 本地 docid → 全局 LSN
    // S27-3 步骤 5:段生命周期钉住——查询期间段可能被并发 drop(段压实)/
    // 封口替换,view 持 shared_ptr 保段存活(见 SegmentSet 快照 API)。
    // `{}`:钉段是**调用方**的事(text_plugin.cpp 拿到 view 后按需 v.pin = ...),
    // 绝大多数构造点本就该留空。给默认成员初始化器,免得每个聚合初始化点都要
    // 补一个 `{}` 才能过 -Wmissing-field-initializers。
    std::shared_ptr<const void>       pin{};
};

// §3.5 多段查询：① 跨段聚合全局 N/sum_dl/df（G-on-the-fly）② 串行逐段用**同一
// idf/avgdl** 打分 ③ 大小 k 的并集归并（一个 doc 只在一个段，故并集不求和）。
// 返回按分数降序（并列以 key 升序稳定）的 top-k。
[[nodiscard]] inline std::vector<SearchHit> multi_segment_search(
    std::span<const SegmentView> segs,
    const std::vector<std::string>& terms,
    std::size_t k,
    const bm25::Bm25Params* params = nullptr) {
    if (terms.empty() || k == 0) return {};

    // ---- 阶段 1：G-on-the-fly 全局统计 ----
    // S29-5：global_df 扁平化 + thread_local 槽位复用（原每查询新建
    // unordered_map + 每 term 一个节点分配）。resize 到本查询词数（收缩即
    // 截掉上次查询的残留槽，防止消费侧误匹配），槽内 string assign 复用
    // 容量——稳态零分配。
    bm25::ExtStats ext;
    static thread_local std::vector<std::pair<std::string, std::uint64_t>>
        global_df;
    global_df.resize(terms.size());
    for (std::size_t i = 0; i < terms.size(); ++i) {
        global_df[i].first.assign(terms[i]);
        global_df[i].second = 0;
    }
    for (const auto& s : segs) {
        ext.N += s.inv->live_doc_count();
        ext.sum_dl += s.inv->sum_doc_len();
        for (auto& [t, df] : global_df) df += s.inv->doc_freq(t);
    }
    ext.df = &global_df;

    // ---- 阶段 2：串行逐段打分（共享 idf）+ 并集收集 ----
    std::vector<SearchHit> merged;
    for (const auto& s : segs) {
        auto hits = s.inv->search(terms, k, *s.live, params, &ext);
        for (const auto& h : hits) {
            const auto docid = static_cast<DocId>(h.ord);  // 段内本地 docid
            merged.push_back(SearchHit{s.key_of(docid), s.lsn_of(docid),
                                       static_cast<double>(h.score)});
        }
    }

    // ---- 全局 top-k：分数降序，并列以 key 升序稳定 ----
    const auto cmp = [](const SearchHit& a, const SearchHit& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.key < b.key;
    };
    if (merged.size() > k) {
        std::partial_sort(merged.begin(),
                          merged.begin() + static_cast<std::ptrdiff_t>(k),
                          merged.end(), cmp);
        merged.resize(k);
    } else {
        std::sort(merged.begin(), merged.end(), cmp);
    }
    return merged;
}

}  // namespace bitcask::search
