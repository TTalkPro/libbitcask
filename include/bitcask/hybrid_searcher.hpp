// HybridSearcher — RRF 混合检索融合器（S18-9，设计 doc/plugin-arch-split-design-zh.md §6）。
//
// 原 SearchLayer::search_hybrid 的整体上移：持 TextPlugin/VectorPlugin 的
// 查询接口引用，两路各超采 K'=max(4k,64)，RRF(60) 融合，ord 决胜——纯算法，
// **非插件**（只装一个插件的部署不链接它，见设计 §8 bitcask_hybrid 目标）。
//
// 线程安全：同两条内核（text 路同 search_text，vec 路同 VectorPlugin::search）。

#pragma once

#include "bitcask/text_plugin.hpp"
#include "bitcask/vector_engine_plugin.hpp"  // S32-M3：引擎契约

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace bitcask::search {

class HybridSearcher {
public:
    HybridSearcher(const text::TextPlugin& text,
                   const vec::VectorEnginePlugin& vec)
        : text_(text), vec_(vec) {}

    // V3.6：RRF 混合检索（hnsw-design §4）。两路各取 K'=max(k×4, 64)；
    // 融合 score(doc)=Σ_路 1/(60+rank_路)，rank 从 1 起，单路文档只累加该
    // 路项（无需分数归一化）；平局取 ord 小者。一路空 → 退化纯另一路，
    // 两路皆空或 vec 维度不符 → 错误。V5：filter 同时作用两路（text 路
    // overfetch 后过滤、vec 路折进 HNSW live callback）。
    // S7：单查询两路串行（实测常见情形并行不赢；inter-query 并发另有
    // search_arena()，见 TASK.md S7-3/S7-4）。
    [[nodiscard]] std::expected<std::vector<SearchHit>, SearchError>
    search(std::string_view text_query, std::span<const float> vec_query,
           std::size_t k, const meta::MetaFilter* filter = nullptr) const;

private:
    const text::TextPlugin&  text_;
    const vec::VectorEnginePlugin& vec_;
};

}  // namespace bitcask::search
