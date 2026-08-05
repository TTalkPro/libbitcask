#include "bitcask/meta_file.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>

#include "bitcask/codec.hpp"
#include "bitcask/detail/file_util.hpp"  // S35：atomic_write_bytes

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
// v3(S12)：保留区偏移 14 起放 CRC32(u32 LE)，覆盖前 14 字节(magic+version+mode+
// 向量配置+保留 11-13)。CRC 字段自身不被覆盖。
inline constexpr std::size_t kMetaCrcOffset   = 14;
inline constexpr std::size_t kMetaCrcCoverLen = 14;  // CRC 覆盖 [0, 14)
inline constexpr std::size_t kMetaFileSize = kMetaMagicSize + 1 + 1 + kMetaReservedSize;  // 18 bytes

// v1 = 大端纪元(legacy);v2 = 小端 flag-day 起;v3(S12) = 加 CRC32 校验和;
// v4 = record 时间戳 u64 flag-day（data header 27B / hint BCH4 / doc value v4）;
// v5 = hint BCH5 flag-day（S33：hint 记录内嵌 ord;data/DocValue 布局与 v4
// 完全相同,仅 hint 与本门禁变更,离线迁移 `bitcask_migrate hintord` 只重写
// hint + meta——data 一字节不动）;
// v6 = S35 原子批纪元（**懒升级**：目录可能含 kBatchHeader 记录;与 v5 的
// 差异仅此一点,首次 put_batch_atomic 前由引擎重写 meta——从不用批的目录
// 永远停留 v5,与 5.1.0 读端互通。设计 doc/atomic-batch-design-zh.md §2）。
// 读端：v1 干净拒绝(大端,提示重建);v2/v3 干净拒绝(u32-tstamp 纪元,record
// 布局不兼容,提示重建——绝不按新偏移把旧字节静默读坏);v4 干净拒绝(提示
// 跑 hintord 迁移);v5/v6 校验 CRC 后接受。
// 写端按 MetaConfig::version 写 5 或 6(目录创建默认 5)。
inline constexpr std::uint8_t kMetaVersion = 5;
inline constexpr std::uint8_t kMetaVersionBatch = 6;  // S35 原子批纪元
inline constexpr char kMetaMagic[kMetaMagicSize + 1] = "BCME";

// header 前 kMetaCrcCoverLen 字节的 CRC32（与 data/hint/field.schema 同多项式）。
inline std::uint32_t meta_crc(const char* header) {
    return codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(header), kMetaCrcCoverLen));
}

}  // namespace

bool meta_exists(std::string_view dirname) {
    const auto path = std::filesystem::path(dirname) / "bitcask.meta";
    return std::filesystem::exists(path);
}

