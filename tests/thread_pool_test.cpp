// T2.7 单元测试：IndexPool + IndexTaskQueue + TbbLifetime

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

TEST(IndexPool, SubmitAndProcess) {
    IndexPool pool(1, 10240);
    std::atomic<std::size_t> count{0};

    pool.start([&](const IndexTask& task) {
        if (task.op != IndexOp::Sentinel) {
            ++count;
        }
        return true;
    });

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
    pool.start([](const IndexTask&) { return true; });
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

    pool.start([&](const IndexTask& task) {
        if (task.op == IndexOp::Sentinel) return false;
        invoked = true;
        return true;
    });

    pool.queue().push(IndexTask{IndexOp::Sentinel});
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

// T4:背压——队列满时 submit 阻塞。worker 卡在首个任务上让队列填满，额外
// 一次 submit 必须阻塞（不立刻返回），释放 worker 后才完成。
TEST(IndexPool, BackpressureBlocksWhenQueueFull) {
    constexpr std::size_t kCap = 4;
    IndexPool pool(1, kCap);
    std::mutex m;
    std::condition_variable cv;
    bool release = false;
    std::atomic<std::size_t> processed{0};
    std::atomic<bool> worker_in_wait{false};

    pool.start([&](const IndexTask& task) {
        if (task.op == IndexOp::Sentinel) return false;
        if (processed.load() == 0) {  // 卡住首个任务 → worker 不再消费
            std::unique_lock<std::mutex> lk(m);
            worker_in_wait = true;
            cv.wait(lk, [&] { return release; });
        }
        ++processed;
        return true;
    });

    // 首任务被 worker 取走并卡住（队列腾空、worker 不再 pop）。
    pool.submit(IndexTask::make(IndexOp::Add, "a0", 0, "t", 0, 0, 0, 0, 0));
    while (!worker_in_wait.load()) std::this_thread::yield();

    // 填满有界队列（kCap 个）。
    for (std::size_t i = 0; i < kCap; ++i) {
        pool.submit(IndexTask::make(IndexOp::Add, "k", i, "t", 0, 0, 0, 0, 0));
    }

    // 再 submit 一个 → 队列满 → 必阻塞。用单独线程提交并观测。
    std::atomic<bool> extra_done{false};
    std::thread t([&] {
        pool.submit(IndexTask::make(IndexOp::Add, "x", 999, "t", 0, 0, 0, 0, 0));
        extra_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(extra_done.load()) << "队列满时 submit 应阻塞（背压）";

    // 释放 worker → 队列排空 → 阻塞的 submit 完成。
    { std::lock_guard<std::mutex> lk(m); release = true; }
    cv.notify_all();
    t.join();
    EXPECT_TRUE(extra_done.load());
    pool.flush();
    pool.stop();
}

// T4:关闭排空契约。close() 的真实序是 flush()→stop()（cask.cpp）：flush()
// 等 pending 归 0（W3 cv），把背压堆积的全部任务消费干净，再 stop() 干净退出。
// （stop() 本身是 abrupt：worker 在循环顶部查 stopped_，忙时设标志会让它处理
//  完当前任务即退出、不排空——索引可由 data 重建，故此设计可接受。）
TEST(IndexPool, FlushDrainsBackpressuredThenStopClean) {
    IndexPool pool(1, 16);
    std::mutex m;
    std::condition_variable cv;
    bool release = false;
    std::atomic<std::size_t> processed{0};
    std::atomic<bool> worker_in_wait{false};

    pool.start([&](const IndexTask& task) {
        if (task.op == IndexOp::Sentinel) return false;
        if (processed.load() == 0) {
            std::unique_lock<std::mutex> lk(m);
            worker_in_wait = true;
            cv.wait(lk, [&] { return release; });
        }
        ++processed;
        return true;
    });

    pool.submit(IndexTask::make(IndexOp::Add, "a0", 0, "t", 0, 0, 0, 0, 0));
    while (!worker_in_wait.load()) std::this_thread::yield();

    // 背压：worker 卡住期间塞满队列（16）。第 17 个会阻塞——用线程提交。
    constexpr std::size_t kN = 16;
    std::thread feeder([&] {
        for (std::size_t i = 1; i <= kN; ++i) {
            pool.submit(IndexTask::make(IndexOp::Add, "k", i, "t", 0, 0, 0, 0, 0));
        }
    });

    // 释放 worker → 队列排空，feeder 的阻塞 submit 也陆续完成。
    { std::lock_guard<std::mutex> lk(m); release = true; }
    cv.notify_all();
    feeder.join();

    pool.flush();  // 真实排空机制：等 pending 归 0
    EXPECT_EQ(processed.load(), kN + 1) << "flush() 必须排空全部已提交任务";
    pool.stop();   // flush 后干净退出，不挂起
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

}  // namespace
