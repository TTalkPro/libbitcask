// bitcask.meta 文件管理：创建/读取 bitcask.meta 二进制格式。
//
// 格式（18 bytes）：
//   [0..3]   Magic        "BCME" (4 bytes)
//   [4]      Version      uint8 = 5（kMetaVersion；沿革与各版拒绝策略见
//                          meta_file.cpp——v1/v2/v3/v4 均干净拒开）
//   [5]      Mode         uint8 = 0(KV) or 1(Index)
//   [6]      VecMetric    uint8 (V3.1)
//   [7..8]   VecDim       uint16 LE (V3.1)
//   [9]      VecQuant     uint8 = 0/1（P3b：向量落盘 int8 量化；旧文件全零=否）
//   [10]     VecInmemInt8 uint8 = 0/1（P5b：HNSW int8-only 内存；旧文件全零=否）
//   [11]     VecEngine    uint8（S32-M0：向量引擎；0=HNSW，旧文件全零=HNSW）
//   [12]     IcuMajor     uint8（S38：建索引时的 ICU 主版本；0=未记录）
//   [13]     UnicodeMajor uint8（S38：同上的 Unicode 主版本；0=未记录）
//   [14..17] CRC32        uint32 LE（v3 起，覆盖 [0,14)）
//   [18..26] 可选尾段（C1）：TailVersion u8=1 + AnalyzerFp u32 + CRC32(覆盖尾段前 5B)；
//            旧读端只读前 18 字节，对其透明（布局详见 meta_file.cpp）
//   [27..]   可选尾段（C2，S55）：分析器配置本身（变长，自带 CRC，读端用 C1 指纹自校验）；
//            只在 C1 尾段存在时出现；C1 读端只读前 27 字节，对其透明
//
// === 线程模型 ===
// 所有函数均为纯函数：线程安全、可重入、无锁。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <expected>

#include "bitcask/analyzer.hpp"  // S55：MetaConfig::analyzer

namespace bitcask::meta {

// 运行模式：KV 纯存储 或 索引模式（BM25 搜索）
enum class Mode : std::uint8_t {
    kKV = 0,       // 纯 KV 模式
    kIndex = 1,    // 索引模式（BM25 搜索）
};

// V3.1:向量距离度量。kNone = 本集合无向量(旧 meta 的保留区全零
// 自然解码为此值,无需版本升级)。
enum class VectorMetric : std::uint8_t {
    kNone = 0,
    kCosineNormalized = 1,   // 写入时归一化,查询用内积(默认推荐)
    kL2 = 2,
    kDot = 3,
};

// S32-M0:向量引擎(设计 doc/vector-dual-engine-selection-zh.md §4)。
// 建库时一次性选定、持久化进 meta;重开不一致 → mode_mismatch。运行期
// 不可切换,唯一切换路径 = 离线转换工具(设计 §6.4)。kHnsw = 0 使旧
// meta 保留区全零自然解码为 HNSW(与 VectorMetric::kNone 同款零升级)。
enum class VectorEngine : std::uint8_t {
    kHnsw = 0,     // 内存图(≤ 数 M 向量档;现行实现)
    kIvfRq = 1,    // IVF-RaBitQ 磁盘档(S32-M3;10M-100M)
    kDiskann = 2,  // DiskANN(S32-M5 条件触发;预留)
};

// bitcask.meta 配置内容。
// V3.1:vector 配置占用原保留区 [6]=metric、[7..8]=dim(LE u16)——
// 库内 dim 恒定、初始化显式配置、重开校验(hnsw-design §1)。
struct MetaConfig {
    Mode mode = Mode::kKV;
    VectorMetric vector_metric = VectorMetric::kNone;
    std::uint16_t vector_dim = 0;   // 0 = 无向量
    bool vector_quantized = false;  // P3b：向量落盘 int8 量化（仅 vector_dim>0 有意义）
    bool vector_inmem_int8 = false; // P5b：HNSW int8-only 内存模式（仅 vector_dim>0 + kDot）
    // S32-M0：向量引擎（仅 vector_dim>0 有意义；无向量恒 kHnsw/0）。
    VectorEngine vector_engine = VectorEngine::kHnsw;
    // === S38：建索引时的 Unicode 数据版本 ===
    // 分词结果 = NFKC_Casefold 表 = Unicode 版本 = ICU 版本。同一份语料在
    // 不同 ICU 版本下会被切成**不同的 term**，于是「用 ICU 76 建的索引，
    // 换 ICU 74 的机器打开后新写入的文档」与老文档互相搜不到——而这没有
    // 任何显式症状，只是召回悄悄少了一块。故建索引时记下版本，重开时比对。
    //
    // **0 = 未记录**，不是「版本 0」：S38 之前建的目录、以及 KV 模式的目录
    // （无文本分析，无所谓）都是 0，比对时跳过。这让本字段无需 bump meta
    // 版本即可加入——沿用 VectorMetric::kNone / VectorEngine::kHnsw 那套
    // 「保留字节全零即默认」的零升级模式。
    //
    // 只记主版本：meta 是固定 18 字节、只剩 [12..13] 两个空闲字节的格式，
    // 而 ICU 主版本与 Unicode 版本一一对应（ICU 74=Unicode 15.1，
    // 76=16.0），主版本一致即数据表一致，够做判别。
    //
    // 这两个字段**默认 0 且只在建索引时显式填写**：write_meta 会被懒升级
    // （v5→v6）等路径调用，若默认取「当前运行时版本」，那些路径就会把原始
    // 记录悄悄覆盖成当次运行的版本——比对从此永远相等，告警永不触发。
    std::uint8_t icu_major = 0;
    std::uint8_t unicode_major = 0;

