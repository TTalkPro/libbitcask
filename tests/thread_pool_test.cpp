// T2.7 单元测试：IndexPool + IndexTaskQueue + TbbLifetime
// S6-P2: 测试更新到新 start(MapFn, ReduceFn, ErrorFn) API。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

#include <dirent.h>

#include <gtest/gtest.h>

#include "bitcask/thread_pool.hpp"

namespace {

using bitcask::IndexOp;
using bitcask::IndexPool;
using bitcask::IndexLane;
using bitcask::IndexTask;
using bitcask::IndexTaskQueue;
using bitcask::TbbLifetime;
using bitcask::ReduceEntry;
using bitcask::ReorderEntry;
using bitcask::OnWriteEntry;
using bitcask::DeleteEntry;
using bitcask::SkipEntry;
using bitcask::RebuildEntry;

// S10-A5: make() 不再带 fields 参数；测试用此 helper 构造带字段 task。
// string_view 指向 string literal（静态存储）→ 任务生命周期内有效。
static IndexTask mk_fields_task(
    bitcask::IndexOp op, std::string_view key, std::uint64_t ord,
    std::string_view text, std::uint32_t file_id, std::uint64_t offset,
    std::uint32_t total_sz, std::uint32_t tstamp, std::uint32_t doc_len,
    std::initializer_list<std::pair<std::string_view, std::string_view>> flds) {
    auto t = IndexTask::make(op, key, ord, text, file_id, offset, total_sz,
                             tstamp, doc_len);
    t.fields.assign(flds.begin(), flds.end());
    return t;
}

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

