// S36-1：BCOK v2（全字段 + 内嵌 bloom）+ SpillingRunBuilder（外排）+
// BCOM v2 单元测试。设计：doc/keydir-disk-resident-design-zh.md §4。
// 覆盖：v2 round-trip（loc 字段 / tomb 免位置 / tstamp 回绕差分）、
// bloom 无假阴性 + FP 率、v2 seek、损坏拒收（未知 flags / 未来版本 /
// bloom 结构）、外排对拍参考实现（(ord, 到达序) 胜出）、墓碑丢弃档、
// manifest v2 与惰性版本选择。


#include <algorithm>
#include "support/test_paths.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bitcask/byte_order.hpp"
#include "bitcask/codec.hpp"  // crc32（损坏注入后重算）
#include "bitcask/oki_run.hpp"

namespace fs = std::filesystem;
using bitcask::oki::kRunTrailerSizeV2;
using bitcask::oki::kRunVersion;
using bitcask::oki::kRunVersion2;
using bitcask::oki::kRunVersion3;
using bitcask::oki::OkiBlockCache;
using bitcask::oki::OkiError;
using bitcask::oki::OkiManifest;
using bitcask::oki::OkiManifestFStats;
using bitcask::oki::OkiManifestMetadata;
using bitcask::oki::OkiRunReader;
using bitcask::oki::OkiRunWriter;
using bitcask::oki::RowLoc;
using bitcask::oki::SpillingRunBuilder;

namespace {

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("bitcask_okiv2_" + std::to_string(bitcask::test::test_pid()) + "_" +
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

std::span<const std::byte> bytes(const std::string& s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::vector<std::byte> read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    const auto* p = reinterpret_cast<const std::byte*>(raw.data());
    return {p, p + raw.size()};
}

void write_all(const std::string& path, std::span<const std::byte> b) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()),
            static_cast<std::streamsize>(b.size()));
}

// 损坏注入后重算全文件 CRC（覆盖 [0, size-8)），保 CRC 关不误伤——
// 让结构校验自己暴露问题。
void fix_crc(std::vector<std::byte>& b) {
    const std::uint32_t crc = bitcask::codec::crc32(
        std::span<const std::byte>(b.data(), b.size() - 8));
    bitcask::le_store_u32(b.data() + b.size() - 8, crc);
}

struct Row {
    std::string key;
    std::uint64_t ord;
    bool tomb;
    bool has_loc;
    RowLoc loc;
};

// 造一批 v2 行：多块（小 block_target 逼出）、tstamp 有降有升（回绕差分）、
// 混入免位置墓碑。
std::vector<Row> make_rows(int n) {
    std::vector<Row> rows;
    rows.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        Row r;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "doc:%08d", i);
        r.key = buf;
        r.ord = static_cast<std::uint64_t>(i) * 3 + 1;
        r.tomb = (i % 7 == 3);
        r.has_loc = !r.tomb;
        if (r.has_loc) {
            r.loc.file_id = static_cast<std::uint32_t>(1 + i % 5);
            r.loc.total_sz = static_cast<std::uint32_t>(64 + i % 512);
            r.loc.offset = static_cast<std::uint64_t>(i) * 128;
            // 有意非单调：偶数行时间倒退，钉死回绕差分。
            const auto iu = static_cast<std::uint64_t>(i);
            r.loc.tstamp = (i % 2 == 0) ? 2'000'000ull - iu : 1'000'000ull + iu;
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

std::string write_v2_run(const TempDir& td, const std::vector<Row>& rows,
                         std::size_t block_target = 96,
                         std::uint32_t version = kRunVersion2) {
    const std::string path = td / "kv.oki.seg-1";
    auto w = OkiRunWriter::create(path, block_target, version,
                                  rows.size());
    EXPECT_TRUE(w.has_value());
    for (const auto& r : rows) {
        auto a = w->add(bytes(r.key), r.ord, r.tomb,
                        r.has_loc ? &r.loc : nullptr);
        EXPECT_TRUE(a.has_value());
    }
    auto f = w->finish();
    EXPECT_TRUE(f.has_value());
    return path;
}

// S43-1：v3 只把数据块 CRC 延迟到首读；开库必须放行块位翻转，并在读端
// 显式报 kCorrupt。缓存与 pread 两条加载路径都要过同一道门。
TEST(OkiRunV3, RoundtripAndLazyBlockCrc) {
    TempDir td;
    const auto rows = make_rows(120);
    const auto path = write_v2_run(td, rows, /*block_target=*/96,
                                   kRunVersion3);

    auto r = OkiRunReader::open(path);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->version(), kRunVersion3);
    EXPECT_EQ(r->entry_count(), rows.size());
    ASSERT_GT(r->block_count(), 1u);

    auto cur = r->begin();
    OkiRunReader::Entry e;
    for (const auto& want : rows) {
        auto n = cur.next(e);
        ASSERT_TRUE(n.has_value() && *n);
        EXPECT_EQ(e.key, want.key);
        EXPECT_EQ(e.has_loc, want.has_loc);
    }

    // 破坏首块数据字节。v3 元数据 CRC 不含数据块，open 仍应成功。
    auto b = read_all(path);
    b[8] = static_cast<std::byte>(
        std::to_integer<std::uint8_t>(b[8]) ^ 0x40);
    write_all(path, b);
    auto lazy = OkiRunReader::open(path);
    ASSERT_TRUE(lazy.has_value());
    auto direct = lazy->find(bytes(rows.front().key));
    ASSERT_FALSE(direct.has_value());
    EXPECT_EQ(direct.error(), OkiError::kCorrupt);

    OkiBlockCache cache(4096);
    auto cached = lazy->find(bytes(rows.front().key), &cache, 1);
    ASSERT_FALSE(cached.has_value());
    EXPECT_EQ(cached.error(), OkiError::kCorrupt);
}

