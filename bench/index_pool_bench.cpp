// IndexPool 分片执行器基准（S45-4 改写；原 S6-P4 map/reduce 流水线版退役）。
//
// 三个度量：
//   1. BM_IndexPool_SubmitDrain —— no-op 应用，隔离队列/调度/LSN 完成跟踪/
//      读屏障的纯开销（tasks/s）。
//   2. BM_IndexPool_ShardSpeedup —— 应用含固定 CPU 负载（模拟 analyze），按
//      分片数（Arg）扫描，对比 1 vs N 分片吞吐 → 多核加速比。不同 key 并发、
//      同 key 串行（key 取 i，均匀散到各分片）。
//   3. BM_IndexPool_MultiLibThroughput —— 一个共享池、多 lib 并发写，验证
//      共享池下多库总吞吐随核数扩展（线程恒定 + 跨库并行）。
//
// Run:  ./bitcask_bench --benchmark_filter=IndexPool

#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "bitcask/thread_pool.hpp"

using bitcask::ApplyResult;
using bitcask::IndexLane;
using bitcask::IndexOp;
using bitcask::IndexPool;
using bitcask::IndexTask;
using bitcask::LaneCallbacks;

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

LaneCallbacks noop_cb(std::atomic<std::uint64_t>* sink = nullptr, bool heavy = false) {
    LaneCallbacks cb;
    cb.shard_apply = [sink, heavy](IndexTask& t, unsigned) {
        if (heavy) {
            const std::uint64_t v = simulated_analyze(t.ord);
            if (sink) sink->fetch_add(v & 1u, std::memory_order_relaxed);
        }
        return ApplyResult::kDone;
    };
    return cb;
}

}  // namespace

// ---- 1) 纯开销 ----
static void BM_IndexPool_SubmitDrain(benchmark::State& state) {
    const auto n = static_cast<std::uint64_t>(state.range(0));
    for (auto _ : state) {
        state.PauseTiming();
        IndexPool pool(1, 1u << 17);
        IndexLane* lane = pool.register_lib(noop_cb(), 0);
        state.ResumeTiming();
        for (std::uint64_t i = 0; i < n; ++i) {
            pool.submit(lane, IndexTask::make(IndexOp::Add, std::to_string(i), i,
                                              "t", 0, 0, 0, 0, 0));
        }
        pool.wait_lsn(lane, n);
        state.PauseTiming();
        (void)pool.unregister_lib(lane);
        pool.stop();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(n));
}
BENCHMARK(BM_IndexPool_SubmitDrain)->Arg(100000)->Unit(benchmark::kMillisecond);

// ---- 2) 分片加速比 ----
static void BM_IndexPool_ShardSpeedup(benchmark::State& state) {
    const int shards = static_cast<int>(state.range(0));
    constexpr std::uint64_t kTasks = 20000;
    std::atomic<std::uint64_t> sink{0};
    for (auto _ : state) {
        state.PauseTiming();
        IndexPool pool(shards, 1u << 16);
        IndexLane* lane = pool.register_lib(noop_cb(&sink, true), 0);
        state.ResumeTiming();
        for (std::uint64_t i = 0; i < kTasks; ++i) {
            pool.submit(lane, IndexTask::make(IndexOp::Add, std::to_string(i), i,
                                              "t", 0, 0, 0, 0, 0));
        }
        pool.wait_lsn(lane, kTasks);
        state.PauseTiming();
        (void)pool.unregister_lib(lane);
        pool.stop();
        state.ResumeTiming();
    }
    benchmark::DoNotOptimize(sink.load());
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(kTasks));
}
BENCHMARK(BM_IndexPool_ShardSpeedup)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

// ---- 3) 多库共享池 ----
static void BM_IndexPool_MultiLibThroughput(benchmark::State& state) {
    const int libs = static_cast<int>(state.range(0));
    const int kShards =
        static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    constexpr std::uint64_t kPerLib = 5000;
    std::atomic<std::uint64_t> sink{0};
    for (auto _ : state) {
        state.PauseTiming();
        IndexPool pool(kShards, 1u << 16);
        std::vector<IndexLane*> lanes;
        for (int l = 0; l < libs; ++l) {
            lanes.push_back(pool.register_lib(noop_cb(&sink, true), 0));
        }
        state.ResumeTiming();
        std::vector<std::thread> writers;
        for (int l = 0; l < libs; ++l) {
            writers.emplace_back([&, l] {
                for (std::uint64_t i = 0; i < kPerLib; ++i) {
                    pool.submit(lanes[static_cast<std::size_t>(l)],
                                IndexTask::make(IndexOp::Add, std::to_string(i), i,
                                                "t", 0, 0, 0, 0, 0));
                }
            });
        }
        for (auto& w : writers) w.join();
        for (auto* lane : lanes) pool.wait_lsn(lane, kPerLib);
        state.PauseTiming();
        for (auto* lane : lanes) (void)pool.unregister_lib(lane);
        pool.stop();
        state.ResumeTiming();
    }
    benchmark::DoNotOptimize(sink.load());
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            libs * static_cast<std::int64_t>(kPerLib));
}
BENCHMARK(BM_IndexPool_MultiLibThroughput)
    ->Arg(1)->Arg(2)->Arg(4)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
