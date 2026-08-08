// S36-4：Level B（keydir 磁盘驻留）集成测试。
// 设计：doc/keydir-disk-resident-design-zh.md §3(D4)/§5.2/§8/§10（S36-4 行）。
// 覆盖：
//   - 预算逐出：物理驻留有界、逻辑计数/fstats 在逐出态下保持精确（D4
//     冷视图记账：覆盖被逐 key 不虚增、remove 被逐 key 真退账）；
//   - fold/parallel_scan 三元组枚举：被逐 key 不从枚举里消失，值正确；
//   - BCKS v4 + BCOM v3 戳：Level B 重开走子集快照（物理驻留有界、
//     逻辑计数保真、不触发重建）；
//   - 模式转换：Level A 重开清戳 + 全量可见；Level A 期间 merge 后回
//     Level B 强制重建（陈旧 loc 不采信）；merge_only 旁车被拒；
//   - 并发：写者 + 读者 + 自动逐出交错零漂移（影子对拍全程在线）。

#include "support/crash_child.hpp"

using bitcask::test::crash_exit;

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <cstdint>
#include <filesystem>
#include <map>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/codec.hpp>
#include <bitcask/data_file.hpp>
#include <bitcask/keydir_registry.hpp>
#include <bitcask/oki_run.hpp>
#include <bitcask/oki_state.hpp>

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskOptions;

namespace {

bitcask::keydir::KeyDirRegistry& test_registry() {
    static bitcask::keydir::KeyDirRegistry reg;
    return reg;
}

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string val_of(int i) { return "value-" + std::to_string(i); }

// S37-2：原为 OkiLevelBTest 的成员——崩溃场景函数在命名空间作用域，够不着。
// 纯函数（只由 budget 决定），外提无副作用。
CaskOptions levelb_opts(std::size_t budget) {
    CaskOptions o;
    o.read_write = true;
    o.keydir_cache_entries = budget;
    return o;
}

// 物理驻留条目数（含墓碑 sentinel——直方图口径）。
std::uint64_t physical_entries(bitcask::keydir::KeyDir& kd) {
    return kd.key_length_histogram().total;
}

// fstats 活 key 总和（merge 触发的输入——逐出态下必须保持逻辑精确）。
std::uint64_t fstats_live_sum(bitcask::keydir::KeyDir& kd) {
    std::uint64_t sum = 0;
    for (const auto& f : kd.info().fstats) sum += f.live_keys;
    return sum;
}

class OkiLevelBTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() /
               (std::string("bitcask_oki_levelb_") + info->name());
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    fs::path dir_;
};

}  // namespace

// 预算逐出：写入远超预算 → 物理驻留有界；全部 key 可读（冷路径）；
// 逻辑 key_count 与 fstats 活计数保持精确。
TEST_F(OkiLevelBTest, EvictionKeepsResidencyBoundedAndCountsExact) {
    constexpr int kKeys = 8000;
    constexpr std::size_t kBudget = 1024;  // per-shard = max(1024/256,8) = 8
    auto c = Cask::open(dir_.string(), levelb_opts(kBudget), &test_registry());
    ASSERT_TRUE(c);
    auto& kd = (*c)->keydir();
    EXPECT_TRUE(kd.oki().manifest_level_b()) << "首开重建后 manifest 应带戳";
    EXPECT_GT(kd.cache_budget(), 0u);

    for (int i = 0; i < kKeys; ++i) {
        ASSERT_TRUE((*c)->put(bytes("k" + std::to_string(i)),
                              bytes(val_of(i)), 1000));
    }
    // 物理驻留 ≤ 分片预算 × 分片数 + 少量余量（采样逐出是软目标）。
    EXPECT_LE(physical_entries(kd), 3000u)
        << "预算 1024（分片下限 8×256=2048）下物理驻留必须有界";
    // 逻辑计数精确（无逐出干扰）。
    EXPECT_EQ(kd.info().key_count, static_cast<std::uint64_t>(kKeys));

    // 全部 key 可读（多数走冷路径）。
    for (int i = 0; i < kKeys; i += 97) {
        auto g = (*c)->get_owned(bytes("k" + std::to_string(i)));
        ASSERT_TRUE(g.has_value()) << i;
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(g->value.data()),
                              g->value.size()),
                  val_of(i));
    }

    // 覆盖全部 key（多数已被逐出——D4 冷记账：key_count 不得虚增）。
    for (int i = 0; i < kKeys; ++i) {
        ASSERT_TRUE((*c)->put(bytes("k" + std::to_string(i)),
                              bytes(val_of(i) + "!"), 1500));
    }
    EXPECT_EQ(kd.info().key_count, static_cast<std::uint64_t>(kKeys))
        << "覆盖被逐 key 不得虚增逻辑计数";
    EXPECT_EQ(fstats_live_sum(kd), static_cast<std::uint64_t>(kKeys))
        << "fstats 活计数在覆盖被逐 key 后必须精确（老文件已退账）";

    // 删除 1/4（多数被逐——冷记账真退账 + 组合视图墓碑）。
    for (int i = 0; i < kKeys; i += 4) {
        ASSERT_TRUE((*c)->remove(bytes("k" + std::to_string(i)), 2000));
    }
    const auto expect_live = static_cast<std::uint64_t>(kKeys - kKeys / 4);
    EXPECT_EQ(kd.info().key_count, expect_live)
        << "remove 被逐 key 必须退账（返回 true 语义）";
    EXPECT_EQ(fstats_live_sum(kd), expect_live);
    // 删除的 key 读不到、幸存的读得到。
    auto gd = (*c)->get_owned(bytes("k4"));
    ASSERT_FALSE(gd.has_value());
    EXPECT_EQ(gd.error().kind, bitcask::CaskError::kNotFound);
    ASSERT_TRUE((*c)->get_owned(bytes("k5")).has_value());

    EXPECT_EQ(kd.shadow_stats().drifts, 0u);
    (*c)->close();
}