TEST(OkiRunV3, MetadataCrcStillRejectedAtOpen) {
    TempDir td;
    const auto rows = make_rows(80);
    const auto path = write_v2_run(td, rows, 96, kRunVersion3);
    auto b = read_all(path);
    // 破坏首个稀疏索引 key 字节；v3 metadata CRC 必须在 open 拦下。
    const std::size_t index_off_pos = b.size() - 24;
    std::uint64_t index_off = 0;
    for (unsigned i = 0; i < 8; ++i) {
        index_off |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(b[index_off_pos + i])) << (8 * i);
    }
    ASSERT_GT(index_off, 8u);
    ASSERT_LT(index_off + 5, b.size());
    b[static_cast<std::size_t>(index_off) + 4] ^=
        static_cast<std::byte>(0x10);
    write_all(path, b);
    auto r = OkiRunReader::open(path);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), OkiError::kCorrupt);
}

TEST(OkiRunV3, ManifestAcceptsMixedV1V2V3) {
    TempDir td;
    OkiManifest m;
    m.runs.push_back({.gen = 1, .cover_ord = 10, .format_ver = 1});
    m.runs.push_back({.gen = 2, .cover_ord = 20, .format_ver = 2});
    m.runs.push_back({.gen = 3, .cover_ord = 30, .format_ver = 3});
    m.wm = 30;
    ASSERT_TRUE(bitcask::oki::write_manifest(td.str(), m));
    auto back = bitcask::oki::read_manifest(td.str());
    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back->runs.size(), 3u);
    EXPECT_EQ(back->runs[2].format_ver, 3);
    EXPECT_EQ(back->wm, 30u);
}

