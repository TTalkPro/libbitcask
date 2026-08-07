// BM25 打分内核 —— AVX-512 档（S37-3.b 自 bm25_kernels.hpp 拆出）。
//
// ⚠️ 本 TU 由 bitcask_simd_tu(... avx512) 施加对应 -m/-arch 开关。
// 只放门后调用的内核，不得有静态初始化器。
//
// **算法与求值顺序逐字未动**——BM25 打分对 FMA 求值顺序敏感（头注释里记着
// 「minor ULP drift is acceptable」的既有契约，且测试靠「两侧用同一内核」
// 才相等），本次拆分不得引入任何新的数值差异。

#include "bitcask/bm25_kernels.hpp"

#if BITCASK_X86_64
#include <immintrin.h>

namespace bitcask::bm25::detail {

// ---------------------------------------------------------------------------
// AVX-512F kernel — 16 uint32→float pairs per iteration.
//
// _mm512_cvtepi32_ps (AVX-512F) is the same signed-int32 conversion as the
// AVX2 path, but the unsigned→signed trick (values < 2^31 produce identical
// bit pattern) gives us the full 16-lane throughput without requiring
// AVX-512BW or AVX-512VL. The single _mm512_div_ps on 16 lanes has the
// same throughput as the AVX2 8-lane version on most cores (Skylake-X
// shipped 1 div/cycle for 256-bit and 2 div/cycle for 512-bit, so we
// double the work per division).
// ---------------------------------------------------------------------------
void bm25_score_avx512(
    const std::uint32_t* tfs,
    const std::uint32_t* dls,
    float k1_plus_1,
    float k1_times_1_minus_b,
    float k1_times_b,
    float delta,
    float idf,
    float inv_avgdl,
    float* contrib,
    std::size_t n) noexcept {
    const __m512 v_k1p1     = _mm512_set1_ps(k1_plus_1);
    const __m512 v_k1_1mb   = _mm512_set1_ps(k1_times_1_minus_b);
    const __m512 v_k1b      = _mm512_set1_ps(k1_times_b);
    const __m512 v_inv_avg  = _mm512_set1_ps(inv_avgdl);
    const __m512 v_delta    = _mm512_set1_ps(delta);
    const __m512 v_idf      = _mm512_set1_ps(idf);

    std::size_t i = 0;
    constexpr std::size_t kStride = 16;  // __m512 = 16 floats

    for (; i + kStride <= n; i += kStride) {
        // 16-lane uint32→float. Both arrays share stride-16 of u32s = 64B
        // (one cache line each) per iteration — friendly to the load pipe.
        const __m512i tfs_i = _mm512_loadu_si512(
            reinterpret_cast<const __m512i*>(tfs + i));
        const __m512i dls_i = _mm512_loadu_si512(
            reinterpret_cast<const __m512i*>(dls + i));
        const __m512 tf_f = _mm512_cvtepi32_ps(tfs_i);
        const __m512 dl_f = _mm512_cvtepi32_ps(dls_i);

        // Same FMA chain as AVX2, widened to 16 lanes.
        const __m512 temp = _mm512_mul_ps(
            _mm512_mul_ps(v_k1b, dl_f), v_inv_avg);
        const __m512 denom = _mm512_add_ps(
            _mm512_add_ps(tf_f, v_k1_1mb), temp);

        const __m512 numer = _mm512_mul_ps(tf_f, v_k1p1);
        const __m512 tf_norm = _mm512_div_ps(numer, denom);

        const __m512 out = _mm512_mul_ps(
            v_idf, _mm512_add_ps(tf_norm, v_delta));

        _mm512_storeu_ps(contrib + i, out);
    }

    // Tail: scalar fallback (n - i) < 16.
    // We don't partial-issue AVX2 here — the tail is small and the AVX2
    // setup cost would exceed the savings.
    for (; i < n; ++i) {
        const auto tf_f = static_cast<float>(tfs[i]);
        const auto dl_f = static_cast<float>(dls[i]);
        const float denom = tf_f + k1_times_1_minus_b +
                            k1_times_b * dl_f * inv_avgdl;
        const float tf_norm = tf_f * k1_plus_1 / denom;
        contrib[i] = idf * (tf_norm + delta);
    }
}

}  // namespace bitcask::bm25::detail

#endif  // BITCASK_X86_64
