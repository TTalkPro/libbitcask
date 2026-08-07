// S33-3：OKI run（BCOK v1）+ manifest（BCOM v1）单元测试。
// 覆盖：round-trip / 前缀差分与 ord 回绕 / seek 边界（块界、精确命中、
// 区间之间、首尾越界）/ 损坏与结构错误拒收（fail-fast → 弃用重建）/
// 空 run / manifest round-trip 与拒收。


#include <algorithm>
#include "support/test_paths.hpp"
#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bitcask/codec.hpp"  // crc32（UnknownFlagBit 测试重算 CRC）
#include "bitcask/oki_run.hpp"

namespace fs = std::filesystem;
using bitcask::oki::OkiError;
using bitcask::oki::OkiManifest;
using bitcask::oki::OkiRunReader;
using bitcask::oki::OkiRunWriter;

namespace {

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("bitcask_oki_" + std::to_string(bitcask::test::test_pid()) + "_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path_);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path_, ec); }
    std::string operator/(const std::string& s) const {
        return (path_ / s).string();
    }
    std::string str() const { return path_.string(); }
private:
    fs::path path_;
};

std::span<const std::byte> as_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

struct Row {
    std::string key;
    std::uint64_t ord;
    bool tomb;
};

// 写一个 run（默认小块目标逼出多块），返回路径。
std::string write_run(const TempDir& td, const std::vector<Row>& rows,
                      std::size_t block_target = 64,
                      const char* name = "kv.oki.seg-1") {
    auto w = OkiRunWriter::create(td / name, block_target);
    EXPECT_TRUE(w.has_value());
    for (const auto& r : rows) {
        auto a = w->add(as_bytes(r.key), r.ord, r.tomb);
        EXPECT_TRUE(a.has_value()) << r.key;
    }
    auto st = w->finish();
    EXPECT_TRUE(st.has_value());
    if (st) {
        EXPECT_EQ(st->entries, rows.size());
        EXPECT_EQ(st->file_bytes, fs::file_size(td / name));
    }
    return td / name;
}

// 全量扫出所有条目。
std::vector<Row> scan_all(const OkiRunReader& r) {
    std::vector<Row> out;
    auto c = r.begin();
    OkiRunReader::Entry e;
    while (true) {
        auto n = c.next(e);
        EXPECT_TRUE(n.has_value());
        if (!n || !*n) break;
        out.push_back({e.key, e.ord, e.tomb});
    }
    return out;
}

// 翻转文件第 off 字节的一个 bit。
void flip_byte(const std::string& path, long off) {
    std::FILE* f = std::fopen(path.c_str(), "rb+");
    ASSERT_NE(f, nullptr);
    std::fseek(f, off, SEEK_SET);
    char c;
    ASSERT_EQ(std::fread(&c, 1, 1, f), 1u);
    c = static_cast<char>(c ^ 0x01);
    std::fseek(f, off, SEEK_SET);
    ASSERT_EQ(std::fwrite(&c, 1, 1, f), 1u);
    std::fclose(f);
}

}  // namespace

// ---------------------------------------------------------------------------
// round-trip
// ---------------------------------------------------------------------------

TEST(OkiRun, RoundTripMultiBlock) {
    TempDir td;
    // `prefix:id` 形态（前缀差分主要受益负载）+ 少量墓碑 + 乱序 ord
    // （merge 场景 ord 可回退——回绕差分必须无损）。
    std::vector<Row> rows;
    std::mt19937_64 rng(0xBC0C);
    for (int g = 0; g < 4; ++g) {
        for (int i = 0; i < 50; ++i) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "g%02d:k%04d", g, i);
            rows.push_back({buf, rng() % 100000, (i % 7) == 0});
        }
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.key < b.key; });

    const auto path = write_run(td, rows, /*block_target=*/64);
    auto r = OkiRunReader::open(path);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->entry_count(), rows.size());
    EXPECT_GT(r->block_count(), 4u) << "小块目标必须逼出多块";

    auto got = scan_all(*r);
    ASSERT_EQ(got.size(), rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(got[i].key, rows[i].key) << i;
        EXPECT_EQ(got[i].ord, rows[i].ord) << i;
        EXPECT_EQ(got[i].tomb, rows[i].tomb) << i;
    }
}

