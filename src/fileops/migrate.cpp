#include "bitcask/migrate.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <filesystem>
#include <vector>

#include "bitcask/byte_order.hpp"
#include "bitcask/codec.hpp"
#include "bitcask/data_file.hpp"
#include "bitcask/field_schema.hpp"
#include "bitcask/format.hpp"
#include "bitcask/hint_file.hpp"

namespace bitcask::migrate {

namespace fs = std::filesystem;

namespace {

// 旧格式（v1）是大端——本工具是唯一仍需读大端的地方,故 BE 解码器自带,
// 不依赖 codec（codec 已 flag-day 切成小端）。
//
// 旧 v1 record 布局的偏移在此钉死：format:: 常量已随 Tstamp u32→u64
// flag-day 漂移到 27B header,而本工具读的旧文件恒为 23B header
// （CRC:4 | Type:1 | Tstamp:u32 | Ord:u64 | KeySz:u16 | ValueSz:u32）。
inline constexpr std::size_t kLegacyHeaderSize    = 23;
inline constexpr std::size_t kLegacyCrcOffset     = 0;
inline constexpr std::size_t kLegacyTypeOffset    = 4;
inline constexpr std::size_t kLegacyTstampOffset  = 5;
inline constexpr std::size_t kLegacyOrdOffset     = 9;
inline constexpr std::size_t kLegacyKeySzOffset   = 17;
inline constexpr std::size_t kLegacyValueSzOffset = 19;
std::uint16_t be_u16(const std::byte* p) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(p[0]) << 8) | static_cast<std::uint16_t>(p[1]));
}
std::uint32_t be_u32(const std::byte* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  |
            static_cast<std::uint32_t>(p[3]);
}
std::uint64_t be_u64(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<std::uint64_t>(p[i]);
    return v;
}

// T21：整读归 detail::read_file_bytes（本函数原为其原型）。此处仅补本模块的
// expected<string> 错误语义——nullopt 无法区分开不了 / 短读，故只给一条
// 合并诊断（migrate 是离线工具路径，诊断粒度够用）。
std::expected<std::vector<std::byte>, std::string>
read_all(const fs::path& path) {
    auto buf = detail::read_file_bytes<>(path.string());
    if (!buf) return std::unexpected("cannot read " + path.string());
    return *std::move(buf);
}

std::expected<void, std::string>
write_all(const fs::path& path, std::span<const std::byte> bytes) {
    // S37-4：原为 path.c_str()。`std::filesystem::path::c_str()` 在 Windows 上
    // 返回 const wchar_t*，喂给窄字符的 std::fopen 直接编译失败（设计稿 C8）。
    // 改用 .string()，与紧邻的 read_all 及全库「路径以 std::string 流转」的
    // 约定一致（同 file_util.hpp 的 fsync_parent_dir）。
    // ⚠️ 这只解决可编译性：Windows 上 .string() 走的是系统 ANSI 代码页，
    // 非 ASCII 路径仍打不开。窄路径统一按 UTF-8 解释、在 io 后端转 UTF-16
    // 是 S37-5 的事，属库级问题，不在本站点单独修。
    std::unique_ptr<std::FILE, detail::FileCloser> f(
        std::fopen(path.string().c_str(), "wb"));
    if (!f) return std::unexpected("cannot create " + path.string());
    const bool ok =
        bytes.empty() ||
        std::fwrite(bytes.data(), 1, bytes.size(), f.get()) == bytes.size();
    f.reset();
    if (!ok) return std::unexpected("write failed " + path.string());
    return {};
}