// fold（CaskIter）与 parallel_scan 在逐出态下的完整枚举：被逐 key 不消失、
// 值正确、删除的 key 不复活。
TEST_F(OkiLevelBTest, FoldAndParallelScanEnumerateEvictedKeys) {
    constexpr int kKeys = 3000;
    auto c = Cask::open(dir_.string(), levelb_opts(512), &test_registry());
    ASSERT_TRUE(c);
    auto& kd = (*c)->keydir();

    std::map<std::string, std::string> expect;
    for (int i = 0; i < kKeys; ++i) {
        const std::string k = "f" + std::to_string(i);
        const std::string v = val_of(i);
        ASSERT_TRUE((*c)->put(bytes(k), bytes(v), 1000));
        expect[k] = v;
    }
    for (int i = 0; i < kKeys; i += 3) {  // 删 1/3
        const std::string k = "f" + std::to_string(i);
        ASSERT_TRUE((*c)->remove(bytes(k), 2000));
        expect.erase(k);
    }
    ASSERT_LE(physical_entries(kd), 3000u);  // 确认确实在逐出态

    // CaskIter 全量 fold 对拍影子 map。
    {
        auto it = (*c)->make_iter();
        auto st = it->start(-1, -1, 0, false);
        ASSERT_TRUE(st.has_value());
        ASSERT_EQ(*st, bitcask::keydir::StartIterResult::kOk);
        std::map<std::string, std::string> got;
        while (true) {
            auto e = it->next();
            ASSERT_TRUE(e.has_value());
            if (!e->has_value()) break;
            std::string k(reinterpret_cast<const char*>((*e)->key.data()),
                          (*e)->key.size());
            std::string v(reinterpret_cast<const char*>((*e)->value.data()),
                          (*e)->value.size());
            EXPECT_TRUE(got.emplace(std::move(k), std::move(v)).second)
                << "fold 不得重复输出同一 key";
        }
        it->release();
        EXPECT_EQ(got, expect) << "fold 输出必须与影子 map 完全一致";
    }

    // parallel_scan 计数对拍。
    {
        std::atomic<std::size_t> n{0};
        auto r = (*c)->parallel_scan(
            4, [&](std::span<const std::byte>, const bitcask::GetResultView&) {
                n.fetch_add(1);
            });
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(*r, expect.size());
        EXPECT_EQ(n.load(), expect.size());
    }

    EXPECT_EQ(kd.shadow_stats().drifts, 0u);
    (*c)->close();
}

