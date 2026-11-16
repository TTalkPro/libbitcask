// S44-2：OKI 归并选段策略（纯函数）。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include <bitcask/detail/oki_compaction_policy.hpp>

namespace {

using bitcask::oki::choose_compaction;
using bitcask::oki::CompactionMode;
using bitcask::oki::CompactionPolicyOptions;

CompactionPolicyOptions tiered(double k = 1.0, std::size_t max_runs = 24) {
    CompactionPolicyOptions o;
    o.mode = CompactionMode::kTiered;
    o.k = k;
    o.max_runs = max_runs;
    o.small_base_rows = 0;  // 关闭小库规则，单测纯几何行为
    return o;
}

TEST(OkiCompactionPolicy, EmptyAndSingle) {
    const std::vector<std::uint64_t> none;
    EXPECT_FALSE(choose_compaction(none, 0, tiered()).merge);
    const std::vector<std::uint64_t> one{100};
    EXPECT_FALSE(choose_compaction(one, 0, tiered()).merge);
    // force_full 对单 run 也生效（回收其中的墓碑行）。
    const auto f = choose_compaction(one, 0, tiered(), /*force_full=*/true);
    EXPECT_TRUE(f.merge);
    EXPECT_EQ(f.begin, 0u);
    EXPECT_TRUE(f.full);
}

TEST(OkiCompactionPolicy, GeometricSuffix) {
    // 底座很大、后面两个小 run 等大：只并两个小的，不碰底座。
    const std::vector<std::uint64_t> a{1000, 10, 10};
    auto c = choose_compaction(a, 0, tiered());
    ASSERT_TRUE(c.merge);
    EXPECT_EQ(c.begin, 1u);
    EXPECT_FALSE(c.full);
    // 前一个比累计量大：不并。
    const std::vector<std::uint64_t> b{1000, 50, 10};
    EXPECT_FALSE(choose_compaction(b, 0, tiered()).merge);
    // 累计量追上底座：全归并（40 ≤ 60 并入，累计 100 ≥ 底座 100）。
    const std::vector<std::uint64_t> d{100, 40, 60};
    c = choose_compaction(d, 0, tiered());
    ASSERT_TRUE(c.merge);
    EXPECT_EQ(c.begin, 0u);
    EXPECT_TRUE(c.full);
}

TEST(OkiCompactionPolicy, StrictlyDecreasingNeverMergesWithoutCap) {
    const std::vector<std::uint64_t> s{1000, 400, 150, 60, 20, 7};
    EXPECT_FALSE(choose_compaction(s, 0, tiered()).merge);
}

TEST(OkiCompactionPolicy, LargeKDegeneratesToFull) {
    const std::vector<std::uint64_t> s{1000000, 400, 150, 60, 20, 7};
    const auto c = choose_compaction(s, 0, tiered(1e18));
    ASSERT_TRUE(c.merge);
    EXPECT_EQ(c.begin, 0u);
    EXPECT_TRUE(c.full);
}

TEST(OkiCompactionPolicy, FloorRestrictsYoungLane) {
    const std::vector<std::uint64_t> s{10, 10, 10, 10};
    const auto c = choose_compaction(s, 2, tiered());
    ASSERT_TRUE(c.merge);
    EXPECT_GE(c.begin, 2u);
    EXPECT_FALSE(c.full);
    // floor 处只剩 1 个 run：无事可做。
    EXPECT_FALSE(choose_compaction(s, 3, tiered()).merge);
    // force_full 对年轻道无效。
    EXPECT_FALSE(choose_compaction(s, 3, tiered(), true).merge);
}

TEST(OkiCompactionPolicy, HardCapForcesWiderSuffix) {
    std::vector<std::uint64_t> s;
    std::uint64_t v = 1u << 20;
    for (int i = 0; i < 10; ++i) { s.push_back(v); v /= 3; }
    ASSERT_FALSE(choose_compaction(s, 0, tiered(1.0, 24)).merge);
    const auto c = choose_compaction(s, 0, tiered(1.0, 6));
    ASSERT_TRUE(c.merge);
    EXPECT_LE(c.begin + 1, 6u) << "归并后 run 数必须 ≤ max_runs";
}

// 硬上限只驱动主道：年轻道（floor > 0）在总 run 数越限时仍只按几何规则选段。
TEST(OkiCompactionPolicy, HardCapDoesNotDriveYoungLane) {
    // 主道在途的 5 个大输入 + 年轻道的 3 个递减小 run；总数 8 > max_runs 4。
    const std::vector<std::uint64_t> s{1000, 900, 800, 700, 600, 50, 20, 7};
    EXPECT_FALSE(choose_compaction(s, 5, tiered(1.0, 4)).merge)
        << "递减序列在几何规则下不并——不得被上限逼着重写";
    const auto main = choose_compaction(s, 0, tiered(1.0, 4));
    ASSERT_TRUE(main.merge);
    EXPECT_LE(main.begin + 1, 4u);
}

// 倒挂修复：老 run 远小于其后的新 run 时，主道从倒挂点起归并，恢复递减序。
TEST(OkiCompactionPolicy, InversionRepairOnMainLane) {
    const std::vector<std::uint64_t> s{33, 25, 18, 8, 6, 1200, 5, 3};
    const auto c = choose_compaction(s, 0, tiered(1.0, 24));
    ASSERT_TRUE(c.merge);
    EXPECT_EQ(c.begin, 0u);
    EXPECT_TRUE(c.full);
    // 小幅倒挂（< 2 倍）不触发修复，走普通几何规则。
    const std::vector<std::uint64_t> mild{100, 60, 70, 3};
    const auto m = choose_compaction(mild, 0, tiered(1.0, 24));
    EXPECT_FALSE(m.merge && m.begin == 0);
    // 年轻道不做修复。
    EXPECT_FALSE(choose_compaction(s, 6, tiered(1.0, 24)).merge);
}

// 被上限扩大后继续往前吸收：上限位置之前的小 run 不得被「新而大」的 run 隔断。
TEST(OkiCompactionPolicy, CapContinuesGeometricWalk) {
    const std::vector<std::uint64_t> s{1000, 400, 80, 60, 20, 7, 3};
    const auto c = choose_compaction(s, 0, tiered(1.0, 4));
    ASSERT_TRUE(c.merge);
    EXPECT_LE(c.begin + 1, 4u);
    // 上限位 3：后缀 60+20+7+3=90，下标 2 的 80 ≤ 90 → 继续吸收；
    // 累计 170 < 400 → 停在下标 2。
    EXPECT_EQ(c.begin, 2u);
}

TEST(OkiCompactionPolicy, FullModeAndSmallBase) {
    CompactionPolicyOptions o;
    o.mode = CompactionMode::kFull;
    std::vector<std::uint64_t> s(8, 100);
    EXPECT_FALSE(choose_compaction(s, 0, o).merge);
    s.push_back(100);
    auto c = choose_compaction(s, 0, o);
    ASSERT_TRUE(c.merge);
    EXPECT_TRUE(c.full);
    // 小库：底座 < small_base_rows 时按 kFull 规则（≤ 8 个 run 不动）。
    CompactionPolicyOptions t;
    t.small_base_rows = 1000;
    const std::vector<std::uint64_t> small{500, 10, 10};
    EXPECT_FALSE(choose_compaction(small, 0, t).merge);
}

// 模拟大量随机大小的 flush：run 数恒 ≤ max_runs；每行平均被重写次数接近
// log2(N / F_min)（宽松上界 + 常数）。
TEST(OkiCompactionPolicy, SimulatedWriteAmplificationIsLogarithmic) {
    const auto opts = tiered(1.0, 24);
    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<std::uint64_t> fsize(1000, 64000);
    std::vector<std::uint64_t> runs;
    std::uint64_t flushed = 0, rewritten = 0;
    for (int i = 0; i < 10000; ++i) {
        const auto f = fsize(rng);
        runs.push_back(f);
        flushed += f;
        for (;;) {
            const auto c = choose_compaction(runs, 0, opts);
            if (!c.merge) break;
            std::uint64_t sum = 0;
            for (std::size_t j = c.begin; j < runs.size(); ++j) sum += runs[j];
            rewritten += sum;
            runs.resize(c.begin);
            runs.push_back(sum);
        }
        ASSERT_LE(runs.size(), opts.max_runs) << "flush " << i;
    }
    const double amp = static_cast<double>(rewritten) / static_cast<double>(flushed);
    const double bound = std::log2(static_cast<double>(flushed) / 1000.0) + 3.0;
    EXPECT_LE(amp, bound) << "写放大 " << amp;
    // 对照：旧策略（全归并）在同一序列上的写放大远高于此。
    CompactionPolicyOptions full;
    full.mode = CompactionMode::kFull;
    runs.clear();
    std::uint64_t rewritten_full = 0;
    rng.seed(12345);
    for (int i = 0; i < 10000; ++i) {
        runs.push_back(fsize(rng));
        const auto c = choose_compaction(runs, 0, full);
        if (c.merge) {
            std::uint64_t sum = 0;
            for (auto x : runs) sum += x;
            rewritten_full += sum;
            runs.assign(1, sum);
        }
    }
    EXPECT_GT(rewritten_full, rewritten * 10);
}

}  // namespace