// 一个 data 文件：逐 record 解大端头 → 小端重编码 + 重生成 hint。
std::expected<void, std::string>
migrate_data_file(const fs::path& src_data, const fs::path& dst_dir,
                  MigrateStats& st) {
    auto bytes = read_all(src_data);
    if (!bytes) return std::unexpected(bytes.error());

    const auto name = src_data.filename().string();
    const auto dst_data_path = (dst_dir / name).string();
    const auto dst_hint_path =
        fileops::mk_hint_filename(dst_data_path);

    // 小端写：DataFile/HintFile 内部用新（小端）codec,CRC/水位重算。
    auto dst_data = fileops::DataFile::open(
        dst_data_path, fileops::DataFile::Mode::kCreate, /*sync*/ false,
        /*mmap_enabled*/ false);
    if (!dst_data) return std::unexpected("create dst data " + dst_data_path);
    auto dst_hint = fileops::HintFile::open(
        dst_hint_path, fileops::HintFile::Mode::kCreate);
    if (!dst_hint) return std::unexpected("create dst hint " + dst_hint_path);

    const std::byte* base = bytes->data();
    const std::uint64_t total = bytes->size();
    std::uint64_t off = 0;
    while (off + kLegacyHeaderSize <= total) {
        const std::byte* p = base + off;
        const std::uint16_t key_sz = be_u16(p + kLegacyKeySzOffset);
        const std::uint32_t value_sz = be_u32(p + kLegacyValueSzOffset);
        const std::uint64_t rec_total =
            kLegacyHeaderSize + static_cast<std::uint64_t>(key_sz) + value_sz;
        if (off + rec_total > total) break;  // torn tail：尾部截断,停。

        // 旧 CRC 校验（大端存储,覆盖 Type..Value）。坏的跳过（与恢复同策略）。
        const std::uint32_t stored_crc = be_u32(p + kLegacyCrcOffset);
        const std::uint32_t calc_crc = codec::crc32(std::span<const std::byte>(
            p + 4, static_cast<std::size_t>(rec_total) - 4));
        if (stored_crc != calc_crc) {
            ++st.skipped_bad_crc;
            off += rec_total;
            continue;
        }

        const auto type = static_cast<format::RecordType>(p[kLegacyTypeOffset]);
        const std::uint32_t tstamp = be_u32(p + kLegacyTstampOffset);
        const std::uint64_t ord = be_u64(p + kLegacyOrdOffset);
        std::span<const std::byte> key(p + kLegacyHeaderSize, key_sz);
        std::span<const std::byte> value(p + kLegacyHeaderSize + key_sz,
                                         value_sz);
        const bool tomb = (type == format::RecordType::kTombstone);

        // 墓碑 v2 shadow file_id 是 4 字节大端值 → 重排成小端,与新格式一致。
        std::vector<std::byte> shadow_le;
        if (tomb && value_sz == 4) {
            const std::uint32_t fid = be_u32(value.data());
            shadow_le = {static_cast<std::byte>(fid & 0xFF),
                         static_cast<std::byte>((fid >> 8) & 0xFF),
                         static_cast<std::byte>((fid >> 16) & 0xFF),
                         static_cast<std::byte>((fid >> 24) & 0xFF)};
            value = std::span<const std::byte>(shadow_le);
        }

        auto w = dst_data->write(type, tstamp, ord, key, value);
        if (!w) return std::unexpected("write record to " + dst_data_path);
        auto h = dst_hint->write(tstamp, w->total_size, w->offset, tomb, key,
                                 ord);
        if (!h) return std::unexpected("write hint to " + dst_hint_path);

        ++st.records;
        if (tomb) ++st.tombstones;
        off += rec_total;
    }
    if (auto r = dst_hint->finalize(); !r) {
        return std::unexpected("finalize hint " + dst_hint_path);
    }
    ++st.data_files;
    return {};
}

