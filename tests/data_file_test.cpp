// M3.1 unit tests: DataFile + HintFile.


#include <cstring>
#include "support/test_paths.hpp"
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <fstream>

#include "bitcask/codec.hpp"
#include "bitcask/data_file.hpp"
#include "bitcask/field_schema.hpp"
#include "bitcask/format.hpp"
#include "bitcask/hint_file.hpp"
#include "bitcask/migrate.hpp"
#include "bitcask/search_checkpoint.hpp"

using bitcask::fileops::DataFile;
using bitcask::fileops::DataFileError;
using bitcask::fileops::HintFile;
using bitcask::fileops::ReadRecord;
using bitcask::format::RecordType;
using bitcask::format::kHeaderSize;

namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("bitcask_dfile_" + std::to_string(bitcask::test::test_pid()) + "_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path_);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path_, ec); }
    std::string operator/(const std::string& s) const {
        return (path_ / s).string();
    }
private:
    fs::path path_;
};

std::span<const std::byte> as_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string view_str(std::span<const std::byte> b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

// 大端编码器（仅迁移测试用——构造 v1 legacy 字节固件）。
void be_put16(std::vector<std::byte>& b, std::uint16_t v) {
    b.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::byte>(v & 0xFF));
}
void be_put32(std::vector<std::byte>& b, std::uint32_t v) {
    for (int i = 3; i >= 0; --i)
        b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
void be_put64(std::vector<std::byte>& b, std::uint64_t v) {
    for (int i = 7; i >= 0; --i)
        b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
// 一条 v1 大端 data record：[crc BE][type][tstamp BE][ord BE][keysz BE][valsz BE][key][val]。
std::vector<std::byte> be_data_record(bitcask::format::RecordType type,
                                      std::uint32_t ts, std::uint64_t ord,
                                      std::string_view key,
                                      std::string_view val) {
    std::vector<std::byte> covered;
    covered.push_back(static_cast<std::byte>(type));
    be_put32(covered, ts);
    be_put64(covered, ord);
    be_put16(covered, static_cast<std::uint16_t>(key.size()));
    be_put32(covered, static_cast<std::uint32_t>(val.size()));
    auto kb = as_bytes(key);
    covered.insert(covered.end(), kb.begin(), kb.end());
    auto vb = as_bytes(val);
    covered.insert(covered.end(), vb.begin(), vb.end());
    std::vector<std::byte> rec;
    be_put32(rec, bitcask::codec::crc32(covered));  // CRC 字段（大端）
    rec.insert(rec.end(), covered.begin(), covered.end());
    return rec;
}

void write_file_bytes(const std::string& path, std::span<const std::byte> b) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    if (!b.empty()) { ASSERT_EQ(std::fwrite(b.data(), 1, b.size(), f), b.size()); }
    std::fclose(f);
}

}  // namespace

// ---------------------------------------------------------------------------
// Filename helpers
// ---------------------------------------------------------------------------
TEST(Filename, MkAndParse) {
    using bitcask::fileops::mk_data_filename;
    using bitcask::fileops::mk_hint_filename;
    using bitcask::fileops::parse_data_tstamp;

    // S37-5：mk_data_filename 走 fs::path 拼接，分隔符是**平台原生**的
    // （Windows 上是 '\'）。期望值按同样方式构造——本用例要守的是
    // 「<tstamp>.bitcask.data 这个文件名怎么拼」，不是分隔符长什么样。
    EXPECT_EQ(mk_data_filename("/tmp/foo", 12345),
              (std::filesystem::path("/tmp/foo") / "12345.bitcask.data").string());
    EXPECT_EQ(mk_hint_filename("/tmp/foo/12345.bitcask.data"),
              "/tmp/foo/12345.bitcask.hint");

    EXPECT_EQ(parse_data_tstamp("/tmp/x/12345.bitcask.data"), 12345u);
    EXPECT_EQ(parse_data_tstamp("12345.bitcask.data"), 12345u);
    EXPECT_FALSE(parse_data_tstamp("not-a-bitcask").has_value());
    EXPECT_FALSE(parse_data_tstamp("abc.bitcask.data").has_value());
}

// ---------------------------------------------------------------------------
// DataFile
// ---------------------------------------------------------------------------
TEST(DataFile, CreateAppendReadRoundTrip) {
    TempDir td;
    const auto path = td / "1.bitcask.data";

    auto f = DataFile::open(path, DataFile::Mode::kCreate);
    ASSERT_TRUE(f);

    auto w1 = f->write(RecordType::kDoc, /*tstamp*/ 100, /*ord*/ 1,
                       as_bytes("k1"), as_bytes("v1"));
    ASSERT_TRUE(w1);
    EXPECT_EQ(w1->offset, 0u);

    auto w2 = f->write(RecordType::kDoc, /*tstamp*/ 101, /*ord*/ 2,
                       as_bytes("k2"), as_bytes("vvv"));
    ASSERT_TRUE(w2);
    EXPECT_EQ(w2->offset, w1->total_size);

    auto r1 = f->read(w1->offset, w1->total_size);
    ASSERT_TRUE(r1);
    EXPECT_EQ(r1->type, RecordType::kDoc);
    EXPECT_EQ(r1->tstamp, 100u);
    EXPECT_EQ(r1->ord, 1u);
    EXPECT_EQ(view_str(r1->key),   "k1");
    EXPECT_EQ(view_str(r1->value), "v1");

    auto r2 = f->read(w2->offset, w2->total_size);
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->tstamp, 101u);
    EXPECT_EQ(r2->ord, 2u);
    EXPECT_EQ(view_str(r2->key),   "k2");
    EXPECT_EQ(view_str(r2->value), "vvv");
}

TEST(DataFile, FoldVisitsAllRecords) {
    TempDir td;
    const auto path = td / "2.bitcask.data";

    auto f = DataFile::open(path, DataFile::Mode::kCreate);
    ASSERT_TRUE(f);
    ASSERT_TRUE(f->write(RecordType::kDoc, 1, 1, as_bytes("a"),   as_bytes("AA")));
    ASSERT_TRUE(f->write(RecordType::kDoc, 2, 2, as_bytes("b"),   as_bytes("BBBB")));
    ASSERT_TRUE(f->write(RecordType::kDoc, 3, 3, as_bytes("ccc"), as_bytes(std::string(257, 'x'))));

    std::vector<std::pair<std::string, std::string>> seen;
    auto fold_res = f->fold(
        [&](const auto& v, std::uint64_t /*off*/, std::uint32_t /*total*/) {
            seen.emplace_back(view_str(v.key), view_str(v.value));
        });
    ASSERT_TRUE(fold_res);
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0].first,  "a");
    EXPECT_EQ(seen[1].first,  "b");
    EXPECT_EQ(seen[2].first,  "ccc");
    EXPECT_EQ(seen[2].second.size(), 257u);
}

