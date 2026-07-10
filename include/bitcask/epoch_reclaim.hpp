// bitcask/epoch_reclaim.hpp — epoch-based reclamation 读者注册表（S29-6 P2）。
//
// 目标:让「逻辑只读」路径做到**零共享写**——读者进入/退出临界区只写自线程
// 的 cacheline 对齐槽位(纯 store,无 RMW,无锁字 ping-pong),写者把待释放
// 内存打上 epoch 戳挂入 limbo,待所有可能持有引用的读者退出后才物理 free。
// 设计与致命场景分析见 docs/design/s29-6-keydir-lockfree-read.md §2.2。
//
// === 协议 ===
// 读者(P3 乐观快路径):
//   slot->active.store(current(), seq_cst)   ← 必须先于任何乐观 deref
//   ... 乐观读(桶探测/entry 拷贝,不追指针,seq 校验) ...
//   slot->active.store(0, release)
// 写者(持结构自身的互斥锁):
//   unlink(结构内摘除) → retire(stamp = advance()) → 攒批后
//   reclaim(stamp < min_active() 的项物理 free)
//
// === 正确性论证(seq_cst 交错) ===
// 读者 slot store 与写者 min_active 的 seq_cst 扫描构成全序:
//   - 若 store 先于扫描:写者看见读者 → stamp ≥ active → 不回收,安全;
//   - 若扫描先于 store:读者的后续 map 读 happens-after 写者的 unlink
//     (unlink 在 retire 之前,retire 在扫描之前)→ 读者只能经**新**结构
//     到达数据,摸不到已 retire 的块,回收安全。
//
// === 使用面 ===
// 进程级单例(多 KeyDir/多索引共享一张注册表;任一活跃读者保守地阻塞全部
// limbo 回收——读临界区是 ns 级,可接受)。槽位固定 kSlots 个,线程首次
// 进入时 CAS 认领、线程退出自动归还;槽耗尽的线程拿到 nullptr → caller
// 必须回退加锁路径(恒正确)。
//
// 线程安全:全部原子操作;Registry 无锁。

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace bitcask::epoch {

class Registry {
public:
    static constexpr std::size_t kSlots = 64;

    struct alignas(64) Slot {
        // 0 = quiescent;非 0 = 读者进入时的 epoch。仅属主线程写。
        std::atomic<std::uint64_t> active{0};
        // 槽位认领标记(线程生命周期粒度,冷路径 CAS)。
        std::atomic<bool> in_use{false};
    };

    static Registry& instance() noexcept {
        static Registry r;
        return r;
    }

    [[nodiscard]] std::uint64_t current() const noexcept {
        return epoch_.load(std::memory_order_acquire);
    }

    // retire 时推进并取新戳(写者调,频率 = 结构性释放次数,低)。
    [[nodiscard]] std::uint64_t advance() noexcept {
        return epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    // 活跃读者最小 epoch;无活跃读者 → uint64 max(一切可回收)。
    // seq_cst load 与读者侧 seq_cst store 配对(见文件头论证)。
    [[nodiscard]] std::uint64_t min_active() const noexcept {
        std::uint64_t m = std::numeric_limits<std::uint64_t>::max();
        for (const auto& s : slots_) {
            const auto v = s.active.load(std::memory_order_seq_cst);
            if (v != 0 && v < m) m = v;
        }
        return m;
    }

    // 认领一个槽(线程首次进入;冷路径)。耗尽返回 nullptr → 回退加锁。
    [[nodiscard]] Slot* acquire_slot() noexcept {
        for (auto& s : slots_) {
            bool expected = false;
            if (!s.in_use.load(std::memory_order_relaxed) &&
                s.in_use.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel)) {
                s.active.store(0, std::memory_order_relaxed);
                return &s;
            }
        }
        return nullptr;
    }

    void release_slot(Slot* s) noexcept {
        if (s == nullptr) return;
        s->active.store(0, std::memory_order_release);
        s->in_use.store(false, std::memory_order_release);
    }

private:
    Registry() = default;
    std::atomic<std::uint64_t> epoch_{1};
    std::array<Slot, kSlots> slots_{};
};

// 每线程槽位(线程退出经 thread_local 析构归还,槽可复用)。
// 返回 nullptr = 槽耗尽,caller 走加锁路径。
[[nodiscard]] inline Registry::Slot* thread_slot() noexcept {
    struct Holder {
        Registry::Slot* s = Registry::instance().acquire_slot();
        ~Holder() { Registry::instance().release_slot(s); }
    };
    thread_local Holder h;
    return h.s;
}

}  // namespace bitcask::epoch
