// HNSW 距离内核 —— AVX2 + FMA 档（S37-3.b 自 hnsw.cpp 拆出）。
//
// ⚠️ 本 TU 由 CMake 的 bitcask_simd_tu(... avx2) 施加 -mavx2 -mfma（MSVC:
// /arch:AVX2）。**编译器有权在本文件的任何函数里生成 AVX2/FMA 指令**，
// 因此这里只放运行时门（simd::have_avx2_fma()）之后才会被调用的内核，
// 且**不得添加任何静态初始化器**——那会在 main 之前无条件执行，在无 AVX2
// 的 CPU 上直接 SIGILL。契约详见 CMakeLists.txt 的 bitcask_simd_tu 注释。
//
// 拆分前这些函数带 __attribute__((target("avx2,fma")))，与 hnsw.cpp 的
// scalar 内核同处一个 TU。MSVC 不支持该属性，故改为按 ISA 分 TU。
// **算法与求和顺序逐字未动**——本次拆分是纯位置移动，不得引入 ULP 漂移。

#include "hnsw_kernels.hpp"

#include "bitcask/detail/cpu_features.hpp"

#if BITCASK_X86_64
#include <immintrin.h>

namespace bitcask::vec::detail {

float hsum256(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_hadd_ps(lo, lo);
    lo = _mm_hadd_ps(lo, lo);
    return _mm_cvtss_f32(lo);
}

// V3.8:4 路独立累加器打破 FMA 依赖链。单累加器下每次 fmadd 依赖上一次
// 结果(FMA 延迟 ~4cyc → 1 FMA/4cyc);4 路交错把循环顶到加载口上限
// (2 加载/cyc = 1 FMA/cyc),内核理论余量 ~4×。384d=12 轮、2560d=80 轮
// 整除主循环;8 宽次级循环 + 标量尾兜任意 n。注:求和顺序改变,结果与
// 旧内核可有最后一两 ulp 漂移(测试容差均覆盖)。
float dot_avx2(const float* a, const float* b, std::size_t n) {
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    __m256 acc2 = _mm256_setzero_ps(), acc3 = _mm256_setzero_ps();
    std::size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i),
                               _mm256_loadu_ps(b + i), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8),
                               _mm256_loadu_ps(b + i + 8), acc1);
        acc2 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 16),
                               _mm256_loadu_ps(b + i + 16), acc2);
        acc3 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 24),
                               _mm256_loadu_ps(b + i + 24), acc3);
    }
    for (; i + 8 <= n; i += 8) {
        acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i),
                               _mm256_loadu_ps(b + i), acc0);
    }
    float s = hsum256(_mm256_add_ps(_mm256_add_ps(acc0, acc1),
                                    _mm256_add_ps(acc2, acc3)));
    for (; i < n; ++i) s += a[i] * b[i];
    return -s;
}

float l2_avx2(const float* a, const float* b, std::size_t n) {
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    __m256 acc2 = _mm256_setzero_ps(), acc3 = _mm256_setzero_ps();
    std::size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        const __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i),
                                        _mm256_loadu_ps(b + i));
        const __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8),
                                        _mm256_loadu_ps(b + i + 8));
        const __m256 d2 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 16),
                                        _mm256_loadu_ps(b + i + 16));
        const __m256 d3 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 24),
                                        _mm256_loadu_ps(b + i + 24));
        acc0 = _mm256_fmadd_ps(d0, d0, acc0);
        acc1 = _mm256_fmadd_ps(d1, d1, acc1);
        acc2 = _mm256_fmadd_ps(d2, d2, acc2);
        acc3 = _mm256_fmadd_ps(d3, d3, acc3);
    }
    for (; i + 8 <= n; i += 8) {
        const __m256 d = _mm256_sub_ps(_mm256_loadu_ps(a + i),
                                       _mm256_loadu_ps(b + i));
        acc0 = _mm256_fmadd_ps(d, d, acc0);
    }
    float s = hsum256(_mm256_add_ps(_mm256_add_ps(acc0, acc1),
                                    _mm256_add_ps(acc2, acc3)));
    for (; i < n; ++i) {
        const float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}
}  // namespace bitcask::vec::detail

#endif  // BITCASK_X86_64
