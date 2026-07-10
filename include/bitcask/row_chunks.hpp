// RowChunks — 追加式并发行数组（S27-4 P2）。
//
// 取代 std::deque 作段 doc_store 底座。deque 的元素**引用**虽稳定，但其内部
// 节点指针表（map）在 push_back 扩容时整表重分配——并发读者 operator[] 两级
// 寻址正遍历旧表 → UAF / TSan race（S27-3「vector→deque 免 realloc-UAF」只
// 对了元素一半，漏了指针表；builder 线程化后写入量足以触发 map 扩容，TSan
// 稳定复现）。本容器把两级结构显式化并按并发契约各自处理：
//   - 元素落固定 kChunkSize 的 chunk，chunk 永不搬移、不释放（至析构）；
//   - chunk 指针表（spine）扩容 = 新表拷指针 + release 原子发布，旧表挂
//     graveyard 至析构才释放（在途读者仍持旧表指针 → 指向同一批 chunk，
//     仍有效——与 SeqShardTable RawLimbo 同思路，规模小到不需 epoch）；
//   - 读者 operator[]：acquire 读 spine + 两级寻址，零锁零重试。
//
// 并发契约：**单写者** push_back（B>1 时每 builder 一段，段内仍单写者）；
// 读者只可访问「已发布」下标——发布点在容器之外（SealedSegment::count_pub_
// 的 release/acquire，写序：行 → 发布计数 → 倒排），故本容器不自带原子
// 计数，size() 仅写者线程可靠。可见性论证：spine 槽位/元素的写 happens-
// before count_pub_ release store；读者 acquire 到新计数后，对 spine_pub_ 的
// load 由 coherence 不可能读到早于该 happens-before 链的旧值。
//
// 元素填充经**赋值**（chunk 以 value-init 预构造）：string/POD 移动赋值，
// std::atomic<> 经 operator= store——故支持 atomic 元素（live_ 位）。

#pragma once

#include <atomic>
#include <cstddef>
#include <utility>
#include <vector>

namespace bitcask::search {

template <typename T, std::size_t kChunkBits = 8>
class RowChunks {
    static constexpr std::size_t kChunkSize = std::size_t{1} << kChunkBits;
    static constexpr std::size_t kMask = kChunkSize - 1;

public:
    RowChunks() = default;
    RowChunks(const RowChunks&) = delete;
    RowChunks& operator=(const RowChunks&) = delete;
    ~RowChunks() { destroy(); }

    // 单写者追加：定位（必要时新建 chunk / 扩 spine）→ 赋值填充。
    template <typename U>
    T& push_back(U&& v) {
        T& s = ensure_slot(size_);
        s = std::forward<U>(v);
        ++size_;
        return s;
    }

    // 读者路径（caller 保证 i 已发布，见头注并发契约）。
    [[nodiscard]] const T& operator[](std::size_t i) const {
        T* const* sp = spine_pub_.load(std::memory_order_acquire);
        return sp[i >> kChunkBits][i & kMask];
    }
    // 非 const 访问：atomic 元素的写路径（mark_dead 等）——寻址同读者，
    // 元素级并发由元素自身（atomic）承担。
    [[nodiscard]] T& operator[](std::size_t i) {
        T* const* sp = spine_pub_.load(std::memory_order_acquire);
        return sp[i >> kChunkBits][i & kMask];
    }

    [[nodiscard]] std::size_t size() const { return size_; }  // 写者视角

    // 单线程重置（decode/载入路径,无并发读者）。
    void clear() {
        destroy();
        chunks_.clear();
        graveyard_.clear();
        spine_pub_.store(nullptr, std::memory_order_relaxed);
        spine_cap_ = 0;
        size_ = 0;
    }

private:
    T& ensure_slot(std::size_t i) {
        const std::size_t ci = i >> kChunkBits;
        if (ci == chunks_.size()) {
            T* chunk = new T[kChunkSize]();  // value-init（atomic 零初始化）
            chunks_.push_back(chunk);
            T** spine = spine_pub_.load(std::memory_order_relaxed);
            if (chunks_.size() > spine_cap_) {
                // spine 扩容：新表拷指针 → release 发布；旧表进 graveyard
                //（在途读者可能仍持有,至析构才释放）。
                const std::size_t cap = spine_cap_ ? spine_cap_ * 2 : 8;
                T** ns = new T*[cap];
                for (std::size_t k = 0; k < chunks_.size(); ++k) {
                    ns[k] = chunks_[k];
                }
                if (spine) graveyard_.push_back(spine);
                spine_pub_.store(ns, std::memory_order_release);
                spine_cap_ = cap;
            } else {
                // 槽位纯写（读者尚不可达此下标——计数未发布）。
                spine[ci] = chunk;
            }
        }
        return chunks_[ci][i & kMask];
    }

    void destroy() {
        for (T* c : chunks_) delete[] c;
        for (T** s : graveyard_) delete[] s;
        if (T** s = spine_pub_.load(std::memory_order_relaxed)) delete[] s;
    }

    std::vector<T*>      chunks_;      // 写者权威 chunk 列表
    std::vector<T**>     graveyard_;   // 退役 spine（析构统一释放）
    std::atomic<T**>     spine_pub_{nullptr};
    std::size_t          spine_cap_ = 0;
    std::size_t          size_ = 0;
};

}  // namespace bitcask::search