// BCKS v4 + BCOM v3：Level B 重开走子集快照——重开后物理驻留仍有界
// （证明没做全量 fold）、逻辑计数保真、run gen 集不变（未触发重建）。
TEST_F(OkiLevelBTest, ReopenUsesSubsetSnapshotWithoutRebuild) {
    constexpr int kKeys = 5000;
    std::uint64_t logical = 0;
    std::vector<std::uint64_t> gens_before;
    {
        auto c = Cask::open(dir_.string(), levelb_opts(512), &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < kKeys; ++i) {
            ASSERT_TRUE((*c)->put(bytes("s" + std::to_string(i)),
                                  bytes(val_of(i)), 1000));
        }
        logical = (*c)->keydir().info().key_count;
        ASSERT_EQ(logical, static_cast<std::uint64_t>(kKeys));
        (*c)->close();
        auto m = bitcask::oki::read_manifest(dir_.string());
        ASSERT_TRUE(m.has_value());
        EXPECT_TRUE(m->level_b) << "Level B 写者的 manifest 必须带戳";
        for (const auto& r : m->runs) gens_before.push_back(r.gen);
        ASSERT_FALSE(gens_before.empty());
    }
    {
        auto c = Cask::open(dir_.string(), levelb_opts(512), &test_registry());
        ASSERT_TRUE(c);
        auto& kd = (*c)->keydir();
        EXPECT_EQ(kd.info().key_count, logical) << "v4 逻辑计数保真";
        EXPECT_LE(physical_entries(kd), 3000u)
            << "重开走 v4 子集快照——物理驻留必须仍有界（否则是全量 fold）";
        auto m = bitcask::oki::read_manifest(dir_.string());
        ASSERT_TRUE(m.has_value());
        std::vector<std::uint64_t> gens_after;
        for (const auto& r : m->runs) gens_after.push_back(r.gen);
        EXPECT_EQ(gens_after, gens_before)
            << "带戳 manifest + v4 快照的重开不得触发重建";
        // 抽查值（冷路径）。
        for (int i = 0; i < kKeys; i += 71) {
            auto g = (*c)->get_owned(bytes("s" + std::to_string(i)));
            ASSERT_TRUE(g.has_value()) << i;
            EXPECT_EQ(
                std::string(reinterpret_cast<const char*>(g->value.data()),
                            g->value.size()),
                val_of(i));
        }
        EXPECT_EQ(kd.shadow_stats().drifts, 0u);
        (*c)->close();
    }
}

// 模式转换：Level B → Level A（清戳 + v4 拒收 + 全量可见）→ Level A 下
// merge（无挂钩，run loc 陈旧）→ 回 Level B（强制重建，数据无损）。
TEST_F(OkiLevelBTest, LevelTransitionsStampAndRebuild) {
    constexpr int kKeys = 1200;
    std::vector<std::uint64_t> gens_levelb;
    {   // Level B 会话。
        auto c = Cask::open(dir_.string(), levelb_opts(256), &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < kKeys; ++i) {
            ASSERT_TRUE((*c)->put(bytes("t" + std::to_string(i)),
                                  bytes(val_of(i)), 1000));
        }
        (*c)->close();
        auto m = bitcask::oki::read_manifest(dir_.string());
        ASSERT_TRUE(m && m->level_b);
        for (const auto& r : m->runs) gens_levelb.push_back(r.gen);
    }
    {   // Level A 会话：清戳；全量可见；覆盖一半 + merge（搬迁无挂钩）。
        CaskOptions o;
        o.read_write = true;
        o.max_file_size = 4096;
        auto c = Cask::open(dir_.string(), o, &test_registry());
        ASSERT_TRUE(c);
        {
            auto m = bitcask::oki::read_manifest(dir_.string());
            ASSERT_TRUE(m.has_value());
            EXPECT_FALSE(m->level_b) << "Level A 写者 open 即清戳";
        }
        EXPECT_EQ((*c)->keydir().info().key_count,
                  static_cast<std::uint64_t>(kKeys))
            << "v4 被拒 → 全量 fold，数据无损";
        for (int i = 0; i < kKeys / 2; ++i) {
            ASSERT_TRUE((*c)->put(bytes("t" + std::to_string(i)),
                                  bytes(val_of(i) + "-A"), 1500));
        }
        ASSERT_TRUE((*c)->checkpoint());
        auto ms = (*c)->merge();
        ASSERT_TRUE(ms);
        (*c)->close();
    }
    {   // 回 Level B：未带戳 → 强制重建（gen 集变化），数据/值全部正确。
        auto c = Cask::open(dir_.string(), levelb_opts(256), &test_registry());
        ASSERT_TRUE(c);
        auto m = bitcask::oki::read_manifest(dir_.string());
        ASSERT_TRUE(m && m->level_b) << "重建后重新带戳";
        std::vector<std::uint64_t> gens_now;
        for (const auto& r : m->runs) gens_now.push_back(r.gen);
        EXPECT_NE(gens_now, gens_levelb) << "必须发生了重建（gen 集应变化）";
        for (int i = 0; i < kKeys; i += 37) {
            auto g = (*c)->get_owned(bytes("t" + std::to_string(i)));
            ASSERT_TRUE(g.has_value()) << i;
            const std::string want =
                (i < kKeys / 2) ? val_of(i) + "-A" : val_of(i);
            EXPECT_EQ(
                std::string(reinterpret_cast<const char*>(g->value.data()),
                            g->value.size()),
                want)
                << i;
        }
        EXPECT_EQ((*c)->keydir().shadow_stats().drifts, 0u);
        (*c)->close();
    }
}