std::expected<void, std::string>
migrate_meta(const fs::path& src_dir, const fs::path& dst_dir,
             MigrateStats& st) {
    const auto src_meta = src_dir / "bitcask.meta";
    if (!fs::exists(src_meta)) {
        return std::unexpected("no bitcask.meta in src (not a bitcask dir)");
    }
    auto bytes = read_all(src_meta);
    if (!bytes) return std::unexpected(bytes.error());
    if (bytes->size() < 18) return std::unexpected("meta too short");
    const std::byte* b = bytes->data();
    if (std::memcmp(b, "BCME", 4) != 0) return std::unexpected("bad meta magic");
    const auto ver = static_cast<std::uint8_t>(b[4]);
    if (ver == 2) {
        return std::unexpected("src meta already v2 (little-endian); "
                               "nothing to migrate");
    }
    if (ver != 1) return std::unexpected("unknown meta version");

    // v1 → v5：version 改 5（data record 已按当前 codec 重编码,hint 已是
    // BCH5）,VecDim u16 大端→小端,其余单字节照搬,偏移 14 放 CRC32
    // (覆盖前 14 字节),与 write_meta 一致。
    std::byte out[18] = {};
    std::memcpy(out, "BCME", 4);
    out[4] = static_cast<std::byte>(5);            // version 5（当前纪元）
    out[5] = b[5];                                 // mode
    out[6] = b[6];                                 // vec metric
    const std::uint16_t dim = be_u16(b + 7);       // 旧大端 → 主机
    out[7] = static_cast<std::byte>(dim & 0xFF);   // 小端
    out[8] = static_cast<std::byte>((dim >> 8) & 0xFF);
    out[9] = b[9];                                 // vec quantized
    out[10] = b[10];                               // vec inmem_int8
    // out[11..13] 保留全零；out[14..18) = CRC32(覆盖 [0,14))。
    le_store_u32(out + 14, codec::crc32(std::span<const std::byte>(out, 14)));
    if (auto r = write_all(dst_dir / "bitcask.meta",
                           std::span<const std::byte>(out, 18)); !r) {
        return std::unexpected(r.error());
    }
    st.meta_migrated = true;
    return {};
}

std::expected<void, std::string>
migrate_field_schema(const fs::path& src_dir, const fs::path& dst_dir,
                     MigrateStats& st) {
    const auto src_fs = src_dir / "field.schema";
    if (!fs::exists(src_fs)) return {};  // 无字段表（纯 KV）→ 跳过。
    auto bytes = read_all(src_fs);
    if (!bytes) return std::unexpected(bytes.error());

    // 输出新格式（S12-3）：文件头 [magic:u32][version:u32] + 每条 [len:u16][name][crc32:u32]。
    std::vector<std::byte> out;
    out.reserve(bytes->size() + FieldSchema::kHeaderSize);
    out.resize(FieldSchema::kHeaderSize);
    le_store_u32(out.data(), FieldSchema::kMagic);
    le_store_u32(out.data() + 4, FieldSchema::kVersion);

    const std::byte* b = bytes->data();
    const std::size_t n = bytes->size();
    std::size_t pos = 0;
    while (pos + 2 <= n) {
        const std::uint16_t nlen = be_u16(b + pos);  // 旧大端
        pos += 2;
        if (pos + nlen > n) break;  // 截断,停。
        // entry = [len:u16 LE][name] + CRC32(over [len|name])，全部小端。
        std::vector<std::byte> entry(2 + nlen);
        le_store_u16(entry.data(), nlen);
        std::memcpy(entry.data() + 2, b + pos, nlen);
        const std::uint32_t crc = codec::crc32(entry);
        out.insert(out.end(), entry.begin(), entry.end());
        std::byte cb[4];
        le_store_u32(cb, crc);
        out.insert(out.end(), cb, cb + 4);
        pos += nlen;
    }
    if (auto r = write_all(dst_dir / "field.schema", out); !r) {
        return std::unexpected(r.error());
    }
    st.field_schema_migrated = true;
    return {};
}

// ---------------------------------------------------------------------------
// u32 时间戳纪元（meta v2/v3）→ 当前纪元（meta v4,u64 时间戳）。
// 与 v1 迁移同为「解旧头 → 当前 codec 重编码」,区别:旧头已是小端
// （kLegacy* 偏移与 v1 相同,仅字节序不同）,且 kDoc 的 value 段须做
// DocValue v3→v4 转码（Ver 字节 3→4;expiry 段 u32→u64,恒为 value 尾部）。
// ---------------------------------------------------------------------------

