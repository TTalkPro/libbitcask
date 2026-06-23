// B2:IndexPool 异步路径基准。度量 W2（IndexTask::make 构造）+ 入队 + worker
// 消费 + W3（flush cv 等待 pending 归 0）的端到端吞吐（tasks/s）。consumer 取
// no-op，隔离队列/调度/flush 开销（不含真实 BM25/HNSW 插入）。
//
// Run:  ./bitcask_bench --benchmark_filter=IndexPool

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "bitcask/thread_pool.hpp"

using bitcask::IndexOp;
using bitcask::IndexPool;
using bitcask::IndexTask;

static void BM_IndexPool_SubmitDrain(benchmark::State& state) {
    const int kTasks = static_cast<int>(state.range(0));

    for (auto _ : state) {
        state.PauseTiming();
        IndexPool pool(1, 1u << 17);  // 大队列：避免背压停顿，纯测吞吐
        std::atomic<std::size_t> n{0};
        pool.start([&](const IndexTask& t) {
            if (t.op != IndexOp::Sentinel) n.fetch_add(1, std::memory_order_relaxed);
            return true;
        });
        state.ResumeTiming();

        for (int i = 0; i < kTasks; ++i) {
            pool.submit(IndexTask::make(IndexOp::Add, "key",
                                        static_cast<std::uint64_t>(i),
                                        "some representative text payload",
                                        0, 0, 0, 0, 0));
        }
        pool.flush();  // W3:cv 等 pending 归 0

        state.PauseTiming();
        pool.stop();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(kTasks));
}
BENCHMARK(BM_IndexPool_SubmitDrain)->Arg(100000)->Unit(benchmark::kMillisecond);