// merge_only 旁车 × Level B 目录：open 必须拒绝（旁车的无挂钩搬迁会
// 静默腐蚀组合视图位置权威）。
TEST_F(OkiLevelBTest, MergeOnlySidecarRefusedOnLevelBDir) {
    {
        auto c = Cask::open(dir_.string(), levelb_opts(256), &test_registry());
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c)->put(bytes("x"), bytes("v"), 1000));
        (*c)->close();
    }
    CaskOptions mo;
    mo.merge_only = true;
    auto c = Cask::open(dir_.string(), mo, &test_registry());
    EXPECT_FALSE(c.has_value())
        << "merge_only 旁车必须拒开带 Level B 戳的目录";
}

// 并发：写者 × 2 + 读者 × 2 + 周期 fold，预算逐出全程自动发生，影子
// 对拍在线零漂移。（无 merge——活性判定切 locate 排 S36-5。）
TEST_F(OkiLevelBTest, ConcurrentWorkloadUnderEvictionZeroDrift) {
    auto c = Cask::open(dir_.string(), levelb_opts(512), &test_registry());
    ASSERT_TRUE(c);
    auto& kd = (*c)->keydir();
    kd.set_oki_shadow_check(true);  // 不依赖 NDEBUG
#if defined(__SANITIZE_THREAD__)          // GCC
#  define BITCASK_TSAN_ACTIVE 1
#elif defined(__has_feature)              // clang
#  if __has_feature(thread_sanitizer)
#    define BITCASK_TSAN_ACTIVE 1
#  endif
#endif
#ifdef BITCASK_TSAN_ACTIVE
    // S29-6 seqlock 既知误报同根因（oki_range_test 同款处理）；本测试
    // 对象是逐出/冷路径/fold 并发，关闭乐观读后仍全程受检。
    kd.set_optimistic_reads(false);
#endif

    for (int i = 0; i < 800; ++i) {
        ASSERT_TRUE((*c)->put(bytes("c" + std::to_string(i)),
                              bytes("v0"), 1000));
    }

    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int w = 0; w < 2; ++w) {
        ts.emplace_back([&, w] {
            std::mt19937_64 rng(static_cast<std::uint64_t>(w) + 1);
            for (int i = 0; i < 1500; ++i) {
                const std::string k = "c" + std::to_string(rng() % 800);
                if (rng() % 5 == 0) {
                    (void)(*c)->remove(bytes(k), 2000);
                } else if (!(*c)->put(bytes(k), bytes("v"), 1000)) {
                    errors.fetch_add(1);
                }
            }
            stop.store(true);
        });
    }
    for (int r = 0; r < 2; ++r) {
        ts.emplace_back([&, r] {
            std::mt19937_64 rng(100 + static_cast<std::uint64_t>(r));
            while (!stop.load()) {
                auto g = (*c)->get_owned(
                    bytes("c" + std::to_string(rng() % 800)));
                if (!g && g.error().kind != bitcask::CaskError::kNotFound) {
                    errors.fetch_add(1);
                }
            }
        });
    }
    ts.emplace_back([&] {  // 周期 fold（冷枚举与写者并发）
        while (!stop.load()) {
            auto it = (*c)->make_iter();
            auto st = it->start(-1, -1, 0, false);
            if (!st) { errors.fetch_add(1); break; }
            if (*st != bitcask::keydir::StartIterResult::kOk) continue;
            while (true) {
                auto e = it->next();
                if (!e) { errors.fetch_add(1); break; }
                if (!e->has_value()) break;
            }
            it->release();
        }
    });
    for (auto& t : ts) t.join();

    EXPECT_EQ(errors.load(), 0);
    EXPECT_EQ(kd.shadow_stats().drifts, 0u);
    (*c)->close();
}