TEST(DataFile, OpenForReadAndFold) {
    TempDir td;
    const auto path = td / "3.bitcask.data";

    {
        auto f = DataFile::open(path, DataFile::Mode::kCreate);
        ASSERT_TRUE(f);
        ASSERT_TRUE(f->write(RecordType::kDoc, 1, 1, as_bytes("k"), as_bytes("v")));
    }
    auto r = DataFile::open(path, DataFile::Mode::kRead);
    ASSERT_TRUE(r);
    int n = 0;
    auto fr = r->fold([&](const auto&, std::uint64_t, std::uint32_t) { ++n; });
    ASSERT_TRUE(fr);
    EXPECT_EQ(n, 1);
}

TEST(DataFile, BadCrcReturnsKBadCrc) {
    TempDir td;
    const auto path = td / "4.bitcask.data";
    {
        auto f = DataFile::open(path, DataFile::Mode::kCreate);
        ASSERT_TRUE(f);
        auto w = f->write(RecordType::kDoc, 1, 1, as_bytes("k"), as_bytes("vvvv"));
        ASSERT_TRUE(w);
    }
    // Corrupt the value.
    {
        std::FILE* fp = std::fopen(path.c_str(), "rb+");
        ASSERT_NE(fp, nullptr);
        std::fseek(fp, -1, SEEK_END);
        char c;
        ASSERT_EQ(std::fread(&c, 1, 1, fp), 1u);
        c ^= 0x01;
        std::fseek(fp, -1, SEEK_END);
        std::fwrite(&c, 1, 1, fp);
        std::fclose(fp);
    }
    auto r = DataFile::open(path, DataFile::Mode::kRead);
    ASSERT_TRUE(r);
    auto rec = r->read(0, kHeaderSize + 1 + 4);
    ASSERT_FALSE(rec);
    EXPECT_EQ(rec.error().kind, DataFileError::kBadCrc);
}

TEST(DataFile, ReuseOffsetMatchesIndex) {
    TempDir td;
    const auto path = td / "5.bitcask.data";
    auto f = DataFile::open(path, DataFile::Mode::kCreate);
    ASSERT_TRUE(f);
    auto w = f->write(RecordType::kDoc, 7, 1, as_bytes("hello"), as_bytes("world"));
    ASSERT_TRUE(w);
    EXPECT_EQ(w->offset, 0u);
    EXPECT_EQ(w->total_size, kHeaderSize + 5u + 5u);
    EXPECT_EQ(f->size(), w->total_size);

    auto rec = f->read(w->offset, w->total_size);
    ASSERT_TRUE(rec);
    EXPECT_EQ(view_str(rec->key),   "hello");
    EXPECT_EQ(view_str(rec->value), "world");
}

// ---------------------------------------------------------------------------
// HintFile
// ---------------------------------------------------------------------------
TEST(HintFile, AppendFinalizeFold) {
    TempDir td;
    const auto path = td / "1.bitcask.hint";

    auto h = HintFile::open(path, HintFile::Mode::kCreate);
    ASSERT_TRUE(h);
    ASSERT_TRUE(h->write(1, /*total_sz*/ 30, /*off*/ 0,   /*tomb*/ false, as_bytes("a"), /*ord*/ 1));
    ASSERT_TRUE(h->write(2, /*total_sz*/ 40, /*off*/ 30,  /*tomb*/ true,  as_bytes("bb"), /*ord*/ 2));
    ASSERT_TRUE(h->write(3, /*total_sz*/ 50, /*off*/ 70,  /*tomb*/ false, as_bytes("ccc"), /*ord*/ 3));
    ASSERT_TRUE(h->finalize());

    auto r = HintFile::open(path, HintFile::Mode::kRead);
    ASSERT_TRUE(r);
    std::vector<std::string> keys;
    std::vector<bool> tombs;
    auto fr = r->fold([&](const auto& rec) {
        keys.push_back(view_str(rec.key));
        tombs.push_back(rec.tombstone);
    });
    ASSERT_TRUE(fr);
    EXPECT_EQ(keys, (std::vector<std::string>{"a", "bb", "ccc"}));
    EXPECT_EQ(tombs, (std::vector<bool>{false, true, false}));
}

TEST(HintFile, ValidateTrailerHappyPath) {
    TempDir td;
    const auto path = td / "good.bitcask.hint";
    auto h = HintFile::open(path, HintFile::Mode::kCreate);
    ASSERT_TRUE(h);
    ASSERT_TRUE(h->write(1, 30, 0, false, as_bytes("a"), /*ord*/ 4));
    ASSERT_TRUE(h->write(2, 40, 30, false, as_bytes("bb"), /*ord*/ 5));
    ASSERT_TRUE(h->finalize());

    auto r = HintFile::open(path, HintFile::Mode::kRead);
    ASSERT_TRUE(r);
    auto v = r->validate_trailer();
    ASSERT_TRUE(v);
    EXPECT_TRUE(*v);
}

TEST(HintFile, ValidateTrailerCorrupted) {
    TempDir td;
    const auto path = td / "bad.bitcask.hint";
    {
        auto h = HintFile::open(path, HintFile::Mode::kCreate);
        ASSERT_TRUE(h);
        ASSERT_TRUE(h->write(1, 30, 0, false, as_bytes("a"), /*ord*/ 6));
        ASSERT_TRUE(h->write(2, 40, 30, false, as_bytes("bb"), /*ord*/ 7));
        ASSERT_TRUE(h->finalize());
    }
    // Flip a byte in the body.
    {
        std::FILE* fp = std::fopen(path.c_str(), "rb+");
        ASSERT_NE(fp, nullptr);
        std::fseek(fp, 5, SEEK_SET);
        char c;
        ASSERT_EQ(std::fread(&c, 1, 1, fp), 1u);
        c ^= 0x01;
        std::fseek(fp, 5, SEEK_SET);
        std::fwrite(&c, 1, 1, fp);
        std::fclose(fp);
    }
    auto r = HintFile::open(path, HintFile::Mode::kRead);
    ASSERT_TRUE(r);
    auto v = r->validate_trailer();
    ASSERT_TRUE(v);
    EXPECT_FALSE(*v);
}

TEST(HintFile, ValidateMissingTrailer) {
    TempDir td;
    const auto path = td / "trunc.bitcask.hint";
    auto h = HintFile::open(path, HintFile::Mode::kCreate);
    ASSERT_TRUE(h);
    ASSERT_TRUE(h->write(1, 30, 0, false, as_bytes("a"), /*ord*/ 8));
    // No finalize() — trailer missing.

    auto r = HintFile::open(path, HintFile::Mode::kRead);
    ASSERT_TRUE(r);
    auto v = r->validate_trailer();
    ASSERT_TRUE(v);
    EXPECT_FALSE(*v);
}

TEST(HintFile, EmptyFileFoldReturnsNoRecords) {
    TempDir td;
    const auto path = td / "empty.bitcask.hint";
    {
        auto h = HintFile::open(path, HintFile::Mode::kCreate);
        ASSERT_TRUE(h);
        ASSERT_TRUE(h->finalize());
    }
    auto r = HintFile::open(path, HintFile::Mode::kRead);
    ASSERT_TRUE(r);
    int n = 0;
    auto fr = r->fold([&](const auto&) { ++n; });
    ASSERT_TRUE(fr);
    EXPECT_EQ(n, 0);
}

