// IndexPool 分片执行器单测（S45-4 改写；原 map/reduce 流水线用例随 reducer 退役）。
// 覆盖：LSN 完成跟踪、乱序/逆序提交、同 key 串行、分片并行、串行道、独占维护、
// 背压、多库共享、注销超时摘钩。

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

#include <algorithm>
#include <future>
#include <set>
#include <stdexcept>
#include <string>

// 统计当前进程 OS 线程数。**Linux 专用**——读 /proc/self/task 的目录项。
// S37-2：改用 std::filesystem 枚举（去掉 <dirent.h>）。
// S37-5（遗留项 W3 结清）：/proc 无 Windows 对应物——原实现在非 Linux 上恒
// 返回 0，而调用方断言的是**差值**（after_first - before == 2），0-0=0 直接
// 失败。这里补 Windows 实现而不是把用例标成 Linux-only：AT5 守的是「线程数
// 与库数解耦」这条结构性保证，它与平台无关，在 Windows 上同样值得守。
#if defined(_WIN32)
[[maybe_unused]] static int count_os_threads() {
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
[[maybe_unused]] static int count_os_threads() {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, ::getpid()};
    struct kinfo_proc kp{};
    std::size_t len = sizeof(kp);
    if (::sysctl(mib, 4, &kp, &len, nullptr, 0) != 0 || len != sizeof(kp)) {
        return 0;
    }
    return static_cast<int>(kp.ki_numthreads);
}
#else
[[maybe_unused]] static int count_os_threads() {
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


namespace {

using namespace std::chrono_literals;
using bitcask::ApplyResult;
using bitcask::IndexLane;
using bitcask::IndexOp;
using bitcask::IndexPool;
using bitcask::IndexTask;
using bitcask::IndexTaskQueue;
using bitcask::LaneCallbacks;
using bitcask::TbbLifetime;

IndexTask add_task(std::string_view key, std::uint64_t ord) {
    return IndexTask::make(IndexOp::Add, key, ord, "t", 0, 0, 0, 0, 0);
}

LaneCallbacks counting_cb(std::atomic<std::size_t>& n) {
    LaneCallbacks cb;
    cb.shard_apply = [&n](IndexTask&, unsigned) {
        ++n;
        return ApplyResult::kDone;
    };
    return cb;
}

// 把 apply 卡在门上（测试背压 / 超时 / 暂停）。
struct Gate {
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

// 找一个落在指定分片的 key。
std::string key_on_shard(const IndexPool& pool, unsigned shard, int salt = 0) {
    for (int i = 0;; ++i) {
        std::string k = "k" + std::to_string(salt) + "_" + std::to_string(i);
        if (pool.shard_of(k) == shard) return k;
    }
}

// ---- IndexTaskQueue ----

TEST(IndexTaskQueue, BoundedCapacity) {
    IndexTaskQueue queue(5);
    for (int i = 0; i < 5; ++i) queue.push(add_task(std::to_string(i), 0));
    EXPECT_EQ(queue.size(), 5u);
}

TEST(IndexTaskQueue, TaskOrderingFIFO) {
    IndexTaskQueue queue(1024);
    constexpr std::size_t kCount = 200;
    for (std::size_t i = 0; i < kCount; ++i) queue.push(add_task("k", i));
    for (std::size_t i = 0; i < kCount; ++i) EXPECT_EQ(queue.pop().ord, i);
}

TEST(IndexTaskQueue, PushBlocksWhenFull) {
    constexpr std::size_t kCap = 4;
    IndexTaskQueue queue(kCap);
    for (std::size_t i = 0; i < kCap; ++i) queue.push(add_task(std::to_string(i), 0));
    std::atomic<bool> pushed{false};
    std::thread t([&] {
        queue.push(add_task("x", 0));
        pushed = true;
    });
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(pushed.load()) << "queue 满时 push 应阻塞（背压）";
    EXPECT_EQ(queue.pop().key(), "0");
    t.join();
    EXPECT_TRUE(pushed.load());
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

// ---- 基本提交 / LSN 完成 ----

TEST(IndexPool, SubmitAppliesAndCompletesLsn) {
    IndexPool pool(4);
    std::atomic<std::size_t> n{0};
    IndexLane* lane = pool.register_lib(counting_cb(n), 100);
    EXPECT_EQ(pool.coverage(lane), 100u) << "init_ord 之前视为已完成";
    for (std::uint64_t o = 100; o < 1100; ++o) pool.submit(lane, add_task(std::to_string(o), o));
    ASSERT_TRUE(pool.wait_lsn(lane, 1100, 10s));
    EXPECT_EQ(n.load(), 1000u);
    EXPECT_EQ(pool.coverage(lane), 1100u);
    EXPECT_TRUE(pool.unregister_lib(lane));
}

TEST(IndexPool, StopIsIdempotentAndDrainsQueuedTasks) {
    IndexPool pool(2);
    std::atomic<std::size_t> n{0};
    IndexLane* lane = pool.register_lib(counting_cb(n), 0);
    for (std::uint64_t o = 0; o < 500; ++o) pool.submit(lane, add_task(std::to_string(o), o));
    pool.stop();
    pool.stop();
    EXPECT_TRUE(pool.is_stopped());
    EXPECT_EQ(n.load(), 500u) << "stop 的哨兵排在真任务之后，须先排干";
}

TEST(IndexPool, SubmitAfterStopCompletesLsnWithoutApplying) {
    IndexPool pool(1);
    std::atomic<std::size_t> n{0};
    IndexLane* lane = pool.register_lib(counting_cb(n), 0);
    pool.stop();
    pool.submit(lane, add_task("k", 0));
    EXPECT_EQ(n.load(), 0u);
    EXPECT_TRUE(pool.wait_lsn(lane, 1, 0ms)) << "丢弃的任务也必须记完成，否则读屏障挂死";
}

// 乱序完成：LSN 逆序提交，前缀照样推进到底（没有任何「等队头」）。
TEST(IndexPool, ReverseOrderSubmissionCompletesPrefix) {
    IndexPool pool(4, 8);  // 小队列：背压与乱序同时存在
    std::atomic<std::size_t> n{0};
    IndexLane* lane = pool.register_lib(counting_cb(n), 0);
    constexpr std::uint64_t kN = 5000;
    std::thread feeder([&] {
        for (std::uint64_t o = kN; o-- > 0;) pool.submit(lane, add_task(std::to_string(o), o));
    });
    feeder.join();
    ASSERT_TRUE(pool.wait_lsn(lane, kN, 10s));
    EXPECT_EQ(n.load(), kN);
    EXPECT_TRUE(pool.unregister_lib(lane));
}

TEST(IndexPool, CompleteFillsHoleWithoutTask) {
    IndexPool pool(2);
    std::atomic<std::size_t> n{0};
    IndexLane* lane = pool.register_lib(counting_cb(n), 0);
    pool.submit(lane, add_task("a", 0));
    pool.submit(lane, add_task("b", 2));
    EXPECT_FALSE(pool.wait_lsn(lane, 3, 100ms)) << "缺 LSN 1 时前缀停在 1";
    pool.complete(lane, 1);
    EXPECT_TRUE(pool.wait_lsn(lane, 3, 5s));
    EXPECT_EQ(n.load(), 2u);
    EXPECT_TRUE(pool.unregister_lib(lane));
}

TEST(IndexPool, ApplyExceptionDoesNotStall) {
    IndexPool pool(2);
    std::atomic<int> errors{0};
    std::atomic<int> ok{0};
    LaneCallbacks cb;
    cb.shard_apply = [&](IndexTask& t, unsigned) {
        if (t.ord % 2) throw std::runtime_error("boom");
        ++ok;
        return ApplyResult::kDone;
    };
    cb.on_error = [&] { ++errors; };
    IndexLane* lane = pool.register_lib(std::move(cb), 0);
    for (std::uint64_t o = 0; o < 100; ++o) pool.submit(lane, add_task(std::to_string(o), o));
    ASSERT_TRUE(pool.wait_lsn(lane, 100, 10s));
    EXPECT_EQ(ok.load(), 50);
    EXPECT_EQ(errors.load(), 50);
    EXPECT_TRUE(pool.unregister_lib(lane));
}

TEST(IndexPool, WaitLsnIsBounded) {
    IndexPool pool(1);
    std::atomic<std::size_t> n{0};
    IndexLane* lane = pool.register_lib(counting_cb(n), 0);
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(pool.wait_lsn(lane, 1, 100ms));
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 3s);
    pool.complete(lane, 0);
    EXPECT_TRUE(pool.unregister_lib(lane));
}

// ---- 分片语义 ----

// 同 key 串行且按提交序；不同 key 可并发。
TEST(IndexPool, SameKeySerializedInSubmissionOrder) {
    IndexPool pool(4);
    constexpr int kKeys = 16;
    constexpr std::uint64_t kVersions = 300;
    std::mutex mu;
    std::vector<std::vector<std::uint64_t>> seen(kKeys);
    std::vector<std::atomic<int>> in_apply(kKeys);
    std::atomic<bool> overlap{false};
    LaneCallbacks cb;
    cb.shard_apply = [&](IndexTask& t, unsigned) {
        const int k = std::stoi(std::string(t.key()));
        if (in_apply[static_cast<std::size_t>(k)].fetch_add(1) != 0) overlap = true;
        {
            std::lock_guard<std::mutex> lk(mu);
            seen[static_cast<std::size_t>(k)].push_back(t.ord);
        }
        in_apply[static_cast<std::size_t>(k)].fetch_sub(1);
        return ApplyResult::kDone;
    };
    IndexLane* lane = pool.register_lib(std::move(cb), 0);
    std::uint64_t ord = 0;
    for (std::uint64_t v = 0; v < kVersions; ++v) {
        for (int k = 0; k < kKeys; ++k) pool.submit(lane, add_task(std::to_string(k), ord++));
    }
    ASSERT_TRUE(pool.wait_lsn(lane, ord, 10s));
    EXPECT_FALSE(overlap.load()) << "同 key 的应用不得并发";
    for (int k = 0; k < kKeys; ++k) {
        const auto& s = seen[static_cast<std::size_t>(k)];
        ASSERT_EQ(s.size(), kVersions);
        EXPECT_TRUE(std::is_sorted(s.begin(), s.end())) << "同 key 须按提交序到达";
    }
    EXPECT_TRUE(pool.unregister_lib(lane));
}

TEST(IndexPool, DifferentShardsApplyInParallel) {
    constexpr unsigned kShards = 4;
    IndexPool pool(kShards);
    LaneCallbacks cb;
    cb.shard_apply = [](IndexTask&, unsigned) {
        std::this_thread::sleep_for(100ms);
        return ApplyResult::kDone;
    };
    IndexLane* lane = pool.register_lib(std::move(cb), 0);
    const auto t0 = std::chrono::steady_clock::now();
    for (unsigned s = 0; s < kShards; ++s) pool.submit(lane, add_task(key_on_shard(pool, s), s));
    ASSERT_TRUE(pool.wait_lsn(lane, kShards, 10s));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(elapsed, 300ms) << "4 个分片各 100ms 应并行（串行需 400ms）";
    EXPECT_TRUE(pool.unregister_lib(lane));
}

TEST(IndexPool, ForwardedTasksRunOnSingleSerialThread) {
    IndexPool pool(4);
    std::mutex mu;
    std::set<std::thread::id> serial_threads;
    std::atomic<int> serial_n{0};
    LaneCallbacks cb;
    cb.shard_apply = [](IndexTask& t, unsigned) {
        return t.ord % 2 ? ApplyResult::kForwardSerial : ApplyResult::kDone;
    };
    cb.serial_apply = [&](IndexTask&) {
        std::lock_guard<std::mutex> lk(mu);
        serial_threads.insert(std::this_thread::get_id());
        ++serial_n;
    };
    IndexLane* lane = pool.register_lib(std::move(cb), 0);
    for (std::uint64_t o = 0; o < 400; ++o) pool.submit(lane, add_task(std::to_string(o), o));
    ASSERT_TRUE(pool.wait_lsn(lane, 400, 10s));
    EXPECT_EQ(serial_n.load(), 200);
    EXPECT_EQ(serial_threads.size(), 1u);
    EXPECT_TRUE(pool.unregister_lib(lane));
}

// ---- 独占维护 ----

TEST(IndexPool, RunExclusivePausesApplies) {
    IndexPool pool(4);
    std::atomic<int> active{0};
    std::atomic<std::uint64_t> applied{0};
    std::atomic<bool> violated{false};
    LaneCallbacks cb;
    cb.shard_apply = [&](IndexTask&, unsigned) {
        ++active;
        std::this_thread::sleep_for(200us);
        ++applied;
        --active;
        return ApplyResult::kDone;
    };
    IndexLane* lane = pool.register_lib(std::move(cb), 0);
    std::atomic<bool> stop{false};
    std::uint64_t submitted = 0;
    std::thread feeder([&] {
        while (!stop.load()) pool.submit(lane, add_task(std::to_string(submitted), submitted)), ++submitted;
    });
    std::this_thread::sleep_for(20ms);
    for (int round = 0; round < 5; ++round) {
        std::promise<void> done;
        auto f = done.get_future();
        pool.run_exclusive(lane, [&] {
            if (active.load() != 0) violated = true;
            const auto before = applied.load();
            std::this_thread::sleep_for(30ms);
            if (applied.load() != before || active.load() != 0) violated = true;
            done.set_value();
        });
        ASSERT_EQ(f.wait_for(10s), std::future_status::ready);
    }
    stop = true;
    feeder.join();
    ASSERT_TRUE(pool.wait_lsn(lane, submitted, 30s));
    EXPECT_FALSE(violated.load()) << "独占闭包执行期间不得有应用在跑或开始";
    EXPECT_TRUE(pool.unregister_lib(lane));
}

TEST(IndexPool, RunExclusiveIsFifo) {
    IndexPool pool(2);
    std::atomic<std::size_t> n{0};
    IndexLane* lane = pool.register_lib(counting_cb(n), 0);
    std::mutex mu;
    std::vector<int> order;
    for (int i = 0; i < 10; ++i) {
        pool.run_exclusive(lane, [&, i] {
            std::lock_guard<std::mutex> lk(mu);
            order.push_back(i);
        });
    }
    ASSERT_TRUE(pool.flush(lane, 10s));
    ASSERT_EQ(order.size(), 10u);
    EXPECT_TRUE(std::is_sorted(order.begin(), order.end()));
    EXPECT_TRUE(pool.unregister_lib(lane));
}

// ---- 背压 ----

TEST(IndexPool, BackpressureBoundsInFlightThenDrains) {
    constexpr std::size_t kCap = 4;
    IndexPool pool(1, kCap);
    auto gate = std::make_shared<Gate>();
    std::atomic<std::size_t> applied{0};
    LaneCallbacks cb;
    cb.shard_apply = [gate, &applied](IndexTask& t, unsigned) {
        if (t.ord == 0) gate->wait();
        ++applied;
        return ApplyResult::kDone;
    };
    IndexLane* lane = pool.register_lib(std::move(cb), 0);
    constexpr std::size_t kTotal = 1000;
    std::atomic<std::size_t> submitted{0};
    std::thread feeder([&] {
        for (std::size_t i = 0; i < kTotal; ++i) {
            pool.submit(lane, add_task(std::to_string(i), i));
            ++submitted;
        }
    });
    std::this_thread::sleep_for(200ms);
    EXPECT_LE(submitted.load(), kCap + 2) << "分片队列有界：在途须封顶";
    gate->release();
    feeder.join();
    ASSERT_TRUE(pool.wait_lsn(lane, kTotal, 10s));
    EXPECT_EQ(applied.load(), kTotal);
    EXPECT_TRUE(pool.unregister_lib(lane));
}

// ---- 多库共享 ----

TEST(IndexPoolMultiLib, ThreadCountIndependentOfLibCount) {
#if BITCASK_TSAN_ENABLED
    GTEST_SKIP() << "TSan 运行时线程计入,OS 线程计数断言失真(S29-T)";
#endif
    constexpr int kAttempts = 5;
    constexpr int kShards = 3;
    int last_delta = -1, last_first = -1, last_many = -1;
    bool ok = false;
    for (int attempt = 0; attempt < kAttempts && !ok; ++attempt) {
        IndexPool pool(kShards);
        std::atomic<std::size_t> n{0};
        const int before = count_os_threads();
        std::vector<IndexLane*> lanes{pool.register_lib(counting_cb(n), 0)};
        const int after_first = count_os_threads();
        for (int i = 0; i < 49; ++i) lanes.push_back(pool.register_lib(counting_cb(n), 0));
        const int after_many = count_os_threads();
        last_delta = after_first - before;
        last_first = after_first;
        last_many = after_many;
        // 首个 register 起 kShards 个分片线程 + 串行道 + 维护线程。
        ok = last_delta == kShards + 2 && after_many == after_first;
        for (auto* l : lanes) (void)pool.unregister_lib(l);
        pool.stop();
    }
    EXPECT_TRUE(ok) << "首个 register 线程增量=" << last_delta << "（应为 "
                    << kShards + 2 << "），50 个 lib 后=" << last_many
                    << "（应等于 " << last_first << "）";
}

TEST(IndexPoolMultiLib, LanesAreIndependent) {
    IndexPool pool(4);
    std::atomic<std::size_t> na{0}, nb{0};
    IndexLane* a = pool.register_lib(counting_cb(na), 0);
    IndexLane* b = pool.register_lib(counting_cb(nb), 1000);
    std::thread ta([&] { for (std::uint64_t o = 0; o < 3000; ++o) pool.submit(a, add_task(std::to_string(o), o)); });
    std::thread tb([&] { for (std::uint64_t o = 1000; o < 2000; ++o) pool.submit(b, add_task(std::to_string(o), o)); });
    ta.join();
    tb.join();
    ASSERT_TRUE(pool.wait_lsn(a, 3000, 10s));
    ASSERT_TRUE(pool.wait_lsn(b, 2000, 10s));
    EXPECT_EQ(na.load(), 3000u);
    EXPECT_EQ(nb.load(), 1000u);
    EXPECT_TRUE(pool.unregister_lib(a));
    EXPECT_TRUE(pool.unregister_lib(b));
}

// 暂停 A 只挡住碰到 A 任务的分片；B 在其它分片上照常推进。
TEST(IndexPoolMultiLib, PausingOneLaneDoesNotStallOthersOnOtherShards) {
    IndexPool pool(2);
    std::atomic<std::size_t> na{0}, nb{0};
    IndexLane* a = pool.register_lib(counting_cb(na), 0);
    IndexLane* b = pool.register_lib(counting_cb(nb), 0);
    auto gate = std::make_shared<Gate>();
    std::promise<void> paused;
    pool.run_exclusive(a, [gate, &paused] {
        paused.set_value();
        gate->wait();
    });
    paused.get_future().wait();
    for (std::uint64_t o = 0; o < 50; ++o) pool.submit(a, add_task(key_on_shard(pool, 0, int(o)), o));
    for (std::uint64_t o = 0; o < 50; ++o) pool.submit(b, add_task(key_on_shard(pool, 1, int(o)), o));
    EXPECT_TRUE(pool.wait_lsn(b, 50, 5s)) << "B 的任务全在分片 1，不受 A 暂停影响";
    EXPECT_EQ(na.load(), 0u) << "A 暂停期间不得应用";
    gate->release();
    ASSERT_TRUE(pool.wait_lsn(a, 50, 5s));
    EXPECT_EQ(na.load(), 50u);
    EXPECT_TRUE(pool.unregister_lib(a));
    EXPECT_TRUE(pool.unregister_lib(b));
}

// ---- 注销 ----

TEST(IndexPoolUnregister, DrainedUnregisterDestroysLane) {
    IndexPool pool(2);
    std::atomic<std::size_t> n{0};
    IndexLane* lane = pool.register_lib(counting_cb(n), 0);
    for (std::uint64_t o = 0; o < 100; ++o) pool.submit(lane, add_task(std::to_string(o), o));
    std::uint64_t cov = 0;
    EXPECT_TRUE(pool.unregister_lib(lane, &cov));
    EXPECT_EQ(cov, 100u);
    EXPECT_EQ(pool.lane_count(), 0u);
    EXPECT_EQ(pool.graveyard_size(), 0u);
}

// 超时：车道摘钩入墓地（不析构 → 在途任务的裸指针不悬垂），此后不再回调宿主。
TEST(IndexPoolUnregister, TimeoutDetachesLaneAndStopsCallbacks) {
    IndexPool pool(1, 64, 64, 100ms);
    auto gate = std::make_shared<Gate>();
    auto calls = std::make_shared<std::atomic<int>>(0);
    LaneCallbacks cb;
    cb.shard_apply = [gate, calls](IndexTask& t, unsigned) {
        ++*calls;
        if (t.ord == 0) gate->wait();
        return ApplyResult::kDone;
    };
    IndexLane* lane = pool.register_lib(std::move(cb), 0);
    for (std::uint64_t o = 0; o < 4; ++o) pool.submit(lane, add_task("k", o));
    while (calls->load() == 0) std::this_thread::yield();
    std::uint64_t cov = 99;
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(pool.unregister_lib(lane, &cov));
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 5s) << "超时必须生效";
    EXPECT_EQ(cov, 0u) << "覆盖水位不得冒充未完成的 LSN";
    EXPECT_EQ(pool.graveyard_size(), 1u);
    gate->release();
    // 墓地车道上排队的 1..3 被消费但不回调宿主。
    ASSERT_TRUE(pool.wait_lsn(lane, 4, 5s));
    EXPECT_EQ(calls->load(), 1);
    pool.submit(lane, add_task("k", 4));  // 摘钩后提交：直接记完成
    EXPECT_TRUE(pool.wait_lsn(lane, 5, 0ms));
    EXPECT_EQ(calls->load(), 1);
}

}  // namespace
