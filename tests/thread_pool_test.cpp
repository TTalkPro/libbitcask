// T2.7 单元测试：IndexPool + IndexTaskQueue + TbbLifetime
// S6-P2: 测试更新到新 start(MapFn, ReduceFn, ErrorFn) API。

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

#if defined(_WIN32)
// S37-5：count_os_threads 的 Windows 实现需要 Toolhelp 快照。
// 本文件是测试，不受「windows.h 只准进移植层 TU」那条约束的约束——
// 那条针对的是产品代码的公开/内部头，测试 TU 自成一体。
#  include <windows.h>
#  include <tlhelp32.h>
#elif defined(__FreeBSD__)
// count_os_threads 的 FreeBSD 实现走 sysctl(KERN_PROC_PID)，见下。
#  include <sys/types.h>
#  include <sys/sysctl.h>
#  include <sys/user.h>
#  include <unistd.h>
#endif


#include <gtest/gtest.h>

#include "bitcask/thread_pool.hpp"
#include "bitcask/detail/cpu_features.hpp"  // S37-4：BITCASK_TSAN_ENABLED

namespace {

using bitcask::IndexOp;
using bitcask::IndexPool;
using bitcask::IndexLane;
using bitcask::IndexTask;
using bitcask::IndexTaskQueue;
using bitcask::TbbLifetime;
using bitcask::PutEntry;
using bitcask::ReorderEntry;
using bitcask::DeleteEntry;
using bitcask::SkipEntry;
using bitcask::RunFnEntry;

// S15-2：MapFn 签名泛化 = IndexTask → 各插件 prepare 产物（注册序）。
// 纯池测试无插件，统一用「空 preps」noop map。
static std::vector<bitcask::plugin::PreparedPtr> no_preps(const bitcask::IndexTask&) {
    return {};
}

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

// S6-P2: 简单计数测试用 — map 返回空 preps，reduce 端计数。
// ALL task 类型走 reducer 计数（S15-2：所有 Add 都过 map_fn）。
static void StartCountingPool(IndexPool& pool,
                              std::atomic<std::size_t>& count) {
    pool.start(
        no_preps,
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
        no_preps,
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
        no_preps,
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
        no_preps,
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
        no_preps,
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

    // 此时 reducer 仍卡在第一条；reorder_pending_ 里有 16 条 PutEntry；
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
// S15-2: 所有 Add 统一走 map（此处空 preps），entry 类型 = PutEntry。
TEST(IndexPool, SkipMarkerFillsOrdHole) {
    IndexPool pool(1, 10240);
    std::atomic<std::size_t> add_count{0};
    std::atomic<std::size_t> skip_count{0};

    pool.start(
        no_preps,
        [&](ReorderEntry& e) {
            if (std::holds_alternative<PutEntry>(e)) ++add_count;
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
        no_preps,
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

// S6-P2 AT1: 管线 vs 串行字节等价 — 简化版（不接真插件，用 mock
// 计数验证 dispatcher → 并行 map → reorder buffer → reducer 全链路）。
//
// 守护的契约（S15-2 修订）：
//   - 所有 Add 走 map（map_fn_ 被调用一次/条；是否真预处理由插件自决）
//   - Delete / Skip / RunFn 不走 map_fn_，直接进 reducer 对应 entry
//   - ALL 任务都在 reducer 串行 apply
//   - exception 路径：map_fn_ 抛 → reducer 仍收到 entry（空 preps）+
//     error_fn_ 被调用 + ord 不 stall
TEST(IndexPool, PipelineProcessesAllTaskTypes) {
    IndexPool pool(1, 10240);
    std::atomic<std::size_t> map_count{0};
    std::atomic<std::size_t> reduce_count{0};
    std::atomic<std::size_t> error_count{0};
    std::atomic<std::size_t> put_count{0};
    std::atomic<std::size_t> delete_count{0};
    std::atomic<std::size_t> skip_count{0};

    pool.start(
        [&](const IndexTask&) {
            ++map_count;
            // 真实 prepare 分发在此被调用（测试中只验证被触发次数）
            return std::vector<bitcask::plugin::PreparedPtr>{};
        },
        [&](ReorderEntry& entry) {
            ++reduce_count;
            // 分发到 variant 各分支计数
            if (std::holds_alternative<PutEntry>(entry)) ++put_count;
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

    // S15-2：所有 Add 统一走 map（单文本预处理与否由插件自决）→ map_count=2
    EXPECT_EQ(map_count.load(), 2);    // Add{0}, Add{3}
    EXPECT_EQ(reduce_count.load(), 4); // ALL tasks go through reduce_fn
    EXPECT_EQ(put_count.load(), 2);    // Add{0}, Add{3}
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
        [&](const IndexTask&) {
            ++map_count;
            return std::vector<bitcask::plugin::PreparedPtr>{};
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
        [](const IndexTask&) -> std::vector<bitcask::plugin::PreparedPtr> {
            throw std::runtime_error("prepare failed");
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
        no_preps,
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
        [&](const IndexTask& task) -> std::vector<bitcask::plugin::PreparedPtr> {
            // 大 ord sleep 短，小 ord sleep 长 → 完成顺序乱
            if (task.ord == 0) std::this_thread::sleep_for(std::chrono::milliseconds(30));
            else if (task.ord == 2) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return {};
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

// S15-2: RunFn 携带 ord（merge 路径 alloc_ord；原 RebuildHnsw 已并入本通道）。
// 验证 RunFn 进 reducer 的 RunFnEntry 分支、闭包在 reducer 执行 + ord 参与
// submitted_ord_hwm。
TEST(IndexPool, RunFnCarriesOrdAndExecutesInReducer) {
    IndexPool pool(1, 10240);
    std::atomic<std::size_t> runfn_entry_count{0};
    std::atomic<std::size_t> fn_ran{0};

    pool.start(
        no_preps,
        [&](ReorderEntry& e) {
            if (auto* rf = std::get_if<RunFnEntry>(&e)) {
                ++runfn_entry_count;
                if (rf->fn) rf->fn();  // 宿主 reduce 闭包的分发语义
            }
        },
        []() {}
    );

    // Add{0} + RunFn{1} + Add{2} —— 验证 RunFn 携带 ord 参与
    // submitted_ord_hwm 且 reducer 按 ord 序 apply。
    pool.submit(IndexTask::make(IndexOp::Add, "k0", 0, "t", 1, 0, 0, 0, 0));
    {
        IndexTask t;
        t.op  = IndexOp::RunFn;
        t.ord = 1;
        t.fn  = [&] { ++fn_ran; };
        pool.submit(std::move(t));
    }
    pool.submit(IndexTask::make(IndexOp::Add, "k2", 2, "t", 1, 0, 0, 0, 0));

    pool.flush();

    EXPECT_EQ(runfn_entry_count.load(), 1);
    EXPECT_EQ(fn_ran.load(), 1);
    EXPECT_EQ(pool.submitted_ord_hwm(), 2);  // RunFn ord=1 included
    EXPECT_EQ(pool.applied_ord(), 2);
    pool.stop();
}

// ===== S6-P3: 多 lib 共享池（AT5）=====

// 统计当前进程 OS 线程数。**Linux 专用**——读 /proc/self/task 的目录项。
// S37-2：改用 std::filesystem 枚举（去掉 <dirent.h>）。
// S37-5（遗留项 W3 结清）：/proc 无 Windows 对应物——原实现在非 Linux 上恒
// 返回 0，而调用方断言的是**差值**（after_first - before == 2），0-0=0 直接
// 失败。这里补 Windows 实现而不是把用例标成 Linux-only：AT5 守的是「线程数
// 与库数解耦」这条结构性保证，它与平台无关，在 Windows 上同样值得守。
#if defined(_WIN32)
static int count_os_threads() {
    // Windows 没有「本进程的线程列表」这种轻量接口：TH32CS_SNAPTHREAD 拍的是
    // **全系统**线程快照，得自己按 owner pid 过滤。调用不便宜，但本用例只调
    // 三次。
    const DWORD me = ::GetCurrentProcessId();
    // ⚠️ **必须重试**：TH32CS_SNAPTHREAD 拍的是全系统线程表，系统繁忙时
    // 会以 ERROR_BAD_LENGTH 失败（微软文档明确要求重试）。首版没重试、
    // 失败即返回 0，于是本用例在 `ctest -j 8` 下偶发失败而单跑 5/5 全过
    // ——典型的「并发负载才复现」的 flaky，比直接不实现更糟。
    HANDLE snap = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 8; ++attempt) {
        snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) break;
        if (::GetLastError() != ERROR_BAD_LENGTH) break;  // 别的错就别空转
    }
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    int n = 0;
    if (::Thread32First(snap, &te)) {
        do {
            // dwSize 是这套老 API 的兼容字段：只有当它大到覆盖了
            // th32OwnerProcessID 时该字段才有效（文档明载的坑）。
            if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) +
                                 sizeof(te.th32OwnerProcessID) &&
                te.th32OwnerProcessID == me) {
                ++n;
            }
            te.dwSize = sizeof(te);  // Thread32Next 每次都要重置
        } while (::Thread32Next(snap, &te));
    }
    ::CloseHandle(snap);
    return n;
}
#elif defined(__FreeBSD__)
// FreeBSD 的 /proc 是 procfs，**不是** linprocfs：/proc/<pid> 下根本没有
// task/ 这个目录（哪怕 procfs 已挂载）。于是下面那份 Linux 实现在这里
// 恒返回 0，而调用方断言的是**差值**（after_first - before == 2），
// 0-0=0 直接失败——与 S37-5 在 Windows 上遇到的是同一件事，故按同样的
// 口径补实现，而不是把用例标成 Linux-only：AT5 守的是「线程数与库数
// 解耦」这条结构性保证，它与平台无关。
// KERN_PROC_PID 一次 sysctl 取回本进程的 kinfo_proc，ki_numthreads 即
// 当前线程数（内核直接维护，比枚举目录项还准）。
static int count_os_threads() {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, ::getpid()};
    struct kinfo_proc kp{};
    std::size_t len = sizeof(kp);
    if (::sysctl(mib, 4, &kp, &len, nullptr, 0) != 0 || len != sizeof(kp)) {
        return 0;
    }
    return static_cast<int>(kp.ki_numthreads);
}
#else
static int count_os_threads() {
    std::error_code ec;
    const std::filesystem::path task_dir{"/proc/self/task"};
    if (!std::filesystem::is_directory(task_dir, ec)) return 0;
    int n = 0;
    for (const auto& e : std::filesystem::directory_iterator(task_dir, ec)) {
        if (!e.path().filename().string().starts_with(".")) ++n;
    }
    return ec ? 0 : n;
}
#endif

// AT5-a：线程数与库数解耦（G2）。注册第 1 个 lib 惰性启动 dispatcher+reducer
// 两个线程；之后注册任意多 lib 都不再起新线程。不提交任务（避免 TBB 懒起
// worker 干扰计数），纯验证「线程数 = 常量，与库数无关」的结构性保证。
TEST(IndexPoolMultiLib, ThreadCountIndependentOfLibCount) {
    // S29-T:TSan 运行时自带后台线程计入 /proc/self/task,精确计数断言在
    // build-tsan 恒失真(git stash 验证过非业务回归)——按 TASK.md 既定修法
    // 跳过;结构性保证由 build-clang/build-rel 继续守护。
#if BITCASK_TSAN_ENABLED   // S37-4：见 detail/cpu_features.hpp
    GTEST_SKIP() << "TSan 运行时线程计入,OS 线程计数断言失真(S29-T)";
#endif
    auto noop_map   = no_preps;
    auto noop_red   = [](ReorderEntry&) {};
    auto noop_err   = []() {};

    // S37-6：整轮测量**重试**，而不是一次定生死。
    //
    // 计数手段是「进程线程总数」，它同时受被测对象之外的因素影响：Windows
    // 上 ntdll 的加载器工作线程、DLL 延迟加载、系统线程池都会在任意时刻
    // 增减本进程的线程；`ctest -j 8` 的并发负载下尤其明显。首版一次测量定
    // 结论，于是单跑 5/5 全过、并发下偶发失败——**flaky 测试比没有测试更糟**，
    // 它会让人习惯性忽略这个用例。
    //
    // 重试不削弱断言强度：真回归（注册一个 lib 就多起一条线程）是确定性的，
    // 每一轮都会违反；只有外部噪声才会时中时不中。
    constexpr int kAttempts = 5;
    int last_delta_first = -1;
    int last_after_first = -1;
    int last_after_many  = -1;
    bool ok = false;
    for (int attempt = 0; attempt < kAttempts && !ok; ++attempt) {
        IndexPool pool(1, 10240);
        const int before = count_os_threads();
        IndexLane* l0 = pool.register_lib(noop_map, noop_red, noop_err, 0);
        const int after_first = count_os_threads();

        std::vector<IndexLane*> lanes{l0};
        for (int i = 0; i < 49; ++i) {
            lanes.push_back(pool.register_lib(noop_map, noop_red, noop_err, 0));
        }
        const int after_many = count_os_threads();

        last_delta_first = after_first - before;
        last_after_first = after_first;
        last_after_many  = after_many;
        // 首个 register 起 dispatcher + reducer 两条线程；
        // 之后 49 个 lib 零新增（无 per-库线程）。
        ok = (last_delta_first == 2) && (after_many == after_first);

        for (IndexLane* l : lanes) (void)pool.unregister_lib(l);
        pool.stop();
    }
    EXPECT_TRUE(ok)
        << "线程数与库数解耦不成立（" << kAttempts << " 轮均未观察到）："
        << "首个 register 的线程增量=" << last_delta_first << "（应为 2），"
        << "注册 50 个 lib 后=" << last_after_many
        << "（应等于首个之后的 " << last_after_first << "）";
}

// AT5-b：库间独立 + 库内 ord 序。一个共享池，N 条 lane，每 lane 各自交错
// 提交 Add-with-fields；reducer 对每条 lane 按其 ord 严格升序 apply（per-lane
// I2）；各 lane 结果互不串扰。
TEST(IndexPoolMultiLib, LanesApplyIndependentlyInOrdOrder) {
    IndexPool pool(1, 10240);
    constexpr std::size_t kLibs   = 4;    // 用作 vector 尺寸/下标,故 size_t
    constexpr std::size_t kPerLib = 200;

    // 每 lane 记录 reducer 看到的 ord 序列（reducer 单线程串行，无需锁）。
    std::vector<std::vector<std::uint64_t>> seen(kLibs);
    std::vector<IndexLane*> lanes(kLibs);

    for (std::size_t lib = 0; lib < kLibs; ++lib) {
        lanes[lib] = pool.register_lib(
            // S15-2：ord 直接随 PutEntry.task 到达 reducer，map 无需透传。
            no_preps,
            [&seen, lib](ReorderEntry& e) {
                // PutEntry 来自 Add；记录其 ord。
                if (auto* pe = std::get_if<PutEntry>(&e)) {
                    seen[lib].push_back(pe->task.ord);
                }
            },
            []() {}, 0);
    }

    // N 个生产者线程，各喂自己的 lane（单写者契约：每 lane 一个 producer）。
    std::vector<std::thread> producers;
    for (std::size_t lib = 0; lib < kLibs; ++lib) {
        producers.emplace_back([&pool, lane = lanes[lib]] {
            for (std::size_t i = 0; i < kPerLib; ++i) {
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

    for (std::size_t lib = 0; lib < kLibs; ++lib) pool.flush(lanes[lib]);

    // 每 lane 恰好看到自己的 kPerLib 条，且严格 0,1,...,kPerLib-1 升序。
    for (std::size_t lib = 0; lib < kLibs; ++lib) {
        ASSERT_EQ(seen[lib].size(), static_cast<std::size_t>(kPerLib))
            << "lib " << lib;
        for (std::size_t i = 0; i < kPerLib; ++i) {
            EXPECT_EQ(seen[lib][i], static_cast<std::uint64_t>(i))
                << "lib " << lib << " pos " << i;
        }
        EXPECT_EQ(lanes[lib]->applied_ord.load(),
                  static_cast<std::uint64_t>(kPerLib - 1));
    }

    for (std::size_t lib = 0; lib < kLibs; ++lib) (void)pool.unregister_lib(lanes[lib]);
    pool.stop();
}

// AT5-c：unregister 后池仍服务其它 lib（生命周期隔离）。
TEST(IndexPoolMultiLib, UnregisterOneLibKeepsOthersRunning) {
    IndexPool pool(1, 10240);
    std::atomic<std::size_t> cntA{0}, cntB{0};
    IndexLane* a = pool.register_lib(
        no_preps,
        [&](ReorderEntry&) { ++cntA; }, []() {}, 0);
    IndexLane* b = pool.register_lib(
        no_preps,
        [&](ReorderEntry&) { ++cntB; }, []() {}, 0);

    for (std::uint64_t i = 0; i < 50; ++i)
        pool.submit(a, IndexTask::make(IndexOp::Add, "k", i, "t", 1, 0, 0, 0, 0));
    pool.flush(a);
    // 返回值就是「是否排空」——注释本来就这么写的,断言它比忽略强,
    // 且与下一行的 cntA==50 是同一件事的两面。
    EXPECT_TRUE(pool.unregister_lib(a)) << "a 应排空后注销";
    EXPECT_EQ(cntA.load(), 50u);

    // b 仍正常工作。
    for (std::uint64_t i = 0; i < 30; ++i)
        pool.submit(b, IndexTask::make(IndexOp::Add, "k", i, "t", 1, 0, 0, 0, 0));
    pool.flush(b);
    EXPECT_EQ(cntB.load(), 30u);

    (void)pool.unregister_lib(b);
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
        no_preps,
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

    (void)pool.unregister_lib(lane);
    pool.stop();
}

}  // namespace
// ===========================================================================
// P6-DL-1 复审：unregister_lib 超时路径
//
// T19 把 unregister_lib 的 flush 从无界改为有界(30s)，为的是不让 Cask::close
// 的 30s 逃生门形同虚设。但有界化**打破了原本使 erase 安全的不变量**——
// 无界 flush 返回 ⇒ in_flight==0 ⇒ ring 必空 ⇒ erase 不泄漏任何东西。
// 超时后 ring 非空却照样 erase，其中 K 个 entry 占用的 `reorder_inflight_`
// （**池全局**背压计数，只在 reducer apply 后减）就永久泄漏了。
//
// 后果比 unregister_lib 认下的 UAF 取舍更糟：池由 registry 跨库共享，
// A 库一次慢关闭会拖垮同进程 B..Z 所有库的索引——且 reorder_cap_ 是硬等待
// 条件（map worker 达限即停 pop），不是软提示。
//
// 这条路径此前零覆盖（30s 硬编码使其不可测，故超时逻辑从未被执行过）。
// 现超时可注入，本组测试把它钉死。
// ===========================================================================

// 卡住 reducer，让 entry 堆在 ring 里出不去。
struct ReducerGate {
    std::mutex mu;
    std::condition_variable cv;
    bool open = false;
    void wait() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this] { return open; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lk(mu);
            open = true;
        }
        cv.notify_all();
    }
};

TEST(IndexPoolUnregister, TimeoutReturnsFalseAndDoesNotHang) {
    // 超时注入 100ms：reducer 被卡死 → flush 必然超时。
    IndexPool pool(1, 64, 16, std::chrono::milliseconds(100));
    // gate 用 shared_ptr 按值捕获，理由与姊妹用例
    // TimeoutDoesNotLeakGlobalReorderBudget 完全一致，而这条当初漏了：
    // 栈上的 `ReducerGate gate` 声明在 `pool` **之后** ⇒ 作用域退出时
    // **gate 先析构**，reducer 却要到随后的 ~IndexPool()→stop()→join()
    // 才停。中间那段窗口里 reducer 仍可能停在 gate.wait() 内 —— 对已析构
    // 的 mutex/condition_variable 动手，是确定的 use-after-scope。
    // reduce_fn 由 reducer 在 reorder_mu_ 下拷走的 lane shared_ptr 续命
    // （见 reducer_loop 的「锁下拷活 lane」），故按值捕获足以让 gate 活过
    // reducer 对它的最后一次访问。
    //
    // 为什么 Linux 上一直是绿的：libstdc++ 对「解锁一把并不持有的 mutex」
    // 静默放过，UB 不表现；libc++（FreeBSD 15 / macOS）检查
    // pthread_mutex_unlock 的返回值，非 0 直接 ud2 → SIGILL，100% 复现。
    // 同一段 UB，一边静默一边必炸。
    auto gate = std::make_shared<ReducerGate>();
    auto* lane = pool.register_lib(
        no_preps, [gate](ReorderEntry&) { gate->wait(); }, [] {});
    ASSERT_NE(lane, nullptr);

    for (std::size_t i = 0; i < 4; ++i) {
        pool.submit(lane, IndexTask::make(IndexOp::Add, std::to_string(i), i,
                                          "text", 1, 0, 0, 0, 0));
    }

    const auto t0 = std::chrono::steady_clock::now();
    const bool drained = pool.unregister_lib(lane);
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_FALSE(drained) << "reducer 卡死时 flush 必须超时返回 false";
    // 有界性本身：不得退化回无界等待。
    EXPECT_LT(elapsed, std::chrono::seconds(5)) << "超时未生效——退化为无界等待";

    gate->release();  // 放行 reducer，让 stop() 能 join
}

TEST(IndexPoolUnregister, TimeoutDoesNotLeakGlobalReorderBudget) {
    // 核心回归，直接钉不变量：**所有 lane 注销、在途 apply 收尾后，全局
    // reorder_inflight_ 必须归零**。
    //
    // 为何不用「lane B 被卡死」做端到端断言：map worker 撞到
    // reorder_inflight_ >= cap 就停 push，故单轮最多堆 cap-1 条在 ring
    // （另一条已被 reducer 取走）。单轮泄漏 cap-1 < cap ⇒ 后续 lane 仍剩
    // 1 个名额、能跑完只是变慢 ⇒ 端到端断言**漏报**（实测如此：注掉修复
    // 后那版测试照样绿）。要卡死须累积多轮，脆弱且依赖算术。直接查计数
    // 精确、无时序依赖。
    constexpr std::size_t kCap = 4;
    IndexPool pool(1, 64, kCap, std::chrono::milliseconds(100));

    // gate 用 shared_ptr：lane 被 erase 后 reducer 可能仍在其 reduce_fn 内，
    // 捕获副本保证 gate 生命周期覆盖到 reducer 退出。
    auto gate = std::make_shared<ReducerGate>();
    auto* lane_a = pool.register_lib(
        no_preps, [gate](ReorderEntry&) { gate->wait(); }, [] {});
    ASSERT_NE(lane_a, nullptr);

    for (std::size_t i = 0; i < kCap; ++i) {
        pool.submit(lane_a, IndexTask::make(IndexOp::Add, std::to_string(i), i,
                                            "text", 1, 0, 0, 0, 0));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // 待其入 ring

    // reducer 卡在 gate 里 → flush 超时 → ring 非空即被 erase。
    EXPECT_FALSE(pool.unregister_lib(lane_a));

    // 放行 reducer，让它手里那条（已离开 ring）走完并归还自己的名额。
    gate->release();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (pool.reorder_inflight_for_test() != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 此刻池内已无任何 lane，在途 apply 也已收尾 → 计数必须归零。
    // 不归零 = 那些随 lane 析构而消失的 ring entry 的名额被永久吞掉；
    // 池由 registry 跨库共享，反复慢关闭会把同进程其它库的索引拖死。
    EXPECT_EQ(pool.reorder_inflight_for_test(), 0u)
        << "超时注销泄漏了全局 reorder 名额（未归还 ring 中未 apply 的 entry）";
}