// ---------------------------------------------------------------------------
// Hint 字节 golden（自洽,非跨语言）。
//   P:flag-day 后全盘统一小端——**不再与 legacy Erlang 大端字节互通**,故原
//   "cross-language golden" 失效。下方 hex 是当前 LE 编码对三条 record + trailer
//   的钉死字节(字段全小端;packed u64 的 tomb 标记落在最后一字节;trailer 的
//   totalsz=running CRC 随 LE 字节重算)。任何编码漂移在此失败。
//
//     R1: key="a"    tstamp=100 totalsz=19 offset=0    tomb=false
//     R2: key="bb"   tstamp=101 totalsz=20 offset=19   tomb=true
//     R3: key="cccc" tstamp=102 totalsz=22 offset=39   tomb=false
//     trailer:       tstamp=0   keysz=0   totalsz=CRC  offset=0x7FFFFFFFFFFFFFFF
//
// 重新生成:跑 EncodingMatchesGoldenByteForByte,取 stderr 的 CAPTURE_LE_HINT_HEX。
// ---------------------------------------------------------------------------
namespace {

std::vector<std::byte> hex_to_bytes(std::string_view h) {
    auto nyb = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        return 0;
    };
    std::vector<std::byte> out;
    out.reserve(h.size() / 2);
    for (std::size_t i = 0; i + 1 < h.size(); i += 2) {
        out.push_back(static_cast<std::byte>((nyb(h[i]) << 4) | nyb(h[i + 1])));
    }
    return out;
}

std::string write_temp_with_bytes(const TempDir& td, std::string_view name,
                                   std::span<const std::byte> bytes) {
    const auto path = td / std::string(name);
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    EXPECT_NE(fp, nullptr);
    std::fwrite(bytes.data(), 1, bytes.size(), fp);
    std::fclose(fp);
    return path;
}

// BCH5 golden 记录字节（S33 flag-day；trailer CRC 运行期由 codec::crc32
// 拼接——CRC 实现自身有独立 golden 锁定）。布局见 format.hpp：
// header "BCH5" + 每条 [vbyte gap][vbyte total_sz][vbyte keysz<<1|tomb]
// [vbyte ord_delta][tstamp u64][key] + trailer "BCHE"+CRC。
// 三条连续记录 gap 恒 0（1B）；ord 9/10/11 → 首条 delta 9、后续 delta 1。
std::vector<std::byte> golden_hint_v5_bytes() {
    auto body = hex_to_bytes(
        "42434835"                                    // "BCH5"
        "80" "93" "82" "89" "6400000000000000" "61"   // R1: sz19,k1,ord9,ts100,"a"
        "80" "94" "85" "81" "6500000000000000" "6262" // R2: sz20,k2|tomb,ord10,ts101,"bb"
        "80" "96" "88" "81" "6600000000000000" "63636363");  // R3: ord11
    const std::uint32_t crc = bitcask::codec::crc32(
        std::span<const std::byte>(body.data(), body.size()));
    auto tr = hex_to_bytes("42434845");  // "BCHE"
    body.insert(body.end(), tr.begin(), tr.end());
    for (int i = 0; i < 4; ++i) {
        body.push_back(static_cast<std::byte>((crc >> (8 * i)) & 0xFF));
    }
    return body;
}

}  // namespace

TEST(HintFileGolden, ReadsGoldenEncodedFile) {
    TempDir td;
    auto bytes = golden_hint_v5_bytes();
    // header(4) + 13 + 14 + 16 记录字节 + trailer(8) = 55B。
    ASSERT_EQ(bytes.size(), 55u);
    const auto path = write_temp_with_bytes(td, "golden.bitcask.hint", bytes);

    auto h = HintFile::open(path, HintFile::Mode::kRead);
    ASSERT_TRUE(h);

    auto valid = h->validate_trailer();
    ASSERT_TRUE(valid);
    EXPECT_TRUE(*valid) << "trailer CRC must validate against golden LE bytes";

    struct R { std::string key; std::uint64_t ts; std::uint32_t sz;
               std::uint64_t off; std::uint64_t ord; bool tomb; };
    std::vector<R> seen;
    auto fr = h->fold([&](const auto& rec) {
        seen.push_back({view_str(rec.key), rec.tstamp, rec.total_sz,
                        rec.offset, rec.ord, rec.tombstone});
    });
    ASSERT_TRUE(fr);

    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0].key, "a");    EXPECT_EQ(seen[0].ts, 100u);
    EXPECT_EQ(seen[0].sz,  19u);    EXPECT_EQ(seen[0].off, 0u);
    EXPECT_EQ(seen[0].ord, 9u);
    EXPECT_FALSE(seen[0].tomb);

    EXPECT_EQ(seen[1].key, "bb");   EXPECT_EQ(seen[1].ts, 101u);
    EXPECT_EQ(seen[1].sz,  20u);    EXPECT_EQ(seen[1].off, 19u);
    EXPECT_EQ(seen[1].ord, 10u);
    EXPECT_TRUE (seen[1].tomb);

    EXPECT_EQ(seen[2].key, "cccc"); EXPECT_EQ(seen[2].ts, 102u);
    EXPECT_EQ(seen[2].sz,  22u);    EXPECT_EQ(seen[2].off, 39u);
    EXPECT_EQ(seen[2].ord, 11u);
    EXPECT_FALSE(seen[2].tomb);
}

// Inverse direction: bytes our HintFile produces must match the pinned LE
// golden for the same logical inputs (drift guard)。写端沿革 v2→v3/v4→v5
// （S33 flag-day）——golden 同步换代，旧纪元读端已整体删除。
TEST(HintFileGolden, EncodingMatchesGoldenByteForByte) {
    TempDir td;
    const auto path = td / "ours.bitcask.hint";
    auto h = HintFile::open(path, HintFile::Mode::kCreate);
    ASSERT_TRUE(h);
    ASSERT_TRUE(h->write(100, 19, 0,   false, as_bytes("a"), /*ord*/ 9));
    ASSERT_TRUE(h->write(101, 20, 19,  true,  as_bytes("bb"), /*ord*/ 10));
    ASSERT_TRUE(h->write(102, 22, 39,  false, as_bytes("cccc"), /*ord*/ 11));
    ASSERT_TRUE(h->finalize());

    // Slurp back the bytes and compare.
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    ASSERT_NE(fp, nullptr);
    std::fseek(fp, 0, SEEK_END);
    const auto sz = static_cast<std::size_t>(std::ftell(fp));
    std::fseek(fp, 0, SEEK_SET);
    std::vector<std::byte> got(sz);
    ASSERT_EQ(std::fread(got.data(), 1, sz, fp), sz);
    std::fclose(fp);

    auto expected = golden_hint_v5_bytes();
    ASSERT_EQ(got.size(), expected.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        EXPECT_EQ(got[i], expected[i])
            << "mismatch at byte " << i << " (0x" << std::hex << i << ")";
    }
}

