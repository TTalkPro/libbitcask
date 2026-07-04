// 搜索结果/错误共享类型（S18-3 自 search_layer.hpp 抽出）。
//
// P4 拆分后 TextPlugin/VectorPlugin/HybridSearcher 与 SearchLayer shim
// 共用这组类型——独立成头避免插件头反向依赖 search_layer.hpp。

#pragma once

#include "bitcask/analyzer.hpp"     // TermPositionsMap（ReduceJob 载荷）
#include "bitcask/highlighter.hpp"  // Snippet（SearchHitEx）

#include <cstdint>
#include <string>
#include <vector>

namespace bitcask::search {

// 默认字段哨兵（S8.6）：旧单-text 文档与无字段限定查询都映射到此字段，
// 使新旧路径收敛；用不可见前缀避免与用户字段名冲突。
// 值为 4 字节 0xFA 'u' 'l' 't'（原意 "\x01default"，但 `\x` 会贪婪吞并后续
// hex 位 "defa" → clang 报错、GCC 静默截断）。写法拆开转义让两端都能编译，
// 字节内容不变——已作 fields_ 键、可能入 checkpoint，不能改。
inline constexpr std::string_view kDefaultField = "\xfa" "ult";

// 搜索结果条目。
struct SearchHit {
    std::string   key;   // 外部 key（由 ord 经 DocTable::ord_to_ext 翻译）
    std::uint64_t ord;   // 文档 ord
    double        score; // BM25 / 距离 / RRF 分数（按查询类型）
};

// S9-P2-d：搜索层错误类型。强类型枚举只表达「哪类错误」，由 Cask 边界
// （cask.cpp `search_fault`）翻译成 CaskFault（kind + 可读 detail）。
// 当前三种都映射到 CaskError::kInvalidOption。
enum class SearchError {
    kNoVectorIndex,       // 无向量索引配置（hnsw 为空）
    kVectorDimMismatch,   // 查询向量维度与配置不符
    kEmptyHybridQuery,    // hybrid 两路皆空（无文本、无向量）
};

// 带高亮的搜索结果。
struct SearchHitEx {
    std::string              key;
    std::uint64_t            ord;
    double                   score;
    std::vector<Snippet>     highlights;
};

// S6-P0: map_analyze 的产出 / apply（reduce 相）的输入。
// 把「纯函数 analyze + catch-all 合并」的结果封装为一个 owning 结构，
// 供 reducer 在锁下逐字段 apply（跨线程传递）。
struct ReduceJob {
    std::string          key;           // owning key (apply 要用)
    std::uint64_t        ord = 0;

    // 每字段的分词结果（field_name 已映射：空名 → kDefaultField）。
    // terms 可能为空（该字段无有效 token）→ apply 跳过 add_doc。
    struct FieldResult {
        std::string                   field_name;
        text::TermPositionsMap        terms;
        std::uint32_t                 doc_len = 0;  // Σ tf
    };
    std::vector<FieldResult> fields;

    std::uint32_t        total_doc_len = 0;

    // catch-all 合并结果（非默认字段词项合并到默认字段，使 search_text 能命中多字段文档）
    text::TermPositionsMap ca_data;      // 空 = 无需 catch-all add_doc
    std::uint32_t        ca_len = 0;
    bool                 wrote_default = false;  // 有字段直接写了默认字段 → 跳过 catch-all

    // 高亮原文缓存（默认取第一个字段的 text）
    std::string          doc_text;

    // DocSlot 定位数据
    std::uint32_t        file_id = 0;
    std::uint64_t        offset = 0;
    std::uint32_t        total_sz = 0;
    std::uint32_t        tstamp = 0;
};

}  // namespace bitcask::search
