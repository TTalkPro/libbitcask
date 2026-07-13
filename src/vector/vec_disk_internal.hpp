// 磁盘档向量段共用内部件（S32-M5 抽取；此前为 ivf_rq.cpp 文件局部）。
// IvfSegment 与 DiskannSegment 共用：1-bit RaBitQ-lite 编码/汉明、f32 内积、
// 并行 for、pwrite 循环。**内部头**（src/vector 私有 include），不进公共 API。

#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <unistd.h>

namespace bitcask::vec::diskint {

inline bool pwrite_all(int fd, const void* buf, std::size_t len,
                       std::uint64_t off) {
    const auto* p = static_cast<const std::uint8_t*>(buf);
    while (len > 0) {
        const ssize_t w = ::pwrite(fd, p, len, static_cast<off_t>(off));
        if (w <= 0) return false;
        p   += w;
        off += static_cast<std::uint64_t>(w);
        len -= static_cast<std::size_t>(w);
    }
    return true;
}

// f32 内积（编译器自动向量化；质心/组心扫描用——非热点瓶颈）。
inline float dot_f32(const float* a, const float* b, std::size_t dim) {
    float acc = 0.0f;
    for (std::size_t d = 0; d < dim; ++d) acc += a[d] * b[d];
    return acc;
}

// 并行 for：[0, n) 动态分批到 hardware_concurrency 线程。
template <typename Fn>
void parallel_for(std::size_t n, Fn&& fn) {
    const std::size_t nt = std::min<std::size_t>(
        std::max<std::size_t>(1, std::thread::hardware_concurrency()),
        n == 0 ? 1 : n);
    if (nt <= 1) {
        for (std::size_t i = 0; i < n; ++i) fn(i);
        return;
    }
    std::atomic<std::size_t> next{0};
    constexpr std::size_t kBatch = 256;
    std::vector<std::thread> pool;
    pool.reserve(nt);
    for (std::size_t t = 0; t < nt; ++t) {
        pool.emplace_back([&]() {
            for (;;) {
                const std::size_t b = next.fetch_add(kBatch);
                if (b >= n) return;
                const std::size_t e = std::min(n, b + kBatch);
                for (std::size_t i = b; i < e; ++i) fn(i);
            }
        });
    }
    for (auto& th : pool) th.join();
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