// ---------------------------------------------------------------------------
// DataFile <-> HintFile pair (typical bitcask scenario)
// ---------------------------------------------------------------------------
TEST(DataAndHint, ParallelStreamsAreConsistent) {
    TempDir td;
    const auto data_path = td / "10.bitcask.data";
    const auto hint_path = td / "10.bitcask.hint";

    auto df = DataFile::open(data_path, DataFile::Mode::kCreate);
    ASSERT_TRUE(df);
    auto hf = HintFile::open(hint_path, HintFile::Mode::kCreate);
    ASSERT_TRUE(hf);

    struct Rec { std::string k, v; std::uint32_t ts; std::uint64_t ord; };
    std::vector<Rec> input = {
        {"alpha",   "1",   100, 1},
        {"bravo",   "22",  101, 2},
        {"charlie", "333", 102, 3},
    };

    for (const auto& r : input) {
        auto w = df->write(RecordType::kDoc, r.ts, r.ord, as_bytes(r.k), as_bytes(r.v));
        ASSERT_TRUE(w);
        ASSERT_TRUE(hf->write(r.ts, w->total_size, w->offset,
                              /*tomb*/ false, as_bytes(r.k), r.ord));
    }
    ASSERT_TRUE(hf->finalize());

    // Re-open hint and use it to fetch from data.
    auto h_read = HintFile::open(hint_path, HintFile::Mode::kRead);
    ASSERT_TRUE(h_read);
    EXPECT_TRUE(*h_read->validate_trailer());

    auto d_read = DataFile::open(data_path, DataFile::Mode::kRead);
    ASSERT_TRUE(d_read);

    std::set<std::string> keys_seen;
    auto fr = h_read->fold([&](const auto& hint) {
        auto rec = d_read->read(hint.offset, hint.total_sz);
        ASSERT_TRUE(rec);
        EXPECT_EQ(view_str(rec->key), view_str(hint.key));
        keys_seen.insert(view_str(rec->key));
    });
    ASSERT_TRUE(fr);
    EXPECT_EQ(keys_seen.size(), input.size());
}

// ---------------------------------------------------------------------------
// migrate_le：v1 大端目录 → v2 小端目录端到端。手工构造 v1 大端 meta /
// field.schema / data（含墓碑 + 4 字节 shadow），迁移后用小端读路径校验。
// ---------------------------------------------------------------------------
TEST(MigrateBEtoLE, RoundTrip) {
    using bitcask::format::RecordType;
    TempDir td;
    const std::string src = td / "src";
    const std::string dst = td / "dst";
    fs::create_directories(src);

    // v1 大端 meta：index 模式(1)、metric=cosine(1)、dim=4(大端 00 04)。
    {
        std::vector<std::byte> m(18, std::byte{0});
        std::memcpy(m.data(), "BCME", 4);
        m[4] = static_cast<std::byte>(1);  // version 1 (legacy 大端)
        m[5] = static_cast<std::byte>(1);  // mode = index
        m[6] = static_cast<std::byte>(1);  // metric = cosine
        m[7] = static_cast<std::byte>(0);  // dim hi (大端)
        m[8] = static_cast<std::byte>(4);  // dim lo → dim=4
        write_file_bytes((fs::path(src) / "bitcask.meta").string(), m);
    }
    // v1 大端 field.schema：title(id0)、body(id1)，NameLen u16 大端。
    {
        std::vector<std::byte> f;
        be_put16(f, 5);
        auto t = as_bytes("title"); f.insert(f.end(), t.begin(), t.end());
        be_put16(f, 4);
        auto b = as_bytes("body");  f.insert(f.end(), b.begin(), b.end());
        write_file_bytes((fs::path(src) / "field.schema").string(), f);
    }
    // data 文件：doc k1->v1、doc k2->v2、墓碑 k1（4 字节大端 shadow file_id=1）。
    {
        std::vector<std::byte> data;
        auto r1 = be_data_record(RecordType::kDoc, 100, 1, "k1", "v1");
        auto r2 = be_data_record(RecordType::kDoc, 101, 2, "k2", "v2");
        const char shadow_be[4] = {0, 0, 0, 1};  // 大端 u32 = 1
        auto r3 = be_data_record(RecordType::kTombstone, 102, 3, "k1",
                                 std::string_view(shadow_be, 4));
        for (auto* r : {&r1, &r2, &r3}) data.insert(data.end(), r->begin(), r->end());
        write_file_bytes((fs::path(src) / "1.bitcask.data").string(), data);
    }

    auto res = bitcask::migrate::migrate_be_to_le(src, dst);
    ASSERT_TRUE(res) << (res ? "" : res.error());
    EXPECT_EQ(res->data_files, 1u);
    EXPECT_EQ(res->records, 3u);
    EXPECT_EQ(res->tombstones, 1u);
    EXPECT_EQ(res->skipped_bad_crc, 0u);
    EXPECT_TRUE(res->meta_migrated);
    EXPECT_TRUE(res->field_schema_migrated);

    // dst meta：version 4（LE + CRC + u64 tstamp 纪元）、dim 小端 = 4、
    // CRC 覆盖 [0,14) 正确。
    {
        std::FILE* f = std::fopen((fs::path(dst) / "bitcask.meta").string().c_str(), "rb");
        ASSERT_NE(f, nullptr);
        unsigned char m[18];
        ASSERT_EQ(std::fread(m, 1, 18, f), 18u);
        std::fclose(f);
        EXPECT_EQ(m[4], 5u);  // S33：迁移目标恒为当前纪元 v5
        EXPECT_EQ(static_cast<std::uint16_t>(m[7] | (m[8] << 8)), 4u);
        const std::uint32_t crc = bitcask::codec::crc32(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(m), 14));
        const std::uint32_t stored =
            static_cast<std::uint32_t>(m[14]) | (static_cast<std::uint32_t>(m[15]) << 8) |
            (static_cast<std::uint32_t>(m[16]) << 16) | (static_cast<std::uint32_t>(m[17]) << 24);
        EXPECT_EQ(stored, crc);
    }

    // dst data：用小端读路径 fold，逐 record 校验（CRC 重算后必须通过）。
    {
        auto df = DataFile::open((fs::path(dst) / "1.bitcask.data").string(),
                                 DataFile::Mode::kRead, false, false);
        ASSERT_TRUE(df);
        struct Rec { RecordType type; std::uint64_t ts; std::uint64_t ord;
                     std::string key; std::string val; };
        std::vector<Rec> recs;
        auto fr = df->fold([&](const bitcask::codec::DataRecordView& v,
                               std::uint64_t, std::uint32_t) {
            recs.push_back({v.type, v.tstamp, v.ord, view_str(v.key),
                            view_str(v.value)});
        });
        ASSERT_TRUE(fr);
        ASSERT_EQ(recs.size(), 3u);
        EXPECT_EQ(recs[0].ts, 100u); EXPECT_EQ(recs[0].ord, 1u);
        EXPECT_EQ(recs[0].key, "k1"); EXPECT_EQ(recs[0].val, "v1");
        EXPECT_EQ(recs[1].key, "k2"); EXPECT_EQ(recs[1].val, "v2");
        EXPECT_EQ(recs[2].type, RecordType::kTombstone);
        EXPECT_EQ(recs[2].key, "k1");
        // 4 字节 shadow 大端→小端：file_id=1 → 01 00 00 00。
        ASSERT_EQ(recs[2].val.size(), 4u);
        EXPECT_EQ(static_cast<unsigned char>(recs[2].val[0]), 1u);
        EXPECT_EQ(static_cast<unsigned char>(recs[2].val[3]), 0u);
    }

    // dst hint：重生成，fold 出 3 条，墓碑标志正确。
    {
        auto h = HintFile::open((fs::path(dst) / "1.bitcask.hint").string(),
                                HintFile::Mode::kRead);
        ASSERT_TRUE(h);
        auto v = h->validate_trailer();
        ASSERT_TRUE(v); EXPECT_TRUE(*v);
        std::vector<std::string> keys; std::vector<bool> tombs;
        auto fr = h->fold([&](const auto& rec) {
            keys.push_back(view_str(rec.key));
            tombs.push_back(rec.tombstone);
        });
        ASSERT_TRUE(fr);
        EXPECT_EQ(keys, (std::vector<std::string>{"k1", "k2", "k1"}));
        EXPECT_EQ(tombs, (std::vector<bool>{false, false, true}));
    }
}

