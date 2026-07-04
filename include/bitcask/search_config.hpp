// SearchLayerConfig — 搜索配置聚合（S19-2 自 search_layer.hpp 抽出）。
//
// 公开配置面（CaskOptions::search_config 的载荷）：一份聚合配置，经
// text_config()/vector_config() 拆分产出两插件各自的配置子集。
// `CaskOptions::plugins` 换代（调用方注入插件对象）留待真有第三方插件
// 需求时再做（P5 决策，见 TASK.md S19 批次头）。

#pragma once

// S20-4 B-C1：只依赖两插件的配置 POD 头（轻量），不再拖入完整插件定义
// （inverted.hpp/hnsw.hpp）——配置聚合层与插件实现层解耦。
#include "bitcask/analyzer.hpp"
#include "bitcask/meta_file.hpp"
#include "bitcask/synonym_map.hpp"
#include "bitcask/text_plugin_config.hpp"
#include "bitcask/vector_plugin_config.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace bitcask::search {

// SearchLayer 配置。
struct SearchLayerConfig {
    text::AnalyzerConfig analyzer_config;
    bm25::Bm25Params     bm25_params;
    std::size_t          cache_max_entries = 256;  // 缓存最大条目数，0 禁用
    // 高亮原文 LRU 上限（S9.3）：只缓存最近写入/查询的文档原文，避免全文常驻。
    // 0 表示不缓存（高亮恒拿不到原文 → 降级为无片段），默认 1024 篇。
    std::size_t          doc_text_cache_max = 1024;
    // 是否索引词位置（S10.10）。默认 true。置 false 时倒排不存 positions，
    // 大幅省内存——代价：search_phrase / search_near 失效（无位置可匹配，返回空）。
    // 仅做 search_text/bool/fuzzy/wildcard 的部署可关闭。
    bool                 index_positions = true;
    // V3.3:向量配置(Cask::open 从 meta 透传)。dim>0 时构造 HnswIndex;
    // metric 映射:kCosineNormalized/kDot → HnswMetric::kDot(cosine 已在
    // 写入端归一化),kL2 → kL2。
    std::uint16_t        vector_dim = 0;
    meta::VectorMetric   vector_metric = meta::VectorMetric::kNone;
    // S13-D11：HNSW 建图参数透传（0 = HnswConfig 默认：M=16、ef_construction=200）。
    // 高召回调大、低内存调小。改参数只影响新插入与 merge 期 rebuild 的图，不追溯
    // 改写既有 checkpoint（属调优参数，不入 meta 格式校验）。
    std::uint32_t        hnsw_m = 0;
    std::uint32_t        hnsw_ef_construction = 0;
    // P5b:HNSW int8-only 内存模式(Cask::open 从 meta 透传)。仅 kDot。
    bool                 vector_inmem_int8 = false;
    // wal_batch_size 已删除（S19-4）：S18 侦查坐实 dead config——从未接线
    // InvertedIndex::enable_wal，两档行为相同。WAL 机制本体保留（倒排内部
    // + 测试直用 enable_wal），将来真接线时以新字段回归。
    // S12-2：后台自动 compaction 的 per-list 死占比阈值。
    //   0（默认） → 关：索引流水线零开销（仅一次 double 比较）。
    //   (0,1]     → 开：reducer 线程内累计退休文档达节流阈值（max(1024, live/2)）时，
    //               对死占比 ≥ 本值的 posting list 触发一次 compact()；与 add_doc 同线程
    //               串行，无并发窗口。
    // 效果：posting list 内存随 churn 有界，不再依赖 merge 回收。代价：触发时短暂扫描压实，
    // 延迟后续文档的**索引可见性**（非 durability——数据已落 data file）。
    double               auto_compact_dead_ratio = 0.0;
    // 同义词词典（open-time，不可变）。由 Cask::open 从 CaskOptions::synonym_map
    // 透传进来（同 vector_dim 的注入方式）。构造后只读 → 并发查询安全，无需锁。
    // 空 = 不展开同义词。
    std::shared_ptr<const text::SynonymMap> synonym_map;
    // S14-5：delta 链长上限。链达此长度后下次 save 强制全量 base（坍缩链、回收
    // delta 文件）——否则纯追加负载（无删除 ⇒ 不触发 merge）会随写入线性堆积、
    // 永不回收（向量库尤甚：每 delta 内联 f32 向量）。权衡：小 → base 重序列化更
    // 频繁（∝ 索引总量）；大 → 崩溃恢复重放更长、磁盘冗余更多。0 = 不设限（不建议）。
    std::uint32_t max_delta_chain = 64;

    // S18-3/4：配置拆分（设计 §6）——产出两插件各自的配置子集。公共 API
    // 面（CaskOptions::search_config）本批不变，P5 换代。
    [[nodiscard]] text::TextPluginConfig text_config() const {
        return {analyzer_config, bm25_params, cache_max_entries,
                doc_text_cache_max, index_positions,
                auto_compact_dead_ratio, synonym_map, max_delta_chain};
    }
    [[nodiscard]] vec::VectorPluginConfig vector_config() const {
        return {vector_dim, vector_metric, hnsw_m, hnsw_ef_construction,
                vector_inmem_int8, max_delta_chain};
    }
};

}  // namespace bitcask::search
