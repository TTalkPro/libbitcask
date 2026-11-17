// S45-0：索引管线活性回归（designs/index-pipeline-unordered-design-zh.md §2/§3）。
//
//   1. 池层：乱序提交（先 ord 1..N、最后 ord 0）+ 极小队列——S45-0 前
//      map worker 被 ring 名额挡死（死锁）；S45-4 起无「等队头」结构。
//   2. 端到端：search 模式下大 put_batch 与并发 put 交错（审计复现负载）。
//   3. close：在途写计数卡住时 close() 必须按注入的超时返回。
//
// 用例自身不得挂住 ctest：所有可能卡死的动作都在子线程里跑，主线程限时
// 等待；端到端用例额外带「无进展看门狗」，超时直接判失败退出。

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>
#include <bitcask/thread_pool.hpp>
#include <bitcask/detail/cpu_features.hpp>  // BITCASK_TSAN_ENABLED

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "support/test_paths.hpp"

namespace {

using namespace std::chrono_literals;
using bitcask::IndexOp;
using bitcask::IndexPool;
using bitcask::IndexTask;

bitcask::oki::IndexRegistry& test_registry() {
    static bitcask::oki::IndexRegistry reg;
    return reg;
}

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

// ---------------------------------------------------------------------------
// 1. 池层：乱序提交不再有任何「等队头」
// ---------------------------------------------------------------------------
// S45-0 时代这里是「饥饿旁路 + 对照组（关旁路必挂死）」。S45-4 起 reorder
// ring 与单 reducer 一并退役：没有线程等待特定 LSN，旁路随之删除，对照组
// 失去对象。保留同一负载形态（先 ord 1..N、最后 ord 0，极小队列）验证结构
// 性活性：LSN 连续完成前缀在乱序完成下照样推进到底。
TEST(IndexPoolLiveness, OutOfOrderSubmitWithTinyQueuesCompletes) {
    IndexPool pool(2, /*shard queue*/ 2, /*serial queue*/ 2);
    std::atomic<std::size_t> applied{0};
    bitcask::LaneCallbacks cb;
    cb.shard_apply = [&applied](IndexTask&, unsigned) {
        ++applied;
        return bitcask::ApplyResult::kDone;
    };
    bitcask::IndexLane* lane = pool.register_lib(std::move(cb), 0);
    constexpr std::uint64_t kN = 2000;
    auto f = std::async(std::launch::async, [&] {
        for (std::uint64_t o = 1; o <= kN; ++o) {
            pool.submit(lane, IndexTask::make(IndexOp::Add, "k" + std::to_string(o),
                                              o, "t", 1, 0, 0, 0, 0));
        }
        pool.submit(lane, IndexTask::make(IndexOp::Add, "k0", 0, "t", 1, 0, 0, 0, 0));
        return pool.wait_lsn(lane, kN + 1, 10s);
    });
    ASSERT_EQ(f.wait_for(10s), std::future_status::ready);
    EXPECT_TRUE(f.get());
    EXPECT_EQ(applied.load(), kN + 1);
    EXPECT_TRUE(pool.unregister_lib(lane));
}

// ---------------------------------------------------------------------------
// 2. 端到端：大 put_batch × 并发 put
// ---------------------------------------------------------------------------

class IndexLivenessCaskTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = bitcask::test::unique_tmpdir(
            std::string("index_liveness_") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::unique_ptr<bitcask::Cask> open_search(bitcask::CaskOptions opts = {}) {
        opts.read_write = true;
        opts.enable_search = true;
        bitcask::search::SearchLayerConfig scfg;
        scfg.analyzer_config.type = bitcask::text::AnalyzerType::Whitespace;
        opts.search_config = scfg;
        auto c = bitcask::Cask::open(dir_.string(), opts, &test_registry());
        EXPECT_TRUE(c) << (c ? "" : c.error().detail);
        return c ? *std::move(c) : nullptr;
    }
    std::filesystem::path dir_;
};

TEST_F(IndexLivenessCaskTest, LargePutBatchWithConcurrentPutsCompletes) {
    // 审计复现：批 60000 + 4 个 put 线程在 7.0.3 上 2/2 死锁。TSan/Debug
    // 树太慢，缩小规模只验「能完成」（不一定触发旧缺陷）。
    std::size_t batch_n = 60000;
#if BITCASK_TSAN_ENABLED || !defined(NDEBUG)
    batch_n = 8000;
#endif
    if (const char* e = std::getenv("BITCASK_LIVENESS_BATCH")) {
        batch_n = static_cast<std::size_t>(std::strtoull(e, nullptr, 10));
    }
    constexpr int kPutters = 4;
    auto c = open_search();
    ASSERT_TRUE(c);

    std::vector<std::string> keys(batch_n), vals(batch_n);
    std::vector<bitcask::Cask::BatchItem> items(batch_n);
    for (std::size_t i = 0; i < batch_n; ++i) {
        keys[i] = "b" + std::to_string(i);
        vals[i] = "hello world batch doc " + std::to_string(i);
        items[i] = {bytes(keys[i]), bytes(vals[i])};
    }
    std::atomic<bool> batch_done{false};
    std::atomic<std::uint64_t> puts{0};
    std::atomic<int> finished{0};
    std::thread tb([&] {
        EXPECT_TRUE(c->put_batch(items));
        batch_done = true;
        ++finished;
    });
    std::vector<std::thread> tp;
    for (int t = 0; t < kPutters; ++t) {
        tp.emplace_back([&, t] {
            std::uint64_t i = 0;
            while (!batch_done.load()) {
                const std::string k =
                    "p" + std::to_string(t) + "_" + std::to_string(i++);
                (void)c->put(bytes(k), bytes("single put doc text"));
                puts.fetch_add(1, std::memory_order_relaxed);
            }
            ++finished;
        });
    }
    // 无进展看门狗：死锁时 CPU 归零、计数不动——直接判失败退出（线程卡在
    // 池里无法 join，继续跑只会挂住 ctest）。
    std::uint64_t last = 0;
    int stalled = 0;
    while (finished.load() < kPutters + 1) {
        std::this_thread::sleep_for(1s);
        const auto p = puts.load();
        stalled = (p == last && !batch_done.load()) ? stalled + 1 : 0;
        last = p;
        if (stalled >= 60) {
            std::fprintf(stderr,
                         "DEADLOCK: no progress for 60s (puts=%llu)\n",
                         static_cast<unsigned long long>(p));
            std::fflush(stderr);
            std::_Exit(1);
        }
    }
    tb.join();
    for (auto& t : tp) t.join();

    ASSERT_TRUE(c->drain_plugins());
    // 批里每一篇都必须可检索（索引未丢）。
    auto hits = c->search_text("batch", batch_n + 10);
    ASSERT_TRUE(hits);
    EXPECT_EQ(hits->hits.size(), batch_n);
    c->close();
}

// S45-3：读屏障只等调用时刻之前分配的 LSN——持续写入下不得饿死（原谓词
// 「in_flight==0」在写入不停时可能长时间不成立）。
TEST_F(IndexLivenessCaskTest, FlushIndexDoesNotStarveUnderContinuousWrites) {
    auto c = open_search();
    ASSERT_TRUE(c);
    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t) {
        writers.emplace_back([&, t] {
            std::uint64_t i = 0;
            while (!stop.load()) {
                const std::string k =
                    "w" + std::to_string(t) + "_" + std::to_string(i++ % 5000);
                (void)c->put(bytes(k), bytes("continuous writer text"));
            }
        });
    }
    std::this_thread::sleep_for(100ms);
    for (int round = 0; round < 20; ++round) {
        const std::string k = "probe" + std::to_string(round);
        ASSERT_TRUE(c->put(bytes(k), bytes("probe_" + std::to_string(round))));
        auto f = std::async(std::launch::async, [&] { c->flush_index(); });
        ASSERT_EQ(f.wait_for(20s), std::future_status::ready)
            << "持续写入下读屏障饿死";
        auto hits = c->search_text("probe_" + std::to_string(round), 5);
        ASSERT_TRUE(hits);
        EXPECT_EQ(hits->hits.size(), 1u) << "读己之写失效 round " << round;
    }
    stop = true;
    for (auto& w : writers) w.join();
    c->close();
}