TEST(OkiManifestV4, MetadataRoundtripAndFailFast) {
    TempDir td;
    OkiManifest m;
    m.runs.push_back({.gen = 1, .cover_ord = 24, .format_ver = 3});
    m.wm = 24;
    m.level_b = true;
    m.metadata = OkiManifestMetadata{
        .next_ord = 41,
        .biggest_file_id = 7,
        .next_file_id = 8,
        .active_keys = 12,
        .live_bytes = 456,
        .oldest_tstamp = 1'000,
        .newest_tstamp = 2'000,
        .reserved = 0,
    };
    m.fstats.push_back(OkiManifestFStats{
        .file_id = 7,
        .pending_delete = true,
        .live_keys = 12,
        .total_keys = 20,
        .live_bytes = 456,
        .total_bytes = 1'024,
        .oldest_tstamp = 1'000,
        .newest_tstamp = 2'000,
        .expiration_epoch = 99,
    });
    m.data_wms.push_back({7, 1'024});
    ASSERT_TRUE(bitcask::oki::write_manifest(td.str(), m));
    ASSERT_EQ(bitcask::le_load_u32(read_all(td / "kv.oki.manifest").data() + 4),
              bitcask::oki::kManifestVersion4);

    auto back = bitcask::oki::read_manifest(td.str());
    ASSERT_TRUE(back.has_value());
    ASSERT_TRUE(back->metadata.has_value());
    EXPECT_EQ(*back->metadata, *m.metadata);
    ASSERT_EQ(back->fstats.size(), 1u);
    EXPECT_EQ(back->fstats[0].file_id, 7u);
    EXPECT_TRUE(back->fstats[0].pending_delete);
    EXPECT_EQ(back->fstats[0].live_keys, 12u);
    EXPECT_EQ(back->fstats[0].expiration_epoch, 99u);
    EXPECT_EQ(back->wm, 24u);
    ASSERT_EQ(back->data_wms.size(), 1u);
    EXPECT_EQ(back->data_wms[0].first, 7u);
    EXPECT_EQ(back->data_wms[0].second, 1'024u);

    // 未知模式位 fail-fast。
    {
        auto b = read_all(td / "kv.oki.manifest");
        b[8] = static_cast<std::byte>(0x03);
        fix_crc(b);
        write_all(td / "kv.oki.manifest", b);
        EXPECT_FALSE(bitcask::oki::read_manifest(td.str()).has_value());
    }

    // 保留位不是“下一个可用字段”：必须显式拒收。
    {
        ASSERT_TRUE(bitcask::oki::write_manifest(td.str(), m));
        auto b = read_all(td / "kv.oki.manifest");
        ASSERT_EQ(b.size(),
                  8 + 1 + 4 + 4 + 4 + 56 + 64 + 12 + 17 + 8 + 8);
        b[8 + 1 + 4 + 4 + 4 + 48] = static_cast<std::byte>(1);
        fix_crc(b);
        write_all(td / "kv.oki.manifest", b);
        EXPECT_FALSE(bitcask::oki::read_manifest(td.str()).has_value());
    }

    // fstats 定长段中的保留 pad 也纳入 CRC/结构校验。
    {
        ASSERT_TRUE(bitcask::oki::write_manifest(td.str(), m));
        auto b = read_all(td / "kv.oki.manifest");
        ASSERT_EQ(b.size(),
                  8 + 1 + 4 + 4 + 4 + 56 + 64 + 12 + 17 + 8 + 8);
        b[8 + 1 + 4 + 4 + 4 + 56 + 1] = static_cast<std::byte>(1);
        fix_crc(b);
        write_all(td / "kv.oki.manifest", b);
        EXPECT_FALSE(bitcask::oki::read_manifest(td.str()).has_value());
    }
}

TEST(OkiRunV2, RoundtripFullFields) {
    TempDir td;
    const auto rows = make_rows(500);
    const auto path = write_v2_run(td, rows);

    auto r = OkiRunReader::open(path);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->version(), kRunVersion2);
    EXPECT_EQ(r->entry_count(), rows.size());
    EXPECT_GT(r->block_count(), 1u);  // 小块目标必须逼出多块

    auto cur = r->begin();
    OkiRunReader::Entry e;
    for (const auto& want : rows) {
        auto n = cur.next(e);
        ASSERT_TRUE(n.has_value() && *n);
        EXPECT_EQ(e.key, want.key);
        EXPECT_EQ(e.ord, want.ord);
        EXPECT_EQ(e.tomb, want.tomb);
        ASSERT_EQ(e.has_loc, want.has_loc);
        if (want.has_loc) {
            EXPECT_EQ(e.loc.file_id, want.loc.file_id);
            EXPECT_EQ(e.loc.total_sz, want.loc.total_sz);
            EXPECT_EQ(e.loc.offset, want.loc.offset);
            EXPECT_EQ(e.loc.tstamp, want.loc.tstamp);
        }
    }
    auto n = cur.next(e);
    ASSERT_TRUE(n.has_value());
    EXPECT_FALSE(*n);
}

TEST(OkiRunV2, BloomNoFalseNegativeAndFpBounded) {
    TempDir td;
    const auto rows = make_rows(10000);
    const auto path = write_v2_run(td, rows, 4096);
    auto r = OkiRunReader::open(path);
    ASSERT_TRUE(r.has_value());

    // 无假阴性：写入过的 key 必须全部 may_contain。
    for (const auto& row : rows) {
        EXPECT_TRUE(r->may_contain(bytes(row.key))) << row.key;
    }
    // FP 率：不存在的 key 命中率应远低于 5%（10 bits/key + k=7 理论 ~1%）。
    int fp = 0;
    for (int i = 0; i < 10000; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "absent:%08d", i);
        if (r->may_contain(bytes(std::string(buf)))) ++fp;
    }
    EXPECT_LT(fp, 500) << "bloom FP rate too high: " << fp << "/10000";
}

TEST(OkiRunV2, SeekCarriesLoc) {
    TempDir td;
    const auto rows = make_rows(300);
    const auto path = write_v2_run(td, rows);
    auto r = OkiRunReader::open(path);
    ASSERT_TRUE(r.has_value());

    for (std::size_t i = 0; i < rows.size(); i += 37) {
        auto cur = r->seek(bytes(rows[i].key));
        ASSERT_TRUE(cur.has_value());
        OkiRunReader::Entry e;
        auto n = cur->next(e);
        ASSERT_TRUE(n.has_value() && *n);
        EXPECT_EQ(e.key, rows[i].key);
        EXPECT_EQ(e.has_loc, rows[i].has_loc);
        if (rows[i].has_loc) { EXPECT_EQ(e.loc.offset, rows[i].loc.offset); }
    }
}

TEST(OkiRunV2, EmptyRunValid) {
    TempDir td;
    const std::string path = td / "kv.oki.seg-9";
    auto w = OkiRunWriter::create(path, 4096, kRunVersion2, 0);
    ASSERT_TRUE(w.has_value());
    ASSERT_TRUE(w->finish().has_value());
    auto r = OkiRunReader::open(path);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->entry_count(), 0u);
    OkiRunReader::Entry e;
    auto cur = r->begin();
    auto n = cur.next(e);
    ASSERT_TRUE(n.has_value());
    EXPECT_FALSE(*n);
}

