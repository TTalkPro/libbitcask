// 一写多读基础件：SnapshotCell（不可变快照发布槽）与 SeqGate（成对一致闸）。

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <bitcask/detail/seq_gate.hpp>
#include <bitcask/detail/snapshot_cell.hpp>

namespace {

using bitcask::detail::SeqGate;
using bitcask::detail::SnapshotCell;

TEST(SnapshotCell, EmptyUntilPublished) {
    SnapshotCell<int> cell;
    EXPECT_EQ(cell.load(), nullptr);
    cell.emplace(7);
    ASSERT_NE(cell.load(), nullptr);
    EXPECT_EQ(*cell.load(), 7);
}

TEST(SnapshotCell, ReaderKeepsOldSnapshotAlive) {
    SnapshotCell<std::vector<int>> cell;
    cell.emplace(std::vector<int>{1, 2, 3});
    const auto old = cell.load();
    cell.emplace(std::vector<int>{9});
    EXPECT_EQ(old->size(), 3u) << "已取走的快照不受后续发布影响";
    EXPECT_EQ(cell.load()->size(), 1u);
}

// 写者整体替换；读者看到的每份快照内部必须自洽（两个字段恒相等），且版本
// 单调不减。
TEST(SnapshotCell, ConcurrentReadersSeeWholeSnapshots) {
    struct Pair {
        std::uint64_t a;
        std::uint64_t b;
    };
    SnapshotCell<Pair> cell;
    cell.emplace(Pair{0, 0});
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> torn{0}, regress{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            std::uint64_t last = 0;
            while (!stop.load()) {
                const auto p = cell.load();
                if (p->a != p->b) ++torn;
                if (p->a < last) ++regress;
                last = p->a;
            }
        });
    }
    for (std::uint64_t i = 1; i <= 20000; ++i) cell.emplace(Pair{i, i});
    stop.store(true);
    for (auto& r : readers) r.join();
    EXPECT_EQ(torn.load(), 0u);
    EXPECT_EQ(regress.load(), 0u);
}

TEST(SeqGate, ReadReturnsCallbackResult) {
    SeqGate gate;
    EXPECT_EQ(gate.read([] { return 42; }), 42);
    EXPECT_EQ(gate.completed_writes(), 0u);
    { const auto w = gate.write(); }
    EXPECT_EQ(gate.completed_writes(), 1u);
}

// 两份状态各自线程安全（各有互斥量），写者在窗口内分两步更新。经闸读取
// 的 (a, b) 必须成对；不经闸直接读则会读到不成对的组合（对照组，证明测试
// 本身有分辨力）。
TEST(SeqGate, PairedReadsNeverTorn) {
    SeqGate gate;
    std::mutex ma, mb, writer_mu;
    std::uint64_t a = 0, b = 0;
    const auto read_a = [&] { std::lock_guard<std::mutex> g(ma); return a; };
    const auto read_b = [&] { std::lock_guard<std::mutex> g(mb); return b; };
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> torn_gated{0}, torn_raw{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                const auto p = gate.read(
                    [&] { return std::pair(read_a(), read_b()); });
                if (p.first != p.second) ++torn_gated;
                const auto ra = read_a();
                std::this_thread::yield();
                const auto rb = read_b();
                if (ra != rb) ++torn_raw;
            }
        });
    }
    // 至少 5 万次写；对照组是概率性的（读者的两次裸读要恰好夹住一次写），
    // 调度不利时（机器满载）固定次数内可能一次都没撞上——那是对照组没采到
    // 样本，不是闸失效。故写满 5 万次后若对照组仍为 0，继续写直到采到为止，
    // 20 s 封顶（届时仍为 0 才判「测试没有分辨力」）。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (std::uint64_t i = 1;
         i <= 50000 ||
         (torn_raw.load() == 0 && std::chrono::steady_clock::now() < deadline);
         ++i) {
        std::lock_guard<std::mutex> wl(writer_mu);  // 写者串行（契约）
        const auto w = gate.write();
        { std::lock_guard<std::mutex> g(ma); a = i; }
        { std::lock_guard<std::mutex> g(mb); b = i; }
    }
    stop.store(true);
    for (auto& r : readers) r.join();
    EXPECT_EQ(torn_gated.load(), 0u);
    EXPECT_GT(torn_raw.load(), 0u)
        << "对照组未观察到不成对读取——测试没有分辨力";
}

// 写窗口里抛异常：守卫析构仍离开窗口，读者不会永远自旋。
TEST(SeqGate, WindowReleasedOnException) {
    SeqGate gate;
    try {
        const auto w = gate.write();
        throw std::runtime_error("boom");
    } catch (const std::runtime_error&) {
    }
    EXPECT_EQ(gate.completed_writes(), 1u);
    EXPECT_EQ(gate.read([] { return 1; }), 1);  // 不挂死
}

// 回调跨过一个完整写窗口时必须重取（序号比较，而非只看奇偶）。
TEST(SeqGate, RetriesWhenWindowCompletesDuringRead) {
    SeqGate gate;
    int calls = 0;
    const int v = gate.read([&] {
        if (++calls == 1) { const auto w = gate.write(); }  // 读取中途发生一次完整写
        return calls;
    });
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(v, 2);
}

}  // namespace
