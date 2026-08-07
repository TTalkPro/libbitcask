// int8 点积/L2 内核 —— AVX-VNNI 档（S37-3.b 自 detail/int8_kernels.hpp 拆出）。
//
// ⚠️ 本 TU 由 bitcask_simd_tu(... vnni) 施加对应 -m/-arch 开关。
// 只放门后调用的内核，不得有静态初始化器。
//
// **算法与累加顺序逐字未动**——int8 量化路径有既有的 ULP 自检
// （int8::self_test，对 f32 参考做容差比对），本次拆分不得引入新的数值差异。

#include "bitcask/detail/int8_kernels.hpp"

#if BITCASK_X86_64
#include <immintrin.h>

namespace bitcask::vec::int8 {

// ---------------------------------------------------------------------------
float dot_vnni(const std::int8_t* query_codes,
                      const std::int8_t* db_codes,
                      std::int32_t sum_db,
                      float scale_q, float scale_db,
                      std::size_t dim) noexcept {
    const __m256i sign_flip = _mm256_set1_epi8(
        static_cast<std::int8_t>(-128));   // 0x80
    __m256i acc = _mm256_setzero_si256();

    std::size_t i = 0;
    constexpr std::size_t kStride = 32;    // __m256i = 32 bytes
    for (; i + kStride <= dim; i += kStride) {
        const __m256i va = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(query_codes + i));
        const __m256i vb = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(db_codes + i));
        const __m256i va_u8 = _mm256_xor_si256(va, sign_flip);
        acc = _mm256_dpbusd_avx_epi32(acc, va_u8, vb);
    }

    // Manual horizontal sum of 8 × i32 lanes (no _mm256_reduce_add_epi32).
    // Split into two 128-bit halves, each has 4 × i32, then pairwise add.
    const __m128i lo = _mm256_castsi256_si128(acc);
    const __m128i hi = _mm256_extracti128_si256(acc, 1);
    const __m128i sum4 = _mm_add_epi32(lo, hi);  // 4 × i32 (each = pair sum)
    const __m128i sum2 = _mm_add_epi32(sum4, _mm_srli_si128(sum4, 8));  // 2 × i32
    const __m128i sum1 = _mm_add_epi32(sum2, _mm_srli_si128(sum2, 4));  // 1 × i32 (low lane)
    const std::int32_t raw = _mm_cvtsi128_si32(sum1);

    // Scalar tail (same biased-tail invariant as the 512-bit kernel —
    // see dot_vnni512 above; the tail must contribute q[i]*b[i] +
    // 128*Σ b[i] so that the -128*sum_db compensation cancels uniformly).
    std::int32_t tail_dot = 0;
    std::int32_t tail_sum_b = 0;
    for (; i < dim; ++i) {
        tail_dot  += static_cast<std::int32_t>(query_codes[i]) *
                     static_cast<std::int32_t>(db_codes[i]);
        tail_sum_b += static_cast<std::int32_t>(db_codes[i]);
    }
    const std::int32_t raw_total = raw + tail_dot + 128 * tail_sum_b;
    const std::int32_t dot_codes = raw_total - 128 * sum_db;
    const float k = (scale_q * scale_db) / (127.0f * 127.0f);
    return static_cast<float>(dot_codes) * k;
}

// ---------------------------------------------------------------------------
// l2_vnni512 — AVX-512 VNNI squared L2 distance kernel.
//
// ||a-b||² = ||a||² + ||b||² - 2·dot(a,b)
// In codes space:
//   sq_diff = sq_a + sq_b - 2 * Σ codes_a[i]*codes_b[i]
// Reconstructed scale: ((scale_a + scale_b) / 2 / 127)² * sq_diff.
//
// Implementation: do the dot kernel (above) to get the integer codes-space
// dot, then assemble the L2 formula on the outside. The query vector's
// sq_norm and scale are passed in; the db vector's are pre-stored in
// QVector. Total work: 1 VNNI pass + O(1) outside-the-loop arithmetic.
//
// The 64-byte blocks of the VNNI pass overlap with dot_vnni512 exactly,
// so a compiler that inlines both will fuse them — but we keep them as
// separate functions for clarity and so callers can choose.

}  // namespace bitcask::vec::int8

#endif  // BITCASK_X86_64
