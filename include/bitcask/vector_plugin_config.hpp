// VectorPluginConfig — HNSW 向量插件配置（S20-4 自 vector_plugin.hpp 抽出）。
//
// 配置 POD 独立成头：使配置聚合层（search_config.hpp）只依赖配置结构、不
// 拖入完整 VectorPlugin 定义（hnsw.hpp）。vector_plugin.hpp 转而包含本头，
// 既有使用点零改动。

#pragma once

#include "bitcask/meta_file.hpp"   // meta::VectorMetric

#include <cstdint>

namespace bitcask::vec {

// 向量插件配置（原 SearchLayerConfig 的 vector 相关字段，由
// SearchLayerConfig::vector_config() 产出）。
struct VectorPluginConfig {
    std::uint16_t      dim = 0;                               // 0 = 无向量
    meta::VectorMetric metric = meta::VectorMetric::kNone;
    // S13-D11：建图参数（0 = HnswConfig 默认）。
    std::uint32_t      hnsw_m = 0;
    std::uint32_t      hnsw_ef_construction = 0;
    bool               inmem_int8 = false;                    // P5b
    // S18-6（S14-5 语义每插件化）：delta 链长上限。0 = 不设限。
    std::uint32_t      max_delta_chain = 64;
};

}  // namespace bitcask::vec
