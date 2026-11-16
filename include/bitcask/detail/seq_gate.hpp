// SeqGate —— 一写多读的「成对一致」闸（seqlock 的校验部分）。
//
// 解决的问题：读者要取**多份**各自线程安全、但由写者分步更新的状态（例如
// OKI 的 delta 快照 + run 集合快照）。每份单独读都没问题，可两次读之间若
// 插进一次「同时改动两者」的提交，读者手里就是不成对的一组。
//
//   写者：auto g = gate.write();   // 进入窗口（序号变奇）
//         …分步更新 A、B…
//         // g 析构：离开窗口（序号变偶）。异常/提前 return 同样离开——
//         // 手写 fetch_add 两次的写法一旦中途抛异常，序号永远停在奇数，
//         // 全部读者死循环。
//   读者：auto v = gate.read([&] { return std::pair(read_a(), read_b()); });
//         // 取数期间若撞上写窗口（或跨过了一个完整窗口），自动重取。
//
// 契约：
//   - 写者之间必须由调用方串行化（同一把互斥量下）；窗口不可嵌套；
//   - read 的回调可能被调用多次：必须无副作用（或幂等），且每一份状态
//     自身的读取必须是线程安全的——本闸只校验「成对」，不让裸数据竞争
//     变安全（这点不同于内核 seqlock 对平凡可拷贝数据的用法）；
//   - 只替换其中一份、且新旧内容等价的更新（例如 run 归并）不需要进窗口；
//   - 写窗口应当短（纯内存操作）：读者在窗口期间让出 CPU 重试，不阻塞。
#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <utility>

namespace bitcask::detail {

class SeqGate {
public:
    class [[nodiscard]] WriteGuard {
    public:
        explicit WriteGuard(SeqGate& g) noexcept : g_(g) {
            const auto prev = g_.seq_.fetch_add(1, std::memory_order_acq_rel);
            assert((prev & 1u) == 0 && "SeqGate: nested/concurrent writers");
            (void)prev;
        }
        ~WriteGuard() { g_.seq_.fetch_add(1, std::memory_order_acq_rel); }
        WriteGuard(const WriteGuard&) = delete;
        WriteGuard& operator=(const WriteGuard&) = delete;

    private:
        SeqGate& g_;
    };

    SeqGate() noexcept = default;
    SeqGate(const SeqGate&) = delete;
    SeqGate& operator=(const SeqGate&) = delete;

    // 进入写窗口；返回的守卫析构时离开。
    WriteGuard write() noexcept { return WriteGuard(*this); }

    // 取一组成对一致的状态。fn 无参，返回值即结果。
    template <class F>
    auto read(F&& fn) const -> std::invoke_result_t<F&> {
        for (;;) {
            const auto before = seq_.load(std::memory_order_acquire);
            if ((before & 1u) != 0) {  // 写窗口进行中
                std::this_thread::yield();
                continue;
            }
            auto result = fn();
            if (seq_.load(std::memory_order_acquire) == before) return result;
        }
    }

    // 诊断：已完成的写窗口数。
    [[nodiscard]] std::uint64_t completed_writes() const noexcept {
        return seq_.load(std::memory_order_acquire) / 2;
    }

private:
    std::atomic<std::uint64_t> seq_{0};
};

}  // namespace bitcask::detail