// 已是小端(v2)的目录再迁移 → 干净报错(不重复迁移)。
TEST(MigrateBEtoLE, RejectsAlreadyV2) {
    TempDir td;
    const std::string src = td / "src2";
    fs::create_directories(src);
    std::vector<std::byte> m(18, std::byte{0});
    std::memcpy(m.data(), "BCME", 4);
    m[4] = static_cast<std::byte>(2);  // 已是 v2
    write_file_bytes((fs::path(src) / "bitcask.meta").string(), m);
    auto res = bitcask::migrate::migrate_be_to_le(src, td / "dst2");
    EXPECT_FALSE(res);
}

// ---------------------------------------------------------------------------
// migrate_u32_to_u64：u32 时间戳纪元（meta v2/v3,23B 头,DocValue v3）
// → 当前纪元（meta v4,27B 头,DocValue v4,tstamp/expiry u64）。
// ---------------------------------------------------------------------------
namespace {

void le_put16(std::vector<std::byte>& b, std::uint16_t v) {
    b.push_back(static_cast<std::byte>(v & 0xFF));
    b.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
}
void le_put32(std::vector<std::byte>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
void le_put64(std::vector<std::byte>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}

// u32 纪元 data record（23B 小端头,布局同大端 v1 仅字节序不同）。
std::vector<std::byte> le_u32era_data_record(bitcask::format::RecordType type,
                                             std::uint32_t ts,
                                             std::uint64_t ord,
                                             std::string_view key,
                                             std::span<const std::byte> val) {
    std::vector<std::byte> covered;
    covered.push_back(static_cast<std::byte>(type));
    le_put32(covered, ts);
    le_put64(covered, ord);
    le_put16(covered, static_cast<std::uint16_t>(key.size()));
    le_put32(covered, static_cast<std::uint32_t>(val.size()));
    auto kb = as_bytes(key);
    covered.insert(covered.end(), kb.begin(), kb.end());
    covered.insert(covered.end(), val.begin(), val.end());
    std::vector<std::byte> rec;
    le_put32(rec, bitcask::codec::crc32(covered));
    rec.insert(rec.end(), covered.begin(), covered.end());
    return rec;
}

// DocValue v3 字节：[Ver=3][Flags] + text 段（varint len + bytes）
// + 可选 expiry 段（u32 LE 追加在最后）。
std::vector<std::byte> docvalue_v3(std::string_view text,
                                   std::uint32_t expiry32) {
    std::vector<std::byte> v;
    v.push_back(std::byte{3});  // Ver = 3（u32 纪元）
    std::uint8_t flags = 0x02;  // kFlagHasText
    if (expiry32 != 0) flags |= 0x20;  // kFlagHasExpiry
    v.push_back(static_cast<std::byte>(flags));
    // 测试辅助只处理单字节 varint（text < 128B）。
    v.push_back(static_cast<std::byte>(text.size() | 0x80));  // varint 终止位
    auto tb = as_bytes(text);
    v.insert(v.end(), tb.begin(), tb.end());
    if (expiry32 != 0) le_put32(v, expiry32);
    return v;
}

}  // namespace

TEST(MigrateU32toU64, RoundTrip) {
    using bitcask::format::RecordType;
    TempDir td;
    const std::string src = td / "u32src";
    const std::string dst = td / "u32dst";
    fs::create_directories(src);

    // u32 纪元 meta v3：mode=index、metric=cosine(1)、dim=4（全小端 + CRC）。
    {
        std::vector<std::byte> m(18, std::byte{0});
        std::memcpy(m.data(), "BCME", 4);
        m[4] = static_cast<std::byte>(3);  // version 3（u32 纪元,带 CRC）
        m[5] = static_cast<std::byte>(1);  // mode = index
        m[6] = static_cast<std::byte>(1);  // metric = cosine
        m[7] = static_cast<std::byte>(4);  // dim = 4（小端 lo）
        const std::uint32_t crc = bitcask::codec::crc32(
            std::span<const std::byte>(m.data(), 14));
        std::vector<std::byte> tail;
        le_put32(tail, crc);
        std::copy(tail.begin(), tail.end(), m.begin() + 14);
        write_file_bytes((fs::path(src) / "bitcask.meta").string(), m);
    }
    // field.schema：当前格式（本次 flag-day 未变）,应被原样拷贝。
    const std::vector<std::byte> schema_bytes = {std::byte{0xAA},
                                                 std::byte{0xBB}};
    write_file_bytes((fs::path(src) / "field.schema").string(), schema_bytes);

    // data：带 TTL 的 doc、无 TTL 的 doc、墓碑（4B 小端 shadow）。
    std::vector<std::byte> dv1, dv2;
    docvalue_v3("aa", /*expiry32=*/0xDEADBEEFu).swap(dv1);
    docvalue_v3("bb", /*expiry32=*/0).swap(dv2);
    {
        std::vector<std::byte> data;
        auto r1 = le_u32era_data_record(RecordType::kDoc, 100, 1, "k1", dv1);
        auto r2 = le_u32era_data_record(RecordType::kDoc, 101, 2, "k2", dv2);
        const std::vector<std::byte> shadow = {std::byte{1}, std::byte{0},
                                               std::byte{0}, std::byte{0}};
        auto r3 = le_u32era_data_record(RecordType::kTombstone, 102, 3, "k1",
                                        shadow);
        for (auto* r : {&r1, &r2, &r3})
            data.insert(data.end(), r->begin(), r->end());
        write_file_bytes((fs::path(src) / "1.bitcask.data").string(), data);
    }

    auto res = bitcask::migrate::migrate_u32_to_u64(src, dst);
    ASSERT_TRUE(res) << (res ? "" : res.error());
    EXPECT_EQ(res->data_files, 1u);
    EXPECT_EQ(res->records, 3u);
    EXPECT_EQ(res->tombstones, 1u);
    EXPECT_EQ(res->skipped_bad_crc, 0u);
    EXPECT_EQ(res->skipped_bad_docvalue, 0u);
    EXPECT_TRUE(res->meta_migrated);
    EXPECT_TRUE(res->field_schema_migrated);

    // dst meta：version 4,其余配置字节保留,CRC 重算正确（v4 门禁字节级
    // 校验;与 MigrateBEtoLE.RoundTrip 同法,避免测试链接 cask 库）。
    {
        std::FILE* f = std::fopen((fs::path(dst) / "bitcask.meta").string().c_str(),
                                  "rb");
        ASSERT_NE(f, nullptr);
        unsigned char m[18];
        ASSERT_EQ(std::fread(m, 1, 18, f), 18u);
        std::fclose(f);
        EXPECT_EQ(m[4], 5u);   // version 5（S33：迁移目标恒为当前纪元）
        EXPECT_EQ(m[5], 1u);   // mode = index 保留
        EXPECT_EQ(m[6], 1u);   // metric = cosine 保留
        EXPECT_EQ(static_cast<std::uint16_t>(m[7] | (m[8] << 8)), 4u);
        const std::uint32_t crc = bitcask::codec::crc32(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(m),
                                       14));
        const std::uint32_t stored =
            static_cast<std::uint32_t>(m[14]) |
            (static_cast<std::uint32_t>(m[15]) << 8) |
            (static_cast<std::uint32_t>(m[16]) << 16) |
            (static_cast<std::uint32_t>(m[17]) << 24);
        EXPECT_EQ(stored, crc);
    }
    // field.schema 原样拷贝。
    {
        std::FILE* f = std::fopen((fs::path(dst) / "field.schema").string().c_str(),
                                  "rb");
        ASSERT_NE(f, nullptr);
        unsigned char buf[4] = {0};
        const auto got = std::fread(buf, 1, sizeof(buf), f);
        std::fclose(f);
        ASSERT_EQ(got, 2u);
        EXPECT_EQ(buf[0], 0xAAu);
        EXPECT_EQ(buf[1], 0xBBu);
    }

