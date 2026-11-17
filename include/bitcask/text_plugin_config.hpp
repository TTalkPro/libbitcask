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
    // S26-2：是否把非默认字段词项合并进默认字段（catch-all）。true（默认）=
    // 既有行为，search_text 能命中多字段文档，代价每词 add_doc 两遍。false =
    // 只建各命名字段倒排（省 reducer/内存/ckpt ~半），多字段文档只能经
    // search_fields 字段限定命中。纯默认字段写入不受影响。
    bool                 index_catch_all = true;
    // S27-4 P2/P3:builder 线程数。**S45-4 起忽略**(open 时告警一次):索引
    // 池的分片线程即 builder——每分片一个 building 段、同 key 串行、不同 key
    // 并行,并行度 = index_workers。保留字段只为不破坏下游配置解析。
    std::size_t          builder_threads = 0;
    // S30-P2:封口段格式。true(默认)= v2 mmap 段——封口即流式落盘、查询
    // 走 mmap 按需解码,**内存副本释放**(倒排出内存的主开关);false = v1
    // 全量驻留(回退开关)。两格式恢复期均可读(load_any 探 magic)。
    bool                 seal_v2_segments = true;
    // S30-P2:building 段 RAM 预算(字节,近似记账见
    // SealedSegment::approx_ram_bytes)。>0 时 apply 路径超预算**就地封口**
    // (不等 checkpoint)→ 写入期 RSS ≈ 预算 × (1+builder_threads);
    // 0(默认)= 关闭,沿用 64K 文档阈值 + ckpt 封口。段数随之增长,
    // 收敛依赖段 merge(S30-P3)。
    std::size_t          seal_ram_budget_bytes = 0;
    // S30-P3:段合并扇入。checkpoint 静止点触发 tiered merge:① 死点占比
    // ≥1/2 的段(物理回收——mmap 段不可原地压实,merge 是唯一回收路径)
    // ② 同量级(log2 doc_count 层)段数 ≥ fan_in → 合并该层最老 fan_in 个。
    // 每次 flush 至多一组(摊销)。0 = 关闭(段数只增,不建议)。
    std::size_t          merge_fan_in = 8;
    // S21-A6:载入 v2 段时跳过 CRC 校验(可信盘/大库加速冷启动的 opt-in;
    // 默认恒校验。跳过后盘损坏由 mmap 读出错误数据,风险自担)。
    bool                 mmap_verify_crc = true;
    double               auto_compact_dead_ratio = 0.0;  // S12-2
    std::shared_ptr<const SynonymMap> synonym_map;       // S11：open-time 不可变
    // S18-6（S14-5 语义每插件化）：delta 链长上限，达到后 flush 强制 base。
    // 0 = 不设限。
    std::uint32_t        max_delta_chain = 64;
};

}  // namespace bitcask::text