TEST(OkiRunV2, UnknownFlagBitRejected) {
    TempDir td;
    const auto rows = make_rows(8);
    const auto path = write_v2_run(td, rows, 4096);
    auto b = read_all(path);
    // 首行 flags 落在 header(8) 之后：[vbyte 0][vbyte klen][key][vbyte ord][flags]。
    // "doc:00000000" 12B：8 + 1 + 1 + 12 + 1 = 23 → flags 在偏移 23。
    const std::size_t flags_off = 8 + 1 + 1 + rows[0].key.size() + 1;
    b[flags_off] = static_cast<std::byte>(
        static_cast<std::uint8_t>(b[flags_off]) | 0x04);
    fix_crc(b);
    write_all(path, b);

    auto r = OkiRunReader::open(path);
    ASSERT_TRUE(r.has_value());  // 结构/CRC 过——行级 fail-fast 在解码时
    auto cur = r->begin();
    OkiRunReader::Entry e;
    auto n = cur.next(e);
    ASSERT_FALSE(n.has_value());
    EXPECT_EQ(n.error(), OkiError::kCorrupt);
}

TEST(OkiRunV2, FutureVersionRejected) {
    TempDir td;
    const auto rows = make_rows(8);
    const auto path = write_v2_run(td, rows, 4096);
    auto b = read_all(path);
    bitcask::le_store_u32(b.data() + 4, 3);  // version 3
    fix_crc(b);
    write_all(path, b);
    auto r = OkiRunReader::open(path);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), OkiError::kCorrupt);
}