    // dst data：新读路径 fold;tstamp 零扩展、DocValue v4、expiry u64。
    {
        auto df = DataFile::open((fs::path(dst) / "1.bitcask.data").string(),
                                 DataFile::Mode::kRead, false, false);
        ASSERT_TRUE(df);
        struct Rec { RecordType type; std::uint64_t ts; std::uint64_t ord;
                     std::string key; std::vector<std::byte> val; };
        std::vector<Rec> recs;
        auto fr = df->fold([&](const bitcask::codec::DataRecordView& v,
                               std::uint64_t, std::uint32_t) {
            recs.push_back({v.type, v.tstamp, v.ord, view_str(v.key),
                            {v.value.begin(), v.value.end()}});
        });
        ASSERT_TRUE(fr);
        ASSERT_EQ(recs.size(), 3u);
        EXPECT_EQ(recs[0].ts, 100u);
        EXPECT_EQ(recs[0].key, "k1");
        auto d1 = bitcask::codec::decode_doc_value(recs[0].val);
        ASSERT_TRUE(d1) << "转码后必须是合法 DocValue v4";
        EXPECT_EQ(d1->ver, 4u);
        EXPECT_EQ(view_str(d1->text), "aa");
        EXPECT_EQ(d1->expiry_at, 0xDEADBEEFull) << "expiry u32 → u64 零扩展";
        auto d2 = bitcask::codec::decode_doc_value(recs[1].val);
        ASSERT_TRUE(d2);
        EXPECT_EQ(view_str(d2->text), "bb");
        EXPECT_EQ(d2->expiry_at, 0u);
        EXPECT_EQ(recs[2].type, RecordType::kTombstone);
        EXPECT_EQ(recs[2].key, "k1");
        ASSERT_EQ(recs[2].val.size(), 4u) << "墓碑 shadow 原样保留";
    }

    // dst hint：v4 重生成,trailer 校验通过,tstamp 不截断。
    {
        auto h = HintFile::open((fs::path(dst) / "1.bitcask.hint").string(),
                                HintFile::Mode::kRead);
        ASSERT_TRUE(h);
        auto v = h->validate_trailer();
        ASSERT_TRUE(v); EXPECT_TRUE(*v);
        std::vector<std::uint64_t> ts;
        auto fr = h->fold([&](const auto& rec) { ts.push_back(rec.tstamp); });
        ASSERT_TRUE(fr);
        EXPECT_EQ(ts, (std::vector<std::uint64_t>{100, 101, 102}));
    }
}

// meta 版本门禁：v1 提示先走 be2le;v4 提示无需迁移。
TEST(MigrateU32toU64, RejectsWrongEra) {
    TempDir td;
    auto mk_meta = [&](const std::string& dir, std::uint8_t ver) {
        fs::create_directories(dir);
        std::vector<std::byte> m(18, std::byte{0});
        std::memcpy(m.data(), "BCME", 4);
        m[4] = static_cast<std::byte>(ver);
        write_file_bytes((fs::path(dir) / "bitcask.meta").string(), m);
    };
    mk_meta(td / "v1src", 1);
    auto r1 = bitcask::migrate::migrate_u32_to_u64(td / "v1src", td / "o1");
    ASSERT_FALSE(r1);
    EXPECT_NE(r1.error().find("be2le"), std::string::npos);

    mk_meta(td / "v4src", 4);
    auto r4 = bitcask::migrate::migrate_u32_to_u64(td / "v4src", td / "o4");
    ASSERT_FALSE(r4);
    EXPECT_NE(r4.error().find("hintord"), std::string::npos)
        << "v4 src 必须被指去 hintord 迁移";

    mk_meta(td / "v5src", 5);
    auto r5 = bitcask::migrate::migrate_u32_to_u64(td / "v5src", td / "o5");
    ASSERT_FALSE(r5);
    EXPECT_NE(r5.error().find("nothing to migrate"), std::string::npos);
}

// ---------------------------------------------------------------------------
// P14e：search.ckpt 分段容器（SearchCheckpoint）。
// ---------------------------------------------------------------------------
namespace {
using bitcask::search::SearchCheckpoint;
using bitcask::search::CkptSection;

std::span<const std::byte> sp(const std::string& s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}
// 翻转文件第 off 字节的一个 bit。
void flip_byte(const std::string& path, long off) {
    std::FILE* f = std::fopen(path.c_str(), "rb+");
    ASSERT_NE(f, nullptr);
    std::fseek(f, off, SEEK_SET);
    unsigned char c = 0;
    ASSERT_EQ(std::fread(&c, 1, 1, f), 1u);
    c ^= 0x01;
    std::fseek(f, off, SEEK_SET);
    ASSERT_EQ(std::fwrite(&c, 1, 1, f), 1u);
    std::fclose(f);
}
long file_size(const std::string& path) {
    return static_cast<long>(fs::file_size(path));
}
}  // namespace

