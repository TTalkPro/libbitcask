// bitcask/bm25_kernels.hpp — SIMD-vectorized BM25 tf_norm scoring kernel.
//
// Computes, for each posting i:
//
//   tf_norm_i = float(tfs[i]) * (k1 + 1) /
//               (float(tfs[i]) + k1*(1 - b) + k1*b * float(dls[i]) / float(avgdl))
//   contrib_i = idf * (tf_norm_i + delta)
//
// Three tiers, picked at runtime via bitcask::simd::cpu_features (S37-3) in the
// dispatcher (bm25_score_dispatch):
//   1. AVX-512F: 16 uint32→float pairs per iteration (Skylake-X / Ice Lake /
//      Zen4+). _mm512_cvtepi32_ps + FMA chain + single _mm512_div_ps.
//   2. AVX2+FMA:  8 uint32→float pairs per iteration (Haswell / Zen / etc.).
//      _mm256_cvtepi32_ps + FMA chain + _mm256_div_ps.
//   3. Scalar:    portable fallback. The dispatcher's last resort and the
//      code path used for tail elements (< 8 / < 16 lanes).
//
// === Correctness / bit-equivalence ===
// The formula structure is preserved (single division of
// (tf*(k1+1)) / (tf + k1*(1-b) + k1*b*dl*inv_avgdl), then idf*(tf_norm+delta)).
// The single SIMD iteration reorders one multiplication — `b*dl*inv_avgdl`
// vs. the original's `b*(dl/avgdl)` — which differs by at most ~1 ULP
// (the inverse is rounded, then re-multiplied vs. the original dividing
// first). This matches the project's pre-existing policy for SIMD paths
// in hnsw.cpp / intersect.cpp: minor ULP drift is acceptable in exchange
// for the FMA-friendly evaluation order. Tests that compare search()
// against itself across kernel invocations (the BmwMatchesFallback*
// / CompactRemovesDeadPostingsPreservesScores family) continue to pass
// because both sides of the comparison use the same kernel.
//
// The dispatcher caches its decision on first call: simd::have_*()
// touches CPUID, but the per-call branch on a static bool is essentially
// free (predicted perfectly after warmup).
//
// === Thread safety ===
// All functions are pure (read-only on inputs) and noexcept. Safe to call
// concurrently from multiple threads.

#pragma once

#include <cstddef>
#include "bitcask/detail/cpu_features.hpp"
#include <cstdint>

#include "bitcask/detail/cpu_features.hpp"

namespace bitcask::bm25::detail {

// ---------------------------------------------------------------------------
// Scalar reference kernel.
//   Used as the fallback on non-x86 and for tail elements on x86.
//   Formula matches the dispatcher contract (see file header).
// ---------------------------------------------------------------------------
inline void bm25_score_scalar(
    const std::uint32_t* tfs,
    const std::uint32_t* dls,
    float k1_plus_1,             // precomputed k1 + 1.0f
    float k1_times_1_minus_b,    // precomputed k1 * (1 - b)
    float k1_times_b,            // precomputed k1 * b
    float delta,
    float idf,
    float inv_avgdl,             // 1.0f / static_cast<float>(avgdl)
    float* contrib,
    std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const auto tf_f = static_cast<float>(tfs[i]);
        const auto dl_f = static_cast<float>(dls[i]);
        // Denominator: tf + k1*(1-b) + k1*b * dl * (1/avgdl)
        // Numerator  : tf * (k1+1)
        const float denom = tf_f + k1_times_1_minus_b +
                            k1_times_b * dl_f * inv_avgdl;
        const float tf_norm = tf_f * k1_plus_1 / denom;
        contrib[i] = idf * (tf_norm + delta);
    }
}

#if BITCASK_X86_64
// S37-3.b：SIMD 内核的**定义**已移出本头，进按 ISA 分的 TU
// （src/bm25/bm25_kernels_avx2.cpp / _avx512.cpp）。
//
// 为什么必须移出：内核原带 __attribute__((target(...)))，MSVC 不支持；而
// MSVC 唯一的等价手段 /arch: 是**每 TU** 生效的——内核只要还内联在头里，
// 就会被编进每一个包含者（本头经 bm25_search_impl.hpp / inverted.cpp 传播），
// 无法对它们单独施加 ISA 开关。
//
// 副作用：跨 TU 调用切断了内联。本档内核按块（kBlockSize=128）调用，
// 单次调用摊薄了调用开销——需 bench 复核（见 TASK.md 落地记录）。
//
// 调用方必须先过 simd::have_avx2_fma() / have_avx512() 门。
void bm25_score_avx2(const std::uint32_t* tfs, const std::uint32_t* dls,
                     float k1_plus_1, float k1_times_1_minus_b,
                     float k1_times_b, float delta, float idf,
                     float inv_avgdl, float* contrib, std::size_t n) noexcept;

void bm25_score_avx512(const std::uint32_t* tfs, const std::uint32_t* dls,
                       float k1_plus_1, float k1_times_1_minus_b,
                       float k1_times_b, float delta, float idf,
                       float inv_avgdl, float* contrib, std::size_t n) noexcept;
#endif  // BITCASK_X86_64

// ---------------------------------------------------------------------------
// Runtime dispatcher — pick the best available kernel for the current CPU.
// Caches the decision on first call (static init), so the per-call cost is
// a single predictable branch.
// ---------------------------------------------------------------------------
inline void bm25_score_dispatch(
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
#if BITCASK_X86_64
    // S37-3：探测下沉到 simd::cpu_features（自实现 CPUID + XCR0 门，MSVC
    // 通用）。注意 AVX-512 档现在要求 F+CD+BW+DQ+VL 整集齐备而非仅 F——
    // 见 cpu_features.hpp（MSVC /arch:AVX512 隐含整集）。
    static const bool kAvx512 = simd::have_avx512();
    if (kAvx512) {
        bm25_score_avx512(tfs, dls,
                          k1_plus_1, k1_times_1_minus_b, k1_times_b,
                          delta, idf, inv_avgdl, contrib, n);
        return;
    }
    static const bool kAvx2Fma = simd::have_avx2_fma();
    if (kAvx2Fma) {
        bm25_score_avx2(tfs, dls,
                        k1_plus_1, k1_times_1_minus_b, k1_times_b,
                        delta, idf, inv_avgdl, contrib, n);
        return;
    }
#endif
    bm25_score_scalar(tfs, dls,
                      k1_plus_1, k1_times_1_minus_b, k1_times_b,
                      delta, idf, inv_avgdl, contrib, n);
}

}  // namespace bitcask::bm25::detail