// DocValue v3 → v4 转码。v3 的 expiry 段（kFlagHasExpiry）固定是 value 的
// 最后 4 字节（encode 恒最后追加）——转码只需改 Ver 字节 + 尾部 4B→8B 零
// 扩展,各中间段（vector/text/meta/fields）布局未变,原样保留。
// 返回 false = 不是合法 v3 DocValue（Ver 不符 / 长度不足）,caller 跳过。
bool transcode_doc_value_v3_to_v4(std::span<const std::byte> in,
                                  std::vector<std::byte>& out) {
    constexpr std::uint8_t kLegacyDocValueVersion = 3;
    if (in.size() < format::kDocValueHeaderSize) return false;
    if (static_cast<std::uint8_t>(in[0]) != kLegacyDocValueVersion) {
        return false;
    }
    const auto flags = static_cast<std::uint8_t>(in[1]);
    const bool has_expiry = (flags & format::kFlagHasExpiry) != 0;
    if (has_expiry && in.size() < format::kDocValueHeaderSize + 4) {
        return false;  // 声称有 expiry 段却装不下 u32 → 损坏
    }
    out.assign(in.begin(), in.end());
    out[0] = static_cast<std::byte>(format::kDocValueVersion);
    if (has_expiry) {
        const std::uint32_t expiry32 = le_load_u32(in.data() + in.size() - 4);
        out.resize(out.size() - 4 + 8);
        le_store_u64(out.data() + out.size() - 8,
                     static_cast<std::uint64_t>(expiry32));
    }
    return true;
}

// 一个 data 文件：逐 record 解 u32 纪元小端头（23B,偏移同 kLegacy*）→
// 当前 codec 重编码（27B 头,u64 tstamp）+ 重生成 hint。
std::expected<void, std::string>
migrate_u32_data_file(const fs::path& src_data, const fs::path& dst_dir,
                      MigrateStats& st) {
    auto bytes = read_all(src_data);
    if (!bytes) return std::unexpected(bytes.error());

    const auto name = src_data.filename().string();
    const auto dst_data_path = (dst_dir / name).string();
    const auto dst_hint_path = fileops::mk_hint_filename(dst_data_path);

    auto dst_data = fileops::DataFile::open(
        dst_data_path, fileops::DataFile::Mode::kCreate, /*sync*/ false,
        /*mmap_enabled*/ false);
    if (!dst_data) return std::unexpected("create dst data " + dst_data_path);
    auto dst_hint = fileops::HintFile::open(
        dst_hint_path, fileops::HintFile::Mode::kCreate);
    if (!dst_hint) return std::unexpected("create dst hint " + dst_hint_path);

    const std::byte* base = bytes->data();
    const std::uint64_t total = bytes->size();
    std::vector<std::byte> value_v4;  // 跨 record 复用容量
    std::uint64_t off = 0;
    while (off + kLegacyHeaderSize <= total) {
        const std::byte* p = base + off;
        const std::uint16_t key_sz = le_load_u16(p + kLegacyKeySzOffset);
        const std::uint32_t value_sz = le_load_u32(p + kLegacyValueSzOffset);
        const std::uint64_t rec_total =
            kLegacyHeaderSize + static_cast<std::uint64_t>(key_sz) + value_sz;
        if (off + rec_total > total) break;  // torn tail：尾部截断,停。

        // 旧 CRC（小端存储,覆盖 Type..Value）。坏的跳过（与恢复同策略）。
        const std::uint32_t stored_crc = le_load_u32(p + kLegacyCrcOffset);
        const std::uint32_t calc_crc = codec::crc32(std::span<const std::byte>(
            p + kLegacyTypeOffset,
            static_cast<std::size_t>(rec_total) - kLegacyTypeOffset));
        if (stored_crc != calc_crc) {
            ++st.skipped_bad_crc;
            off += rec_total;
            continue;
        }

        const auto type = static_cast<format::RecordType>(p[kLegacyTypeOffset]);
        // 时间戳 u32 → u64 零扩展（值域不变,仅位宽升级）。
        const std::uint64_t tstamp = le_load_u32(p + kLegacyTstampOffset);
        const std::uint64_t ord = le_load_u64(p + kLegacyOrdOffset);
        std::span<const std::byte> key(p + kLegacyHeaderSize, key_sz);
        std::span<const std::byte> value(p + kLegacyHeaderSize + key_sz,
                                         value_sz);
        const bool tomb = (type == format::RecordType::kTombstone);

        // kDoc：DocValue v3→v4 转码。墓碑 value（空 / 4B 小端 shadow
        // file_id）不是 DocValue,原样照搬。
        if (!tomb) {
            if (!transcode_doc_value_v3_to_v4(value, value_v4)) {
                ++st.skipped_bad_docvalue;
                off += rec_total;
                continue;
            }
            value = std::span<const std::byte>(value_v4);
        }

        auto w = dst_data->write(type, tstamp, ord, key, value);
        if (!w) return std::unexpected("write record to " + dst_data_path);
        auto h = dst_hint->write(tstamp, w->total_size, w->offset, tomb, key,
                                 ord);
        if (!h) return std::unexpected("write hint to " + dst_hint_path);

        ++st.records;
        if (tomb) ++st.tombstones;
        off += rec_total;
    }
    if (auto r = dst_hint->finalize(); !r) {
        return std::unexpected("finalize hint " + dst_hint_path);
    }
    ++st.data_files;
    return {};
}