TEST(SearchCheckpoint, RoundTrip) {
    TempDir td;
    const std::string path = td / "search.ckpt";
    const std::string s1 = "docmap-bytes", s2 = "bm25-bytes!", s3 = "hnsw";
    std::vector<CkptSection> secs = {
        {1, 0, sp(s1)}, {2, 0, sp(s2)}, {4, 7, sp(s3)}};
    ASSERT_TRUE(SearchCheckpoint::write(path, /*watermark*/ 4242, secs));

    auto lc = SearchCheckpoint::read(path);
    ASSERT_TRUE(lc.has_value());
    EXPECT_EQ(lc->watermark, 4242u);
    ASSERT_EQ(lc->sections.size(), 3u);
    EXPECT_EQ(lc->sections[0].type, 1u);
    EXPECT_EQ(lc->sections[1].type, 2u);
    EXPECT_EQ(lc->sections[2].type, 4u);
    EXPECT_EQ(lc->sections[2].flags, 7u);
    EXPECT_EQ(view_str(lc->sections[0].payload), s1);
    EXPECT_EQ(view_str(lc->sections[1].payload), s2);
    EXPECT_EQ(view_str(lc->sections[2].payload), s3);
    for (auto& ls : lc->sections) EXPECT_TRUE(ls.crc_ok);
}

// 单段 payload 损坏 → 仅该段 crc_ok=false，其余段正常、结构完整（损坏隔离）。
TEST(SearchCheckpoint, SectionCorruptionIsolated) {
    TempDir td;
    const std::string path = td / "search.ckpt";
    const std::string s1 = "AAAA", s2 = "BBBBBB";  // 段0[16,20) 段1[20,26)
    std::vector<CkptSection> secs = {{2, 0, sp(s1)}, {4, 0, sp(s2)}};
    ASSERT_TRUE(SearchCheckpoint::write(path, 9, secs));

    flip_byte(path, 16);  // 段0 第一字节(payload 区)。
    auto lc = SearchCheckpoint::read(path);
    ASSERT_TRUE(lc.has_value());  // 结构仍完整。
    ASSERT_EQ(lc->sections.size(), 2u);
    EXPECT_FALSE(lc->sections[0].crc_ok);  // 坏段。
    EXPECT_TRUE(lc->sections[1].crc_ok);   // 好段照常。
}

// 页脚损坏（trailer / footerCrc）→ 结构性拒绝（read 返回 nullopt）。
TEST(SearchCheckpoint, FooterCorruptRejected) {
    TempDir td;
    const std::string path = td / "search.ckpt";
    const std::string s1 = "x";
    std::vector<CkptSection> secs = {{1, 0, sp(s1)}};
    ASSERT_TRUE(SearchCheckpoint::write(path, 1, secs));
    const long sz = file_size(path);
    flip_byte(path, sz - 1);  // trailer 最后一字节。
    EXPECT_FALSE(SearchCheckpoint::read(path).has_value());

    // footerCrc 区损坏(dir 内容与 crc 不符)。
    ASSERT_TRUE(SearchCheckpoint::write(path, 1, secs));
    flip_byte(path, file_size(path) - 12);  // footerCrc 首字节。
    EXPECT_FALSE(SearchCheckpoint::read(path).has_value());
}

// 截断 → 拒绝。
TEST(SearchCheckpoint, TruncatedRejected) {
    TempDir td;
    const std::string path = td / "search.ckpt";
    // S24 补（ASan 实证）：span 源必须具名——sp(临时 string) 在整表达式末
    // 即悬垂，write 读到已亡栈帧。
    const std::string payload = "payload";
    std::vector<CkptSection> secs = {{1, 0, sp(payload)}};
    ASSERT_TRUE(SearchCheckpoint::write(path, 1, secs));
    const long sz = file_size(path);
    std::filesystem::resize_file(path, static_cast<std::uintmax_t>(sz / 2));
    EXPECT_FALSE(SearchCheckpoint::read(path).has_value());
}

// 空段集 round-trip（仅头部+空目录+页脚）。
TEST(SearchCheckpoint, EmptySections) {
    TempDir td;
    const std::string path = td / "search.ckpt";
    ASSERT_TRUE(SearchCheckpoint::write(path, 77, {}));
    auto lc = SearchCheckpoint::read(path);
    ASSERT_TRUE(lc.has_value());
    EXPECT_EQ(lc->watermark, 77u);
    EXPECT_TRUE(lc->sections.empty());
}

// ------------------------------------------------------------------
// FieldSchema magic/version/CRC 健壮性（S12-3）。LE-only 主机：原始 u32
// 读写与 le_store/le_load 一致，故测试直接用原始 u32。
// ------------------------------------------------------------------

// 新格式写入 → 文件带 magic 头 → 重开 id 保持。
TEST(FieldSchema, NewFormatRoundTripAndMagicHeader) {
    TempDir td;
    const std::string path = td / "field.schema";
    {
        bitcask::FieldSchema fs;
        ASSERT_TRUE(fs.open(path));
        EXPECT_EQ(fs.intern("title"), 0u);
        EXPECT_EQ(fs.intern("body"), 1u);
        EXPECT_EQ(fs.intern("title"), 0u);  // 幂等
    }
    std::ifstream in(path, std::ios::binary);
    std::uint32_t magic = 0;
    in.read(reinterpret_cast<char*>(&magic), 4);
    EXPECT_EQ(magic, bitcask::FieldSchema::kMagic);

    bitcask::FieldSchema fs2;
    ASSERT_TRUE(fs2.open(path));
    EXPECT_EQ(fs2.size(), 2u);
    ASSERT_TRUE(fs2.name_of(0).has_value());
    EXPECT_EQ(fs2.name_of(0).value(), "title");
    EXPECT_EQ(fs2.name_of(1).value(), "body");
    EXPECT_EQ(fs2.intern("body"), 1u);         // 重开后仍认得旧字段
    EXPECT_EQ(fs2.intern("author"), 2u);       // 续接新 id
}

// 完整 entry 的 CRC 被篡改 → open fail-fast。
TEST(FieldSchema, DetectsCrcCorruption) {
    TempDir td;
    const std::string path = td / "field.schema";
    {
        bitcask::FieldSchema fs;
        ASSERT_TRUE(fs.open(path));
        fs.intern("title");
        fs.intern("body");
    }
    // 翻转第一条 entry 的首个 name 字节（越过 8 字节头 + 2 字节 len）。
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekg(bitcask::FieldSchema::kHeaderSize + 2, std::ios::beg);
        char c = 0;
        f.read(&c, 1);
        c = static_cast<char>(c ^ 0xFF);
        f.seekp(bitcask::FieldSchema::kHeaderSize + 2, std::ios::beg);
        f.write(&c, 1);
    }
    bitcask::FieldSchema fs2;
    EXPECT_FALSE(fs2.open(path));  // CRC 不符 → 硬失败
}

// magic 匹配但 version 未知 → open fail-fast。
TEST(FieldSchema, RejectsUnknownVersion) {
    TempDir td;
    const std::string path = td / "field.schema";
    {
        std::ofstream out(path, std::ios::binary);
        std::uint32_t magic = bitcask::FieldSchema::kMagic;
        std::uint32_t ver = 99;
        out.write(reinterpret_cast<const char*>(&magic), 4);
        out.write(reinterpret_cast<const char*>(&ver), 4);
    }
    bitcask::FieldSchema fs;
    EXPECT_FALSE(fs.open(path));
}