// ---------------------------------------------------------------------------
// 3. close 有界等待
// ---------------------------------------------------------------------------

class CloseWaitTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = bitcask::test::unique_tmpdir(
            std::string("close_wait_") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::filesystem::path dir_;
};

TEST_F(CloseWaitTest, StuckWriterCountTimesOutAtInjectedBound) {
    std::mutex mu;
    std::vector<std::string> errors;
    bitcask::CaskOptions opts;
    opts.read_write = true;
    opts.log_fn = [&](bitcask::CaskOptions::LogLevel lvl, std::string_view msg) {
        if (lvl == bitcask::CaskOptions::LogLevel::kError) {
            std::lock_guard<std::mutex> lk(mu);
            errors.emplace_back(msg);
        }
    };
    auto oc = bitcask::Cask::open(dir_.string(), opts, &test_registry());
    ASSERT_TRUE(oc) << oc.error().detail;
    auto c = std::move(*oc);
    ASSERT_TRUE(c->put(bytes("k"), bytes("v")));

    c->set_close_wait_timeout_for_test(100ms);
    auto pin = c->pin_write_gate_for_test();  // 模拟被 kill 的写线程

    auto fut = std::async(std::launch::async, [&] {
        const auto t0 = std::chrono::steady_clock::now();
        c->close();
        return std::chrono::steady_clock::now() - t0;
    });
    // 修复前：atomic::wait 无超时 → close 永不返回（此处 5s 判失败，而非挂住）。
    ASSERT_EQ(fut.wait_for(5s), std::future_status::ready)
        << "close() 未按注入的超时返回——逃生门失效";
    const auto elapsed = fut.get();
    EXPECT_GE(elapsed, 90ms);
    EXPECT_LT(elapsed, 3s);
    pin.reset();
    std::lock_guard<std::mutex> lk(mu);
    bool saw = false;
    for (const auto& e : errors) {
        if (e.find("writes_in_flight_") != std::string::npos) saw = true;
    }
    EXPECT_TRUE(saw) << "超时路径必须记 error 日志";
}

TEST_F(CloseWaitTest, InFlightWriterThatFinishesIsAwaitedWithoutTimeout) {
    std::atomic<int> error_logs{0};
    bitcask::CaskOptions opts;
    opts.read_write = true;
    opts.log_fn = [&](bitcask::CaskOptions::LogLevel lvl, std::string_view) {
        if (lvl == bitcask::CaskOptions::LogLevel::kError) ++error_logs;
    };
    auto oc = bitcask::Cask::open(dir_.string(), opts, &test_registry());
    ASSERT_TRUE(oc) << oc.error().detail;
    auto c = std::move(*oc);
    c->set_close_wait_timeout_for_test(10s);
    auto pin = c->pin_write_gate_for_test();
    std::thread releaser([&] {
        std::this_thread::sleep_for(50ms);
        pin.reset();
    });
    const auto t0 = std::chrono::steady_clock::now();
    c->close();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    releaser.join();
    EXPECT_GE(elapsed, 40ms) << "close 必须等在途写完成";
    EXPECT_LT(elapsed, 2s);
    EXPECT_EQ(error_logs.load(), 0) << "正常收敛不得走超时日志";
}

}  // namespace