    // === C1：建索引时的 analyzer 配置指纹（text::analyzer_fingerprint）===
    // 同 S38 的失败形态：换分词配置重开，新旧文档 term 集静默分叉。**0 = 未
    // 记录**（C1 之前建的目录 / KV 模式 / 建库时无 search_config / 被旧二进制
    // 重写 meta 丢了尾段），比对时跳过。与 icu_major 同纪律：只在建索引时
    // 显式填写，其余 write_meta 路径原样透传读回的值。
    std::uint32_t analyzer_fp = 0;

    // === S55：建索引时的 analyzer 配置本身（C2 尾段）===
    // 只有指纹，打开方无从得知建库用的分词器（只读查看工具只能按缺省开、猜错了召回静默
    // 变化）。记下 analyzer_fingerprint 覆盖的全部字段；**dict_path 不记**（机器相关的路径，
    // 打开时仍取选项），读回恒空。nullopt = 未记录（S55 之前建的目录 / KV / 被旧二进制重写
    // meta 丢了尾段 / C2 损坏 / 与 analyzer_fp 自校验不符）。只与 analyzer_fp 同时写：
    // write_meta 在 analyzer_fp == 0 时不写 C2。纪律同 analyzer_fp（只在建索引时显式填写，
    // 其余路径原样透传）。
    std::optional<text::AnalyzerConfig> analyzer;

    // S35：盘上纪元。5 = 无原子批（保守纪元标记）；6 = 目录可能含
    // kBatchHeader 记录（首次 put_batch_atomic 前懒升级，见
    // doc/atomic-batch-design-zh.md §2）。read_meta 回填实际值；write_meta
    // 按此值写出（仅接受 5/6）。目录创建默认 5。
    std::uint8_t version = 5;
};

// meta 文件操作错误
struct MetaError {
    // S55：失败分类（C 入口 bitcask_meta_read 按它映射错误码）。
    enum class Kind : std::uint8_t {
        kIo,           // 打不开 / 读不全以外的 I/O 失败（含文件不存在：见 meta_exists）
        kCorrupt,      // magic 不对、截断、CRC 不符、字段取值非法
        kUnsupported,  // 纪元门禁：v1～v4 等干净拒开的旧格式 / 未知版本
    };
    int errnum = 0;
    std::string message;
    Kind kind = Kind::kIo;
};

// 检查 meta 文件是否存在
[[nodiscard]] bool meta_exists(std::string_view dirname);

// 读取 meta 文件
//   - 文件不存在返回错误
//   - magic 不对返回错误
//   - version 非 1 返回错误
//   - 成功返回 MetaConfig
[[nodiscard]] std::expected<MetaConfig, MetaError> read_meta(std::string_view dirname);

// 写入 meta 文件
//   - 创建/覆盖 dirname/bitcask.meta
//   - 成功返回空，失败返回错误
[[nodiscard]] std::expected<void, MetaError> write_meta(std::string_view dirname, const MetaConfig& config);

}  // namespace bitcask::meta