TEST(OkiRunV2, CorruptBloomRejected) {
    TempDir td;
    const auto rows = make_rows(8);
    const auto path = write_v2_run(td, rows, 4096);
    auto b = read_all(path);
    // trailer 里的 bloom_off 指向 [entry_count u64][index_off u64][bloom_off]。
    const std::size_t bloom_off_pos = b.size() - kRunTrailerSizeV2 + 16;
    const std::uint64_t bloom_off = bitcask::le_load_u64(b.data() + bloom_off_pos);
    // n_bits 改成非 64 倍数 → 结构校验拒收。
    bitcask::le_store_u64(b.data() + bloom_off, 100);
    fix_crc(b);
    write_all(path, b);
    auto r = OkiRunReader::open(path);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), OkiError::kCorrupt);
}

TEST(OkiRunV2, V1StillReadableNoLoc) {
    TempDir td;
    const std::string path = td / "kv.oki.seg-2";
    auto w = OkiRunWriter::create(path);  // 默认 v1
    ASSERT_TRUE(w.has_value());
    ASSERT_TRUE(w->add(bytes(std::string("a")), 1, false).has_value());
    ASSERT_TRUE(w->add(bytes(std::string("b")), 2, true).has_value());
    // v1 拒收 loc。
    RowLoc loc;
    ASSERT_FALSE(w->add(bytes(std::string("c")), 3, false, &loc).has_value());
    ASSERT_TRUE(w->finish().has_value());

    auto r = OkiRunReader::open(path);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->version(), kRunVersion);
    EXPECT_TRUE(r->may_contain(bytes(std::string("zzz"))));  // v1 恒 true
    auto cur = r->begin();
    OkiRunReader::Entry e;
    ASSERT_TRUE(cur.next(e).value_or(false));
    EXPECT_FALSE(e.has_loc);
}

// ---------------------------------------------------------------------------
// SpillingRunBuilder
// ---------------------------------------------------------------------------

TEST(SpillingBuilder, MatchesReferenceAcrossSpills) {
    TempDir td;
    // 8k key 池、30k 无序行（重 key、重 ord）→ 小 spill 逼出多次落盘。
    std::mt19937 rng(0x5361);
    std::uniform_int_distribution<int> kd(0, 7999);
    struct Ref {
        std::uint64_t ord;
        std::uint64_t seq;
        bool tomb;
        std::uint64_t off;
    };
    std::map<std::string, Ref> ref;

    auto b = SpillingRunBuilder::create(td.str(), 7, kRunVersion2,
                                        /*spill_bytes=*/32 << 10);
    ASSERT_TRUE(b.has_value());
    for (std::uint64_t seq = 0; seq < 30000; ++seq) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "k:%06d", kd(rng));
        const std::string key = buf;
        const std::uint64_t ord = seq / 3;  // 制造同 ord 冲突（后到者胜）
        const bool tomb = (seq % 11 == 5);
        RowLoc loc;
        loc.file_id = 1;
        loc.total_sz = 64;
        loc.offset = seq * 100;
        loc.tstamp = 1000 + seq;
        auto a = b->add(bytes(key), ord, tomb, tomb ? nullptr : &loc);
        ASSERT_TRUE(a.has_value());
        auto it = ref.find(key);
        if (it == ref.end() || ord > it->second.ord ||
            (ord == it->second.ord && seq > it->second.seq)) {
            ref[key] = {ord, seq, tomb, loc.offset};
        }
    }
    auto st = b->finish(/*fsync_dir=*/false);
    ASSERT_TRUE(st.has_value());
    EXPECT_EQ(st->entries, ref.size());

    auto r = OkiRunReader::open(td / "kv.oki.seg-7");
    ASSERT_TRUE(r.has_value());
    auto cur = r->begin();
    OkiRunReader::Entry e;
    auto it = ref.begin();
    while (true) {
        auto n = cur.next(e);
        ASSERT_TRUE(n.has_value());
        if (!*n) break;
        ASSERT_NE(it, ref.end());
        EXPECT_EQ(e.key, it->first);
        EXPECT_EQ(e.ord, it->second.ord);
        EXPECT_EQ(e.tomb, it->second.tomb);
        if (!e.tomb) {
            ASSERT_TRUE(e.has_loc);
            EXPECT_EQ(e.loc.offset, it->second.off);
        }
        ++it;
    }
    EXPECT_EQ(it, ref.end());

    // spill 残件必须清光。
    int strays = 0;
    for (const auto& f : fs::directory_iterator(td.str())) {
        if (f.path().filename().string().rfind("kv.oki.spill-", 0) == 0) {
            ++strays;
        }
    }
    EXPECT_EQ(strays, 0);
}

