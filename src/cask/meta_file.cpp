#include "bitcask/meta_file.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>

#include "bitcask/hw_crc32.hpp"

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

// v1 = 大端纪元(legacy);v2 = 小端 flag-day 起;v3(S12) = 加 CRC32 校验和。
// 读端：v1 干净拒绝(大端,提示重建);v2 向后兼容读(无 CRC,旧库不破坏);v3 校验 CRC。
// 写端恒写 v3(带 CRC)。见 doc/format-zh.md 字节序说明。
inline constexpr std::uint8_t kMetaVersion = 3;
inline constexpr char kMetaMagic[kMetaMagicSize + 1] = "BCME";

// header 前 kMetaCrcCoverLen 字节的 CRC32（与 data/hint/field.schema 同多项式）。
inline std::uint32_t meta_crc(const char* header) {
    return hw::crc32(std::span<const std::byte>(
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
    if (ver != 2 && ver != 3) {
        return std::unexpected(MetaError{0, "unsupported meta version"});
    }
    // v3：校验 CRC32（检出位翻转/损坏 → fail-fast）。v2：无 CRC 字段，向后兼容读
    // （旧库不破坏；写端恒写 v3）。
    if (ver == 3) {
        std::uint32_t stored = 0;
        std::memcpy(&stored, header + kMetaCrcOffset, 4);
        if (stored != meta_crc(header)) {
            return std::unexpected(MetaError{0, "bitcask.meta CRC mismatch (corrupt)"});
        }
    }

    const std::uint8_t mode_val = static_cast<std::uint8_t>(header[kMetaModeOffset]);
    MetaConfig cfg;
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
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        return std::unexpected(MetaError{errno, "cannot create bitcask.meta"});
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
    header[kMetaVersionOffset] = static_cast<char>(kMetaVersion);
    header[kMetaModeOffset] = static_cast<char>(
        config.mode == Mode::kKV ? 0 : 1);

    // v3：所有覆盖字段填好后算 CRC32 存入偏移 14（LE-only 主机，host 序 memcpy）。
    const std::uint32_t crc = meta_crc(header);
    std::memcpy(header + kMetaCrcOffset, &crc, 4);

    f.write(header, static_cast<std::streamsize>(kMetaFileSize));
    if (!f) {
        return std::unexpected(MetaError{errno, "write meta file failed"});
    }
    return {};
}

}  // namespace bitcask::meta