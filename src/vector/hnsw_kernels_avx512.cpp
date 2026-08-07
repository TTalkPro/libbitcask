// HNSW 距离内核 —— AVX-512 档（S37-3.b 自 hnsw.cpp 拆出）。
//
// ⚠️ 本 TU 由 bitcask_simd_tu(... avx512) 施加
// -mavx512f -mavx512cd -mavx512bw -mavx512dq -mavx512vl（MSVC: /arch:AVX512）。
// 同 avx2 档的告警：只放门后调用的内核，不得有静态初始化器。
//
// **注意选项集比原 target("avx512f") 宽**：MSVC 的 /arch:AVX512 隐含
// F+CD+BW+DQ+VL，GCC/Clang 侧对齐同一集合，运行时门（simd::have_avx512()）
// 也按整集放行——三者一致才不会出现「编出了门不放行的指令」。
// 算法逐字未动。

#include "hnsw_kernels.hpp"

#include "bitcask/detail/cpu_features.hpp"

#if BITCASK_X86_64
#include <immintrin.h>

namespace bitcask::vec::detail {

// V3.9:AVX-512 距离内核。__m512 = 16 floats,主循环 stride = 64(4 路累加 ×
// 16),把 384d 缩到 6 轮、1536d 缩到 24 轮;在 Skylake-SP/Ice Lake 这类双
// FMA 单元上,主循环理论上限 ~2 FMA/cyc(2 加载 + 2 FMA / cyc),相对 AVX2
// 内核理论再翻倍。仅用 AVX512F 子集(无 BW/VL),最大化可移植。注意:
// 求和顺序随累加器宽度变宽,与 AVX2 末位 ulp 可能有数 ulp 漂移,正确性
// 检验见 cpp/bench/distance_bench.cpp。
float hsum512(__m512 v) {
#if defined(__clang__) || (defined(__GNUC__) && (__GNUC__ >= 10))
    // Clang 与 GCC 10+ 的 _mm512_reduce_add_ps:内部即树形归并,单指令。
    // 注意:clang 的 __GNUC__ 恒为 4（GCC 4.2.1 兼容伪装），必须显式 defined(__clang__)
    // 才能走此路径——否则落入下方 #else 的手工归并（S12-7 修:clang 曾在此编译失败）。
    return _mm512_reduce_add_ps(v);
#else
    // 兼容旧编译器:手工两两归并(8 步加法 vs 横向 ~16 步)。
    // 取高 256 位:经 f64 视图 extract 高 4×f64（= 256 位），仅需 AVX512F。
    __m256 lo = _mm512_castps512_ps256(v);
    __m256 hi = _mm256_castpd_ps(
        _mm512_extractf64x4_pd(_mm512_castps_pd(v), 1));
    __m256 s256 = _mm256_add_ps(lo, hi);
    __m128 lo2 = _mm256_castps256_ps128(s256);
    __m128 hi2 = _mm256_extractf128_ps(s256, 1);
    __m128 s128 = _mm_add_ps(lo2, hi2);
    s128 = _mm_hadd_ps(s128, s128);
    s128 = _mm_hadd_ps(s128, s128);
    return _mm_cvtss_f32(s128);
#endif
}

float dot_avx512(const float* a, const float* b, std::size_t n) {
    __m512 acc0 = _mm512_setzero_ps(), acc1 = _mm512_setzero_ps();
    __m512 acc2 = _mm512_setzero_ps(), acc3 = _mm512_setzero_ps();
    std::size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        acc0 = _mm512_fmadd_ps(_mm512_loadu_ps(a + i),
                               _mm512_loadu_ps(b + i), acc0);
        acc1 = _mm512_fmadd_ps(_mm512_loadu_ps(a + i + 16),
                               _mm512_loadu_ps(b + i + 16), acc1);
        acc2 = _mm512_fmadd_ps(_mm512_loadu_ps(a + i + 32),
                               _mm512_loadu_ps(b + i + 32), acc2);
        acc3 = _mm512_fmadd_ps(_mm512_loadu_ps(a + i + 48),
                               _mm512_loadu_ps(b + i + 48), acc3);
    }
    for (; i + 16 <= n; i += 16) {
        acc0 = _mm512_fmadd_ps(_mm512_loadu_ps(a + i),
                               _mm512_loadu_ps(b + i), acc0);
    }
    float s = hsum512(_mm512_add_ps(_mm512_add_ps(acc0, acc1),
                                    _mm512_add_ps(acc2, acc3)));
    for (; i < n; ++i) s += a[i] * b[i];
    return -s;
}

float l2_avx512(const float* a, const float* b, std::size_t n) {
    __m512 acc0 = _mm512_setzero_ps(), acc1 = _mm512_setzero_ps();
    __m512 acc2 = _mm512_setzero_ps(), acc3 = _mm512_setzero_ps();
    std::size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        const __m512 d0 = _mm512_sub_ps(_mm512_loadu_ps(a + i),
                                        _mm512_loadu_ps(b + i));
        const __m512 d1 = _mm512_sub_ps(_mm512_loadu_ps(a + i + 16),
                                        _mm512_loadu_ps(b + i + 16));
        const __m512 d2 = _mm512_sub_ps(_mm512_loadu_ps(a + i + 32),
                                        _mm512_loadu_ps(b + i + 32));
        const __m512 d3 = _mm512_sub_ps(_mm512_loadu_ps(a + i + 48),
                                        _mm512_loadu_ps(b + i + 48));
        acc0 = _mm512_fmadd_ps(d0, d0, acc0);
        acc1 = _mm512_fmadd_ps(d1, d1, acc1);
        acc2 = _mm512_fmadd_ps(d2, d2, acc2);
        acc3 = _mm512_fmadd_ps(d3, d3, acc3);
    }
    for (; i + 16 <= n; i += 16) {
        const __m512 d = _mm512_sub_ps(_mm512_loadu_ps(a + i),
                                       _mm512_loadu_ps(b + i));
        acc0 = _mm512_fmadd_ps(d, d, acc0);
    }
    float s = hsum512(_mm512_add_ps(_mm512_add_ps(acc0, acc1),
                                    _mm512_add_ps(acc2, acc3)));
    for (; i < n; ++i) {
        const float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}
}  // namespace bitcask::vec::detail

#endif  // BITCASK_X86_64
