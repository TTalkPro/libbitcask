// Bm25Params — BM25 可调参数（S20-4 自 inverted.hpp 抽出的轻量头）。
//
// 独立成头，使配置 POD（TextPluginConfig / SearchLayerConfig）无须为取此
// 结构而拖入完整 inverted.hpp（InvertedIndex + oneTBB）。inverted.hpp 转而
// 包含本头，既有使用点零改动。

#pragma once

namespace bitcask::bm25 {

// BM25 可调参数。
struct Bm25Params {
    float k1 = 1.2F;
    float b  = 0.75F;
    // BM25+ 的下界常数 δ（S8.10）：每个在文档中出现的 term 的 tf 归一化项加 δ，
    // 缓解标准 BM25 对长文档的过度惩罚（Lv & Zhai 2011）。
    // 默认 0 = 标准 BM25（向后兼容）。典型值 1.0。
    float delta = 0.0F;
};

}  // namespace bitcask::bm25