TEST(OkiRun, EmptyRunLegal) {
    TempDir td;
    const auto path = write_run(td, {});
    auto r = OkiRunReader::open(path);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->entry_count(), 0u);
    EXPECT_EQ(r->block_count(), 0u);
    EXPECT_TRUE(scan_all(*r).empty());
    auto c = r->seek(as_bytes("x"));
    ASSERT_TRUE(c.has_value());
    OkiRunReader::Entry e;
    auto n = c->next(e);
    ASSERT_TRUE(n.has_value());
    EXPECT_FALSE(*n);
}

TEST(OkiRun, SingleEntryAndLongSharedPrefix) {
    TempDir td;
    // 公共前缀远超块首 key + 二进制 key（含 \0）。
    const std::string p(100, 'p');
    std::vector<Row> rows = {
        {p + std::string(1, '\0') + "a", 5, false},
        {p + std::string(1, '\0') + "b", 3, true},   // ord 回退（回绕）
        {p + "zz", 1ull << 40, false},
    };
    const auto path = write_run(td, rows, /*block_target=*/4096);
    auto r = OkiRunReader::open(path);
    ASSERT_TRUE(r.has_value());
    auto got = scan_all(*r);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0].key, rows[0].key);
    EXPECT_EQ(got[1].ord, 3u);
    EXPECT_TRUE(got[1].tomb);
    EXPECT_EQ(got[2].ord, 1ull << 40);
}

// ---------------------------------------------------------------------------
// writer 契约
// ---------------------------------------------------------------------------

TEST(OkiRun, WriterRejectsOutOfOrderAndDuplicate) {
    TempDir td;
    auto w = OkiRunWriter::create(td / "kv.oki.seg-1");
    ASSERT_TRUE(w.has_value());
    ASSERT_TRUE(w->add(as_bytes("bb"), 1, false).has_value());
    auto dup = w->add(as_bytes("bb"), 2, false);
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error(), OkiError::kOutOfOrder);
    auto lt = w->add(as_bytes("aa"), 3, false);
    ASSERT_FALSE(lt.has_value());
    EXPECT_EQ(lt.error(), OkiError::kOutOfOrder);
    // 合法后继仍可写；finish 后 add/finish 拒绝。
    ASSERT_TRUE(w->add(as_bytes("cc"), 4, false).has_value());
    ASSERT_TRUE(w->finish().has_value());
    auto after = w->add(as_bytes("dd"), 5, false);
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error(), OkiError::kBadState);
    auto fin2 = w->finish();
    ASSERT_FALSE(fin2.has_value());
    EXPECT_EQ(fin2.error(), OkiError::kBadState);
}

TEST(OkiRun, AbandonedWriterLeavesNoFile) {
    TempDir td;
    const auto path = td / "kv.oki.seg-9";
    {
        auto w = OkiRunWriter::create(path);
        ASSERT_TRUE(w.has_value());
        ASSERT_TRUE(w->add(as_bytes("k"), 1, false).has_value());
        // 不 finish：析构必须清 tmp，最终路径不存在。
    }
    EXPECT_FALSE(fs::exists(path));
    for (const auto& de : fs::directory_iterator(td.str())) {
        FAIL() << "目录必须为空，残留: " << de.path();
    }
}

// ---------------------------------------------------------------------------
// seek
// ---------------------------------------------------------------------------

