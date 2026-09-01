// atomic_shared_ptr — `std::atomic<std::shared_ptr<T>>` 的可移植归宿。
//
// 背景：C++20 的 P0718R2 给 std::atomic 加了 shared_ptr 偏特化，本库的
// 「读端 acquire 取快照、写端 release 换指针」范式（vector_plugin 的
// hnsw_、text_plugin 的 building 槽、oki_state 的 runs_snap_、
// sealed_segment_vector_plugin 的 sealed_/window_）全部建立在它之上。
//
// 问题：**libstdc++ 有，libc++ 至今没有**。FreeBSD 15 / macOS 的默认标准库
// 是 libc++（实测 _LIBCPP_VERSION=190107 仍未定义 __cpp_lib_atomic_shared_ptr），
// 于是 `std::atomic<std::shared_ptr<X>>` 落到主模板上，撞 std::atomic 的
//
//     static_assert(is_trivially_copyable<T>::value, ...)
//
// —— 报错点在 <atomic> 内部、错误消息只字不提「你的平台缺这个特性」，
// 是最难从报错反推原因的那一类。
//
// 解法：全库统一走本头的 AtomicSharedPtr<T>。
//   ① 标准库有偏特化（libstdc++）→ 直接 alias 到 std::atomic，逐字零开销、
//      行为与改动前完全一致；
//   ② 没有（libc++）→ 用互斥量兜底的等价实现。
//
// 为什么兜底选互斥量而不是 std::atomic_load/store(shared_ptr*) 那套自由
// 函数：它们在 C++20 已 deprecated、C++26 移除，等于把一个已知会到期的
// 依赖写进移植层；而且 libc++ 对它们的实现本身就是全局锁分片，性能并不比
// 这里的每对象一把锁更好。本库这几个站点的写端都是低频换指针（rebuild /
// 封口 / flush），读端虽在热路径上但只做一次 shared_ptr 拷贝——加锁区间
// 是一次引用计数自增，与无锁版的原子操作同量级。
//
// 只实现库内实际用到的 load/store 两个操作（外加构造与赋值）。exchange /
// compare_exchange / wait / notify 一律不提供：缺了会在编译期报错，好过
// 提供一个语义上似是而非的版本。

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <version>

namespace bitcask::detail {

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L

template <class T>
using AtomicSharedPtr = std::atomic<std::shared_ptr<T>>;

#else

template <class T>
class AtomicSharedPtr {
public:
    AtomicSharedPtr() noexcept = default;
    // 非 explicit：与 std::atomic<shared_ptr<T>> 一致，`= p` / `{nullptr}`
    // 两种写法在站点上照旧成立。
    AtomicSharedPtr(std::shared_ptr<T> desired) noexcept
        : p_(std::move(desired)) {}

    // std::atomic 不可拷贝/移动，兜底版照抄——否则「本地能编、FreeBSD 上
    // 悄悄多出一条拷贝路径」。
    AtomicSharedPtr(const AtomicSharedPtr&)            = delete;
    AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;

    std::shared_ptr<T> load(
        std::memory_order = std::memory_order_seq_cst) const noexcept {
        const std::lock_guard<std::mutex> g(mu_);
        return p_;
    }

    void store(std::shared_ptr<T> desired,
               std::memory_order = std::memory_order_seq_cst) noexcept {
        // 旧值搬到锁外析构：换指针的那一刻往往是最后一个引用消失的时刻，
        // 而这里的 T 是整张 HNSW 图 / 整个倒排段，析构可能是毫秒级的。
        // 在锁内析构会让并发读端的 load 一起陪跑。
        std::shared_ptr<T> old;
        {
            const std::lock_guard<std::mutex> g(mu_);
            old.swap(p_);
            p_ = std::move(desired);
        }
    }

    AtomicSharedPtr& operator=(std::shared_ptr<T> desired) noexcept {
        store(std::move(desired));
        return *this;
    }

    operator std::shared_ptr<T>() const noexcept { return load(); }

private:
    mutable std::mutex mu_;
    std::shared_ptr<T> p_;
};

#endif

}  // namespace bitcask::detail