// meta v2/v3（小端 u32 纪元）→ v4。除 version 字节与 CRC 外逐字节照搬
// （mode / 向量配置在两纪元间布局未变）。v3 入口校验 CRC——迁移工具坚持
// fail-fast,不把损坏的配置静默带进新库;v2 无 CRC 字段,跳过校验。
std::expected<void, std::string>
migrate_u32_meta(const fs::path& src_dir, const fs::path& dst_dir,
                 MigrateStats& st) {
    const auto src_meta = src_dir / "bitcask.meta";
    if (!fs::exists(src_meta)) {
        return std::unexpected("no bitcask.meta in src (not a bitcask dir)");
    }
    auto bytes = read_all(src_meta);
    if (!bytes) return std::unexpected(bytes.error());
    if (bytes->size() < 18) return std::unexpected("meta too short");
    const std::byte* b = bytes->data();
    if (std::memcmp(b, "BCME", 4) != 0) return std::unexpected("bad meta magic");
    const auto ver = static_cast<std::uint8_t>(b[4]);
    if (ver == 1) {
        return std::unexpected(
            "src meta is v1 (big-endian era); run the be2le migration first");
    }
    if (ver == 4) {
        return std::unexpected(
            "src meta is v4 (ord-less-hint era); run the hintord migration");
    }
    if (ver == 5) {
        return std::unexpected(
            "src meta already v5 (current era); nothing to migrate");
    }
    if (ver != 2 && ver != 3) return std::unexpected("unknown meta version");
    if (ver == 3) {
        const std::uint32_t stored = le_load_u32(b + 14);
        const std::uint32_t crc =
            codec::crc32(std::span<const std::byte>(b, 14));
        if (stored != crc) {
            return std::unexpected("src bitcask.meta CRC mismatch (corrupt)");
        }
    }

    std::byte out[18];
    std::memcpy(out, b, 18);
    out[4] = static_cast<std::byte>(5);  // version 5（当前纪元）
    le_store_u32(out + 14, codec::crc32(std::span<const std::byte>(out, 14)));
    if (auto r = write_all(dst_dir / "bitcask.meta",
                           std::span<const std::byte>(out, 18)); !r) {
        return std::unexpected(r.error());
    }
    st.meta_migrated = true;
    return {};
}

// ---------------------------------------------------------------------------
// u64 纪元（meta v4）→ 当前纪元（meta v5,hint BCH5）。S33 flag-day。
// data 一字节不动：硬链接进 dst（跨设备退化为拷贝）;hint 从 data 重扫生成
// （ord 在 record header 内现成,DataFile::fold 直接给出）;meta 最后写
// = dst 的 commit point（中途 kill 的 dst 无 meta → 不会被误开;重跑幂等）。
// ---------------------------------------------------------------------------

