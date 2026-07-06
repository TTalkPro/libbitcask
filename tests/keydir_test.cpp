#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "bitcask/keydir.hpp"

namespace {

using bitcask::keydir::EntryProxy;
using bitcask::keydir::KeyDir;
using bitcask::keydir::PutResult;

}  // namespace

TEST(KeyDir, AllocOrdMonotonic) {
    KeyDir kd;
    EXPECT_EQ(kd.alloc_ord(), 0u);
    EXPECT_EQ(kd.alloc_ord(), 1u);
    EXPECT_EQ(kd.alloc_ord(), 2u);
    EXPECT_EQ(kd.get_epoch(), 0u);
}

TEST(KeyDir, AdvanceOrd) {
    KeyDir kd;
    kd.alloc_ord();
    kd.alloc_ord();
    EXPECT_EQ(kd.alloc_ord(), 2u);

    kd.advance_ord(5);
    EXPECT_EQ(kd.alloc_ord(), 6u);

    kd.advance_ord(3);
    EXPECT_EQ(kd.alloc_ord(), 7u);

    kd.advance_ord(100);
    EXPECT_EQ(kd.alloc_ord(), 101u);
}

TEST(KeyDir, PutAndGetWithOrd) {
    KeyDir kd;

    auto r1 = kd.put("k1",
                     /*file_id*/ 1, /*total_sz*/ 10,
                     /*offset*/ 100, /*tstamp*/ 1000,
                     /*now_sec*/ 0,
                     /*newest_put*/ true,
                     /*old_file_id*/ 0, /*old_offset*/ 0,
                     /*ord*/ 5);
    EXPECT_EQ(r1, PutResult::kOk);

    auto e1 = kd.get("k1");
    ASSERT_TRUE(e1.has_value());
    EXPECT_EQ(e1->ord, 5u);
    EXPECT_EQ(e1->file_id, 1u);
    EXPECT_EQ(e1->offset, 100u);

    auto r2 = kd.put("k1",
                     /*file_id*/ 2, /*total_sz*/ 20,
                     /*offset*/ 200, /*tstamp*/ 2000,
                     /*now_sec*/ 0,
                     /*newest_put*/ true,
                     /*old_file_id*/ 0, /*old_offset*/ 0,
                     /*ord*/ 10);
    EXPECT_EQ(r2, PutResult::kOk);

    auto e2 = kd.get("k1");
    ASSERT_TRUE(e2.has_value());
    EXPECT_EQ(e2->ord, 10u);
    EXPECT_EQ(e2->file_id, 2u);
}

TEST(KeyDir, PutWithDefaultOrd) {
    KeyDir kd;

    auto r = kd.put("k1",
                    /*file_id*/ 1, /*total_sz*/ 10,
                    /*offset*/ 100, /*tstamp*/ 1000,
                    /*now_sec*/ 0,
                    /*newest_put*/ true,
                    /*old_file_id*/ 0, /*old_offset*/ 0);
    EXPECT_EQ(r, PutResult::kOk);

    auto e = kd.get("k1");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->ord, 0u);
}

TEST(KeyDir, PutMultipleKeysDifferentOrd) {
    KeyDir kd;

    kd.put("a", 1, 10, 100, 1000, 0, true, 0, 0, 1);
    kd.put("b", 1, 10, 100, 1000, 0, true, 0, 0, 2);
    kd.put("c", 1, 10, 100, 1000, 0, true, 0, 0, 3);

    EXPECT_EQ(kd.get("a")->ord, 1u);
    EXPECT_EQ(kd.get("b")->ord, 2u);
    EXPECT_EQ(kd.get("c")->ord, 3u);
}

