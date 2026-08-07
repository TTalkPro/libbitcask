// int8 点积/L2 内核 —— AVX2 档（S37-3.b 自 detail/int8_kernels.hpp 拆出）。
//
// ⚠️ 本 TU 由 bitcask_simd_tu(... avx2) 施加对应 -m/-arch 开关。
// 只放门后调用的内核，不得有静态初始化器。
//
// **算法与累加顺序逐字未动**——int8 量化路径有既有的 ULP 自检
// （int8::self_test，对 f32 参考做容差比对），本次拆分不得引入新的数值差异。

#include "bitcask/detail/int8_kernels.hpp"

#if BITCASK_X86_64
#include <immintrin.h>

namespace bitcask::vec::int8 {

// ---------------------------------------------------------------------------
float dot_avx2(const std::int8_t* query_codes,
                      const std::int8_t* db_codes,
                      std::int32_t /*sum_db*/, float scale_q, float scale_db,
                      std::size_t dim) noexcept {
    __m256i acc = _mm256_setzero_si256();
    const __m256i ones16 = _mm256_set1_epi16(1);
    std::size_t i = 0;
    constexpr std::size_t kStride = 32;
    for (; i + kStride <= dim; i += kStride) {
        const __m256i q = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(query_codes + i));
        const __m256i d = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(db_codes + i));
        const __m256i ad = _mm256_abs_epi8(d);       // u8 ∈ [0,127]
        const __m256i sq = _mm256_sign_epi8(q, d);   // d==0 → 0(乘积本为 0)
        const __m256i p16 = _mm256_maddubs_epi16(ad, sq);
        acc = _mm256_add_epi32(acc, _mm256_madd_epi16(p16, ones16));
    }
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(acc),
                              _mm256_extracti128_si256(acc, 1));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1));
    std::int64_t raw = _mm_cvtsi128_si32(s);
    for (; i < dim; ++i) {  // 尾部标量（dim 非 32 倍数）
        raw += static_cast<std::int32_t>(query_codes[i]) *
               static_cast<std::int32_t>(db_codes[i]);
    }
    return static_cast<float>(raw) * (scale_q * scale_db) / (127.0f * 127.0f);
}

}  // namespace bitcask::vec::int8

#endif  // BITCASK_X86_64