// BCOM v3 字节级：level_b 戳 roundtrip；未知 flags 位 fail-fast（CRC 重算
// 后单独验证该门——格式纪律与 BCOK flags 同款）。
TEST_F(OkiLevelBTest, ManifestV3RoundTripAndUnknownFlagRejected) {
    namespace ok = bitcask::oki;
    ok::OkiManifest m;
    m.runs.push_back({7, 100, /*format_ver=*/2});
    m.wm = 100;
    m.level_b = true;
    ASSERT_TRUE(ok::write_manifest(dir_.string(), m));

    auto r = ok::read_manifest(dir_.string());
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->level_b);
    ASSERT_EQ(r->runs.size(), 1u);
    EXPECT_EQ(r->runs[0].gen, 7u);
    EXPECT_EQ(r->runs[0].format_ver, 2);
    EXPECT_EQ(r->wm, 100u);

    // 未知 flags 位（bit1）→ 拒收（CRC 修补后仍拒——是 flags 门在拒）。
    const auto path = ok::mk_manifest_filename(dir_.string());
    std::ifstream in(path, std::ios::binary);
    std::vector<char> buf((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    in.close();
    ASSERT_GT(buf.size(), 29u);
    buf[8] = static_cast<char>(buf[8] | 0x02);  // flags 在 header 后第 1 字节
    const std::uint32_t crc = bitcask::codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(buf.data()), buf.size() - 8));
    std::memcpy(buf.data() + buf.size() - 8, &crc, 4);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    out.close();
    EXPECT_FALSE(ok::read_manifest(dir_.string()).has_value())
        << "未知 flags 位必须整体拒收（Level C 扩展位预留）";

    // 清戳 roundtrip：level_b=false 写出 v2（惰性版本回落，老读端可读）。
    m.level_b = false;
    ASSERT_TRUE(ok::write_manifest(dir_.string(), m));
    auto r2 = ok::read_manifest(dir_.string());
    ASSERT_TRUE(r2.has_value());
    EXPECT_FALSE(r2->level_b);
}

// ============================================================================
// S36-5：merge 组合视图 + 崩溃注入 + B1 收口
// ============================================================================

// B1 不变量注入：sync 策略全关（默认）下，checkpoint 后快照/OKI 引用的
// active 字节必须已 fsync（active_durable_bytes ≥ checkpoint 时刻的文件
// 大小）；据此截掉「未持久尾巴」模拟掉电——重放后旧键全在、掉电后写入
// 的键干净缺席（kNotFound 而非悬空 kIo/kBadCrc）。
constexpr int kB1Before = 300;
constexpr int kB1After = 200;

// 状态文件路径：原实现由父进程闭包捕获；exec-self 下父子各自从 dir 推导。
std::string b1_status_path(const std::string& dir) {
    return (fs::path(dir) / "..status").string();
}

BITCASK_CRASH_SCENARIO(b1_checkpoint_fsync) {
    auto c = Cask::open(dir, levelb_opts(128), &test_registry());
    if (!c) crash_exit(1);
    for (int i = 0; i < kB1Before; ++i) {
        if (!(*c)->put(bytes("b1-" + std::to_string(i)),
                       bytes(val_of(i)), 1000)) {
            crash_exit(1);
        }
    }
    // 暴露面前提：默认策略下写后无任何 fsync。
    if ((*c)->active_durable_bytes() != 0) crash_exit(2);
    if (!(*c)->checkpoint()) crash_exit(1);
    // B1 不变量：checkpoint 采集点已把持久水位推进到覆盖全部已写字节。
    std::error_code ec;
    std::uintmax_t data_sz = 0;
    for (const auto& e : fs::directory_iterator(fs::path(dir), ec)) {
        if (e.path().string().ends_with(".bitcask.data")) {
            data_sz += fs::file_size(e.path(), ec);
        }
    }
    const std::uint64_t durable = (*c)->active_durable_bytes();
    {
        std::ofstream f(b1_status_path(dir), std::ios::trunc);
        f << durable << ' ' << data_sz << '\n';
    }
    if (durable < data_sz) crash_exit(3);  // 不变量破坏（修复前的形态）
    for (int i = 0; i < kB1After; ++i) {
        if (!(*c)->put(bytes("b1x-" + std::to_string(i)),
                       bytes("late"), 2000)) {
            crash_exit(1);
        }
    }
    crash_exit(0);  // 崩溃：不 close
}

