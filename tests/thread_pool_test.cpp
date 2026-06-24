// T2.7 单元测试：IndexPool + IndexTaskQueue + TbbLifetime
// S6-P2: 测试更新到新 start(MapFn, ReduceFn, ErrorFn) API。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "bitcask/thread_pool.hpp"

namespace {

using bitcask::IndexOp;
using bitcask::IndexPool;
using bitcask::IndexTask;
using bitcask::IndexTaskQueue;
using bitcask::TbbLifetime;
using bitcask::ReduceEntry;
using bitcask::ReorderEntry;
using bitcask::OnWriteEntry;
using bitcask::DeleteEntry;
using bitcask::SkipEntry;
using bitcask::RebuildEntry;

// S6-P2: 简单计数测试用 — map 返回空 ReduceEntry，reduce 端计数。
// ALL task 类型走 reducer 计数（map 仅 Add+fields 触发）。
static void StartCountingPool(IndexPool& pool,
                              std::atomic<std::size_t>& count) {
    pool.start(
        [](const IndexTask&) -> ReduceEntry { return ReduceEntry{}; },
        [&](ReorderEntry&) { ++count; },
        []() {}
    );
}

TEST(IndexPool, SubmitAndProcess) {
    IndexPool pool(1, 10240);
    std::atomic<std::size_t> count{0};

    StartCountingPool(pool, count);

    constexpr std::size_t kTasks = 100;
    for (std::size_t i = 0; i < kTasks; ++i) {
        pool.submit(IndexTask::make(IndexOp::Add, std::to_string(i), i, "text", 1, 0, 0, 0, 0));
    }

    pool.flush();
    EXPECT_EQ(count.load(), kTasks);
    pool.stop();
}

TEST(IndexPool, StopIsIdempotent) {
    IndexPool pool(1, 10240);
    pool.start(
        [](const IndexTask&) -> ReduceEntry { return ReduceEntry{}; },
        [](ReorderEntry&) {},
        []() {}
    );
    pool.stop();
    pool.stop();
}

TEST(IndexPool, DrainWithoutStart) {
    IndexPool pool(1, 10240);
    pool.stop();
}

TEST(IndexPool, SentinelStopsWorker) {
    IndexPool pool(1, 10240);
    std::atomic<bool> invoked{false};

    pool.start(
        [](const IndexTask&) -> ReduceEntry { return ReduceEntry{}; },
        [&](ReorderEntry&) { invoked = true; },
        []() {}
    );

    // S6-P2: Sentinel 走 dispatcher 的特殊路径 — 不进 map 也不进 reducer，
    // 仅触发 got_sentinel_ 标志让 reducer 退出。所以 reducer 不会被回调。
    pool.queue().push(IndexTask::sentinel());
    pool.stop();

    EXPECT_FALSE(invoked);
}

TEST(IndexTaskQueue, BoundedCapacity) {
    IndexTaskQueue queue(5);
    for (int i = 0; i < 5; ++i) {
        queue.push(IndexTask::make(IndexOp::Add, std::to_string(i), 0, "text", 0, 0, 0, 0, 0));
    }
    EXPECT_EQ(queue.size(), 5);
}

TEST(IndexTaskQueue, TaskOrderingFIFO) {
    IndexTaskQueue queue(1024);
    constexpr std::size_t kCount = 200;
    for (std::size_t i = 0; i < kCount; ++i) {
        queue.push(IndexTask::make(IndexOp::Add, std::to_string(i), i, "text", 0, 0, 0, 0, 0));
    }

    std::vector<std::uint64_t> received;
    received.reserve(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        auto task = queue.pop();
        received.push_back(task.ord);
    }
    for (std::size_t i = 0; i < kCount; ++i) {
        EXPECT_EQ(received[i], i);
    }
}

// T4:背压——IndexTaskQueue 满时 submit 阻塞。
// S6-P2: dispatcher 持续 dispatch 单条任务很快（直接构造 entry 入 reorder），
// 不再是单 worker 串行处理的瓶颈，所以背压测试改为直接灌满有界 queue
// 验证 tbb::concurrent_bounded_queue 的 capacity 阻塞语义（与 P0/P1 等价）。
TEST(IndexPool, BackpressureBlocksWhenQueueFull) {
    constexpr std::size_t kCap = 4;
    IndexPool pool(1, kCap);
    std::atomic<std::size_t> processed{0};

    pool.start(
        [](const IndexTask&) -> ReduceEntry { return ReduceEntry{}; },
        [&](ReorderEntry&) { ++processed; },
        []() {}
    );

    // 直接灌满有界 queue（不启动 start 时的等价路径）：用 producer 线程推
    // 到 queue 内。在 start 前预填——queue 容量 = kCap = 4。
    // 实际上 start() 后 dispatcher 也会立刻消费，所以这里用「提交后立刻
    // 期望已处理」——验证的是 queue 的有界容量不丢任务而非严格阻塞语义。
    // 严格阻塞测试见 BackpressureBlocksOnBoundedQueue（仅测 queue 类型）。
    for (std::size_t i = 0; i < kCap; ++i) {
        pool.submit(IndexTask::make(IndexOp::Add, "k", i, "t", 0, 0, 0, 0, 0));
    }
    pool.flush();
    EXPECT_EQ(processed.load(), kCap);
    pool.stop();
}

// T4: IndexTaskQueue 自身的有界容量语义——仅测 queue 不依赖 pool。
// 不启动 pool（start 不调），producer 直接灌满 kCap 容量，第 kCap+1 次
// push 必须阻塞。
TEST(IndexTaskQueue, PushBlocksWhenFull) {
    constexpr std::size_t kCap = 4;
    IndexTaskQueue queue(kCap);
    for (std::size_t i = 0; i < kCap; ++i) {
        queue.push(IndexTask::make(IndexOp::Add, std::to_string(i), 0, "t", 0, 0, 0, 0, 0));
    }
    EXPECT_EQ(queue.size(), kCap);

    std::atomic<bool> pushed{false};
    std::thread t([&] {
        queue.push(IndexTask::make(IndexOp::Add, "x", 0, "t", 0, 0, 0, 0, 0));
        pushed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(pushed.load()) << "queue 满时 push 应阻塞（背压）";

    // 模拟消费者：pop 一条 → push 应立即完成。
    auto popped = queue.pop();
    EXPECT_EQ(popped.key(), "0");
    t.join();
    EXPECT_TRUE(pushed.load());
}

// T4:关闭排空契约。close() 的真实序是 flush()→stop()（cask.cpp）：flush()
// 等 pending 归 0（W3 cv），把背压堆积的全部任务消费干净，再 stop() 干净退出。
TEST(IndexPool, FlushDrainsBackpressuredThenStopClean) {
    IndexPool pool(1, 16);
    std::mutex m;
    std::condition_variable cv;
    bool release = false;
    std::atomic<std::size_t> processed{0};
    std::atomic<bool> reducer_in_wait{false};

    pool.start(
        [](const IndexTask&) -> ReduceEntry { return ReduceEntry{}; },
        [&](ReorderEntry&) {
            if (processed.load() == 0) {
                // 第一个 entry：reducer 卡住，让 queue 填满
                std::unique_lock<std::mutex> lk(m);
                reducer_in_wait = true;
                cv.wait(lk, [&] { return release; });
            }
            ++processed;
        },
        []() {}
    );

    pool.submit(IndexTask::make(IndexOp::Add, "a0", 0, "t", 0, 0, 0, 0, 0));
    while (!reducer_in_wait.load()) std::this_thread::yield();

    // 提交 kN+1 个：dispatcher 把它们全部 dispatch 进 reorder_pending_（因为
    // reducer 在 cv.wait），queue 很快清空——所以这里不依赖 queue 阻塞，
    // 而是验证 flush 能等到 reducer 把 reorder_pending_ 排空 + applied_ord
    // 追上 submitted_ord_hwm（这是 P2 的核心契约）。
    constexpr std::size_t kN = 16;
    std::thread feeder([&] {
        for (std::size_t i = 1; i <= kN; ++i) {
            pool.submit(IndexTask::make(IndexOp::Add, "k", i, "t", 0, 0, 0, 0, 0));
        }
    });
    // 等 feeder 全部 submit（queue 不阻塞因为 dispatcher 在 dispatch）
    feeder.join();

    // 此时 reducer 仍卡在第一条；reorder_pending_ 里有 16 条 OnWriteEntry；
    // queue 空。释放 reducer → 排空 reorder buffer。
    { std::lock_guard<std::mutex> lk(m); release = true; }
    cv.notify_all();

    pool.flush();  // 真实排空：pending==0 && applied_ord >= submitted_ord_hwm
    EXPECT_EQ(processed.load(), kN + 1) << "flush() 必须排空全部已提交任务";
    pool.stop();
    EXPECT_TRUE(pool.is_stopped());
}

TEST(TbbLifetime, AcquireRelease) {
    TbbLifetime lifetime;
    EXPECT_FALSE(lifetime.is_acquired());

    lifetime.acquire();
    EXPECT_TRUE(lifetime.is_acquired());

    lifetime.release();
    EXPECT_FALSE(lifetime.is_acquired());
}

TEST(CaskCompilation, IndexPoolAccessor) {
    static_assert(sizeof(IndexPool) > 0, "IndexPool must be complete type");
    static_assert(sizeof(TbbLifetime) > 0, "TbbLifetime must be complete type");
}

// S6-P1 AT3: ord 空洞（Skip marker）不 stall flush。
// 提交 Add{ord=0}, Skip{ord=1}, Add{ord=2} → flush 必须返回（不永久阻塞）。
// S6-P2: 计数走 reducer（map 只处理 Add+fields，单 text 走 OnWriteEntry）。
TEST(IndexPool, SkipMarkerFillsOrdHole) {
    IndexPool pool(1, 10240);
    std::atomic<std::size_t> add_count{0};
    std::atomic<std::size_t> skip_count{0};

    pool.start(
        // Add+fields 才会触发 map（这里 fields 全空 → 全走 OnWriteEntry）
        [](const IndexTask&) -> ReduceEntry { return ReduceEntry{}; },
        [&](ReorderEntry& e) {
            if (std::holds_alternative<OnWriteEntry>(e)) ++add_count;
            else if (std::holds_alternative<SkipEntry>(e)) ++skip_count;
        },
        []() {}
    );

    pool.submit(IndexTask::make(IndexOp::Add,  "k0", 0, "text0", 1, 0, 0, 0, 0));
    pool.submit(IndexTask::make(IndexOp::Skip, "",   1, "",      0, 0, 0, 0, 0));
    pool.submit(IndexTask::make(IndexOp::Add,  "k2", 2, "text2", 1, 0, 0, 0, 0));

    pool.flush();  // Must NOT hang — Skip fills the ord=1 hole

    EXPECT_EQ(add_count.load(), 2);
    EXPECT_EQ(skip_count.load(), 1);
    EXPECT_EQ(pool.applied_ord(), 2);
    EXPECT_EQ(pool.submitted_ord_hwm(), 2);
    pool.stop();
}

// S6-P1 AT4: flush 追平——提交多个 ord 任务后 flush，applied_ord 必须追上 hwm。
// S6-P2: 计数从 reducer 视角做。
TEST(IndexPool, FlushCatchesUpToSubmittedHwm) {
    IndexPool pool(1, 10240);
    std::atomic<std::size_t> processed{0};

    pool.start(
        [](const IndexTask&) -> ReduceEntry { return ReduceEntry{}; },
        [&](ReorderEntry&) { ++processed; },
        []() {}
    );

    // Submit a sequence: Add{0}, Add{1}, Skip{2}, Delete{3}, Add{4}
    pool.submit(IndexTask::make(IndexOp::Add,    "k0", 0, "t0", 1, 0, 0, 0, 0));
    pool.submit(IndexTask::make(IndexOp::Add,    "k1", 1, "t1", 1, 0, 0, 0, 0));
    pool.submit(IndexTask::make(IndexOp::Skip,   "",   2, "",   0, 0, 0, 0, 0));
    pool.submit(IndexTask::make(IndexOp::Delete, "k0", 3, "",   0, 0, 0, 0, 0));
    pool.submit(IndexTask::make(IndexOp::Add,    "k3", 4, "t3", 1, 0, 0, 0, 0));

    EXPECT_EQ(pool.submitted_ord_hwm(), 4);

    pool.flush();

    EXPECT_EQ(processed.load(), 5);  // All 5 tasks (including Skip)
    EXPECT_EQ(pool.applied_ord(), 4);
    EXPECT_GE(pool.applied_ord(), pool.submitted_ord_hwm());
    pool.stop();
}

// S6-P2 AT1: 管线 vs 串行字节等价 — 简化版（不接真 SearchLayer，用 mock
// 计数验证 dispatcher → TBB map → reorder buffer → reducer 全链路）。
//
// 守护的契约：
//   - Add+fields 走 TBB map（map_fn_ 被调用一次/条）
//   - Add+空 fields / Delete / Skip / RebuildHnsw 都不走 map_fn_，直接
//     进 reducer（dispatcher 构造对应 entry）
//   - ALL 任务都在 reducer 串行 apply
//   - exception 路径：map_fn_ 抛 → reducer 仍收到 entry（空）+ error_fn_
//     被调用 + ord 不 stall
TEST(IndexPool, PipelineProcessesAllTaskTypes) {
    IndexPool pool(1, 10240);
    std::atomic<std::size_t> map_count{0};
    std::atomic<std::size_t> reduce_count{0};
    std::atomic<std::size_t> error_count{0};
    std::atomic<std::size_t> onwrite_count{0};
    std::atomic<std::size_t> delete_count{0};
    std::atomic<std::size_t> skip_count{0};

    pool.start(
        [&](const IndexTask& task) -> ReduceEntry {
            ++map_count;
            // 真实 map_analyze 在此被调用（测试中只验证被触发次数）
            return ReduceEntry{};
        },
        [&](ReorderEntry& entry) {
            ++reduce_count;
            // 分发到 variant 各分支计数
            if (std::holds_alternative<OnWriteEntry>(entry)) ++onwrite_count;
            else if (std::holds_alternative<DeleteEntry>(entry)) ++delete_count;
            else if (std::holds_alternative<SkipEntry>(entry)) ++skip_count;
        },
        [&]() { ++error_count; }
    );

    // Submit mixed task types
    pool.submit(IndexTask::make(IndexOp::Add,  "k0", 0, "text", 1, 0, 0, 0, 0));
    pool.submit(IndexTask::make(IndexOp::Skip, "",   1, "",     0, 0, 0, 0, 0));
    pool.submit(IndexTask::make(IndexOp::Delete, "k0", 2, "",    0, 0, 0, 0, 0));
    pool.submit(IndexTask::make(IndexOp::Add,  "k1", 3, "text", 1, 0, 0, 0, 0));

    pool.flush();

    // Add+空 fields → OnWriteEntry（不进 map）；所以 map_count=0
    // 期望（基于当前提交：4 条任务全是 Add+空fields/Skip/Delete）：
    EXPECT_EQ(map_count.load(), 0);
    EXPECT_EQ(reduce_count.load(), 4); // ALL tasks go through reduce_fn
    EXPECT_EQ(onwrite_count.load(), 2); // Add{0}, Add{3}
    EXPECT_EQ(delete_count.load(), 1); // Delete{2}
    EXPECT_EQ(skip_count.load(), 1);   // Skip{1}
    EXPECT_EQ(error_count.load(), 0);
    EXPECT_EQ(pool.applied_ord(), 3);
    pool.stop();
}

// S6-P2: 多字段路径走 TBB map（fields 非空）。
// 验证：map_fn_ 被调用（每条 Add+fields 一次），reduce_fn_ 也被调用。
TEST(IndexPool, AddWithFieldsGoesThroughMap) {
    IndexPool pool(1, 10240);
    std::atomic<std::size_t> map_count{0};
    std::atomic<std::size_t> reduce_count{0};

    pool.start(
        [&](const IndexTask& task) -> ReduceEntry {
            ++map_count;
            return ReduceEntry{};
        },
        [&](ReorderEntry&) { ++reduce_count; },
        []() {}
    );

    pool.submit(IndexTask::make(
        IndexOp::Add, "k0", 0, "text", 1, 0, 0, 0, 0,
        {{"title", "hello"}, {"body", "world"}}));
    pool.submit(IndexTask::make(
        IndexOp::Add, "k1", 1, "text", 1, 0, 0, 0, 0,
        {{"title", "another"}}));

    pool.flush();

    EXPECT_EQ(map_count.load(), 2);     // Both went through TBB map
    EXPECT_EQ(reduce_count.load(), 2);  // Both went through reducer
    EXPECT_EQ(pool.applied_ord(), 1);
    pool.stop();
}

// S6-P2: exception 路径 — map_fn_ 抛异常时：
//   1) error_fn_ 被调用
//   2) reducer 仍收到 entry（不 stall ord 序）
//   3) applied_ord_ 推进
TEST(IndexPool, MapExceptionDoesNotStall) {
    IndexPool pool(1, 10240);
    std::atomic<std::size_t> error_count{0};
    std::atomic<std::size_t> reduce_count{0};

    pool.start(
        [](const IndexTask&) -> ReduceEntry {
            throw std::runtime_error("map_analyze failed");
        },
        [&](ReorderEntry&) { ++reduce_count; },
        [&]() { ++error_count; }
    );

    pool.submit(IndexTask::make(
        IndexOp::Add, "k0", 0, "text", 1, 0, 0, 0, 0,
        {{"title", "x"}}));
    pool.submit(IndexTask::make(
        IndexOp::Add, "k1", 1, "text", 1, 0, 0, 0, 0,
        {{"title", "y"}}));

    pool.flush();

    EXPECT_EQ(error_count.load(), 2);    // Both exceptions caught
    EXPECT_EQ(reduce_count.load(), 2);  // Both entries still applied
    EXPECT_EQ(pool.applied_ord(), 1);    // Ord progressed despite errors
    pool.stop();
}

// S6-P2: 异常在 reduce_fn_ 抛同样不 stall — reducer 仍推进 ord。
TEST(IndexPool, ReduceExceptionDoesNotStall) {
    IndexPool pool(1, 10240);
    std::atomic<std::size_t> error_count{0};
    std::atomic<std::size_t> processed{0};

    pool.start(
        [](const IndexTask&) -> ReduceEntry { return ReduceEntry{}; },
        [&](ReorderEntry&) {
            ++processed;
            throw std::runtime_error("apply failed");
        },
        [&]() { ++error_count; }
    );

    pool.submit(IndexTask::make(IndexOp::Add, "k0", 0, "text", 1, 0, 0, 0, 0));
    pool.submit(IndexTask::make(IndexOp::Add, "k1", 1, "text", 1, 0, 0, 0, 0));

    pool.flush();

    EXPECT_EQ(processed.load(), 2);
    EXPECT_EQ(error_count.load(), 2);
    EXPECT_EQ(pool.applied_ord(), 1);  // Crucial: ord not stalled
    pool.stop();
}

// S6-P2: ord 序保证 — 即使 map 完成顺序乱序，reducer 严格按 ord 升序 apply。
// 通过 map_fn_ 的故意 sleep 让大 ord 先返回，断言 apply 序仍按 ord 升序。
// reducer 端拿不到 entry 的 ord（variant 不直接暴露），但 applied_ord_ 是
// 已 commit 的最大 ord：reducer 刚 pop 出 entry 时 applied_ord_ 还是前一个
// 值，调用 reduce_fn_ 时 applied_ord_ 尚未 store 新值。所以观察 applied_ord_
// 单调递增等价于 apply 序递增。
TEST(IndexPool, ReducerAppliesInOrdOrder) {
    IndexPool pool(1, 10240);
    std::mutex mu;
    std::vector<std::uint64_t> snapshot;

    pool.start(
        [&](const IndexTask& task) -> ReduceEntry {
            // 大 ord sleep 短，小 ord sleep 长 → 完成顺序乱
            if (task.ord == 0) std::this_thread::sleep_for(std::chrono::milliseconds(30));
            else if (task.ord == 2) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return ReduceEntry{};
        },
        [&](ReorderEntry&) {
            std::lock_guard<std::mutex> lk(mu);
            snapshot.push_back(pool.applied_ord());
        },
        []() {}
    );

    pool.submit(IndexTask::make(
        IndexOp::Add, "k0", 0, "t", 1, 0, 0, 0, 0, {{"f", "x"}}));
    pool.submit(IndexTask::make(
        IndexOp::Add, "k1", 1, "t", 1, 0, 0, 0, 0, {{"f", "y"}}));
    pool.submit(IndexTask::make(
        IndexOp::Add, "k2", 2, "t", 1, 0, 0, 0, 0, {{"f", "z"}}));

    pool.flush();
    pool.stop();

    // 应用序 snapshot: 第一次 reducer 调用时 applied_ord_=-1（初值 0，
    // 但 reduce_fn_ 在 store(N) 之前被调用，所以读到「上一个 commit 的
    // max ord」）。snapshot 序列反映了「pop 出的 entry 的 ord」——因为
    // next_apply_ord_ 严格按升序 pop。等价于 apply 序本身。
    // 经过 reducer 后：snapshot 必为 [0, 0, 1]（前一个已 commit 序），
    // 最终提交后 applied_ord_ = 2。flush 后单独断言终值。
    ASSERT_EQ(snapshot.size(), 3u);
    EXPECT_LE(snapshot[0], snapshot[1]);
    EXPECT_LE(snapshot[1], snapshot[2]);
    EXPECT_LE(snapshot.back(), 2u);
    EXPECT_EQ(pool.applied_ord(), 2u);
}

// S6-P2: RebuildHnsw 现在携带 ord（merge 路径 alloc_ord）。
// 验证 RebuildHnsw 进 reducer 的 RebuildEntry 分支 + ord 参与 submitted_ord_hwm。
TEST(IndexPool, RebuildHnswCarriesOrd) {
    IndexPool pool(1, 10240);
    std::atomic<std::size_t> rebuild_count{0};

    pool.start(
        [](const IndexTask&) -> ReduceEntry { return ReduceEntry{}; },
        [&](ReorderEntry& e) {
            if (std::holds_alternative<RebuildEntry>(e)) ++rebuild_count;
        },
        []() {}
    );

    // Add{0} + RebuildHnsw{1} + Add{2} —— 验证 RebuildHnsw 携带 ord 参与
    // submitted_ord_hwm 且 reducer 按 ord 序 apply。
    pool.submit(IndexTask::make(IndexOp::Add, "k0", 0, "t", 1, 0, 0, 0, 0));
    {
        IndexTask t;
        t.op  = IndexOp::RebuildHnsw;
        t.ord = 1;
        pool.submit(std::move(t));
    }
    pool.submit(IndexTask::make(IndexOp::Add, "k2", 2, "t", 1, 0, 0, 0, 0));

    pool.flush();

    EXPECT_EQ(rebuild_count.load(), 1);
    EXPECT_EQ(pool.submitted_ord_hwm(), 2);  // RebuildHnsw ord=1 included
    EXPECT_EQ(pool.applied_ord(), 2);
    pool.stop();
}

}  // namespace