std::expected<void, std::string>
hintord_link_and_rehint(const fs::path& src_data, const fs::path& dst_dir,
                        MigrateStats& st) {
    const auto name = src_data.filename().string();
    const auto dst_data_path = (dst_dir / name).string();
    const auto dst_hint_path = fileops::mk_hint_filename(dst_data_path);

    std::error_code ec;
    fs::create_hard_link(src_data, dst_data_path, ec);
    if (ec) {  // 跨设备等 → 退化为拷贝
        ec.clear();
        fs::copy_file(src_data, dst_data_path,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return std::unexpected("copy data " + dst_data_path + ": " +
                                   ec.message());
        }
    }

    auto df = fileops::DataFile::open(dst_data_path,
                                      fileops::DataFile::Mode::kRead,
                                      /*sync*/ false, /*mmap_enabled*/ false);
    if (!df) return std::unexpected("open data " + dst_data_path);
    auto dst_hint = fileops::HintFile::open(
        dst_hint_path, fileops::HintFile::Mode::kCreate);
    if (!dst_hint) return std::unexpected("create dst hint " + dst_hint_path);

    // tolerate_crc_errors：单条损坏跳过（与恢复同策略）——被跳过的 record
    // hint 里也不会有,fold(data) 恢复同样读不到它,语义一致。
    std::string werr;
    auto fr = df->fold(
        [&](const codec::DataRecordView& view, std::uint64_t offset,
            std::uint32_t total_size) {
            if (!werr.empty()) return;
            const bool tomb =
                view.type == format::RecordType::kTombstone;
            auto h = dst_hint->write(view.tstamp, total_size, offset, tomb,
                                     view.key, view.ord);
            if (!h) { werr = "write hint to " + dst_hint_path; return; }
            ++st.records;
            if (tomb) ++st.tombstones;
        },
        /*tolerate_crc_errors*/ true);
    if (!fr) return std::unexpected("fold data " + dst_data_path);
    if (!werr.empty()) return std::unexpected(werr);
    if (auto r = dst_hint->finalize(); !r) {
        return std::unexpected("finalize hint " + dst_hint_path);
    }
    ++st.data_files;
    return {};
}

// 前置校验（迁移开工前跑,任何数据工作之前 fail-fast）：src meta 必须是
// 带合法 CRC 的 v4。返回原 18 字节,commit 时改 version + 重算 CRC 用。
std::expected<std::vector<std::byte>, std::string>
hintord_check_meta(const fs::path& src_dir) {
    auto bytes = read_all(src_dir / "bitcask.meta");
    if (!bytes) return std::unexpected(bytes.error());
    if (bytes->size() < 18) return std::unexpected("meta too short");
    const std::byte* b = bytes->data();
    if (std::memcmp(b, "BCME", 4) != 0) return std::unexpected("bad meta magic");
    const auto ver = static_cast<std::uint8_t>(b[4]);
    if (ver == 1) {
        return std::unexpected(
            "src meta is v1 (big-endian era); run the be2le migration first");
    }
    if (ver == 2 || ver == 3) {
        return std::unexpected(
            "src meta is v2/v3 (u32-tstamp era); run the tstamp64 migration");
    }
    if (ver == 5) {
        return std::unexpected(
            "src meta already v5 (current era); nothing to migrate");
    }
    if (ver != 4) return std::unexpected("unknown meta version");
    // v4 带 CRC——迁移工具坚持 fail-fast,不把损坏配置带进新库。
    const std::uint32_t stored = le_load_u32(b + 14);
    const std::uint32_t crc = codec::crc32(std::span<const std::byte>(b, 14));
    if (stored != crc) {
        return std::unexpected("src bitcask.meta CRC mismatch (corrupt)");
    }
    return *std::move(bytes);
}

}  // namespace

