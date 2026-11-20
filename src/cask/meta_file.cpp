#include "bitcask/meta_file.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

#include "bitcask/codec.hpp"
#include "bitcask/detail/file_util.hpp"  // S35：atomic_write_bytes
#include "bitcask/detail/path_utf8.hpp"

namespace bitcask::meta {

namespace {

// bitcask.meta binary format constants
inline constexpr std::size_t kMetaMagicSize = 4;
inline constexpr std::size_t kMetaVersionOffset = 4;
inline constexpr std::size_t kMetaModeOffset = 5;
inline constexpr std::size_t kMetaReservedSize = 12;
// V3.1:向量配置占用保留区前 3 字节(旧文件全零 → kNone/0,自然兼容)。
inline constexpr std::size_t kMetaVecMetricOffset = 6;
inline constexpr std::size_t kMetaVecDimOffset    = 7;  // u16 LE
inline constexpr std::size_t kMetaVecQuantOffset  = 9;  // P3b：u8 0/1（旧文件全零=否）
inline constexpr std::size_t kMetaVecInmemInt8Offset = 10;  // P5b：u8 0/1（旧文件全零=否）
inline constexpr std::size_t kMetaVecEngineOffset = 11;  // S32-M0：u8（旧文件全零=kHnsw）
// S38：建索引时的 Unicode 数据版本（旧文件全零 = 未记录 → 重开时跳过比对）。
// 落在 CRC 覆盖区 [0,14) 内，故与其余字段同受校验保护。用掉保留区最后两个
// 空闲字节——再加字段就必须扩文件长度并 bump 版本了。
inline constexpr std::size_t kMetaIcuMajorOffset     = 12;
inline constexpr std::size_t kMetaUnicodeMajorOffset = 13;
// v3(S12)：保留区偏移 14 起放 CRC32(u32 LE)，覆盖前 14 字节(magic+version+mode+
// 向量配置+S38 的 Unicode 版本记录 12-13)。CRC 字段自身不被覆盖。
inline constexpr std::size_t kMetaCrcOffset   = 14;
inline constexpr std::size_t kMetaCrcCoverLen = 14;  // CRC 覆盖 [0, 14)
inline constexpr std::size_t kMetaFileSize = kMetaMagicSize + 1 + 1 + kMetaReservedSize;  // 18 bytes

// C1:可选尾段(定长头之后)。定长头已无空闲字节;旧读端只读前 18 字节、
// 不查文件长度,故追加尾段对旧二进制透明(它懒升级重写 meta 时会丢掉尾段,
// 退化为「未记录」——只少一次告警,不致错)。尾段自带 CRC,损坏同样按未记录
// 处理(诊断信息,不做 fail-fast,与 S38 同策略)。
//   [18]      TailVersion u8 = 1
//   [19..22]  AnalyzerFp  u32 LE(0 = 未记录,此时不写尾段)
//   [23..26]  CRC32       u32 LE,覆盖 [18, 23)
inline constexpr std::size_t kMetaTailOffset = kMetaFileSize;
inline constexpr std::size_t kMetaTailCoverLen = 5;
inline constexpr std::size_t kMetaTailSize = kMetaTailCoverLen + 4;  // 9 bytes
inline constexpr std::uint8_t kMetaTailVersion = 1;

// S55:C2 尾段(紧跟 C1 尾段;只在 C1 存在时写)——分析器配置本身。C1 读端只读前 27 字节,
// 对其透明。自带 CRC;读端再用 C1 指纹自校验(analyzer_fingerprint(读回) == analyzer_fp),
// 任何一项不过 ⇒ 当未记录(诊断信息,同 C1 不 fail-fast)。
//   [27]          Tail2Version u8 = 1
//   [28..31]      PayloadLen   u32 LE
//   [32..32+L)    Payload:
//                   Type u8(text::AnalyzerType) · MinN u32 · MaxN u32 · StopWords u8 ·
//                   MinTokenLength u32 · MaxTokenBytes u32 · Stemming u8 ·
//                   NStop u32 · NStop × (Len u32 + UTF-8 字节)(自定义停用词表;空 = 内置表)
//   [32+L..+4)    CRC32        u32 LE,覆盖 [27, 32+L)
// dict_path 不记(机器相关路径)。
inline constexpr std::size_t kMetaTail2Offset = kMetaTailOffset + kMetaTailSize;  // 27
inline constexpr std::size_t kMetaTail2HeadLen = 5;                               // 版本 + 长度
inline constexpr std::uint8_t kMetaTail2Version = 1;
inline constexpr std::uint32_t kMetaTail2MaxPayload = 16u << 20;  // 防坏长度撑爆内存

void put_u32(std::vector<char>& out, std::uint32_t v) {
    char b[4];
    std::memcpy(b, &v, 4);  // LE-only 主机(同全文件)
    out.insert(out.end(), b, b + 4);
}

std::vector<char> encode_analyzer(const text::AnalyzerConfig& c) {
    std::vector<char> p;
    p.push_back(static_cast<char>(c.type));
    put_u32(p, c.min_n);
    put_u32(p, c.max_n);
    p.push_back(static_cast<char>(c.enable_stop_words ? 1 : 0));
    put_u32(p, c.min_token_length);
    put_u32(p, c.max_token_bytes);
    p.push_back(static_cast<char>(c.enable_stemming ? 1 : 0));
    put_u32(p, static_cast<std::uint32_t>(c.stop_words.size()));
    for (const auto& w : c.stop_words) {
        put_u32(p, static_cast<std::uint32_t>(w.size()));
        p.insert(p.end(), w.begin(), w.end());
    }
    return p;
}

// 解码失败(越界 / 未知类型 / 尾随垃圾) ⇒ nullopt。
std::optional<text::AnalyzerConfig> decode_analyzer(std::span<const char> p) {
    std::size_t off = 0;
    auto u8 = [&](std::uint8_t& v) {
        if (off + 1 > p.size()) return false;
        v = static_cast<std::uint8_t>(p[off]);
        off += 1;
        return true;
    };
    auto u32 = [&](std::uint32_t& v) {
        if (off + 4 > p.size()) return false;
        std::memcpy(&v, p.data() + off, 4);
        off += 4;
        return true;
    };
    text::AnalyzerConfig c;
    std::uint8_t type = 0, stop = 0, stem = 0;
    std::uint32_t n_stop = 0;
    if (!u8(type) || !u32(c.min_n) || !u32(c.max_n) || !u8(stop) ||
        !u32(c.min_token_length) || !u32(c.max_token_bytes) || !u8(stem) ||
        !u32(n_stop)) {
        return std::nullopt;
    }
    if (type > static_cast<std::uint8_t>(text::AnalyzerType::Jieba)) return std::nullopt;
    c.type = static_cast<text::AnalyzerType>(type);
    c.enable_stop_words = stop != 0;
    c.enable_stemming = stem != 0;
    for (std::uint32_t i = 0; i < n_stop; ++i) {
        std::uint32_t len = 0;
        if (!u32(len) || len > p.size() - off) return std::nullopt;
        c.stop_words.emplace_back(p.data() + off, len);
        off += len;
    }
    if (off != p.size()) return std::nullopt;
    return c;
}

// v1 = 大端纪元(legacy);v2 = 小端 flag-day 起;v3(S12) = 加 CRC32 校验和;
// v4 = record 时间戳 u64 flag-day（data header 27B / hint BCH4 / doc value v4）;
// v5 = BCH5 hint 纪元（S33：hint 记录内嵌 ord;data/DocValue 布局与 v4
// 完全相同,仅本门禁变更,离线迁移 `bitcask_migrate hintord` 只重写
// meta——data 一字节不动;S43-8 起 hint 停写并在可写开库清理）;
// v6 = S35 原子批纪元（**懒升级**：目录可能含 kBatchHeader 记录;与 v5 的
// 差异仅此一点,首次 put_batch_atomic 前由引擎重写 meta——从不用批的目录
// 永远停留 v5,保守纪元标记。设计 doc/atomic-batch-design-zh.md §2）。
// 读端：v1 干净拒绝(大端,提示重建);v2/v3 干净拒绝(u32-tstamp 纪元,record
// 布局不兼容,提示重建——绝不按新偏移把旧字节静默读坏);v4 干净拒绝(提示
// 跑 hintord 迁移);v5/v6 校验 CRC 后接受。
// 写端按 MetaConfig::version 写 5 或 6(目录创建默认 5)。
inline constexpr std::uint8_t kMetaVersion = 5;
inline constexpr std::uint8_t kMetaVersionBatch = 6;  // S35 原子批纪元
inline constexpr std::uint8_t kMetaVersionMultiValue = 7;  // S62 meta 字符串数组纪元（蕴含 v6）
inline constexpr char kMetaMagic[kMetaMagicSize + 1] = "BCME";

// header 前 kMetaCrcCoverLen 字节的 CRC32（与 data/field.schema 同多项式）。
inline std::uint32_t meta_crc(const char* header) {
    return codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(header), kMetaCrcCoverLen));
}

}  // namespace