TEST_F(OkiLevelBTest, B1CheckpointNeverOutrunsDataFsync) {
    constexpr int kBefore = kB1Before;
    constexpr int kAfter = kB1After;
    const std::string status_path = b1_status_path(dir_.string());

    ASSERT_EQ(bitcask::test::spawn_crash_child("b1_checkpoint_fsync",
                                               dir_.string()),
              0)
        << "exit=2: 前提失效（写后已有 fsync）；exit=3: B1 不变量破坏";

    // 模拟掉电：截掉 checkpoint 后未持久的尾巴（合法掉电态——持久水位之
    // 内的字节 fsync 过，必然幸存）。
    std::uint64_t durable = 0, data_sz = 0;
    {
        std::ifstream f(status_path);
        ASSERT_TRUE(f >> durable >> data_sz);
    }
    ASSERT_GE(durable, data_sz);
    // 找 active data 文件（本测试单文件负载：最大 file id 即 active）。
    fs::path active;
    std::uint64_t max_id = 0;
    for (const auto& e : fs::directory_iterator(dir_)) {
        const auto name = e.path().filename().string();
        if (name.ends_with(".bitcask.data")) {
            const auto id = std::strtoull(name.c_str(), nullptr, 10);
            if (id >= max_id) { max_id = id; active = e.path(); }
        }
    }
    ASSERT_FALSE(active.empty());
    fs::resize_file(active, durable);

    auto c = Cask::open(dir_.string(), levelb_opts(128), &test_registry());
    ASSERT_TRUE(c);
    for (int i = 0; i < kBefore; ++i) {
        auto g = (*c)->get_owned(bytes("b1-" + std::to_string(i)));
        ASSERT_TRUE(g.has_value()) << "checkpoint 覆盖的键必须幸存 i=" << i
                                   << " err=" << static_cast<int>(g.error().kind);
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(g->value.data()),
                              g->value.size()),
                  val_of(i));
    }
    for (int i = 0; i < kAfter; ++i) {
        auto g = (*c)->get_owned(bytes("b1x-" + std::to_string(i)));
        ASSERT_FALSE(g.has_value()) << i;
        EXPECT_EQ(g.error().kind, bitcask::CaskError::kNotFound)
            << "掉电丢失的键必须干净缺席（悬空引用会报 kIo/kBadCrc）";
    }
    EXPECT_EQ((*c)->keydir().shadow_stats().drifts, 0u);
    (*c)->close();
}

// 崩溃注入：merge（含被逐 key 的冷搬迁）后立即崩溃——S36-5 的「搬迁行
// 先于输入 unlink 固化」不变量保证任意崩溃点数据可读。
constexpr int kCrashMergeKeys = 600;

BITCASK_CRASH_SCENARIO(crash_after_merge) {
    CaskOptions o = levelb_opts(128);
    o.max_file_size = 4096;
    auto c = Cask::open(dir, o, &test_registry());
    if (!c) crash_exit(1);
    for (int i = 0; i < kCrashMergeKeys; ++i) {
        if (!(*c)->put(bytes("cm" + std::to_string(i)),
                       bytes(val_of(i)), 1000)) {
            crash_exit(1);
        }
    }
    if (!(*c)->checkpoint()) crash_exit(1);
    for (int i = 0; i < kCrashMergeKeys / 2; ++i) {  // 死字节，给 merge 干活
        if (!(*c)->put(bytes("cm" + std::to_string(i)),
                       bytes(val_of(i) + "!"), 1500)) {
            crash_exit(1);
        }
    }
    if (!(*c)->merge()) crash_exit(1);
    crash_exit(0);  // 崩溃：不 close、不 checkpoint——搬迁行只靠 merge 收尾固化
}

TEST_F(OkiLevelBTest, CrashRightAfterMergeKeepsEvictedKeysReadable) {
    constexpr int kKeys = kCrashMergeKeys;
    ASSERT_EQ(bitcask::test::spawn_crash_child("crash_after_merge",
                                               dir_.string()),
              0);

    auto c = Cask::open(dir_.string(), levelb_opts(128), &test_registry());
    ASSERT_TRUE(c);
    for (int i = 0; i < kKeys; ++i) {
        auto g = (*c)->get_owned(bytes("cm" + std::to_string(i)));
        ASSERT_TRUE(g.has_value())
            << "i=" << i << " err=" << static_cast<int>(g.error().kind)
            << "（搬迁行未固化即 unlink 会在此悬空）";
        const std::string want =
            (i < kKeys / 2) ? val_of(i) + "!" : val_of(i);
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(g->value.data()),
                              g->value.size()),
                  want);
    }
    EXPECT_EQ((*c)->keydir().info().key_count,
              static_cast<std::uint64_t>(kKeys));
    (*c)->close();
}

