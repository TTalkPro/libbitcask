// 搜索结果高亮/摘要生成器。

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bitcask/analyzer.hpp"

namespace bitcask::search {

// 高亮片段。
struct Snippet {
    std::string text;
    double      score;
};

// 高亮配置。
struct HighlightOptions {
    std::string pre_tag  = "<em>";
    std::string post_tag = "</em>";
    std::size_t fragment_size = 120;
    std::size_t max_fragments = 3;
};

// S53：高亮时原文 LRU 未命中（「冷」文档：本进程没写过它——新开进程 / 只读快照 / 重启后 /
// 超过 doc_text_cache_max）怎么办。kSkip = 原行为：该条命中照常返回、片段为空；
// kLoad = 宿主从存储读回正文再切片段（每条冷命中一次点查；见 Cask::search_text_highlight）。
enum class ColdText : std::uint8_t {
    kSkip = 0,
    kLoad = 1,
};

// 高亮结果。
struct HighlightResult {
    std::vector<Snippet> snippets;
};

// 生成高亮片段。
HighlightResult highlight(
    std::string_view text,
    const std::unordered_map<std::string, std::vector<text::TokenInfo>>& query_token_offsets,
    const HighlightOptions& opts = {});

}  // namespace bitcask::search