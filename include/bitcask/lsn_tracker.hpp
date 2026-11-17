// bitcask/lsn_tracker.hpp — LSN 完成跟踪器（S45-3，设计
// designs/index-pipeline-unordered-design-zh.md §4.3）。
//
// 索引管线去全局顺序化后，「已应用到哪」不再是一个由单 reducer 推进的
// 连续计数——任务乱序完成。本类维护「已完成 LSN 集合」的**连续前缀**：
//
//   contiguous() = 最大的 C，使所有 LSN < C 都已 done()。
//
// 用途：读屏障（等 contiguous() ≥ 入口时刻的下一个待分配 LSN）、checkpoint
// 覆盖水位（暂停期 contiguous()）、序号空洞（写失败的 LSN 直接 done()，
// 不再提交 Skip 任务排队）。
//
// 实现：固定窗口滑动位图（kWindow 位，LSN 按取模定位；窗口 = [base,
// base+kWindow)）+ 溢出集合（窗口之外的超前 LSN，正常负载下为空）。
//
// 契约：
//   - 每个 LSN ≥ 初始 base 必须恰好被 done() 一次（重复 done 幂等无害；
//     LSN < base 视为已完成，直接忽略）。任一 LSN 永不 done ⇒ contiguous
//     停住（读屏障挂住 / ckpt 水位不前进）——与旧 reducer 等不到队头等价。
//   - done() 不阻塞、不抛异常（溢出集合分配失败时吞掉，后果同上：前缀
//     停住，由各有界等待路径兜底）；可在析构函数里调用。
//   - 线程安全：任意线程并发 done()/contiguous()/wait_until()。

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace bitcask {

class LsnTracker {
public:
    static constexpr std::uint64_t kWindow = std::uint64_t{1} << 16;

    explicit LsnTracker(std::uint64_t base = 0)
        : words_(kWindow / 64, 0), base_(base), contiguous_(base) {}

    LsnTracker(const LsnTracker&) = delete;
    LsnTracker& operator=(const LsnTracker&) = delete;

    // 重置到新起点（仅在无并发 done 时调用，如 lane 注册）。
    void reset(std::uint64_t base) {
        std::lock_guard<std::mutex> lk(mu_);
        std::fill(words_.begin(), words_.end(), 0);
        overflow_.clear();
        base_ = base;
        contiguous_.store(base, std::memory_order_release);
    }

    void done(std::uint64_t lsn) noexcept {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (lsn < base_) return;
            if (lsn - base_ >= kWindow) {
                try {
                    overflow_.insert(lsn);
                } catch (...) {
                    // 见类注释：前缀停住，由有界等待兜底。
                }
                return;
            }
            set_bit(lsn);
            if (lsn != base_) return;
            while (test_bit(base_)) {
                clear_bit(base_);
                ++base_;
                // 窗口右移：把落入新窗口的溢出 LSN 收回位图。
                while (!overflow_.empty() &&
                       *overflow_.begin() < base_ + kWindow) {
                    const std::uint64_t o = *overflow_.begin();
                    overflow_.erase(overflow_.begin());
                    if (o >= base_) set_bit(o);
                }
            }
            contiguous_.store(base_, std::memory_order_release);
            // 持锁 notify：等待者被唤醒后可能立即析构本对象（车道注销），
            // 锁外 notify 会碰到已释放的 cv。
            if (waiters_ > 0) cv_.notify_all();
        }
    }

    [[nodiscard]] std::uint64_t contiguous() const noexcept {
        return contiguous_.load(std::memory_order_acquire);
    }

    // 等 contiguous() ≥ target。timeout 为空 = 无界。返回是否达成。
    bool wait_until(std::uint64_t target,
                    std::optional<std::chrono::milliseconds> timeout =
                        std::nullopt) {
        if (contiguous() >= target) return true;
        std::unique_lock<std::mutex> lk(mu_);
        ++waiters_;
        auto pred = [&] { return base_ >= target; };
        bool ok = true;
        if (timeout) {
            ok = cv_.wait_for(lk, *timeout, pred);
        } else {
            cv_.wait(lk, pred);
        }
        --waiters_;
        return ok;
    }

    // 诊断：溢出集合大小（> 0 说明有 LSN 落后窗口之外仍未完成）。
    [[nodiscard]] std::size_t overflow_size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return overflow_.size();
    }

private:
    void set_bit(std::uint64_t lsn) noexcept {
        const std::uint64_t i = lsn & (kWindow - 1);
        words_[i >> 6] |= std::uint64_t{1} << (i & 63);
    }
    void clear_bit(std::uint64_t lsn) noexcept {
        const std::uint64_t i = lsn & (kWindow - 1);
        words_[i >> 6] &= ~(std::uint64_t{1} << (i & 63));
    }
    [[nodiscard]] bool test_bit(std::uint64_t lsn) const noexcept {
        const std::uint64_t i = lsn & (kWindow - 1);
        return (words_[i >> 6] >> (i & 63)) & 1u;
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::uint64_t> words_;   // 受 mu_ 保护
    std::set<std::uint64_t> overflow_;   // 受 mu_ 保护
    std::uint64_t base_;                 // 受 mu_ 保护（= contiguous_）
    std::size_t waiters_ = 0;            // 受 mu_ 保护
    std::atomic<std::uint64_t> contiguous_;
};

}  // namespace bitcask
