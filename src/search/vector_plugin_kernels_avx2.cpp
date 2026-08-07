// 向量归一化内核 —— AVX2 + FMA 档（S37-3.b 自 vector_plugin.cpp 拆出）。
//
// ⚠️ 本 TU 由 bitcask_simd_tu(... avx2) 施加对应 -m/-arch 开关。
// 只放门后调用的内核，不得有静态初始化器。算法逐字未动。

#include "vector_plugin_kernels.hpp"

#if BITCASK_X86_64
#include <immintrin.h>

namespace bitcask::vec::kernels {

double sum_sq_avx2(const float* v, std::size_t n) noexcept {
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    std::size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        const __m256 a0 = _mm256_loadu_ps(v + i);
        const __m256 a1 = _mm256_loadu_ps(v + i + 8);
        acc0 = _mm256_fmadd_ps(a0, a0, acc0);
        acc1 = _mm256_fmadd_ps(a1, a1, acc1);
    }
    if (i + 8 <= n) {
        const __m256 a = _mm256_loadu_ps(v + i);
        acc0 = _mm256_fmadd_ps(a, a, acc0);
        i += 8;
    }
    __m256 s = _mm256_add_ps(acc0, acc1);
    __m128 lo = _mm256_castps256_ps128(s);
    __m128 hi = _mm256_extractf128_ps(s, 1);
    __m128 s128 = _mm_add_ps(lo, hi);
    s128 = _mm_hadd_ps(s128, s128);
    s128 = _mm_hadd_ps(s128, s128);
    double sq = static_cast<double>(_mm_cvtss_f32(s128));
    for (; i < n; ++i) sq += static_cast<double>(v[i]) * v[i];
    return sq;
}

void scale_avx2(float* dst, const float* src, float inv, std::size_t n) noexcept {
    const __m256 vinv = _mm256_set1_ps(inv);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 a = _mm256_loadu_ps(src + i);
        _mm256_storeu_ps(dst + i, _mm256_mul_ps(a, vinv));
    }
    for (; i < n; ++i) dst[i] = src[i] * inv;
}

}  // namespace bitcask::vec::kernels

#endif  // BITCASK_X86_64