TEST(SpillingBuilder, DropTombstones) {
    TempDir td;
    auto b = SpillingRunBuilder::create(td.str(), 3, kRunVersion2,
                                        /*spill_bytes=*/4096,
                                        /*drop_tombstones=*/true);
    ASSERT_TRUE(b.has_value());
    RowLoc loc;
    ASSERT_TRUE(b->add(bytes(std::string("a")), 1, false, &loc).has_value());
    ASSERT_TRUE(b->add(bytes(std::string("b")), 2, true).has_value());
    ASSERT_TRUE(b->add(bytes(std::string("c")), 3, false, &loc).has_value());
    // b 的更高 ord 墓碑覆盖 a？不——不同 key 互不影响；同 key 墓碑胜出后被丢。
    ASSERT_TRUE(b->add(bytes(std::string("a")), 9, true).has_value());
    auto st = b->finish();
    ASSERT_TRUE(st.has_value());
    EXPECT_EQ(st->entries, 1u);  // 仅 c 幸存（a 被高 ord 墓碑吃掉后丢弃，b 墓碑丢弃）

    auto r = OkiRunReader::open(td / "kv.oki.seg-3");
    ASSERT_TRUE(r.has_value());
    auto cur = r->begin();
    OkiRunReader::Entry e;
    ASSERT_TRUE(cur.next(e).value_or(false));
    EXPECT_EQ(e.key, "c");
}

// ---------------------------------------------------------------------------
// Manifest v2
// ---------------------------------------------------------------------------

TEST(OkiManifestV2, RoundtripAndLazyVersion) {
    TempDir td;
    // 全 v1 条目 → 写出 BCOM v1 字节（老读端可读）。
    OkiManifest m1;
    m1.runs.push_back({.gen = 1, .cover_ord = 10, .format_ver = 1});
    m1.wm = 10;
    ASSERT_TRUE(bitcask::oki::write_manifest(td.str(), m1));
    {
        auto b = read_all(td / "kv.oki.manifest");
        EXPECT_EQ(bitcask::le_load_u32(b.data() + 4), 1u);  // 惰性 v1
        auto back = bitcask::oki::read_manifest(td.str());
        ASSERT_TRUE(back.has_value());
        ASSERT_EQ(back->runs.size(), 1u);
        EXPECT_EQ(back->runs[0].format_ver, 1);
    }
    // 含 v2 条目 → BCOM v2，format_ver 往返保真。
    OkiManifest m2;
    m2.runs.push_back({.gen = 1, .cover_ord = 10, .format_ver = 1});
    m2.runs.push_back({.gen = 2, .cover_ord = 20, .format_ver = 2});
    m2.wm = 20;
    ASSERT_TRUE(bitcask::oki::write_manifest(td.str(), m2));
    {
        auto b = read_all(td / "kv.oki.manifest");
        EXPECT_EQ(bitcask::le_load_u32(b.data() + 4), 2u);
        auto back = bitcask::oki::read_manifest(td.str());
        ASSERT_TRUE(back.has_value());
        ASSERT_EQ(back->runs.size(), 2u);
        EXPECT_EQ(back->runs[0].format_ver, 1);
        EXPECT_EQ(back->runs[1].format_ver, 2);
        EXPECT_EQ(back->wm, 20u);
    }
    // 未知 format_ver → 整体拒收。
    {
        auto b = read_all(td / "kv.oki.manifest");
        b[12 + 16] = std::byte{9};  // 首条目 format_ver
        const std::uint32_t crc = bitcask::codec::crc32(
            std::span<const std::byte>(b.data(), b.size() - 8));
        bitcask::le_store_u32(b.data() + b.size() - 8, crc);
        write_all(td / "kv.oki.manifest", b);
        EXPECT_FALSE(bitcask::oki::read_manifest(td.str()).has_value());
    }
}

}  // namespace

