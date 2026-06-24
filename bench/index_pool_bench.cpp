// B2:IndexPool 异步流水线基准（S6-P2 双线程：dispatcher + TBB 并行 map + reducer）。
// 度量 IndexTask::make 构造 + submit + dispatch（Add-with-fields 走 TBB 并行 map）
// + reorder buffer + reducer 按 ord 序 apply + W3 flush（cv 等 pending 归 0 且
// applied_ord 追上 hwm）的端到端吞吐（tasks/s）。map_fn/reduce_fn 取 no-op，
// 隔离队列/调度/reorder/flush 开销（不含真实 BM25/HNSW 插入）。
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
using bitcask::ReduceEntry;
using bitcask::ReorderEntry;

static void BM_IndexPool_SubmitDrain(benchmark::State& state) {
    const int kTasks = static_cast<int>(state.range(0));

    for (auto _ : state) {
        state.PauseTiming();
        IndexPool pool(1, 1u << 17);  // 大队列：避免背压停顿，纯测吞吐
        std::atomic<std::size_t> n{0};
        // S6-P2 三段回调：map（TBB 并行，Add-with-fields 路径）→ reduce（reducer
        // 串行按 ord 序）→ error。均 no-op，仅在 reduce 计数以防优化掉。
        pool.start(
            /*map_fn=*/[](const IndexTask&) { return ReduceEntry{}; },
            /*reduce_fn=*/[&](ReorderEntry&) {
                n.fetch_add(1, std::memory_order_relaxed);
            },
            /*error_fn=*/[] {});
        state.ResumeTiming();

        for (int i = 0; i < kTasks; ++i) {
            // 带 fields → 经 dispatcher 路由到 TBB 并行 map（P2 核心路径）。
            pool.submit(IndexTask::make(
                IndexOp::Add, "key", static_cast<std::uint64_t>(i),
                "some representative text payload", 0, 0, 0, 0, 0,
                {{"body", "some representative text payload"}}));
        }
        pool.flush();  // W3:cv 等 pending 归 0 + applied_ord 追上 hwm

        state.PauseTiming();
        pool.stop();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(kTasks));
}
BENCHMARK(BM_IndexPool_SubmitDrain)->Arg(100000)->Unit(benchmark::kMillisecond);