std::expected<MetaConfig, MetaError> read_meta(std::string_view dirname) {
    const auto path = std::filesystem::path(dirname) / "bitcask.meta";
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return std::unexpected(MetaError{errno, "cannot open bitcask.meta"});
    }

    char header[kMetaFileSize];
    f.read(header, static_cast<std::streamsize>(kMetaFileSize));
    if (!f || f.gcount() != static_cast<std::streamsize>(kMetaFileSize)) {
        return std::unexpected(MetaError{EIO, "read meta file truncated"});
    }

    if (std::memcmp(header, kMetaMagic, kMetaMagicSize) != 0) {
        return std::unexpected(MetaError{0, "bad magic"});
    }

    const std::uint8_t ver = static_cast<std::uint8_t>(header[kMetaVersionOffset]);
    // v1 = 大端 legacy 格式;干净拒绝,提示重建——绝不静默把大端字节按小端读坏。
    if (ver == 1) {
        return std::unexpected(MetaError{0,
            "incompatible legacy big-endian format (meta v1); "
            "little-endian flag-day requires rebuild — re-ingest data"});
    }
    // v2/v3 = u32-tstamp 纪元：record 布局(23B header/hint v3/doc value v3)与
    // 当前 64 位时间戳布局二进制不兼容。干净拒绝,提示重建——与 v1 大端同策略。
    if (ver == 2 || ver == 3) {
        return std::unexpected(MetaError{0,
            "incompatible u32-tstamp era format (meta v2/v3); "
            "64-bit tstamp flag-day requires rebuild — re-ingest data"});
    }
    // v4 = ord-less-hint 纪元：data/DocValue 布局与 v5 相同，仅 hint 格式
    // 不同。离线迁移即可（不重建）——绝不静默按 v5 打开（否则 BCH4 hint
    // 会被当校验失败逐文件退 fold(data)，掩盖纪元错位）。
    if (ver == 4) {
        return std::unexpected(MetaError{0,
            "ord-less-hint era format (meta v4); "
            "run `bitcask_migrate hintord <src> <dst>` to migrate "
            "(data files are copied unchanged; only hints + meta rewritten)"});
    }
    if (ver != kMetaVersion && ver != kMetaVersionBatch) {
        return std::unexpected(MetaError{0, "unsupported meta version"});
    }
    // v5/v6：校验 CRC32（检出位翻转/损坏 → fail-fast）。
    {
        std::uint32_t stored = 0;
        std::memcpy(&stored, header + kMetaCrcOffset, 4);
        if (stored != meta_crc(header)) {
            return std::unexpected(MetaError{0, "bitcask.meta CRC mismatch (corrupt)"});
        }
    }

    const std::uint8_t mode_val = static_cast<std::uint8_t>(header[kMetaModeOffset]);
    MetaConfig cfg;
    cfg.version = ver;  // S35：回填实际纪元（5 或 6）
    if (mode_val == 0) {
        cfg.mode = Mode::kKV;
    } else if (mode_val == 1) {
        cfg.mode = Mode::kIndex;
    } else {
        return std::unexpected(MetaError{0, "unknown mode"});
    }
    const auto metric_val =
        static_cast<std::uint8_t>(header[kMetaVecMetricOffset]);
    if (metric_val > static_cast<std::uint8_t>(VectorMetric::kDot)) {
        return std::unexpected(MetaError{0, "unknown vector metric"});
    }
    cfg.vector_metric = static_cast<VectorMetric>(metric_val);
    std::memcpy(&cfg.vector_dim, header + kMetaVecDimOffset, 2);
    if ((cfg.vector_metric == VectorMetric::kNone) != (cfg.vector_dim == 0)) {
        return std::unexpected(MetaError{0, "inconsistent vector config"});
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
        return std::unexpected(MetaError{0, "unknown vector engine"});
    }
    cfg.vector_engine = static_cast<VectorEngine>(engine_val);
    return cfg;
}

std::expected<void, MetaError> write_meta(std::string_view dirname, const MetaConfig& config) {
    const auto path = std::filesystem::path(dirname) / "bitcask.meta";
    if (config.version != kMetaVersion && config.version != kMetaVersionBatch) {
        return std::unexpected(MetaError{0, "invalid meta version to write"});
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
    header[kMetaVersionOffset] = static_cast<char>(config.version);  // S35：5/6
    header[kMetaModeOffset] = static_cast<char>(
        config.mode == Mode::kKV ? 0 : 1);

    // v3：所有覆盖字段填好后算 CRC32 存入偏移 14（LE-only 主机，host 序 memcpy）。
    const std::uint32_t crc = meta_crc(header);
    std::memcpy(header + kMetaCrcOffset, &crc, 4);

    // S35：原子写（tmp + rename + fsync 目录）。原实现裸 ofstream 截断重写，
    // 懒升级重写 meta 时若中途崩溃会留下半截 meta（整目录拒开）——meta 是
    // 唯一纪元门禁，必须要么旧要么新。
    if (!detail::atomic_write_bytes(
            path.string(),
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(header), kMetaFileSize),
            /*fsync_dir=*/true)) {
        return std::unexpected(MetaError{errno, "write meta file failed"});
    }
    return {};
}

}  // namespace bitcask::meta