TEST(KeyDir, AdvanceOrdAfterPut) {
    KeyDir kd;

    kd.put("k1", 1, 10, 100, 1000, 0, true, 0, 0, 0);
    kd.advance_ord(50);

    auto o = kd.alloc_ord();
    EXPECT_EQ(o, 51u);

    auto r = kd.put("k2", 1, 10, 100, 1000, 0, true, 0, 0, o);
    EXPECT_EQ(r, PutResult::kOk);
    EXPECT_EQ(kd.get("k2")->ord, 51u);
}

// T5:key_length_histogram 诊断探针（tier-2 ⑤）。验证桶边界 + sso/heap 计数。
// 桶：[0,8) [8,16) [16,24) [24,32) [32,48) [48,64) [64,128) [128,∞)；
// sso：len≤15（libstdc++ SSO 内联），heap：len>15。
TEST(KeyDir, KeyLengthHistogramBucketsAndSso) {
    KeyDir kd;
    // 每个长度恰插一个 key（不同长度天然是不同的 key）。每桶取「下界」与
    // 「上界−1」两个长度，校验边界归桶正确。
    const std::vector<std::size_t> lens = {
        1, 7,     // 桶 0
        8, 15,    // 桶 1（均 sso，≤15）
        16, 23,   // 桶 2（起 heap）
        24, 31,   // 桶 3
        32, 47,   // 桶 4
        48, 63,   // 桶 5
        64, 127,  // 桶 6
        128, 200  // 桶 7
    };
    for (std::size_t L : lens) {
        std::string k(L, 'x');
        ASSERT_EQ(kd.put(k, 1, 10, 100, 1000, 0, true, 0, 0, 0), PutResult::kOk);
    }

    auto h = kd.key_length_histogram();
    EXPECT_EQ(h.total, lens.size());
    // sso = {1,7,8,15} = 4；heap = 其余 12。
    EXPECT_EQ(h.sso, 4u);
    EXPECT_EQ(h.heap, lens.size() - 4);
    for (std::size_t b = 0; b < 8; ++b) {
        EXPECT_EQ(h.buckets[b], 2u) << "桶 " << b << " 应有 2 个 key";
    }
}

// T5:空 keydir → 全零；墓碑（删除）不计入 entries 直方图。
TEST(KeyDir, KeyLengthHistogramEmptyAndAfterRemove) {
    KeyDir kd;
    auto h0 = kd.key_length_histogram();
    EXPECT_EQ(h0.total, 0u);
    EXPECT_EQ(h0.sso, 0u);
    EXPECT_EQ(h0.heap, 0u);

    kd.put("short", 1, 10, 100, 1000, 0, true, 0, 0, 0);          // len 5
    kd.put("a_much_longer_key_over_15", 1, 10, 100, 1000, 0, true, 0, 0, 1);  // len>15
    auto h1 = kd.key_length_histogram();
    EXPECT_EQ(h1.total, 2u);
    EXPECT_EQ(h1.sso, 1u);
    EXPECT_EQ(h1.heap, 1u);

    kd.remove("short", 2000);  // 删除后该 key 不再是 live entry
    auto h2 = kd.key_length_histogram();
    EXPECT_EQ(h2.total, 1u) << "墓碑不应计入直方图";
    EXPECT_EQ(h2.sso, 0u);
    EXPECT_EQ(h2.heap, 1u);
}

TEST(KeyDir, AllocOrdThreadSafety) {
    KeyDir kd;

    std::vector<std::uint64_t> ords;
    std::mutex m;
    const int N = 100;

    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&]() {
            auto o = kd.alloc_ord();
            std::lock_guard<std::mutex> l(m);
            ords.push_back(o);
        });
    }
    for (auto& t : threads) t.join();

    std::sort(ords.begin(), ords.end());
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(ords[i], static_cast<std::uint64_t>(i));
    }
}
// ---- S21-2 A3：快照 v2（entries 块 vbyte）----

#include <cstring>
#include <filesystem>
#include <fstream>

#include "bitcask/codec.hpp"

