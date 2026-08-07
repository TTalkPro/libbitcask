// V3.9:距离内核内部声明。供 hnsw.cpp 的派发器与 bench 对拍/计时调用;
// 不进入 public include/。
//
// S37-3.b：SIMD 内核的**定义**已从 hnsw.cpp 拆到按 ISA 分的 TU
// （hnsw_kernels_avx2.cpp / hnsw_kernels_avx512.cpp，由 CMake 的
// bitcask_simd_tu 施加 -m/-arch 开关）——原先靠 __attribute__((target))
// 让单个 TU 内混编多档，MSVC 不支持该属性。本头只留声明，**不带 target
// 属性**（属性属于定义侧，且现已由 TU 级编译选项承担）。
//
// 调用方**必须先过运行时门**（simd::have_avx2_fma() / have_avx512()）
// 再调用对应内核——在不支持的 CPU 上调用会 SIGILL。

#pragma once

#include <cstddef>

#include "bitcask/detail/cpu_features.hpp"

namespace bitcask::vec::detail {

// 标量内核(始终可用,无 ISA 守卫)。
float dot_scalar(const float* a, const float* b, std::size_t n);
float l2_scalar(const float* a, const float* b, std::size_t n);

#if BITCASK_X86_64
// AVX2+FMA 内核（定义在 hnsw_kernels_avx2.cpp）。
float dot_avx2(const float* a, const float* b, std::size_t n);
float l2_avx2(const float* a, const float* b, std::size_t n);

// AVX-512 内核（定义在 hnsw_kernels_avx512.cpp）。选项集为
// F+CD+BW+DQ+VL——与 MSVC /arch:AVX512 的隐含集合及运行时门对齐。
float dot_avx512(const float* a, const float* b, std::size_t n);
float l2_avx512(const float* a, const float* b, std::size_t n);
#endif

}  // namespace bitcask::vec::detail
