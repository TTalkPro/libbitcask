// S45-3：LsnTracker 单测（乱序完成、窗口溢出、重复、并发、有界等待）。

#include <gtest/gtest.h>

#include <bitcask/lsn_tracker.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

namespace {

using bitcask::LsnTracker;
using namespace std::chrono_literals;

TEST(LsnTracker, InOrderAdvances) {
    LsnTracker t(10);
    EXPECT_EQ(t.contiguous(), 10u);
    t.done(10);
    t.done(11);
    EXPECT_EQ(t.contiguous(), 12u);
}

TEST(LsnTracker, OutOfOrderWaitsForGap) {
    LsnTracker t(0);
    t.done(2);
    t.done(1);
    EXPECT_EQ(t.contiguous(), 0u) << "缺 0 时前缀不得前进";
    t.done(0);
    EXPECT_EQ(t.contiguous(), 3u);
}

TEST(LsnTracker, DuplicatesAndBelowBaseAreIgnored) {
    LsnTracker t(5);
    t.done(3);   // < base
    t.done(6);
    t.done(6);   // 重复
    EXPECT_EQ(t.contiguous(), 5u);
    t.done(5);
    EXPECT_EQ(t.contiguous(), 7u);
    t.done(5);   // 已越过
    EXPECT_EQ(t.contiguous(), 7u);
}

TEST(LsnTracker, OverflowBeyondWindowIsReclaimed) {
    constexpr std::uint64_t W = LsnTracker::kWindow;
    LsnTracker t(0);
    // 窗口之外的超前 LSN 先完成（进溢出集合）。
    t.done(W + 5);
    t.done(2 * W + 1);
    EXPECT_EQ(t.overflow_size(), 2u);
    for (std::uint64_t i = 0; i < W + 5; ++i) t.done(i);
    EXPECT_EQ(t.contiguous(), W + 6) << "W+5 随窗口右移收回位图并接上前缀";
    for (std::uint64_t i = W + 6; i <= 2 * W; ++i) t.done(i);
    EXPECT_EQ(t.contiguous(), 2 * W + 2);
    EXPECT_EQ(t.overflow_size(), 0u);
}

TEST(LsnTracker, ConcurrentShuffledCompletion) {
    constexpr std::uint64_t N = 200000;
    constexpr int kThreads = 8;
    LsnTracker t(100);
    std::vector<std::uint64_t> all(N);
    std::iota(all.begin(), all.end(), 100);
    std::shuffle(all.begin(), all.end(), std::mt19937_64(42));
    std::vector<std::thread> th;
    for (int k = 0; k < kThreads; ++k) {
        th.emplace_back([&, k] {
            for (std::size_t i = static_cast<std::size_t>(k); i < N;
                 i += kThreads) {
                t.done(all[i]);
            }
        });
    }
    for (auto& x : th) x.join();
    EXPECT_EQ(t.contiguous(), 100 + N);
    EXPECT_EQ(t.overflow_size(), 0u);
}

TEST(LsnTracker, WaitUntilBoundedAndWakes) {
    LsnTracker t(0);
    EXPECT_FALSE(t.wait_until(1, 50ms)) << "未完成时有界等待须超时返回";
    auto f = std::async(std::launch::async, [&] { return t.wait_until(3); });
    std::this_thread::sleep_for(20ms);
    t.done(1);
    t.done(2);
    EXPECT_EQ(f.wait_for(50ms), std::future_status::timeout);
    t.done(0);
    ASSERT_EQ(f.wait_for(5s), std::future_status::ready);
    EXPECT_TRUE(f.get());
    EXPECT_TRUE(t.wait_until(2, 0ms)) << "已达成的目标立即返回";
}

}  // namespace
