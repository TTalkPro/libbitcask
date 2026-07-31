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

    // S33-1:内存估算字段口径。
    std::uint64_t key_sum = 0, heap_sum = 0;
    for (std::size_t L : lens) {
        key_sum += L;
        if (L > 15) heap_sum += L + 1;
    }
    EXPECT_EQ(h.key_bytes, key_sum);
    EXPECT_EQ(h.heap_key_bytes, heap_sum);
    // 稠密数组容量下界 = 已存条数 × pair 大小；桶块非空。
    using Pair = std::pair<std::string, bitcask::keydir::Entry>;
    EXPECT_GE(h.entry_slot_bytes, lens.size() * sizeof(Pair));
    EXPECT_GT(h.bucket_bytes, 0u);
    EXPECT_EQ(h.estimated_bytes,
              h.entry_slot_bytes + h.bucket_bytes + h.heap_key_bytes);
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

// v1（定宽 entries，u32-tstamp 纪元）：64 位时间戳 flag-day 后读端仅收
// v3——旧版本必须被干净拒收（返回 nullopt → 退全量 fold），绝不误读。
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
    EXPECT_FALSE(loaded.has_value())
        << "u32 纪元 v1 快照必须被干净拒收（退全量 fold）";
    EXPECT_FALSE(kd.get("kv1ky").has_value()) << "拒收后不得残留半解析状态";
    fs::remove(p);
}

// ---- S29-6 P1:remove 墓碑化（erase → tombstone-in-place + 增量 sweep）----
// 背景:remove 无 fold 分支不再物理 erase（为 P3 乐观读者保住 key 缓冲/槽位），
// 改留 sibling sentinel 墓碑;物理回收走写者增量 sweep + quiescent 点全量清。
// 详见 docs/design/s29-6-keydir-lockfree-read.md §2.1。

// 删除后不可见、重复删除 false、put 复活（走 put_insert 的 Single 墓碑覆写分支）。
TEST(KeyDirTombstone, RemoveHidesAndPutRevives) {
    KeyDir kd;
    ASSERT_EQ(kd.put("k", 1, 10, 100, 1000, 0, true, 0, 0, 1), PutResult::kOk);
    ASSERT_TRUE(kd.remove("k", 2000));
    EXPECT_FALSE(kd.get("k").has_value()) << "墓碑必须不可见";
    EXPECT_FALSE(kd.remove("k", 2001)) << "重复删除应返回 false";

    ASSERT_EQ(kd.put("k", 2, 20, 200, 3000, 0, true, 0, 0, 2), PutResult::kOk);
    auto e = kd.get("k");
    ASSERT_TRUE(e.has_value()) << "put 应复活墓碑 key";
    EXPECT_EQ(e->file_id, 2u);
    EXPECT_EQ(e->ord, 2u);
    EXPECT_EQ(kd.info().key_count, 1u);
}

// delete-heavy:交错删一半，可见性/计数/直方图口径全程一致（增量 sweep 在
// 写操作中被反复触发——本用例即 sweep 路径的回归护栏）。
TEST(KeyDirTombstone, DeleteHeavyVisibilityAndCounts) {
    KeyDir kd;
    constexpr int N = 2000;
    for (int i = 0; i < N; ++i) {
        ASSERT_EQ(kd.put("key" + std::to_string(i), 1, 10,
                         static_cast<std::uint64_t>(i), 1000, 0, true, 0, 0,
                         static_cast<std::uint64_t>(i)),
                  PutResult::kOk);
    }
    for (int i = 0; i < N; i += 2) {
        ASSERT_TRUE(kd.remove("key" + std::to_string(i), 2000));
    }
    EXPECT_EQ(kd.info().key_count, static_cast<std::uint64_t>(N / 2));
    for (int i = 0; i < N; ++i) {
        const bool live = (i % 2) == 1;
        EXPECT_EQ(kd.get("key" + std::to_string(i)).has_value(), live)
            << "key" << i;
    }
    auto h = kd.key_length_histogram();
    EXPECT_EQ(h.total, static_cast<std::size_t>(N / 2)) << "直方图只计活 key";
}

