// index.manifest：P3 checkpoint 拆分的唯一 commit 点（S17-1）。
//
// 单文件 search.ckpt 拆为 3 个 per-component 文件族后，需要一个原子提交点
// 告知 recovery「哪一代的哪些 delta 链已提交」。manifest 记录每组件的
// {base_watermark, chain_seq, chain_watermark}，最后经 tmp+rename 写入——
// 它是唯一的 commit 点，crash 前的未提交写入会被 header-wm≠manifest 拒绝。
//
// 布局（~80 字节，全小端）：
//   magic "BCMF"(4) | version u32=1 | component_count u32
//   per component [0=docmap, 1=bm25, 2=vec]:
//     base_watermark u64 | chain_seq u32 | chain_watermark u64
//   footer_crc32 u32（CRC32 覆盖 magic..last chain_watermark）| trailer "BCMF"(4)
//
// 无 manifest .prev——80 字节 + CRC + 原子 rename 足够可靠；损坏退全量 fold。
// 详见 doc/plugin-arch-split-design-zh.md §5 + Oracle P3 设计分析。

#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>


#include "bitcask/codec.hpp"  // crc32
#include "bitcask/detail/file_util.hpp"  // detail::FilePtr（RED-2 归并）

namespace bitcask {

inline constexpr std::string_view kManifestName = "index.manifest";

// 组件索引（manifest entries 数组下标）。
enum class ComponentId : std::uint8_t {
    kDocmap = 0,
    kBm25   = 1,
    kVec    = 2,
};
inline constexpr std::size_t kComponentCount = 3;

struct ManifestEntry {
    std::uint64_t base_watermark  = 0;
    std::uint32_t chain_seq       = 0;
    std::uint64_t chain_watermark = 0;

    bool operator==(const ManifestEntry&) const = default;
};

struct Manifest {
    std::array<ManifestEntry, kComponentCount> entries{};

    ManifestEntry&       operator[](ComponentId c) { return entries[static_cast<std::size_t>(c)]; }
    const ManifestEntry& operator[](ComponentId c) const { return entries[static_cast<std::size_t>(c)]; }

    [[nodiscard]] std::uint64_t min_chain_watermark() const {
        std::uint64_t m = UINT64_MAX;
        for (const auto& e : entries) m = std::min(m, e.chain_watermark);
        return entries[0].chain_watermark == 0 ? 0 : m;
    }
};

// ---- 序列化（二进制小端 buffer）----

inline constexpr char kManifestMagic[4] = {'B', 'C', 'M', 'F'};
inline constexpr std::uint32_t kManifestVersion = 1;

namespace detail {

inline void append_u32(std::vector<std::byte>& buf, std::uint32_t v) {
    buf.push_back(static_cast<std::byte>(v));
    buf.push_back(static_cast<std::byte>(v >> 8));
    buf.push_back(static_cast<std::byte>(v >> 16));
    buf.push_back(static_cast<std::byte>(v >> 24));
}

inline void append_u64(std::vector<std::byte>& buf, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        buf.push_back(static_cast<std::byte>(v >> (i * 8)));
}

inline std::uint32_t read_u32(const std::byte* p) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

inline std::uint64_t read_u64(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(p[i])) << (i * 8);
    return v;
}

}  // namespace detail

// Manifest 总尺寸：12 header + 3×20 body + 8 footer = 80。
inline constexpr std::size_t kManifestSize = 12 + kComponentCount * 20 + 8;

inline std::vector<std::byte> serialize_manifest(const Manifest& m) {
    std::vector<std::byte> buf;
    buf.reserve(kManifestSize);
    // Header
    for (char c : kManifestMagic) buf.push_back(static_cast<std::byte>(c));
    detail::append_u32(buf, kManifestVersion);
    detail::append_u32(buf, static_cast<std::uint32_t>(kComponentCount));
    // Body
    for (const auto& e : m.entries) {
        detail::append_u64(buf, e.base_watermark);
        detail::append_u32(buf, e.chain_seq);
        detail::append_u64(buf, e.chain_watermark);
    }
    // Footer CRC（覆盖 header + body）
    const std::uint32_t crc = codec::crc32({buf.data(), buf.size()});
    detail::append_u32(buf, crc);
    // Trailer
    for (char c : kManifestMagic) buf.push_back(static_cast<std::byte>(c));
    return buf;
}

[[nodiscard]] inline std::optional<Manifest>
deserialize_manifest(const std::byte* raw, std::size_t len) {
    if (len != kManifestSize) return std::nullopt;
    // Magic
    if (std::memcmp(raw, kManifestMagic, 4) != 0) return std::nullopt;
    // Version
    if (detail::read_u32(raw + 4) != kManifestVersion) return std::nullopt;
    // Component count
    if (detail::read_u32(raw + 8) != kComponentCount) return std::nullopt;
    // Footer CRC
    const std::uint32_t stored_crc = detail::read_u32(raw + kManifestSize - 8);
    const std::uint32_t actual_crc = codec::crc32({raw, kManifestSize - 8});
    if (stored_crc != actual_crc) return std::nullopt;
    // Trailer
    if (std::memcmp(raw + kManifestSize - 4, kManifestMagic, 4) != 0)
        return std::nullopt;
    // Body
    Manifest m;
    const std::byte* p = raw + 12;
    for (std::size_t i = 0; i < kComponentCount; ++i) {
        m.entries[i].base_watermark  = detail::read_u64(p);      p += 8;
        m.entries[i].chain_seq       = detail::read_u32(p);      p += 4;
        m.entries[i].chain_watermark = detail::read_u64(p);      p += 8;
    }
    return m;
}

// ---- 文件 I/O（tmp + fdatasync + rename + dirfsync）----

// S20-3 B-B1：FILE* 走 RAII。RED-2 归并后统一用 bitcask::detail::FilePtr
// （原 manifest_io 命名空间是为避免与 field_schema 撞名而存在——单一真相源
// 后已无必要）。
// T21：原子写归 detail::atomic_write_bytes。manifest 是**唯一 commit 点**，
// 故 fsync_dir=true（rename 本身也须持久，否则断电后组件已落盘而 manifest
// 的目录项丢失）。原实现丢弃 fdatasync 返回值——归并后统一检查（disk-full
// 下不再静默 rename 出半截 manifest）。
[[nodiscard]] inline bool write_manifest(const std::string& path, const Manifest& m) {
    auto buf = serialize_manifest(m);
    return detail::atomic_write_bytes(path, buf, /*fsync_dir=*/true);
}

[[nodiscard]] inline std::optional<Manifest>
read_manifest(const std::string& path) {
    detail::FilePtr f(std::fopen(path.c_str(), "rb"));
    if (!f) return std::nullopt;
    std::array<std::byte, kManifestSize> buf{};
    const bool read_ok =
        std::fread(buf.data(), 1, buf.size(), f.get()) == buf.size();
    f.reset();
    if (!read_ok) return std::nullopt;
    return deserialize_manifest(buf.data(), buf.size());
}

}  // namespace bitcask