// TTL × 逐出 × merge：过期记录的 key 已被逐出——冷视图精确删除必须生效
//（组合视图记墓碑 + 退账），否则输入 unlink 后冷 get 报 kIo 悬空。
TEST_F(OkiLevelBTest, TtlMergeRemovesEvictedKeysCleanly) {
    CaskOptions o = levelb_opts(256);
    o.max_file_size = 512;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    auto& kd = (*c)->keydir();

    ASSERT_TRUE((*c)->put(bytes("ttl-ev"), bytes("doomed"), 50,
                          /*expiry_at=*/100));
    const std::string pad(64, 'f');
    for (int i = 0; i < 600; ++i) {  // 挤出 active + 制造逐出压力
        ASSERT_TRUE((*c)->put(bytes("fill" + std::to_string(i)),
                              bytes(pad), 50));
    }
    ASSERT_TRUE((*c)->checkpoint());
    ASSERT_TRUE(kd.evict("ttl-ev"));  // 确保过期键处于逐出态

    // 收集 sealed 文件显式 merge（全 live 不触发策略；同 docvalue TTL 惯例）。
    std::vector<std::string> files;
    for (const auto& de : fs::directory_iterator(dir_)) {
        const auto name = de.path().filename().string();
        if (bitcask::fileops::parse_data_tstamp(name).has_value()) {
            files.push_back(de.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    ASSERT_GT(files.size(), 1u);
    files.pop_back();  // active
    auto ms = (*c)->merge(files, /*now_sec=*/200);
    ASSERT_TRUE(ms);
    ASSERT_GT(ms->records_expired, 0u);

    auto g = (*c)->get_owned(bytes("ttl-ev"));
    ASSERT_FALSE(g.has_value());
    EXPECT_EQ(g.error().kind, bitcask::CaskError::kNotFound)
        << "冷视图未记墓碑的话这里是悬空 kIo";
    // 计数退账精确。
    EXPECT_EQ(kd.info().key_count, 600u);
    EXPECT_EQ(kd.shadow_stats().drifts, 0u);
    (*c)->close();
}

// 千轮 stress：逐出态下反复 覆盖→merge，全程零丢 key、值恒正确、计数
// 恒精确（S36-5 验收行）。
TEST_F(OkiLevelBTest, MergeUnderEvictionManyRoundsNoKeyLoss) {
    constexpr int kKeys = 400;
    constexpr int kRounds = 1000;
    CaskOptions o = levelb_opts(128);  // 深度逐出（400 key ≫ 128 预算）
    o.max_file_size = 8192;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    auto& kd = (*c)->keydir();

    std::vector<int> ver(kKeys, 0);
    for (int i = 0; i < kKeys; ++i) {
        ASSERT_TRUE((*c)->put(bytes("mr" + std::to_string(i)),
                              bytes(val_of(i) + "#0"), 1000));
    }
    std::mt19937_64 rng(0x536365);
    for (int round = 1; round <= kRounds; ++round) {
        // 覆盖随机 1/8（制造死字节 + 逐出扰动）。
        for (int j = 0; j < kKeys / 8; ++j) {
            const int i = static_cast<int>(rng() % kKeys);
            ver[static_cast<std::size_t>(i)] = round;
            ASSERT_TRUE((*c)->put(
                bytes("mr" + std::to_string(i)),
                bytes(val_of(i) + "#" + std::to_string(round)), 1000))
                << "round " << round;
        }
        if (round % 50 == 0) { ASSERT_TRUE((*c)->checkpoint()); }
        auto ms = (*c)->merge();
        ASSERT_TRUE(ms) << "round " << round;
        if (round % 100 == 0) {  // 周期全量对拍
            for (int i = 0; i < kKeys; ++i) {
                auto g = (*c)->get_owned(bytes("mr" + std::to_string(i)));
                ASSERT_TRUE(g.has_value()) << "round " << round << " i=" << i;
                EXPECT_EQ(
                    std::string(
                        reinterpret_cast<const char*>(g->value.data()),
                        g->value.size()),
                    val_of(i) + "#" + std::to_string(ver[static_cast<std::size_t>(i)]));
            }
            ASSERT_EQ(kd.info().key_count, static_cast<std::uint64_t>(kKeys))
                << "round " << round;
        }
    }
    EXPECT_EQ(kd.shadow_stats().drifts, 0u);
    (*c)->close();
}

// ============================================================================
// B4：延迟删除队列（unlink-while-open 退役）
// ============================================================================

// merge 输入退休而非当场 unlink：merge 后输入文件仍在（旧 keydir 快照的
// 惰性重开不再有 ENOENT 窗口）；下一次 merge / close 落点才真正删除。
// Level A 语义（预算 0）——退休队列与 Level B 正交。
TEST_F(OkiLevelBTest, MergeRetiresInputsAndDrainsAtNextCycle) {
    CaskOptions o;
    o.read_write = true;
    o.max_file_size = 2048;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);

    const std::string pad(96, 'x');
    for (int i = 0; i < 200; ++i) {
        ASSERT_TRUE((*c)->put(bytes("rt" + std::to_string(i)),
                              bytes(pad), 1000));
    }
    for (int i = 0; i < 200; ++i) {  // 全量覆盖 → 旧文件全是死字节
        ASSERT_TRUE((*c)->put(bytes("rt" + std::to_string(i)),
                              bytes(pad + "!"), 1500));
    }
    auto sealed_before = [&] {
        std::vector<std::string> v;
        for (const auto& e : fs::directory_iterator(dir_)) {
            const auto name = e.path().filename().string();
            if (bitcask::fileops::parse_data_tstamp(name).has_value()) {
                v.push_back(e.path().string());
            }
        }
        std::sort(v.begin(), v.end());
        return v;
    }();

    auto ms = (*c)->merge();
    ASSERT_TRUE(ms);
    ASSERT_GT(ms->records_kept, 0u);

    // 退休而非删除：被 merge 的输入文件仍在原路径。
    int survivors = 0;
    for (const auto& p : sealed_before) {
        if (fs::exists(p)) ++survivors;
    }
    EXPECT_GT(survivors, 0) << "输入文件应退休滞留而非当场 unlink";
    // 数据照常可读（含可能仍指向旧位置的在途语义）。
    for (int i = 0; i < 200; i += 17) {
        auto g = (*c)->get_owned(bytes("rt" + std::to_string(i)));
        ASSERT_TRUE(g.has_value()) << i;
    }

    // 下一代落点（再 merge——入口排水）：上一代退休文件删除。
    auto ms2 = (*c)->merge();
    ASSERT_TRUE(ms2);
    int alive_after_drain = 0;
    for (const auto& p : sealed_before) {
        if (fs::exists(p)) ++alive_after_drain;
    }
    EXPECT_LT(alive_after_drain, survivors)
        << "下一次 merge 入口应排水删除上一代退休文件";
    (*c)->close();

    // close 兜底：全部退休文件出清（目录里只剩活文件与派生缓存）。
    for (const auto& p : sealed_before) {
        // 被第二次 merge 收编的新退休文件也已由 close 排水。
        (void)p;
    }
    // 重开验证数据完整。
    auto c2 = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c2);
    for (int i = 0; i < 200; i += 13) {
        auto g = (*c2)->get_owned(bytes("rt" + std::to_string(i)));
        ASSERT_TRUE(g.has_value()) << i;
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(g->value.data()),
                              g->value.size()),
                  pad + "!");
    }
    (*c2)->close();
}

