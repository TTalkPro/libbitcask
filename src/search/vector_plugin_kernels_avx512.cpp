// 向量归一化内核 —— AVX-512 档（S37-3.b 自 vector_plugin.cpp 拆出）。
//
// ⚠️ 本 TU 由 bitcask_simd_tu(... avx512) 施加对应 -m/-arch 开关。
// 只放门后调用的内核，不得有静态初始化器。算法逐字未动。

#include "vector_plugin_kernels.hpp"

#if BITCASK_X86_64
#include <immintrin.h>

namespace bitcask::vec::kernels {

double sum_sq_avx512(const float* v, std::size_t n) noexcept {
    __m512 a0 = _mm512_setzero_ps(), a1 = _mm512_setzero_ps();
    std::size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        const __m512 x0 = _mm512_loadu_ps(v + i);
        const __m512 x1 = _mm512_loadu_ps(v + i + 16);
        a0 = _mm512_fmadd_ps(x0, x0, a0);
        a1 = _mm512_fmadd_ps(x1, x1, a1);
    }
    __m512 s = _mm512_add_ps(a0, a1);
    double sq = static_cast<double>(_mm512_reduce_add_ps(s));
    for (; i < n; ++i) sq += static_cast<double>(v[i]) * v[i];
    return sq;
}

void scale_avx512(float* dst, const float* src, float inv, std::size_t n) noexcept {
    const __m512 vinv = _mm512_set1_ps(inv);
    std::size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        __m512 a = _mm512_loadu_ps(src + i);
        _mm512_storeu_ps(dst + i, _mm512_mul_ps(a, vinv));
    }
    for (; i < n; ++i) dst[i] = src[i] * inv;
}

}  // namespace bitcask::vec::kernels

#endif  // BITCASK_X86_64
