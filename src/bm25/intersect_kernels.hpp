// 求交内核声明 + 标量收尾（S37-3.b 自 intersect.cpp 拆出）。
//
// 拆分前 AVX2 / AVX-512 内核带 __attribute__((target(...))) 与标量实现同
// 处一个 TU；MSVC 不支持该属性，改为按 ISA 分 TU（intersect_kernels_avx2.cpp
// / intersect_kernels_avx512.cpp，由 CMake 的 bitcask_simd_tu 施加开关）。
//
// intersect_scalar 之所以放在本头而非留在 intersect.cpp：两个 SIMD 内核都
// 用它做**块对齐后的尾部收尾**。它是 inline，会在每个 TU 各编一份——在
// 带 -mavx2 的 TU 里编译器可能把它自动向量化，这是安全的：那份副本只由
// 已过运行时门的 AVX2 内核调用。普通 TU 里的副本仍是纯标量。
//
// 调用方必须先过 simd::have_avx2() / have_avx512() 门。

#pragma once

#include <cstddef>
#include <cstdint>

#include "bitcask/detail/cpu_features.hpp"

namespace bitcask::bm25::kernels {

// 经典双游标求交。SIMD 内核块对齐后的尾部也走它。
inline std::uint64_t* intersect_scalar(const std::uint64_t* a, std::size_t na,
                                       const std::uint64_t* b, std::size_t nb,
                                       std::uint64_t* cur) {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < na && j < nb) {
        if (a[i] < b[j]) {
            ++i;
        } else if (b[j] < a[i]) {
            ++j;
        } else {
            *cur++ = a[i];
            ++i;
            ++j;
        }
    }
    return cur;
}

#if BITCASK_X86_64
// Inoue 块过滤 + SIMD 精确匹配。返回写出游标终点。
std::uint64_t* intersect_inoue_avx2(const std::uint64_t* a, std::size_t na,
                                    const std::uint64_t* b, std::size_t nb,
                                    std::uint64_t* cur);
std::uint64_t* intersect_inoue_avx512(const std::uint64_t* a, std::size_t na,
                                      const std::uint64_t* b, std::size_t nb,
                                      std::uint64_t* cur);
#endif

}  // namespace bitcask::bm25::kernels