// 墓碑在场时快照往返:墓碑不入快照（count 与写出条目严格一致），load 后
// 等价不存在;活 key 完整还原。
TEST(KeyDirTombstone, SnapshotSkipsTombstones) {
    const fs::path p =
        fs::temp_directory_path() / "bitcask_kd_snap_tomb.ckpt";
    fs::remove(p);
    KeyDir kd;
    ASSERT_EQ(kd.put("live1", 1, 10, 100, 1000, 0, true, 0, 0, 1), PutResult::kOk);
    ASSERT_EQ(kd.put("dead1", 1, 10, 200, 1000, 0, true, 0, 0, 2), PutResult::kOk);
    ASSERT_EQ(kd.put("live2", 1, 10, 300, 1000, 0, true, 0, 0, 3), PutResult::kOk);
    ASSERT_TRUE(kd.remove("dead1", 2000));

    ASSERT_TRUE(kd.save_snapshot(p.string(), {}));
    KeyDir kd2;
    ASSERT_TRUE(kd2.load_snapshot(p.string()).has_value());
    EXPECT_TRUE(kd2.get("live1").has_value());
    EXPECT_TRUE(kd2.get("live2").has_value());
    EXPECT_FALSE(kd2.get("dead1").has_value()) << "墓碑不应进快照";
    EXPECT_EQ(kd2.info().key_count, 2u);
    fs::remove(p);
}

// ---- S29-6 P3/P4:get 乐观快路径(SeqShardTable + epoch-limbo)压力 ----
// 三类并发压力同开:① stable key 覆写(检测撕裂:写者恒保持 offset==ord,
// 读者命中即断言相等——采纳撕裂 Entry 必破)② volatile key put/remove 交替
// (墓碑/复活/sweep/backward-shift)③ 新 key 持续插入(触发 grow/rehash →
// 旧数组 retire → 混代路径)。ASan 下跑 = 对「deref 恒 in-bounds 且存活」
// 设计的真实检验(乐观读未豁免 ASan)。
#include <atomic>
#include <thread>

TEST(KeyDirOptimisticRead, ConcurrentGetPutRemoveGrowStress) {
    KeyDir kd;
    ASSERT_TRUE(kd.optimistic_reads());
    constexpr int kStable = 512;
    auto stable_key = [](int i) { return "stable" + std::to_string(i); };
    auto vol_key = [](int i) { return "vol" + std::to_string(i); };
    for (int i = 0; i < kStable; ++i) {
        ASSERT_EQ(kd.put(stable_key(i), 1, 10, static_cast<std::uint64_t>(i),
                         1000, 0, true, 0, 0, static_cast<std::uint64_t>(i)),
                  PutResult::kOk);
    }

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> torn{0}, stable_miss{0};

    std::thread writer([&] {
        std::uint64_t x = kStable;
        int grow_i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            // ① stable 覆写:offset == ord 恒成立(撕裂检测不变量)。
            const int s = static_cast<int>(x % kStable);
            (void)kd.put(stable_key(s), 1, 10, x, 1000, 0, true, 0, 0, x);
            // ② volatile:put → remove(墓碑 + sweep 路径)。
            const int v = static_cast<int>(x % 64);
            (void)kd.put(vol_key(v), 1, 10, x, 1000, 0, true, 0, 0, x + 1);
            (void)kd.remove(vol_key(v), 2000);
            // ③ 新 key:驱动 values/buckets grow → 旧数组 retire。
            (void)kd.put("grow" + std::to_string(grow_i++), 1, 10, x, 1000, 0,
                         true, 0, 0, x + 2);
            x += 3;
        }
    });

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&, t] {
            std::uint64_t seed = 0x9E3779B97F4A7C15ull * (t + 1);
            while (!stop.load(std::memory_order_relaxed)) {
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                const int s = static_cast<int>((seed >> 33) % kStable);
                if (auto e = kd.get(stable_key(s))) {
                    if (e->offset != e->ord) torn.fetch_add(1);
                } else {
                    stable_miss.fetch_add(1);  // stable 永不删除:必命中
                }
                (void)kd.get(vol_key(static_cast<int>((seed >> 20) % 64)));
                (void)kd.get("nonexistent" + std::to_string(seed % 97));
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    stop.store(true);
    writer.join();
    for (auto& r : readers) r.join();

    EXPECT_EQ(torn.load(), 0u) << "乐观读采纳了撕裂的 Entry(seq 校验失效)";
    EXPECT_EQ(stable_miss.load(), 0u) << "stable key 出现 miss(探测/混代缺陷)";
}

// 开关关闭 → 纯锁路径,行为不变(回退开关回归护栏)。
TEST(KeyDirOptimisticRead, RuntimeSwitchFallback) {
    KeyDir kd;
    kd.set_optimistic_reads(false);
    ASSERT_FALSE(kd.optimistic_reads());
    ASSERT_EQ(kd.put("k", 1, 10, 100, 1000, 0, true, 0, 0, 1), PutResult::kOk);
    auto e = kd.get("k");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->ord, 1u);
    ASSERT_TRUE(kd.remove("k", 2000));
    EXPECT_FALSE(kd.get("k").has_value());
}
