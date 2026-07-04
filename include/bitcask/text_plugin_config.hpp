// TextPluginConfig — BM25 文本插件配置（S20-4 自 text_plugin.hpp 抽出）。
//
// 配置 POD 独立成头：使配置聚合层（search_config.hpp）只依赖配置结构、不
// 拖入完整 TextPlugin 定义（inverted.hpp + oneTBB）。text_plugin.hpp 转而
// 包含本头，既有使用点零改动。

#pragma once

#include "bitcask/analyzer.hpp"
#include "bitcask/bm25_params.hpp"
#include "bitcask/synonym_map.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace bitcask::text {

// 文本插件配置（原 SearchLayerConfig 的文本相关字段，由
// SearchLayerConfig::text_config() 产出）。各字段语义见 search_config.hpp。
struct TextPluginConfig {
    AnalyzerConfig       analyzer_config;
    bm25::Bm25Params     bm25_params;
    std::size_t          cache_max_entries = 256;
    std::size_t          doc_text_cache_max = 1024;
    bool                 index_positions = true;
    double               auto_compact_dead_ratio = 0.0;  // S12-2
    std::shared_ptr<const SynonymMap> synonym_map;       // S11：open-time 不可变
    // S18-6（S14-5 语义每插件化）：delta 链长上限，达到后 flush 强制 base。
    // 0 = 不设限。
    std::uint32_t        max_delta_chain = 64;
};

}  // namespace bitcask::text