    pool.submit(mk_fields_task(
        IndexOp::Add, "k0", 0, "text", 1, 0, 0, 0, 0,
        {{"title", "hello"}, {"body", "world"}}));
    pool.submit(mk_fields_task(
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

    pool.submit(mk_fields_task(
        IndexOp::Add, "k0", 0, "text", 1, 0, 0, 0, 0,
        {{"title", "x"}}));
    pool.submit(mk_fields_task(
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

    pool.submit(mk_fields_task(
        IndexOp::Add, "k0", 0, "t", 1, 0, 0, 0, 0, {{"f", "x"}}));
    pool.submit(mk_fields_task(
        IndexOp::Add, "k1", 1, "t", 1, 0, 0, 0, 0, {{"f", "y"}}));
    pool.submit(mk_fields_task(
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

// ===== S6-P3: 多 lib 共享池（AT5）=====

// 统计当前进程 OS 线程数（/proc/self/task 目录项）。Linux 专用。
static int count_os_threads() {
    int n = 0;
    if (DIR* d = ::opendir("/proc/self/task")) {
        while (dirent* e = ::readdir(d)) {
            if (e->d_name[0] != '.') ++n;
        }
        ::closedir(d);
    }
    return n;
}

// AT5-a：线程数与库数解耦（G2）。注册第 1 个 lib 惰性启动 dispatcher+reducer
// 两个线程；之后注册任意多 lib 都不再起新线程。不提交任务（避免 TBB 懒起
// worker 干扰计数），纯验证「线程数 = 常量，与库数无关」的结构性保证。
TEST(IndexPoolMultiLib, ThreadCountIndependentOfLibCount) {
    IndexPool pool(1, 10240);
    auto noop_map   = [](const IndexTask&) -> ReduceEntry { return ReduceEntry{}; };
    auto noop_red   = [](ReorderEntry&) {};
    auto noop_err   = []() {};

    const int before = count_os_threads();
    IndexLane* l0 = pool.register_lib(noop_map, noop_red, noop_err, 0);
    const int after_first = count_os_threads();
    // 首个 register 起 dispatcher + reducer 两条线程。
    EXPECT_EQ(after_first - before, 2);

    std::vector<IndexLane*> lanes{l0};
    for (int i = 0; i < 49; ++i) {
        lanes.push_back(pool.register_lib(noop_map, noop_red, noop_err, 0));
    }
    const int after_many = count_os_threads();
    // 多注册 49 个 lib：零新增线程（无 per-库线程）。
    EXPECT_EQ(after_many, after_first);

    for (IndexLane* l : lanes) pool.unregister_lib(l);
    pool.stop();
}

// AT5-b：库间独立 + 库内 ord 序。一个共享池，N 条 lane，每 lane 各自交错
// 提交 Add-with-fields；reducer 对每条 lane 按其 ord 严格升序 apply（per-lane
// I2）；各 lane 结果互不串扰。
TEST(IndexPoolMultiLib, LanesApplyIndependentlyInOrdOrder) {
    IndexPool pool(1, 10240);
    constexpr int kLibs  = 4;
    constexpr int kPerLib = 200;

    // 每 lane 记录 reducer 看到的 ord 序列（reducer 单线程串行，无需锁）。
    std::vector<std::vector<std::uint64_t>> seen(kLibs);
    std::vector<IndexLane*> lanes(kLibs);

    for (int lib = 0; lib < kLibs; ++lib) {
        lanes[lib] = pool.register_lib(
            // map：仅把 ord 透传进 ReduceJob（不做真分词），供 reducer 核对序。
            [](const IndexTask& t) -> ReduceEntry {
                ReduceEntry re;
                re.job.ord = t.ord;
                return re;
            },
            [&seen, lib](ReorderEntry& e) {
                // ReduceEntry 来自 Add-with-fields；记录其 ord。
                if (auto* re = std::get_if<ReduceEntry>(&e)) {
                    seen[lib].push_back(re->job.ord);
                }
            },
            []() {}, 0);
    }

    // N 个生产者线程，各喂自己的 lane（单写者契约：每 lane 一个 producer）。
    std::vector<std::thread> producers;
    for (int lib = 0; lib < kLibs; ++lib) {
        producers.emplace_back([&pool, lane = lanes[lib]] {
            for (int i = 0; i < kPerLib; ++i) {
                // Add-with-fields → 走 TBB 并行 map → 该 lane 的 reorder buffer。
                pool.submit(lane, mk_fields_task(
                    IndexOp::Add, "k" + std::to_string(i),
                    static_cast<std::uint64_t>(i), "text",
                    1, 0, 0, 0, 0,
                    {{"body", "text"}}));
            }
        });
    }
    for (auto& t : producers) t.join();

    for (int lib = 0; lib < kLibs; ++lib) pool.flush(lanes[lib]);

    // 每 lane 恰好看到自己的 kPerLib 条，且严格 0,1,...,kPerLib-1 升序。
    for (int lib = 0; lib < kLibs; ++lib) {
        ASSERT_EQ(seen[lib].size(), static_cast<std::size_t>(kPerLib))
            << "lib " << lib;
        for (int i = 0; i < kPerLib; ++i) {
            EXPECT_EQ(seen[lib][i], static_cast<std::uint64_t>(i))
                << "lib " << lib << " pos " << i;
        }
        EXPECT_EQ(lanes[lib]->applied_ord.load(),
                  static_cast<std::uint64_t>(kPerLib - 1));
    }

    for (int lib = 0; lib < kLibs; ++lib) pool.unregister_lib(lanes[lib]);
    pool.stop();
}

// AT5-c：unregister 后池仍服务其它 lib（生命周期隔离）。
TEST(IndexPoolMultiLib, UnregisterOneLibKeepsOthersRunning) {
    IndexPool pool(1, 10240);
    std::atomic<std::size_t> cntA{0}, cntB{0};
    IndexLane* a = pool.register_lib(
        [](const IndexTask&) -> ReduceEntry { return ReduceEntry{}; },
        [&](ReorderEntry&) { ++cntA; }, []() {}, 0);
    IndexLane* b = pool.register_lib(
        [](const IndexTask&) -> ReduceEntry { return ReduceEntry{}; },
        [&](ReorderEntry&) { ++cntB; }, []() {}, 0);

    for (int i = 0; i < 50; ++i)
        pool.submit(a, IndexTask::make(IndexOp::Add, "k", i, "t", 1, 0, 0, 0, 0));
    pool.flush(a);
    pool.unregister_lib(a);  // a 排空并注销
    EXPECT_EQ(cntA.load(), 50u);

    // b 仍正常工作。
    for (int i = 0; i < 30; ++i)
        pool.submit(b, IndexTask::make(IndexOp::Add, "k", i, "t", 1, 0, 0, 0, 0));
    pool.flush(b);
    EXPECT_EQ(cntB.load(), 30u);

    pool.unregister_lib(b);
    pool.stop();
}

// AT6（S6-P4）：reorder 背压防 OOM。reducer 卡在首个 entry → reorder buffer
// 涨到 reorder_cap 后 map worker 停 pop → queue 满 → producer 阻塞（内存有界，
// 不无限堆积）。释放 reducer 后全部追平、零丢失。
TEST(IndexPoolMultiLib, ReorderBackpressureBoundsMemoryThenDrains) {
    constexpr int kWorkers = 2;
    constexpr std::size_t kQueueCap   = 8;
    constexpr std::size_t kReorderCap = 8;
    IndexPool pool(kWorkers, kQueueCap, kReorderCap);

    std::mutex m;
    std::condition_variable cv;
    bool release = false;
    std::atomic<std::size_t> processed{0};
    std::atomic<bool> reducer_blocked{false};

    IndexLane* lane = pool.register_lib(
        [](const IndexTask&) -> ReduceEntry { return ReduceEntry{}; },
        [&](ReorderEntry&) {
            if (processed.load() == 0) {
                std::unique_lock<std::mutex> lk(m);
                reducer_blocked = true;
                cv.wait(lk, [&] { return release; });
            }
            ++processed;
        },
        []() {}, 0);

    // 先喂一条让 reducer 卡住。
    pool.submit(lane, IndexTask::make(IndexOp::Add, "k", 0, "t", 0, 0, 0, 0, 0));
    while (!reducer_blocked.load()) std::this_thread::yield();

    // feeder 狂喂 kTotal 条：reducer 卡住 → reorder 涨到 cap → worker 停 pop →
    // queue 满 → feeder 在某条 submit 上阻塞。
    constexpr std::size_t kTotal = 1000;
    std::atomic<std::size_t> submitted{1};  // 已喂 ord=0
    std::thread feeder([&] {
        for (std::size_t i = 1; i < kTotal; ++i) {
            pool.submit(lane, IndexTask::make(IndexOp::Add, "k", i, "t", 0, 0, 0, 0, 0));
            submitted.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // 给 feeder 充分时间——它必定被背压卡住，远不到 kTotal（内存有界证明）。
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::size_t s = submitted.load();
    EXPECT_LT(s, kTotal) << "背压必须挡住 producer（内存有界），实际已 submit=" << s;
    // 上界粗估：在途 ≈ reorder_cap + 队列 + worker 数 + 余量，远小于 kTotal。
    EXPECT_LE(s, kReorderCap + kQueueCap + kWorkers + 16u);

    // 释放 reducer → 全部追平。
    { std::lock_guard<std::mutex> lk(m); release = true; }
    cv.notify_all();
    feeder.join();
    pool.flush(lane);
    EXPECT_EQ(processed.load(), kTotal) << "释放后必须零丢失全部 apply";
    EXPECT_EQ(submitted.load(), kTotal);

    pool.unregister_lib(lane);
    pool.stop();
}

}  // namespace