bool meta_exists(std::string_view dirname) {
    const auto meta_path = bitcask::detail::from_utf8(dirname) / "bitcask.meta";
    return std::filesystem::exists(meta_path);
}

std::expected<MetaConfig, MetaError> read_meta(std::string_view dirname) {
    const auto path = bitcask::detail::from_utf8(dirname) / "bitcask.meta";
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return std::unexpected(MetaError{errno, "cannot open bitcask.meta", MetaError::Kind::kIo});
    }

    char header[kMetaFileSize];
    f.read(header, static_cast<std::streamsize>(kMetaFileSize));
    if (!f || f.gcount() != static_cast<std::streamsize>(kMetaFileSize)) {
        return std::unexpected(MetaError{EIO, "read meta file truncated", MetaError::Kind::kCorrupt});
    }
    // C1:可选尾段。缺失/截断/版本未知/CRC 不符 → analyzer_fp 保持 0(未记录)。
    char tail[kMetaTailSize] = {0};
    f.read(tail, static_cast<std::streamsize>(kMetaTailSize));
    const bool have_tail =
        f.gcount() == static_cast<std::streamsize>(kMetaTailSize);
    // S55:C2 尾段(若有)——其余字节一并读入,解析在下面。
    std::vector<char> tail2;
    if (have_tail) {
        tail2.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }

    if (std::memcmp(header, kMetaMagic, kMetaMagicSize) != 0) {
        return std::unexpected(MetaError{0, "bad magic", MetaError::Kind::kCorrupt});
    }

    const std::uint8_t ver = static_cast<std::uint8_t>(header[kMetaVersionOffset]);
    // v1 = 大端 legacy 格式;干净拒绝,提示重建——绝不静默把大端字节按小端读坏。
    if (ver == 1) {
        return std::unexpected(MetaError{0,
            "incompatible legacy big-endian format (meta v1); "
            "little-endian flag-day requires rebuild — re-ingest data", MetaError::Kind::kUnsupported});
    }
    // v2/v3 = u32-tstamp 纪元：record 布局(23B header/hint v3/doc value v3)与
    // 当前 64 位时间戳布局二进制不兼容。干净拒绝,提示重建——与 v1 大端同策略。
    if (ver == 2 || ver == 3) {
        return std::unexpected(MetaError{0,
            "incompatible u32-tstamp era format (meta v2/v3); "
            "64-bit tstamp flag-day requires rebuild — re-ingest data", MetaError::Kind::kUnsupported});
    }
    // v4 = ord-less-hint 纪元：data/DocValue 布局与 v5 相同，仅 hint 格式
    // 不同。离线迁移即可（不重建）——绝不静默按 v5 打开（否则 BCH4 hint
    // 会被当校验失败逐文件退 fold(data)，掩盖纪元错位）。
    if (ver == 4) {
        return std::unexpected(MetaError{0,
            "ord-less-hint era format (meta v4); "
            "run `bitcask_migrate hintord <src> <dst>` to migrate "
            "(data files are copied unchanged; only meta is rewritten)", MetaError::Kind::kUnsupported});
    }
    if (ver != kMetaVersion && ver != kMetaVersionBatch && ver != kMetaVersionMultiValue) {
        return std::unexpected(MetaError{0, "unsupported meta version", MetaError::Kind::kUnsupported});
    }
    // v5/v6/v7：校验 CRC32（检出位翻转/损坏 → fail-fast）。
    {
        std::uint32_t stored = 0;
        std::memcpy(&stored, header + kMetaCrcOffset, 4);
        if (stored != meta_crc(header)) {
            return std::unexpected(MetaError{0, "bitcask.meta CRC mismatch (corrupt)", MetaError::Kind::kCorrupt});
        }
    }

    const std::uint8_t mode_val = static_cast<std::uint8_t>(header[kMetaModeOffset]);
    MetaConfig cfg;
    cfg.version = ver;  // S35：回填实际纪元（5 / 6 / 7）
    if (mode_val == 0) {
        cfg.mode = Mode::kKV;
    } else if (mode_val == 1) {
        cfg.mode = Mode::kIndex;
    } else {
        return std::unexpected(MetaError{0, "unknown mode", MetaError::Kind::kCorrupt});
    }
    const auto metric_val =
        static_cast<std::uint8_t>(header[kMetaVecMetricOffset]);
    if (metric_val > static_cast<std::uint8_t>(VectorMetric::kDot)) {
        return std::unexpected(MetaError{0, "unknown vector metric", MetaError::Kind::kCorrupt});
    }
    cfg.vector_metric = static_cast<VectorMetric>(metric_val);
    std::memcpy(&cfg.vector_dim, header + kMetaVecDimOffset, 2);
    if ((cfg.vector_metric == VectorMetric::kNone) != (cfg.vector_dim == 0)) {
        return std::unexpected(MetaError{0, "inconsistent vector config", MetaError::Kind::kCorrupt});
    }
    cfg.vector_quantized =
        static_cast<std::uint8_t>(header[kMetaVecQuantOffset]) != 0;
    cfg.vector_inmem_int8 =
        static_cast<std::uint8_t>(header[kMetaVecInmemInt8Offset]) != 0;
    // S32-M0：向量引擎（旧文件全零 → kHnsw，零升级）。未知值 fail-fast——
    // 新引擎写的库绝不能被旧读端按 HNSW 静默误开。
    const auto engine_val =
        static_cast<std::uint8_t>(header[kMetaVecEngineOffset]);
    if (engine_val > static_cast<std::uint8_t>(VectorEngine::kDiskann)) {
        return std::unexpected(MetaError{0, "unknown vector engine", MetaError::Kind::kCorrupt});
    }
    cfg.vector_engine = static_cast<VectorEngine>(engine_val);
    // S38：Unicode 数据版本。任何值都合法（含 0 = 未记录）——这是**诊断信息，
    // 不是纪元门禁**，不做 fail-fast：未知的新版本号只该让人看到告警，不该
    // 让目录打不开。比对与告警在 Cask::open（见 cask.cpp）。
    cfg.icu_major =
        static_cast<std::uint8_t>(header[kMetaIcuMajorOffset]);
    cfg.unicode_major =
        static_cast<std::uint8_t>(header[kMetaUnicodeMajorOffset]);
    if (have_tail && static_cast<std::uint8_t>(tail[0]) == kMetaTailVersion) {
        std::uint32_t stored = 0;
        std::memcpy(&stored, tail + kMetaTailCoverLen, 4);
        const std::uint32_t crc = codec::crc32(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(tail), kMetaTailCoverLen));
        if (stored == crc) std::memcpy(&cfg.analyzer_fp, tail + 1, 4);
    }
    // S55:C2——C1 有效才看;CRC / 解码 / 指纹自校验任一不过 ⇒ 未记录。
    if (cfg.analyzer_fp != 0 && tail2.size() >= kMetaTail2HeadLen + 4 &&
        static_cast<std::uint8_t>(tail2[0]) == kMetaTail2Version) {
        std::uint32_t len = 0;
        std::memcpy(&len, tail2.data() + 1, 4);
        if (len <= kMetaTail2MaxPayload &&
            tail2.size() >= kMetaTail2HeadLen + std::size_t{len} + 4) {
            const std::size_t covered = kMetaTail2HeadLen + len;
            std::uint32_t stored = 0;
            std::memcpy(&stored, tail2.data() + covered, 4);
            const std::uint32_t crc = codec::crc32(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(tail2.data()), covered));
            if (stored == crc) {
                auto a = decode_analyzer(std::span<const char>(
                    tail2.data() + kMetaTail2HeadLen, len));
                if (a && text::analyzer_fingerprint(*a) == cfg.analyzer_fp) {
                    cfg.analyzer = std::move(a);
                }
            }
        }
    }
    return cfg;
}

