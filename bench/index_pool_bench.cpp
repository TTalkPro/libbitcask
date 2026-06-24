// B2 / S6-P4:IndexPool 异步流水线基准。
//
// 三个度量：
//   1. BM_IndexPool_SubmitDrain —— no-op map/reduce，隔离队列/调度/reorder/
//      flush 的纯开销（tasks/s）。
//   2. BM_IndexPool_MapSpeedup —— map_fn 含固定 CPU 负载（模拟 analyze），
//      按 map worker 数（Arg）扫描，对比 1 vs N worker 的吞吐 → 多核加速比
//      （G1：热点库 analyze 并行吃满多核）。
//   3. BM_IndexPool_MultiLibThroughput —— 一个共享池、多 lib 并发写，验证
//      共享池下多库总吞吐随核数扩展（G2 线程恒定 + 跨库并行）。
//
// Run:  ./bitcask_bench --benchmark_filter=IndexPool

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "bitcask/thread_pool.hpp"

using bitcask::IndexLane;
using bitcask::IndexOp;
using bitcask::IndexPool;
using bitcask::IndexTask;
using bitcask::ReduceEntry;
using bitcask::ReorderEntry;

namespace {

// 固定 CPU 负载，模拟 analyze（分词）的每文档开销。volatile sink 防优化。
inline std::uint64_t simulated_analyze(std::uint64_t seed) {
    std::uint64_t acc = seed;
    for (int i = 0; i < 4000; ++i) {
        acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
        acc ^= acc >> 29;
    }
    return acc;
}

}  // namespace

// ---- 1) 纯流水线开销（no-op map/reduce）----
static void BM_IndexPool_SubmitDrain(benchmark::State& state) {
    const int kTasks = static_cast<int>(state.range(0));
    for (auto _ : state) {
        state.PauseTiming();
        IndexPool pool(1, 1u << 17);
        std::atomic<std::size_t> n{0};
        pool.start(
            [](const IndexTask&) { return ReduceEntry{}; },
            [&](ReorderEntry&) { n.fetch_add(1, std::memory_order_relaxed); },
            [] {});
        state.ResumeTiming();

        for (int i = 0; i < kTasks; ++i) {
            pool.submit(IndexTask::make(
                IndexOp::Add, "key", static_cast<std::uint64_t>(i),
                "some representative text payload", 0, 0, 0, 0, 0,
                {{"body", "some representative text payload"}}));
        }
        pool.flush();

        state.PauseTiming();
        pool.stop();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(kTasks));
}
BENCHMARK(BM_IndexPool_SubmitDrain)->Arg(100000)->Unit(benchmark::kMillisecond);

// ---- 2) 单 lib map 多核加速比（Arg = map worker 数）----
// map_fn 含固定 CPU 负载；reduce no-op。worker=1 → 串行 analyze；worker=N →
// N 文档并发 analyze。对比吞吐即多核加速比（G1）。
static void BM_IndexPool_MapSpeedup(benchmark::State& state) {
    const int kWorkers = static_cast<int>(state.range(0));
    constexpr int kTasks = 20000;
    std::atomic<std::uint64_t> sink{0};
    for (auto _ : state) {
        state.PauseTiming();
        IndexPool pool(kWorkers, 1u << 16);
        pool.start(
            [&sink](const IndexTask& t) -> ReduceEntry {
                sink.fetch_add(simulated_analyze(t.ord),
                               std::memory_order_relaxed);
                return ReduceEntry{};
            },
            [](ReorderEntry&) {},
            [] {});
        state.ResumeTiming();

        for (int i = 0; i < kTasks; ++i) {
            pool.submit(IndexTask::make(
                IndexOp::Add, "k", static_cast<std::uint64_t>(i), "t",
                0, 0, 0, 0, 0, {{"body", "t"}}));
        }
        pool.flush();

        state.PauseTiming();
        pool.stop();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(kTasks));
    benchmark::DoNotOptimize(sink.load());
}
// 1 → N worker：吞吐比即加速比。RangeMultiplier 2，覆盖到 8。
// UseRealTime：map 跑在 pool 线程上，必须按墙钟（而非 benchmark 线程 CPU）计速。
BENCHMARK(BM_IndexPool_MapSpeedup)
    ->RangeMultiplier(2)->Range(1, 8)->UseRealTime()
    ->Unit(benchmark::kMillisecond);

// ---- 3) 共享池多 lib 并发吞吐（Arg = lib 数）----
// 一个池（map worker = 硬件并发）、Arg 条 lane、每 lane 一个 producer 线程
// 并发喂入；含 CPU 负载的 map。度量总吞吐（tasks/s）随库数的扩展。
static void BM_IndexPool_MultiLibThroughput(benchmark::State& state) {
    const int kLibs = static_cast<int>(state.range(0));
    constexpr int kPerLib = 5000;
    unsigned hc = std::thread::hardware_concurrency();
    const int kWorkers = static_cast<int>(hc > 1 ? hc : 2);
    std::atomic<std::uint64_t> sink{0};

    for (auto _ : state) {
        state.PauseTiming();
        IndexPool pool(kWorkers, 1u << 16);
        std::vector<IndexLane*> lanes;
        for (int l = 0; l < kLibs; ++l) {
            lanes.push_back(pool.register_lib(
                [&sink](const IndexTask& t) -> ReduceEntry {
                    sink.fetch_add(simulated_analyze(t.ord),
                                   std::memory_order_relaxed);
                    return ReduceEntry{};
                },
                [](ReorderEntry&) {}, [] {}, 0));
        }
        state.ResumeTiming();

        std::vector<std::thread> producers;
        for (std::size_t l = 0; l < lanes.size(); ++l) {
            producers.emplace_back([&pool, lane = lanes[l]] {
                for (int i = 0; i < kPerLib; ++i) {
                    pool.submit(lane, IndexTask::make(
                        IndexOp::Add, "k", static_cast<std::uint64_t>(i), "t",
                        0, 0, 0, 0, 0, {{"body", "t"}}));
                }
            });
        }
        for (auto& t : producers) t.join();
        for (auto* lane : lanes) pool.flush(lane);

        state.PauseTiming();
        for (auto* lane : lanes) pool.unregister_lib(lane);
        pool.stop();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(kLibs) *
                            static_cast<std::int64_t>(kPerLib));
    benchmark::DoNotOptimize(sink.load());
}
BENCHMARK(BM_IndexPool_MultiLibThroughput)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime()
    ->Unit(benchmark::kMillisecond);
