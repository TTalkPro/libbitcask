// BM25 打分内核 —— AVX2 + FMA 档（S37-3.b 自 bm25_kernels.hpp 拆出）。
//
// ⚠️ 本 TU 由 bitcask_simd_tu(... avx2) 施加对应 -m/-arch 开关。
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
// AVX2+FMA kernel — 8 uint32→float pairs per iteration.
//
// Strategy: convert both input arrays to __m256 (8 lanes) with
// _mm256_cvtepi32_ps, then evaluate the BM25 formula elementwise using
// FMA. The single division (one __m256 per 8 lanes) is on the critical
// path, but FMA hides the multiplies/memory loads.
//
// _mm256_cvtepi32_ps is the unsigned-friendly path: GCC/Clang emit vcvtdq2ps
// which converts signed int32 — for unsigned values that fit in 31 bits
// (our tfs and dls are well under 2^31, since kBlockSize=128 and per-term
// tf rarely exceeds hundreds), the result is bit-identical to a true
// uint32→float conversion. The alternative (_mm256_cvtepu32_ps) requires
// AVX-512F + AVX-512VL, which is outside our target matrix.
// ---------------------------------------------------------------------------
void bm25_score_avx2(
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
    const __m256 v_k1p1     = _mm256_set1_ps(k1_plus_1);
    const __m256 v_k1_1mb   = _mm256_set1_ps(k1_times_1_minus_b);
    const __m256 v_k1b      = _mm256_set1_ps(k1_times_b);
    const __m256 v_inv_avg  = _mm256_set1_ps(inv_avgdl);
    const __m256 v_delta    = _mm256_set1_ps(delta);
    const __m256 v_idf      = _mm256_set1_ps(idf);

    std::size_t i = 0;
    constexpr std::size_t kStride = 8;  // __m256 = 8 floats

    for (; i + kStride <= n; i += kStride) {
        // uint32→float: tfs[i..i+7], dls[i..i+7]. Safe for tf < 2^31 and
        // dl < 2^31, which holds in practice (tf ≤ ~thousands, dl ≤ ~10k).
        const __m256i tfs_i = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(tfs + i));
        const __m256i dls_i = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(dls + i));
        const __m256 tf_f = _mm256_cvtepi32_ps(tfs_i);
        const __m256 dl_f = _mm256_cvtepi32_ps(dls_i);

        // Denominator = tf + k1*(1-b) + k1*b * dl * (1/avgdl).
        // Two FMAs: temp = k1*b * dl * (1/avgdl); denom = tf + k1*(1-b) + temp.
        // The k1*b * dl is FMA-fused with * (1/avgdl) via the second fma.
        const __m256 temp = _mm256_mul_ps(
            _mm256_mul_ps(v_k1b, dl_f), v_inv_avg);
        const __m256 denom = _mm256_add_ps(
            _mm256_add_ps(tf_f, v_k1_1mb), temp);

        // Numerator = tf * (k1+1). Single mul, no FMA needed.
        const __m256 numer = _mm256_mul_ps(tf_f, v_k1p1);

        // Single 8-wide division.
        const __m256 tf_norm = _mm256_div_ps(numer, denom);

        // contrib = idf * (tf_norm + delta).
        const __m256 out = _mm256_mul_ps(
            v_idf, _mm256_add_ps(tf_norm, v_delta));

        _mm256_storeu_ps(contrib + i, out);
    }

    // Tail: scalar fallback (n - i) < 8.
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