TEST(OkiRun, SeekBoundaries) {
    TempDir td;
    // key = "k0000".."k0199"（步长 2：k 偶数存在，奇数不存在）。
    std::vector<Row> rows;
    for (int i = 0; i < 200; i += 2) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "k%04d", i);
        rows.push_back({buf, static_cast<std::uint64_t>(i), false});
    }
    const auto path = write_run(td, rows, /*block_target=*/48);  // 多块
    auto r = OkiRunReader::open(path);
    ASSERT_TRUE(r.has_value());
    ASSERT_GT(r->block_count(), 8u);

    auto first_ge = [&](std::string_view lo) -> std::optional<std::string> {
        auto c = r->seek(as_bytes(lo));
        EXPECT_TRUE(c.has_value());
        if (!c) return std::nullopt;
        OkiRunReader::Entry e;
        auto n = c->next(e);
        EXPECT_TRUE(n.has_value());
        if (!n || !*n) return std::nullopt;
        return e.key;
    };

    EXPECT_EQ(first_ge(""), "k0000");        // 空 lo = begin
    EXPECT_EQ(first_ge("a"), "k0000");       // 全部 key 之前
    EXPECT_EQ(first_ge("k0000"), "k0000");   // 精确命中首条
    EXPECT_EQ(first_ge("k0100"), "k0100");   // 精确命中中段
    EXPECT_EQ(first_ge("k0101"), "k0102");   // 区间之间（奇数不存在）
    EXPECT_EQ(first_ge("k0198"), "k0198");   // 精确命中末条
    EXPECT_EQ(first_ge("k0199"), std::nullopt);  // 全部 key 之后
    EXPECT_EQ(first_ge("z"), std::nullopt);

    // 每个存在的 key 精确 seek 全验（覆盖所有块界）；seek 后游标继续
    // 顺序推进语义正确。
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto c = r->seek(as_bytes(rows[i].key));
        ASSERT_TRUE(c.has_value());
        OkiRunReader::Entry e;
        auto n = c->next(e);
        ASSERT_TRUE(n.has_value() && *n) << rows[i].key;
        EXPECT_EQ(e.key, rows[i].key);
        EXPECT_EQ(e.ord, rows[i].ord);
        if (i + 1 < rows.size()) {
            auto n2 = c->next(e);
            ASSERT_TRUE(n2.has_value() && *n2);
            EXPECT_EQ(e.key, rows[i + 1].key) << "seek 后顺序推进";
        }
    }
}

// ---------------------------------------------------------------------------
// 损坏拒收（派生缓存语义：任何校验不过 → 整体弃用重建，绝不静默）
// ---------------------------------------------------------------------------

TEST(OkiRun, CorruptionRejected) {
    TempDir td;
    std::vector<Row> rows;
    for (int i = 0; i < 100; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "c%04d", i);
        rows.push_back({buf, static_cast<std::uint64_t>(i * 3), false});
    }
    const auto path = write_run(td, rows, 64);
    const auto size = static_cast<long>(fs::file_size(path));

    // 逐区域翻 bit：header magic / 数据块中部 / 索引区 / trailer CRC 字段。
    for (long off : {0L, size / 2, size - 30L, size - 8L}) {
        const auto copy = td / ("mut" + std::to_string(off));
        fs::copy_file(path, copy);
        flip_byte(copy, off);
        auto r = OkiRunReader::open(copy);
        EXPECT_FALSE(r.has_value()) << "offset " << off << " 翻 bit 必须拒收";
        if (!r) { EXPECT_EQ(r.error(), OkiError::kCorrupt); }
    }

    // 截尾（丢 trailer）。
    {
        const auto copy = td / "trunc";
        fs::copy_file(path, copy);
        fs::resize_file(copy, static_cast<std::uintmax_t>(size - 10));
        auto r = OkiRunReader::open(copy);
        EXPECT_FALSE(r.has_value());
    }
    // 空文件 / 过短文件。
    {
        const auto copy = td / "tiny";
        std::FILE* f = std::fopen(copy.c_str(), "wb");
        std::fwrite("BCOK", 1, 4, f);
        std::fclose(f);
        auto r = OkiRunReader::open(copy);
        EXPECT_FALSE(r.has_value());
    }
    // 好文件仍可开（对照组，排除测试自身写坏）。
    auto ok = OkiRunReader::open(path);
    EXPECT_TRUE(ok.has_value());
}

