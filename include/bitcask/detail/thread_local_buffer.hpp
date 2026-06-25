// ThreadLocalBuffer — thread_local 字节复用缓冲的统一管理（S9-P1-d）。
//
// 多个 reader 可并发调同一 DataFile::read / HintFile::fold；pread 线程安全，
// 但读缓冲不能共享 → 各路径用 `static thread_local` 缓冲做稳态零分配复用。
// 该模式在 data_file.cpp（get 热路径）与 hint_file.cpp（chunked fold）重复出现，
// 共有三段逻辑：
//   ① ensure(n)      —— 按需扩容（只增不缩），返回数据指针；
//   ② data()/size()  —— 直接访问底层缓冲（fold 的 cursor/memmove 需要）；
//   ③ maybe_shrink() —— 用完后若超过 retain 阈值则释放，防一次超大读长期
//                       占住线程内存。
// 本类把这三段收敛成单一抽象，消除重复且语义与原地内联逐字等价。

#pragma once

#include <cstddef>
#include <vector>

namespace bitcask::detail {

class ThreadLocalBuffer {
public:
    // retain_bytes：用完后允许保留的最大字节数；超过则在 maybe_shrink() 释放。
    explicit ThreadLocalBuffer(std::size_t retain_bytes = 1u << 20) noexcept
        : retain_(retain_bytes) {}

    // 保证至少 n 字节容量（只增不缩，等价 `if (size()<n) resize(n)`）。
    // 返回底层数据指针（扩容后有效）。
    std::byte* ensure(std::size_t n) {
        if (buf_.size() < n) buf_.resize(n);
        return buf_.data();
    }

    [[nodiscard]] std::byte* data() noexcept { return buf_.data(); }
    [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }

    // 用完后调用：缓冲超过 retain 阈值则彻底释放（clear + shrink_to_fit），
    // 防单次超大 record 把线程内存长期撑住。阈值内不动（保留容量供复用）。
    void maybe_shrink() {
        if (buf_.size() > retain_) {
            buf_.clear();
            buf_.shrink_to_fit();
        }
    }

private:
    std::vector<std::byte> buf_;
    std::size_t retain_;
};

}  // namespace bitcask::detail
