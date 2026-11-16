// SnapshotCell<T> —— 一写多读的「不可变快照发布槽」。
//
// 用法契约（把库里散落的「读端取快照、写端换指针」范式固化成类型）：
//   - 写者（调用方自行串行化）构造一份**新的**不可变对象，publish() 一次
//     原子替换；绝不原地修改已发布的对象；
//   - 读者 load() 得到 shared_ptr<const T>：拿到的是某个发布点的完整对象，
//     此后写者怎么发布都不影响它（旧对象随最后一个读者释放）；
//   - 读者不得把快照内部的引用/指针留到 shared_ptr 之外。
//
// 与读写锁的区别：读者之间、读者与写者之间都不互斥，读端成本 = 一次
// shared_ptr 拷贝（引用计数自增）。适合「读远多于写、写是整体替换」的状态
// （run 集合、段列表、manifest 派生视图等）。需要「多个状态成对一致」时，
// 把它们放进同一个 T 一起发布；确实做不到再用 SeqGate。
//
// 底层走 AtomicSharedPtr（libstdc++ 下即 std::atomic<shared_ptr>；libc++
// 无该偏特化时为互斥量兜底，见 atomic_shared_ptr.hpp）。
#pragma once

#include <memory>
#include <utility>

#include "bitcask/detail/atomic_shared_ptr.hpp"

namespace bitcask::detail {

template <class T>
class SnapshotCell {
public:
    using Ptr = std::shared_ptr<const T>;

    SnapshotCell() noexcept = default;
    explicit SnapshotCell(Ptr initial) noexcept : p_(std::move(initial)) {}
    SnapshotCell(const SnapshotCell&) = delete;
    SnapshotCell& operator=(const SnapshotCell&) = delete;

    // 读：当前发布的快照；尚未发布过 → 空指针。
    [[nodiscard]] Ptr load() const noexcept {
        return p_.load(std::memory_order_acquire);
    }

    // 写：发布一份新快照（写者之间由调用方串行化）。
    void publish(Ptr next) noexcept {
        p_.store(std::move(next), std::memory_order_release);
    }

    // 便捷：就地构造并发布。
    template <class... Args>
    void emplace(Args&&... args) {
        publish(std::make_shared<const T>(std::forward<Args>(args)...));
    }

private:
    AtomicSharedPtr<const T> p_{nullptr};
};

}  // namespace bitcask::detail