TEST(OkiRun, UnknownFlagBitFailFast) {
    TempDir td;
    const auto path = write_run(td, {{"only", 7, false}}, 4096);
    // 唯一条目的 flags 是数据块最后一个字节（块后紧跟索引区）。
    // 布局：header(8) + 块 [shared(1)+sfxlen(1)+"only"(4)+ord(1)+flags(1)]
    // → flags 在 offset 8+7 = 15。置一个保留位并**重算 CRC**——绕过 CRC
    // 校验，专门验证 flags 位的独立 fail-fast。
    {
        auto bytes = bitcask::detail::read_file_bytes<>(path);
        ASSERT_TRUE(bytes.has_value());
        auto& b = *bytes;
        ASSERT_EQ(static_cast<std::uint8_t>(b[15]), 0u) << "flags 定位错误";
        b[15] = static_cast<std::byte>(0x40);  // 未知保留位
        const std::uint32_t crc = bitcask::codec::crc32(
            std::span<const std::byte>(b.data(), b.size() - 8));
        // trailer: [... crc u32][magic u32]
        b[b.size() - 8] = static_cast<std::byte>(crc & 0xFF);
        b[b.size() - 7] = static_cast<std::byte>((crc >> 8) & 0xFF);
        b[b.size() - 6] = static_cast<std::byte>((crc >> 16) & 0xFF);
        b[b.size() - 5] = static_cast<std::byte>((crc >> 24) & 0xFF);
        ASSERT_TRUE(bitcask::detail::atomic_write_bytes(path, b));
    }
    auto r = OkiRunReader::open(path);
    ASSERT_TRUE(r.has_value()) << "CRC 已重算，open 应通过";
    auto c = r->begin();
    OkiRunReader::Entry e;
    auto n = c.next(e);
    ASSERT_FALSE(n.has_value()) << "未知 flags 位必须 fail-fast";
    EXPECT_EQ(n.error(), OkiError::kCorrupt);
}

// ---------------------------------------------------------------------------
// manifest
// ---------------------------------------------------------------------------

TEST(OkiManifestIo, RoundTrip) {
    TempDir td;
    OkiManifest m;
    m.runs = {{1, 100}, {7, 5000}, {8, 12345}};
    m.wm = 12345;
    ASSERT_TRUE(bitcask::oki::write_manifest(td.str(), m));
    auto got = bitcask::oki::read_manifest(td.str());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->wm, 12345u);
    ASSERT_EQ(got->runs.size(), 3u);
    EXPECT_EQ(got->runs[1].gen, 7u);
    EXPECT_EQ(got->runs[1].cover_ord, 5000u);

    // 空 manifest（零 run）也 round-trip。
    OkiManifest empty;
    ASSERT_TRUE(bitcask::oki::write_manifest(td.str(), empty));
    auto ge = bitcask::oki::read_manifest(td.str());
    ASSERT_TRUE(ge.has_value());
    EXPECT_TRUE(ge->runs.empty());
    EXPECT_EQ(ge->wm, 0u);
}

TEST(OkiManifestIo, CorruptionAndAbsenceRejected) {
    TempDir td;
    // 不存在 → nullopt。
    EXPECT_FALSE(bitcask::oki::read_manifest(td.str()).has_value());

    OkiManifest m;
    m.runs = {{1, 100}};
    m.wm = 100;
    ASSERT_TRUE(bitcask::oki::write_manifest(td.str(), m));
    const auto path = bitcask::oki::mk_manifest_filename(td.str());
    const auto size = static_cast<long>(fs::file_size(path));
    for (long off = 0; off < size; ++off) {
        flip_byte(path, off);
        EXPECT_FALSE(bitcask::oki::read_manifest(td.str()).has_value())
            << "manifest 第 " << off << " 字节翻 bit 必须整体拒收";
        flip_byte(path, off);  // 还原
    }
    EXPECT_TRUE(bitcask::oki::read_manifest(td.str()).has_value());
}