std::expected<void, MetaError> write_meta(std::string_view dirname, const MetaConfig& config) {
    const auto path = bitcask::detail::from_utf8(dirname) / "bitcask.meta";
    if (config.version != kMetaVersion && config.version != kMetaVersionBatch &&
        config.version != kMetaVersionMultiValue) {
        return std::unexpected(MetaError{0, "invalid meta version to write", MetaError::Kind::kUnsupported});
    }

    char header[kMetaFileSize] = {0};
    std::memcpy(header, kMetaMagic, kMetaMagicSize);
    header[kMetaVecMetricOffset] =
        static_cast<char>(config.vector_metric);
    std::memcpy(header + kMetaVecDimOffset, &config.vector_dim, 2);
    header[kMetaVecQuantOffset] = static_cast<char>(config.vector_quantized ? 1 : 0);
    header[kMetaVecInmemInt8Offset] =
        static_cast<char>(config.vector_inmem_int8 ? 1 : 0);
    header[kMetaVecEngineOffset] =
        static_cast<char>(config.vector_engine);  // S32-M0（CRC 覆盖区内）
    // S38：原样写出 config 给的值（0 = 未记录）。**不在这里取当前运行时
    // 版本**——write_meta 也被懒升级（v5→v6）等路径调用，那样会把建索引时
    // 的原始记录覆盖成当次运行的版本，比对从此恒相等、告警永不触发。
    header[kMetaIcuMajorOffset] = static_cast<char>(config.icu_major);
    header[kMetaUnicodeMajorOffset] = static_cast<char>(config.unicode_major);
    header[kMetaVersionOffset] = static_cast<char>(config.version);  // S35：5/6
    header[kMetaModeOffset] = static_cast<char>(
        config.mode == Mode::kKV ? 0 : 1);

    // v3：所有覆盖字段填好后算 CRC32 存入偏移 14（LE-only 主机，host 序 memcpy）。
    const std::uint32_t crc = meta_crc(header);
    std::memcpy(header + kMetaCrcOffset, &crc, 4);

    // C1:有指纹才写尾段(未记录的目录保持 18 字节,与旧版逐字节相同)。
    std::vector<char> buf(kMetaFileSize + kMetaTailSize, 0);
    std::memcpy(buf.data(), header, kMetaFileSize);
    std::size_t len = kMetaFileSize;
    if (config.analyzer_fp != 0) {
        char* tail = buf.data() + kMetaTailOffset;
        tail[0] = static_cast<char>(kMetaTailVersion);
        std::memcpy(tail + 1, &config.analyzer_fp, 4);
        const std::uint32_t tcrc = codec::crc32(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(tail), kMetaTailCoverLen));
        std::memcpy(tail + kMetaTailCoverLen, &tcrc, 4);
        len += kMetaTailSize;
        // S55:C2——只在配置与指纹一致时写(不一致的记录读端也会丢,不如不写)。
        if (config.analyzer &&
            text::analyzer_fingerprint(*config.analyzer) == config.analyzer_fp) {
            const auto payload = encode_analyzer(*config.analyzer);
            buf.resize(kMetaTail2Offset);
            buf.push_back(static_cast<char>(kMetaTail2Version));
            put_u32(buf, static_cast<std::uint32_t>(payload.size()));
            buf.insert(buf.end(), payload.begin(), payload.end());
            const std::uint32_t crc2 = codec::crc32(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(buf.data() + kMetaTail2Offset),
                buf.size() - kMetaTail2Offset));
            put_u32(buf, crc2);
            len = buf.size();
        }
    }

    // S35：原子写（tmp + rename + fsync 目录）。原实现裸 ofstream 截断重写，
    // 懒升级重写 meta 时若中途崩溃会留下半截 meta（整目录拒开）——meta 是
    // 唯一纪元门禁，必须要么旧要么新。
    if (!detail::atomic_write_bytes(
            bitcask::detail::to_utf8(path),
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(buf.data()), len),
            /*fsync_dir=*/true)) {
        return std::unexpected(MetaError{errno, "write meta file failed", MetaError::Kind::kIo});
    }
    return {};
}

}  // namespace bitcask::meta