// legacy 无头文件 → 读得出 + 原子升级为带头新格式 + 重开仍可用。
TEST(FieldSchema, LegacyHeaderlessAutoUpgrades) {
    TempDir td;
    const std::string path = td / "field.schema";
    {
        std::ofstream out(path, std::ios::binary);
        auto put = [&](std::string_view s) {
            const auto n = static_cast<std::uint16_t>(s.size());
            const char lb[2] = {static_cast<char>(n & 0xFF),
                                static_cast<char>((n >> 8) & 0xFF)};
            out.write(lb, 2);
            out.write(s.data(), static_cast<std::streamsize>(s.size()));
        };
        put("title");
        put("body");
    }
    bitcask::FieldSchema fs;
    ASSERT_TRUE(fs.open(path));
    EXPECT_EQ(fs.size(), 2u);
    EXPECT_EQ(fs.name_of(0).value(), "title");
    EXPECT_EQ(fs.name_of(1).value(), "body");

    // 文件已升级：现在以 magic 开头。
    std::ifstream in(path, std::ios::binary);
    std::uint32_t magic = 0;
    in.read(reinterpret_cast<char*>(&magic), 4);
    EXPECT_EQ(magic, bitcask::FieldSchema::kMagic);

    // 以新格式重开仍正确，且续接新 id。
    bitcask::FieldSchema fs2;
    ASSERT_TRUE(fs2.open(path));
    EXPECT_EQ(fs2.size(), 2u);
    EXPECT_EQ(fs2.name_of(1).value(), "body");
    EXPECT_EQ(fs2.intern("extra"), 2u);
}

// 尾部半条（torn tail，崩溃常态）→ 容忍跳过，不算损坏。
TEST(FieldSchema, ToleratesTornTailNewFormat) {
    TempDir td;
    const std::string path = td / "field.schema";
    {
        bitcask::FieldSchema fs;
        ASSERT_TRUE(fs.open(path));
        fs.intern("title");
        fs.intern("body");
    }
    // 追加一条截断 entry：len 声称 5，但只有 2 字节 name、无 CRC。
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        const char lb[2] = {static_cast<char>(5), static_cast<char>(0)};
        out.write(lb, 2);
        out.write("ab", 2);
    }
    bitcask::FieldSchema fs2;
    ASSERT_TRUE(fs2.open(path));   // torn tail 容忍
    EXPECT_EQ(fs2.size(), 2u);     // 只保留两条完整 entry
    EXPECT_EQ(fs2.name_of(0).value(), "title");
}

// ---------------------------------------------------------------------------
// HintFile v3（S23-A1）+ v2 兼容读
// ---------------------------------------------------------------------------

// v3 非连续 offset（gap≠0，模拟未来写端跳记录）：正确性不依赖连续性。
TEST(HintFile, V3NonContiguousOffsets) {
    TempDir td;
    const auto path = td / "gap.bitcask.hint";
    auto h = HintFile::open(path, HintFile::Mode::kCreate);
    ASSERT_TRUE(h);
    ASSERT_TRUE(h->write(1, 30, /*off*/ 0,   false, as_bytes("a"), /*ord*/ 12));
    ASSERT_TRUE(h->write(2, 40, /*off*/ 100, false, as_bytes("bb"), /*ord*/ 13));  // 洞
    ASSERT_TRUE(h->write(3, 50, /*off*/ 140, true,  as_bytes("ccc"), /*ord*/ 14));
    ASSERT_TRUE(h->finalize());

    auto r = HintFile::open(path, HintFile::Mode::kRead);
    ASSERT_TRUE(r);
    std::vector<std::uint64_t> offs;
    std::vector<bool> tombs;
    auto fr = r->fold_validated([&](const auto& rec) {
        offs.push_back(rec.offset);
        tombs.push_back(rec.tombstone);
    });
    ASSERT_TRUE(fr);
    EXPECT_TRUE(*fr);
    EXPECT_EQ(offs, (std::vector<std::uint64_t>{0, 100, 140}));
    EXPECT_EQ(tombs, (std::vector<bool>{false, false, true}));
}

// S33 flag-day：BCH4 及更早纪元的 hint 无读端——validate/fold_validated
// 返回 false（caller 退 fold(data) 重建）、fold 报错。手工构造一个带 BCH4
// 文件头 magic 的字节流验证三入口全拒。纪元硬门禁在 bitcask.meta v5，
// 此处只保证「陈旧 hint 绝不被静默误读」。
TEST(HintFile, Bch4LegacyFileRejected) {
    TempDir td;
    const auto path = td / "legacy.bitcask.hint";
    {
        // BCH4 magic + 若干字节旧格式载荷 + 伪 trailer（内容无关紧要——
        // 应在 magic 检查处即被拒）。
        std::vector<std::byte> buf;
        auto push_u32 = [&](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) {
                buf.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
            }
        };
        push_u32(bitcask::format::kHintMagicV4);
        for (int i = 0; i < 16; ++i) buf.push_back(std::byte{0x42});
        push_u32(bitcask::format::kHintTrailerMagic);
        push_u32(0xDEADBEEF);
        std::ofstream f(std::string(path), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
    }
    auto r = HintFile::open(path, HintFile::Mode::kRead);
    ASSERT_TRUE(r);
    auto v = r->validate_trailer();
    ASSERT_TRUE(v);
    EXPECT_FALSE(*v) << "BCH4 hint 必须判不可用（退 fold(data)）";

    bool called = false;
    auto fr = r->fold_validated([&](const auto&) { called = true; });
    ASSERT_TRUE(fr);
    EXPECT_FALSE(*fr);
    EXPECT_FALSE(called) << "拒收路径绝不能回调";

    auto fo = r->fold([&](const auto&) { called = true; });
    EXPECT_FALSE(fo.has_value()) << "fold 对 BCH4 必须报错";
    EXPECT_FALSE(called);
}

// v3 未封口（崩溃丢 trailer）：validate/fold_validated 拒绝 → fold(data) 兜底。
TEST(HintFile, V3UnfinalizedRejected) {
    TempDir td;
    const auto path = td / "torn.bitcask.hint";
    {
        auto h = HintFile::open(path, HintFile::Mode::kCreate);
        ASSERT_TRUE(h);
        ASSERT_TRUE(h->write(1, 30, 0, false, as_bytes("a"), /*ord*/ 15));
        // 手动 flush 但不 finalize（模拟崩溃）——sync 触发不了 pending 落盘，
        // 直接析构会丢缓冲；这里用 finalize 前的 write 大小不足以自动 flush，
        // 故重开写一批超过缓冲阈值不现实——改为 finalize 后截尾 8B。
        ASSERT_TRUE(h->finalize());
    }
    // 截掉 trailer 模拟未封口。
    std::error_code ec;
    const auto sz = std::filesystem::file_size(std::string(path), ec);
    ASSERT_FALSE(ec);
    std::filesystem::resize_file(std::string(path), sz - 8, ec);
    ASSERT_FALSE(ec);

    auto r = HintFile::open(path, HintFile::Mode::kRead);
    ASSERT_TRUE(r);
    auto v = r->validate_trailer();
    ASSERT_TRUE(v);
    EXPECT_FALSE(*v);
    auto fr = r->fold_validated([&](const auto&) {});
    ASSERT_TRUE(fr);
    EXPECT_FALSE(*fr);
}
