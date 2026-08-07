// int8 点积/L2 内核 —— AVX512-VNNI 档（S37-3.b 自 detail/int8_kernels.hpp 拆出）。
//
// ⚠️ 本 TU 由 bitcask_simd_tu(... vnni512) 施加对应 -m/-arch 开关。
// 只放门后调用的内核，不得有静态初始化器。
//
// **算法与累加顺序逐字未动**——int8 量化路径有既有的 ULP 自检
// （int8::self_test，对 f32 参考做容差比对），本次拆分不得引入新的数值差异。

#include "bitcask/detail/int8_kernels.hpp"

#if BITCASK_X86_64
#include <immintrin.h>

namespace bitcask::vec::int8 {

// ---------------------------------------------------------------------------
// dot_vnni512 — AVX-512 VNNI dot product kernel.
//
// 64 int8 elements per iteration (__m512i). The intrinsic signature is
// unsigned × signed (u8 × s8 → i32 accumulate):
//   query_u8 = query_codes XOR 0x80      (s8 in [-127,127] → u8 in [1,255])
//   db_s8    = db_codes                  (kept as signed — the second arg
//                                          of dpbusd is the signed operand)
// Compensation (folded into the return value):
//   raw  = Σ (query_u8[i] * db_s8[i])
//         = Σ query[i] * db[i] + 128 * Σ db[i]
//   dot  = raw - 128 * sum_db
//   res  = (scale_q * scale_db / (127*127)) * dot
//
// Tail (< 64 elements) is handled by a scalar loop.
// ---------------------------------------------------------------------------
float dot_vnni512(const std::int8_t* query_codes,
                         const std::int8_t* db_codes,
                         std::int32_t sum_db,
                         float scale_q, float scale_db,
                         std::size_t dim) noexcept {
    const __m512i sign_flip = _mm512_set1_epi8(
        static_cast<std::int8_t>(-128));   // 0x80
    __m512i acc = _mm512_setzero_si512();

    std::size_t i = 0;
    constexpr std::size_t kStride = 64;    // __m512i = 64 bytes
    for (; i + kStride <= dim; i += kStride) {
        const __m512i va = _mm512_loadu_si512(query_codes + i);
        const __m512i vb = _mm512_loadu_si512(db_codes     + i);
        // XOR 0x80 flips the sign bit: s8 [-127,127] → u8 [1,255] safely
        // (codes never equal -128 because quantize() clamps to [-127,127]).
        const __m512i va_u8 = _mm512_xor_si512(va, sign_flip);
        // vpdpbusd: acc[i32] += Σ va_u8[u8] * vb[s8], per 4-byte lane
        acc = _mm512_dpbusd_epi32(acc, va_u8, vb);
    }

    // Horizontal reduce of 16 × i32 lanes to a single i32 scalar.
    const std::int32_t raw = _mm512_reduce_add_epi32(acc);

    // Scalar tail. The SIMD loop contributed the biased form
    // Σ (q[i]+128) * b[i] for the head; the tail must match so that the
    // -128*sum_db compensation is applied uniformly. Concretely:
    //   biased_tail = Σ q[i]*b[i] + 128 * Σ b[i]   (i in tail)
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
// dot_vnni — AVX-VNNI (256-bit, VEX-encoded) dot product kernel.
//
// 32 int8 elements per iteration (__m256i). Uses _mm256_dpbusd_avx_epi32
// (the VEX-encoded variant — NOT the EVEX _mm256_dpbusd_epi32, which would
// require AVX-512-VNNI even for a 256-bit operation on this toolchain).
//
// _mm256_reduce_add_epi32 does not exist on AVX/AVX2 — we do the 8-lane
// horizontal sum by hand: extract low/high 128, each -> 4 × i32 pair,
// add, then accumulate the four pairs.

// ---------------------------------------------------------------------------
float l2_vnni512(const std::int8_t* query_codes,
                        const std::int8_t* db_codes,
                        std::int32_t sum_db,
                        std::int32_t sq_norm_db,
                        float scale_q, float scale_db,
                        std::size_t dim) noexcept {
    const __m512i sign_flip = _mm512_set1_epi8(
        static_cast<std::int8_t>(-128));
    __m512i acc = _mm512_setzero_si512();

    std::size_t i = 0;
    constexpr std::size_t kStride = 64;
    for (; i + kStride <= dim; i += kStride) {
        const __m512i va = _mm512_loadu_si512(query_codes + i);
        const __m512i vb = _mm512_loadu_si512(db_codes     + i);
        const __m512i va_u8 = _mm512_xor_si512(va, sign_flip);
        acc = _mm512_dpbusd_epi32(acc, va_u8, vb);
    }

    const std::int32_t raw = _mm512_reduce_add_epi32(acc);

    // Scalar tail for dot part (biased, same invariant as dot_vnni512).
    std::int32_t tail_dot = 0;
    std::int32_t tail_sum_b = 0;
    for (; i < dim; ++i) {
        tail_dot  += static_cast<std::int32_t>(query_codes[i]) *
                     static_cast<std::int32_t>(db_codes[i]);
        tail_sum_b += static_cast<std::int32_t>(db_codes[i]);
    }
    const std::int32_t dot_codes =
        (raw + tail_dot + 128 * tail_sum_b) - 128 * sum_db;

    // L2 assembly. sq_norm_query is computed by the caller and passed in
    // implicitly via the .sq_norm_codes of the QVector (we don't take it
    // as a kernel arg to keep the API symmetric with dot_vnni512 — the
    // caller does sq_a + sq_b outside the call). Here we just expose the
    // dot_codes and the caller plugs in the precomputed sq_norms.
    (void)sq_norm_db;  // documented arg for the L2 API; unused in this
                       // implementation (we compute the L2 outside the
                       // kernel after the dot pass).
    (void)scale_q;
    (void)scale_db;
    (void)dim;
    return static_cast<float>(dot_codes);
}

// ---------------------------------------------------------------------------
// dot_avx2 — S29-11-②:无 VNNI 机器的缺口补齐（AVX2 自 2013 Haswell 起
// 普及）。经典两步 vpmaddubsw(u8×s8→s16 对和) + vpmaddwd(s16→s32 横加)。
//
// **sign 技巧防饱和**（与 VNNI 的 XOR-0x80 偏置法不同）:vpmaddubsw 的
// s16 对和会饱和——若走偏置法,u8∈[1,255] × s8∈[-127,127] 的对和上界
// 2·255·127 = 64770 > 32767,静默饱和 = 静默错分。改用恒等式
// q·d = |d| ⊙ sign(q, d):|d| ≤ 127 作 u8 操作数、sign(q,d) ∈ [-127,127]
// 作 s8 操作数 → 对和上界 2·127·127 = 32258 < 32767,**永不饱和**。
// 整数部分与 dot_scalar_raw 精确一致(kernel 对拍契约;sum_db 无用——
// 无偏置需补偿)。s32 lane 累计上界 (dim/32)·64516,dim ≤ 65535 时
// ≈1.3e8 ≪ 2^31,安全。

}  // namespace bitcask::vec::int8

#endif  // BITCASK_X86_64