// S66（feedbacks/2026-11-23-point-get-after-flush-scans-run-block-linearly.md）：find 的块内
// 定位不再走「逐条完整解码」——改成前缀压缩上的增量比较，块缓存开着时再配采样索引。
// 对照：顺序游标读出的全部行就是真相。各种 key 形态（互为前缀、长公共前缀、空后缀、
// 高位字节、长 key）、各版本、各块大小、三种缓存状态（无缓存 / 缓存开 / 缓存容量 0），
// 在场的 key 每一个都要逐字段相同，不在场的探针（比全 run 小、夹在两行之间、是某行的
// 前缀或延伸、比全 run 大）都要判缺席。
TEST(OkiRunFind, MatchesSequentialScanForAllKeyShapes) {
    std::mt19937 rng(66);
    std::vector<std::string> keys;
    for (int i = 0; i < 1500; ++i) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "doc:%06d", i * 3);
        keys.emplace_back(buf);
    }
    // 互为前缀的链、只差末字节、含 0x00 / 0xFF 的 key、长 key。
    for (int i = 0; i < 40; ++i) keys.push_back(std::string(static_cast<std::size_t>(i + 1), 'a'));
    for (int i = 0; i < 60; ++i) keys.push_back("pfx/shared/long/common/prefix/" + std::string(1, static_cast<char>('!' + i)));
    for (int i = 0; i < 30; ++i) keys.push_back(std::string("z") + static_cast<char>(i) + std::string("\xff", 1));
    for (int i = 0; i < 30; ++i) keys.push_back(std::string("\xff\xfe", 2) + std::to_string(1000 + i));
    for (int i = 0; i < 20; ++i) keys.push_back("L" + std::string(300 + static_cast<std::size_t>(i) * 7, static_cast<char>('m' + i % 5)) + std::to_string(i));
    for (int i = 0; i < 300; ++i) {
        std::string k;
        const int len = 1 + static_cast<int>(rng() % 12);
        for (int j = 0; j < len; ++j) k.push_back(static_cast<char>("abAB01"[rng() % 6]));
        keys.push_back("r/" + k);
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    auto make = [&](std::uint32_t version) {
        std::vector<Row> rows;
        std::uint64_t ord = 0;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            Row r;
            r.key = keys[i];
            ord += 1 + rng() % 1000;
            r.ord = (i % 97 == 5) ? ord - 500 : ord;       // ord 不单调（差分回绕）
            r.tomb = (i % 11 == 4);
            r.has_loc = version != kRunVersion && !r.tomb;
            if (r.has_loc) {
                r.loc.file_id = static_cast<std::uint32_t>(1 + rng() % 9);
                r.loc.total_sz = static_cast<std::uint32_t>(40 + rng() % 5000);
                r.loc.offset = rng() % (1ull << 33);
                r.loc.tstamp = 1700000000ull + (rng() % 2 ? rng() % 100000 : 0);  // 有升有降
            }
            rows.push_back(std::move(r));
        }
        return rows;
    };

    std::vector<std::string> probes;
    probes.push_back("");
    probes.push_back(std::string(1, '\0'));
    probes.push_back(std::string("\xff\xff\xff\xff", 4));
    for (std::size_t i = 0; i < keys.size(); i += 3) {
        probes.push_back(keys[i] + std::string(1, '\0'));          // 紧跟在后
        probes.push_back(keys[i] + "x");                           // 延伸
        if (keys[i].size() > 1) probes.push_back(keys[i].substr(0, keys[i].size() - 1));  // 前缀
        std::string bump = keys[i];
        bump.back() = static_cast<char>(static_cast<unsigned char>(bump.back()) + 1);
        probes.push_back(bump);
    }

    for (const std::uint32_t version : {kRunVersion, kRunVersion2, kRunVersion3}) {
        for (const std::size_t block_target : {std::size_t{64}, std::size_t{300}, std::size_t{4096}}) {
            SCOPED_TRACE("version " + std::to_string(version) + " block_target " +
                         std::to_string(block_target));
            TempDir td;
            const auto rows = make(version);
            std::string path;
            if (version == kRunVersion) {
                path = td / "kv.oki.seg-1";
                auto w = OkiRunWriter::create(path, block_target);
                ASSERT_TRUE(w.has_value());
                for (const auto& r : rows) ASSERT_TRUE(w->add(bytes(r.key), r.ord, r.tomb).has_value());
                ASSERT_TRUE(w->finish().has_value());
            } else {
                path = write_v2_run(td, rows, block_target, version);
            }
            auto r = OkiRunReader::open(path);
            ASSERT_TRUE(r.has_value());
            if (block_target < 4096) {
                ASSERT_GT(r->block_count(), 8u);
            }

            // 真相：顺序游标。
            std::map<std::string, OkiRunReader::Entry> truth;
            {
                auto cur = r->begin();
                OkiRunReader::Entry e;
                while (true) {
                    auto n = cur.next(e);
                    ASSERT_TRUE(n.has_value());
                    if (!*n) break;
                    truth.emplace(e.key, e);
                }
                ASSERT_EQ(truth.size(), keys.size());
            }

            OkiBlockCache on(8u << 20);
            OkiBlockCache off(0);
            ASSERT_TRUE(on.enabled());
            ASSERT_FALSE(off.enabled());
            OkiBlockCache* const modes[] = {nullptr, &on, &off, &on};  // &on 两遍：第二遍全命中
            for (OkiBlockCache* cache : modes) {
                for (const auto& [k, want] : truth) {
                    auto got = r->find(bytes(k), cache, 7);
                    ASSERT_TRUE(got.has_value()) << k;
                    ASSERT_TRUE(got->has_value()) << "missing present key " << k;
                    const auto& e = **got;
                    ASSERT_EQ(e.key, want.key);
                    ASSERT_EQ(e.ord, want.ord) << k;
                    ASSERT_EQ(e.tomb, want.tomb) << k;
                    ASSERT_EQ(e.has_loc, want.has_loc) << k;
                    if (want.has_loc) {
                        ASSERT_EQ(e.loc.file_id, want.loc.file_id) << k;
                        ASSERT_EQ(e.loc.total_sz, want.loc.total_sz) << k;
                        ASSERT_EQ(e.loc.offset, want.loc.offset) << k;
                        ASSERT_EQ(e.loc.tstamp, want.loc.tstamp) << k;
                    }
                }
                for (const auto& p : probes) {
                    if (truth.count(p) != 0) continue;
                    auto got = r->find(bytes(p), cache, 7);
                    ASSERT_TRUE(got.has_value());
                    ASSERT_FALSE(got->has_value()) << "found absent key, probe size " << p.size();
                }
            }
        }
    }
}