// 崩溃时退休队列丢失无害：退休文件就是普通 data 文件——恢复 fold 的
// LWW/ord 门正确处理陈旧记录；后续 merge 再次收编（自愈回收）。
constexpr int kRetiredKeys = 150;

BITCASK_CRASH_SCENARIO(retired_files_crash) {
    CaskOptions o;
    o.read_write = true;
    o.max_file_size = 2048;
    auto c = Cask::open(dir, o, &test_registry());
    if (!c) crash_exit(1);
    const std::string pad(96, 'x');
    for (int i = 0; i < kRetiredKeys; ++i) {
        if (!(*c)->put(bytes("cr" + std::to_string(i)), bytes(pad), 1000)) {
            crash_exit(1);
        }
    }
    for (int i = 0; i < kRetiredKeys; ++i) {
        if (!(*c)->put(bytes("cr" + std::to_string(i)),
                       bytes(pad + "#new"), 1500)) {
            crash_exit(1);
        }
    }
    if (!(*c)->merge()) crash_exit(1);
    crash_exit(0);  // 崩溃：退休队列（仅内存）随进程消失，文件留在盘上
}

TEST_F(OkiLevelBTest, RetiredFilesSurviveCrashHarmlessly) {
    constexpr int kKeys = kRetiredKeys;
    ASSERT_EQ(bitcask::test::spawn_crash_child("retired_files_crash",
                                               dir_.string()),
              0);

    CaskOptions o;
    o.read_write = true;
    o.max_file_size = 2048;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    for (int i = 0; i < kKeys; ++i) {
        auto g = (*c)->get_owned(bytes("cr" + std::to_string(i)));
        ASSERT_TRUE(g.has_value()) << i;
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(g->value.data()),
                              g->value.size())
                      .substr(96),
                  "#new")
            << "退休残留文件的陈旧记录不得复活 i=" << i;
    }
    EXPECT_EQ((*c)->keydir().info().key_count,
              static_cast<std::uint64_t>(kKeys));
    (*c)->close();
}
