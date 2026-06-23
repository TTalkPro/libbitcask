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