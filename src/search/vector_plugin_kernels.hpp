// 向量归一化内核声明（S37-3.b 自 vector_plugin.cpp 拆出）。
//
// 拆分前带 __attribute__((target(...)))，MSVC 不支持；改按 ISA 分 TU。
// 调用方必须先过 simd::have_avx2_fma() / have_avx512() 门。
//
// 语义契约（拆分前的注释原样保留）：sq = Σ v*v 用 **double 累加**保留标量版
// 精度契约；缩放 v *= inv 用 float 乘。

#pragma once

#include <cstddef>

#include "bitcask/detail/cpu_features.hpp"

namespace bitcask::vec::kernels {

#if BITCASK_X86_64
double sum_sq_avx2(const float* v, std::size_t n) noexcept;
void scale_avx2(float* dst, const float* src, float inv, std::size_t n) noexcept;
double sum_sq_avx512(const float* v, std::size_t n) noexcept;
void scale_avx512(float* dst, const float* src, float inv,
                  std::size_t n) noexcept;
#endif

}  // namespace bitcask::vec::kernels
