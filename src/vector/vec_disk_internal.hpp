// 磁盘档向量段共用内部件（S32-M5 抽取；此前为 ivf_rq.cpp 文件局部）。
// IvfSegment 与 DiskannSegment 共用：1-bit RaBitQ-lite 编码/汉明、f32 内积、
// 并行 for、pwrite 循环。**内部头**（src/vector 私有 include），不进公共 API。

#pragma once

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bitcask/io.hpp"  // S37-1：宿主原语一律经 io seam（原 <unistd.h>）

namespace bitcask::vec::diskint {

// S37-1：pwrite_all / pread_all 的实现（含 2026-07-13 的 EINTR 重试审计修复）
// 已下沉到 io seam。此处保留同名转发，使 ivf_rq / diskann / hnsw 的数十处
// 调用站点零改动。
using io::pread_all;
using io::pwrite_all;

// build 用 tmp 文件 RAII:异常/早退时 close(fd) + 删 tmp（审计修复——
// 此前 build 中途 bad_alloc 会泄 fd 并残留 tmp）。成功路径 commit() 后
// 调用方自行 rename。
struct TmpFile {
    io::FileHandle fd = io::kInvalidHandle;
    std::string path;
    bool committed = false;
    ~TmpFile() {
        if (io::handle_valid(fd)) io::close_handle(fd);
        if (!committed && !path.empty()) std::remove(path.c_str());
    }
    // fdatasync + close;成功返回 true 并转入 committed（rename 由调用方做,
    // rename 失败调用方应手动 remove——见各 build 尾部）。
    [[nodiscard]] bool sync_close() {
        if (!io::handle_valid(fd)) return false;
        const bool ok = io::sync_data(fd);
        io::close_handle(fd);
        fd = io::kInvalidHandle;
        return ok;
    }
};

// f32 内积（编译器自动向量化；质心/组心扫描用——非热点瓶颈）。
inline float dot_f32(const float* a, const float* b, std::size_t dim) {
    float acc = 0.0f;
    for (std::size_t d = 0; d < dim; ++d) acc += a[d] * b[d];
    return acc;
}

// 并行 for（worker 版）：[0, n) 动态分批;fn(i, wid) 的 wid ∈ [0, nt)
// 为稳定工位号（每线程恒定——替代此前 diskann 的 thread_local tls_tid
// 跨调用残留方案,审计脆弱性修复）。
// 异常安全（审计修复）:worker 内异常被捕获为 exception_ptr,全体 join 后
// 在调用方线程重抛（此前线程内逃逸异常 → std::terminate）;首个异常胜出,
// 其余 worker 经 failed 旗尽快收敛。
template <typename Fn>
void parallel_for_worker(std::size_t n, Fn&& fn) {
    const std::size_t nt = std::min<std::size_t>(
        std::max<std::size_t>(1, std::thread::hardware_concurrency()),
        n == 0 ? 1 : n);
    if (nt <= 1) {
        for (std::size_t i = 0; i < n; ++i) fn(i, std::size_t{0});
        return;
    }
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::exception_ptr eptr;
    std::mutex eptr_mu;
    constexpr std::size_t kBatch = 256;
    std::vector<std::thread> pool;
    pool.reserve(nt);
    for (std::size_t t = 0; t < nt; ++t) {
        pool.emplace_back([&, t]() {
            try {
                for (;;) {
                    if (failed.load(std::memory_order_relaxed)) return;
                    const std::size_t b = next.fetch_add(kBatch);
                    if (b >= n) return;
                    const std::size_t e = std::min(n, b + kBatch);
                    for (std::size_t i = b; i < e; ++i) fn(i, t);
                }
            } catch (...) {
                const std::lock_guard<std::mutex> lk(eptr_mu);
                if (!failed.exchange(true)) eptr = std::current_exception();
            }
        });
    }
    for (auto& th : pool) th.join();
    if (eptr) std::rethrow_exception(eptr);
}

// 兼容薄壳:fn(i)。
template <typename Fn>
void parallel_for(std::size_t n, Fn&& fn) {
    parallel_for_worker(n, [&fn](std::size_t i, std::size_t) { fn(i); });
}

// 1-bit RaBitQ-lite sign 编码（bit j = v[j] >= 0）+ μ = mean|v|。
// 尾部补零位（两侧同 pad → XOR 后为 0，不扰 hamming）。
inline void sign_encode(const float* v, std::size_t dim, std::uint8_t* out,
                        float* mu) {
    const std::size_t nbytes = (dim + 7) / 8;
    std::memset(out, 0, nbytes);
    double asum = 0.0;
    for (std::size_t j = 0; j < dim; ++j) {
        asum += std::fabs(static_cast<double>(v[j]));
        if (v[j] >= 0.0f) {
            out[j >> 3] |= static_cast<std::uint8_t>(1u << (j & 7));
        }
    }
    *mu = static_cast<float>(asum / static_cast<double>(dim));
}

inline std::uint32_t hamming_bytes(const std::uint8_t* a,
                                   const std::uint8_t* b,
                                   std::size_t nbytes) {
    std::uint32_t h = 0;
    std::size_t i = 0;
    for (; i + 8 <= nbytes; i += 8) {
        std::uint64_t x, y;
        std::memcpy(&x, a + i, 8);
        std::memcpy(&y, b + i, 8);
        h += static_cast<std::uint32_t>(__builtin_popcountll(x ^ y));
    }
    for (; i < nbytes; ++i) {
        h += static_cast<std::uint32_t>(
            __builtin_popcount(static_cast<unsigned>(a[i] ^ b[i])));
    }
    return h;
}

}  // namespace bitcask::vec::diskint