// 块缓存命中不再重算块 CRC；坏块仍在装载时被拒、且不进缓存（之后每次都报错，而不是
// 第一次报错、第二次从缓存里拿到坏块）。
TEST(OkiRunFind, CorruptBlockNeverEntersCache) {
    TempDir td;
    const auto rows = make_rows(400);
    const auto path = write_v2_run(td, rows, 96, kRunVersion3);
    {
        auto good = OkiRunReader::open(path);
        ASSERT_TRUE(good.has_value());
        OkiBlockCache cache(1u << 20);
        for (int round = 0; round < 3; ++round) {
            for (const auto& row : rows) {
                auto got = good->find(bytes(row.key), &cache, 1);
                ASSERT_TRUE(got.has_value() && got->has_value()) << row.key;
                EXPECT_EQ((**got).ord, row.ord);
            }
        }
        EXPECT_GT(cache.stats().hits, cache.stats().misses);
    }
    auto b = read_all(path);
    b[8] = static_cast<std::byte>(std::to_integer<std::uint8_t>(b[8]) ^ 0x40);
    write_all(path, b);
    auto bad = OkiRunReader::open(path);
    ASSERT_TRUE(bad.has_value());
    OkiBlockCache cache(1u << 20);
    for (int i = 0; i < 3; ++i) {
        auto got = bad->find(bytes(rows.front().key), &cache, 2);
        ASSERT_FALSE(got.has_value());
        EXPECT_EQ(got.error(), OkiError::kCorrupt);
    }
    EXPECT_EQ(cache.stats().blocks, 0u);
    // 没坏的块照常可读。
    auto ok = bad->find(bytes(rows.back().key), &cache, 2);
    ASSERT_TRUE(ok.has_value() && ok->has_value());
    EXPECT_EQ((**ok).ord, rows.back().ord);
}