std::expected<MigrateStats, std::string>
migrate_be_to_le(std::string_view src_dir, std::string_view dst_dir) {
    const fs::path src(src_dir);
    const fs::path dst(dst_dir);
    if (!fs::exists(src)) return std::unexpected("src dir does not exist");
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) return std::unexpected("cannot create dst dir: " + ec.message());

    MigrateStats st;
    // meta 先行（同时校验 src 确为 v1 大端目录）。
    if (auto r = migrate_meta(src, dst, st); !r) {
        return std::unexpected(r.error());
    }
    if (auto r = migrate_field_schema(src, dst, st); !r) {
        return std::unexpected(r.error());
    }
    // 逐 data 文件（hint 由其重生成）。ckpt/seg/wal/旧 hint/锁不迁移。
    for (const auto& de : fs::directory_iterator(src)) {
        const auto fname = de.path().filename().string();
        if (fileops::parse_data_tstamp(fname).has_value()) {
            if (auto r = migrate_data_file(de.path(), dst, st); !r) {
                return std::unexpected(r.error());
            }
        }
    }
    return st;
}

std::expected<MigrateStats, std::string>
migrate_u32_to_u64(std::string_view src_dir, std::string_view dst_dir) {
    const fs::path src(src_dir);
    const fs::path dst(dst_dir);
    if (!fs::exists(src)) return std::unexpected("src dir does not exist");
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) return std::unexpected("cannot create dst dir: " + ec.message());

    MigrateStats st;
    // meta 先行（同时校验 src 确为 u32 纪元 v2/v3 目录）。
    if (auto r = migrate_u32_meta(src, dst, st); !r) {
        return std::unexpected(r.error());
    }
    // field.schema 格式在本次 flag-day 未变（S12-3 版式,不含时间戳）,
    // 原样拷贝。
    if (fs::exists(src / "field.schema")) {
        fs::copy_file(src / "field.schema", dst / "field.schema",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return std::unexpected("copy field.schema: " + ec.message());
        }
        st.field_schema_migrated = true;
    }
    // 逐 data 文件（hint 由其重生成）。ckpt/seg/wal/旧 hint/锁不迁移。
    for (const auto& de : fs::directory_iterator(src)) {
        const auto fname = de.path().filename().string();
        if (fileops::parse_data_tstamp(fname).has_value()) {
            if (auto r = migrate_u32_data_file(de.path(), dst, st); !r) {
                return std::unexpected(r.error());
            }
        }
    }
    return st;
}

std::expected<MigrateStats, std::string>
migrate_hint_ord(std::string_view src_dir, std::string_view dst_dir) {
    const fs::path src(src_dir);
    const fs::path dst(dst_dir);
    if (!fs::exists(src)) return std::unexpected("src dir does not exist");

    // 前置校验先行（任何数据工作之前）：src 确为 v4 且 meta CRC 合法。
    auto meta_bytes = hintord_check_meta(src);
    if (!meta_bytes) return std::unexpected(meta_bytes.error());

    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) return std::unexpected("cannot create dst dir: " + ec.message());

    MigrateStats st;
    // field.schema 格式未变,原样拷贝。
    if (fs::exists(src / "field.schema")) {
        fs::copy_file(src / "field.schema", dst / "field.schema",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return std::unexpected("copy field.schema: " + ec.message());
        }
        st.field_schema_migrated = true;
    }
    // 逐 data 文件：硬链接 + 从 data 重扫生成 BCH5 hint。
    // ckpt/seg/wal/旧 hint/锁不迁移（新库首开 fold 重建,与既有迁移器同策略）。
    for (const auto& de : fs::directory_iterator(src)) {
        const auto fname = de.path().filename().string();
        if (fileops::parse_data_tstamp(fname).has_value()) {
            if (auto r = hintord_link_and_rehint(de.path(), dst, st); !r) {
                return std::unexpected(r.error());
            }
        }
    }
    // meta 最后写 = commit point（version 4→5,CRC 重算,其余 14 字节照搬）。
    std::byte out[18];
    std::memcpy(out, meta_bytes->data(), 18);
    out[4] = static_cast<std::byte>(5);
    le_store_u32(out + 14, codec::crc32(std::span<const std::byte>(out, 14)));
    if (auto r = write_all(dst / "bitcask.meta",
                           std::span<const std::byte>(out, 18)); !r) {
        return std::unexpected(r.error());
    }
    st.meta_migrated = true;
    return st;
}

}  // namespace bitcask::migrate