namespace {
namespace fs = std::filesystem;

void le32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF);
}
void le64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back((v >> (8 * i)) & 0xFF);
}
}  // namespace

// v2 写读 roundtrip：save → 全新 KeyDir load → entry 字段/水位/标量逐项一致。
TEST(KeyDirSnapshot, V2RoundTrip) {
    const fs::path p =
        fs::temp_directory_path() / "bitcask_kd_snap_v2.ckpt";
    fs::remove(p);
    KeyDir kd;
    ASSERT_EQ(kd.put("alpha", /*file_id*/ 3, /*total_sz*/ 40, /*offset*/ 1000,
                     /*tstamp*/ 1700000000, 0, true, 0, 0, /*ord*/ 7),
              PutResult::kOk);
    ASSERT_EQ(kd.put("k-long-" + std::string(200, 'x'), 4, 50, 2000,
                     1700000001, 0, true, 0, 0, 8),
              PutResult::kOk);
    std::vector<std::pair<std::uint32_t, std::uint64_t>> wms{{4, 12345}};
    ASSERT_TRUE(kd.save_snapshot(p.string(), wms));

    KeyDir kd2;
    auto loaded = kd2.load_snapshot(p.string());
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->size(), 1u);
    EXPECT_EQ((*loaded)[0].first, 4u);
    EXPECT_EQ((*loaded)[0].second, 12345u);
    auto e = kd2.get("alpha");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->file_id, 3u);
    EXPECT_EQ(e->total_sz, 40u);
    EXPECT_EQ(e->offset, 1000u);
    EXPECT_EQ(e->tstamp, 1700000000u);
    EXPECT_EQ(e->ord, 7u);
    EXPECT_TRUE(kd2.get("k-long-" + std::string(200, 'x')).has_value());
    fs::remove(p);
}

// v1（定宽 entries）兼容读：手工构造 v1 字节流——写端已恒写 v2，此路径
// 无自然覆盖，防回归。
TEST(KeyDirSnapshot, V1CompatRead) {
    const fs::path p =
        fs::temp_directory_path() / "bitcask_kd_snap_v1.ckpt";
    std::vector<std::uint8_t> b;
    le32(b, 0x42434B53);  // "BCKS"
    le32(b, 1);           // v1
    le64(b, 10);          // next_ord
    le64(b, 2);           // epoch
    le32(b, 5);           // biggest_file_id
    le64(b, 1);           // key_count
    le64(b, 5);           // key_bytes
    le32(b, 0);           // fstats_n
    le32(b, 1);           // wm_n
    le32(b, 5); le64(b, 999);  // watermark {5, 999}
    le64(b, 1);           // entry_n
    const char* key = "kv1ky";
    b.push_back(5); b.push_back(0);  // klen u16
    b.insert(b.end(), key, key + 5);
    le32(b, 5);            // file_id
    le32(b, 40);           // total_sz
    le64(b, 4096);         // offset
    le64(b, 2);            // epoch
    le32(b, 1700000000);   // tstamp
    le64(b, 9);            // ord
    const std::uint32_t crc = bitcask::codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(b.data() + 8), b.size() - 8));
    le32(b, crc);
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(b.data()),
                static_cast<std::streamsize>(b.size()));
    }
    KeyDir kd;
    auto loaded = kd.load_snapshot(p.string());
    ASSERT_TRUE(loaded.has_value()) << "v1 快照必须仍可读（兼容分支）";
    ASSERT_EQ(loaded->size(), 1u);
    EXPECT_EQ((*loaded)[0].first, 5u);
    EXPECT_EQ((*loaded)[0].second, 999u);
    auto e = kd.get("kv1ky");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->file_id, 5u);
    EXPECT_EQ(e->total_sz, 40u);
    EXPECT_EQ(e->offset, 4096u);
    EXPECT_EQ(e->tstamp, 1700000000u);
    EXPECT_EQ(e->ord, 9u);
    fs::remove(p);
}
