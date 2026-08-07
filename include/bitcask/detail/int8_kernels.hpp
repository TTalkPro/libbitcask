// bitcask/int8_kernels.hpp — per-vector symmetric int8 quantization +
// AVX-512 VNNI / AVX-VNNI dot & L2 distance kernels.
//
// === Quantization (per-vector symmetric) ===
// Input: a normalized f32 vector v with values in [-1, 1] (cosine upstream
// guarantees ||v|| = 1).
//   scale      = max |v[i]|         (per-vector, scalar)
//   codes[i]   = round(v[i] / scale * 127)   clamped to [-127, 127]
// Reconstruction: v_hat[i] = codes[i] * scale / 127
//
// === Dot product from quantized codes ===
// dot(a,b) = Σ a[i]*b[i]
//          = Σ codes_a[i] * codes_b[i] * (scale_a * scale_b) / (127 * 127)
//
// === VNNI bias compensation (the key trick) ===
// _mm512_dpbusd_epi32 / _mm256_dpbusd_avx_epi32 are unsigned × signed
// (u8 × s8 → i32 accumulate). Our codes are signed int8. To use them:
//   - query codes XOR 0x80  → unsigned byte (s8 in [-127, 127] maps to u8
//     in [1, 255], so XOR is safe — no -128 saturation risk)
//   - db codes stay as signed int8
//   - raw = Σ (query_u8[i] * db_s8[i])
//         = Σ (query[i]+128) * db[i]
//         = Σ query[i]*db[i] + 128 * Σ db[i]
//   - so:  Σ query[i]*db[i]  = raw - 128 * sum_codes_db
// sum_codes_db is precomputed at quantize() time and stored in QVector.
//
// === L2 squared distance from quantized codes ===
// ||a - b||² = ||a||² + ||b||² - 2·dot(a, b)
// For codes with a uniform average scale factor:
//   codes_diff_sq = Σ (codes_a[i] - codes_b[i])²
//                 = sq_a + sq_b - 2 * Σ codes_a[i] * codes_b[i]
// (precomputed sq_a, sq_b in QVector; dot from above).
//
// === Kernel tiers (runtime dispatch via bitcask::simd::cpu_features) ===
//   1. AVX-512 VNNI (avx512vnni):  64 int8 per iteration, __m512i accum
//   2. AVX-VNNI    (avxvnni):      32 int8 per iteration, __m256i accum
//   3. Scalar fallback (non-x86 or CPU without VNNI)
//
// === Correctness / bit-equivalence ===
// The scalar reference matches the VNNI result exactly for dot product
// (integer part), and the VNNI reduction applies the same -128 * sum_db
// compensation as the reference. Reconstruction is approximate (int8
// quantization is lossy by construction); expected error on cosine-
// normalized 384-dim vectors is well under 5 % relative to f32 dot.
//
// === Thread safety ===
// All functions are pure (read-only inputs) and noexcept. Safe to call
// concurrently. Dispatch decision is cached on first call (static init).

#pragma once

#include <algorithm>
#include "bitcask/detail/cpu_features.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "bitcask/detail/cpu_features.hpp"

namespace bitcask::vec::int8 {

// ---------------------------------------------------------------------------
// QVector — per-vector quantized representation.
// codes/scale are the minimum needed to reconstruct the f32 vector.
// sum_codes and sq_norm_codes are precomputed once at quantization time
// and amortized across every subsequent VNNI distance call.
// ---------------------------------------------------------------------------
struct QVector {
    std::vector<std::int8_t> codes;   // dim elements, range [-127, 127]
    float                    scale;   // max |v[i]| of original f32 vector
    std::int32_t             sum_codes;     // Σ codes[i]            — VNNI bias compensation
    std::int32_t             sq_norm_codes; // Σ codes[i]²           — L2 fast path
};

// ---------------------------------------------------------------------------
// quantize — f32 vector → int8 QVector.
//
// Per-vector symmetric quantization. scale = max |v[i]| (avoids overflow
// across the dynamic range, gives 1 sign bit + 7 magnitude bits per
// element). max_abs == 0 is replaced by 1.0 so the all-zero vector still
// has a well-defined scale (the codes are all 0 in that case).
//
// Precomputes sum_codes and sq_norm_codes so each downstream VNNI call is
// a pure arithmetic op (no per-vector reduction loop).
// ---------------------------------------------------------------------------
// S13-P5：就地量化——codes 复用 out 的既有容量（thread_local 调用方稳态
// 零分配）。逻辑与 quantize() 逐运算一致（含 std::round 半离零舍入——不换
// SIMD round：_mm256_cvtps_epi32 是 round-half-to-even，.5 边界结果不同，
// codes 会入 checkpoint，违反位级不变约定）。
inline void quantize_into(const float* v, std::size_t dim,
                          QVector& out) noexcept {
    float max_abs = 0.0f;
    for (std::size_t i = 0; i < dim; ++i) {
        const float a = std::abs(v[i]);
        if (a > max_abs) max_abs = a;
    }
    if (max_abs == 0.0f) max_abs = 1.0f;

    out.codes.resize(dim);  // 容量只增：thread_local 复用后零分配
    out.scale = max_abs;

    const float inv_scale = 127.0f / max_abs;
    std::int32_t sum = 0;
    std::int32_t sq  = 0;
    for (std::size_t i = 0; i < dim; ++i) {
        float val = v[i] * inv_scale;
        if (val >  127.0f) val =  127.0f;
        if (val < -127.0f) val = -127.0f;
        const auto code = static_cast<std::int8_t>(std::round(val));
        out.codes[i] = code;
        sum += static_cast<std::int32_t>(code);
        sq  += static_cast<std::int32_t>(code) * static_cast<std::int32_t>(code);
    }
    out.sum_codes     = sum;
    out.sq_norm_codes = sq;
}

inline QVector quantize(const float* v, std::size_t dim) noexcept {
    float max_abs = 0.0f;
    for (std::size_t i = 0; i < dim; ++i) {
        const float a = std::abs(v[i]);
        if (a > max_abs) max_abs = a;
    }
    if (max_abs == 0.0f) max_abs = 1.0f;

    QVector qv;
    qv.codes.resize(dim);
    qv.scale = max_abs;

    const float inv_scale = 127.0f / max_abs;
    std::int32_t sum = 0;
    std::int32_t sq  = 0;
    for (std::size_t i = 0; i < dim; ++i) {
        float val = v[i] * inv_scale;
        // Clamp to [-127, 127]. The signed-int8 representable range
        // excludes -128; clamp keeps the multiplication well-defined.
        if (val >  127.0f) val =  127.0f;
        if (val < -127.0f) val = -127.0f;
        const auto code = static_cast<std::int8_t>(std::round(val));
        qv.codes[i] = code;
        sum += static_cast<std::int32_t>(code);
        sq  += static_cast<std::int32_t>(code) * static_cast<std::int32_t>(code);
    }
    qv.sum_codes     = sum;
    qv.sq_norm_codes = sq;
    return qv;
}

// ---------------------------------------------------------------------------
// dot_scalar — exact (in the integer sense) dot product from two QVectors.
// Returns the reconstructed dot ≈ scale_a * scale_b / (127²) * Σ codes_a*codes_b.
// Used as the non-x86 fallback and as the reference for self-test.
// ---------------------------------------------------------------------------
inline float dot_scalar(const QVector& a, const QVector& b,
                        std::size_t /*dim*/) noexcept {
    std::int64_t raw = 0;
    const std::size_t dim = a.codes.size();
    for (std::size_t i = 0; i < dim; ++i) {
        raw += static_cast<std::int32_t>(a.codes[i]) *
               static_cast<std::int32_t>(b.codes[i]);
    }
    const float k = (a.scale * b.scale) / (127.0f * 127.0f);
    return static_cast<float>(raw) * k;
}

// ---------------------------------------------------------------------------
// l2_scalar — squared L2 distance from two QVectors (scalar reference).
// Uses the identity ||a-b||² = sq_a + sq_b - 2·dot, with a per-vector
// scale factor of (scale_a + scale_b) / 2 / 127 applied uniformly. This
// average is the natural choice when both vectors have been quantized
// with their own max-abs scales.
// ---------------------------------------------------------------------------
inline float l2_scalar(const QVector& a, const QVector& b,
                       std::size_t /*dim*/) noexcept {
    // Codes-space squared difference: Σ (a[i] - b[i])²
    // (computed directly to keep the integer path simple).
    std::int64_t raw = 0;
    const std::size_t dim = a.codes.size();
    for (std::size_t i = 0; i < dim; ++i) {
        const auto d = static_cast<std::int32_t>(a.codes[i]) -
                       static_cast<std::int32_t>(b.codes[i]);
        raw += d * d;
    }
    const float scale = (a.scale + b.scale) * 0.5f / 127.0f;
    return static_cast<float>(raw) * scale * scale;
}

// ---------------------------------------------------------------------------
// dequantize — reconstruct an f32 vector from a QVector (test helper).
// Out buffer must have at least `dim` elements (== qv.codes.size()).
// ---------------------------------------------------------------------------
inline void dequantize(const QVector& qv, float* out,
                       std::size_t /*dim*/) noexcept {
    const float factor = qv.scale / 127.0f;
    const std::size_t dim = qv.codes.size();
    for (std::size_t i = 0; i < dim; ++i) {
        out[i] = static_cast<float>(qv.codes[i]) * factor;
    }
}

// ---------------------------------------------------------------------------
// dot_scalar_raw — Int8DotFn-compatible scalar reconstructed dot from raw
// code pointers (P5 int8-only fallback). Matches dot_scalar (QVector) and the
// VNNI result exactly for the dot product. sum_db is unused here (it only
// corrects VNNI's unsigned×signed +128 bias); symmetric int8 codes are signed
// so the plain signed accumulation needs no correction. Lets int8-only mode
// (which has no resident f32) work on machines without AVX(512)-VNNI.
// ---------------------------------------------------------------------------
inline float dot_scalar_raw(const std::int8_t* a, const std::int8_t* b,
                            std::int32_t /*sum_db*/, float scale_a,
                            float scale_b, std::size_t dim) noexcept {
    std::int64_t raw = 0;
    for (std::size_t i = 0; i < dim; ++i) {
        raw += static_cast<std::int32_t>(a[i]) * static_cast<std::int32_t>(b[i]);
    }
    return static_cast<float>(raw) * (scale_a * scale_b) / (127.0f * 127.0f);
}

#if BITCASK_X86_64
// S37-3.b：SIMD 内核的**定义**已移出本头，进按 ISA 分的 TU
// （src/vector/int8_kernels_{avx2,vnni,vnni512}.cpp）。移出原因同 bm25_kernels：
// 原带 __attribute__((target(...)))，MSVC 不支持，而本头被 format.hpp /
// hnsw.hpp / codec.cpp 等大量 TU 包含，内联在头里就无法分别施加 ISA 开关。
//
// 调用方一律经 pick_int8_dot_kernel() 取函数指针（内部已过运行时门），
// 不要直接调这些符号。
// 三者签名一致，可直接赋给 Int8DotFn（见下）。
float dot_vnni512(const std::int8_t* query_codes, const std::int8_t* db_codes,
                  std::int32_t sum_db, float scale_q, float scale_db,
                  std::size_t dim) noexcept;
float dot_vnni(const std::int8_t* query_codes, const std::int8_t* db_codes,
               std::int32_t sum_db, float scale_q, float scale_db,
               std::size_t dim) noexcept;
float dot_avx2(const std::int8_t* query_codes, const std::int8_t* db_codes,
               std::int32_t sum_db, float scale_q, float scale_db,
               std::size_t dim) noexcept;
// L2 多一个 sq_norm_db 参数，故不属于 Int8DotFn 族。
float l2_vnni512(const std::int8_t* query_codes, const std::int8_t* db_codes,
                 std::int32_t sum_db, std::int32_t sq_norm_db, float scale_q,
                 float scale_db, std::size_t dim) noexcept;
#endif  // BITCASK_X86_64

// ---------------------------------------------------------------------------
// Runtime dispatcher. Returns the best int8 dot kernel for this CPU.
// S29-11-②:三级 SIMD 分发 VNNI512 → VNNI256 → AVX2;全部缺席才 nullptr
// (调用方退 f32 / 标量)。
// ---------------------------------------------------------------------------
using Int8DotFn = float (*)(const std::int8_t*, const std::int8_t*,
                            std::int32_t, float, float, std::size_t);

inline Int8DotFn pick_int8_dot_kernel() noexcept {
#if BITCASK_X86_64
    static const Int8DotFn kFn = []() -> Int8DotFn {
        // S37-3：探测经 simd::cpu_features（见 cpu_features.hpp）。
        if (simd::have_avx512_vnni()) return &dot_vnni512;
        if (simd::have_avx_vnni())    return &dot_vnni;
        if (simd::have_avx2())        return &dot_avx2;  // S29-11-②
        return nullptr;
    }();
    return kFn;
#else
    return nullptr;
#endif
}

// ---------------------------------------------------------------------------
// self_test — verifies int8 quantization + VNNI kernels against an f32
// reference on a normalized 384-dim random vector (typical embedding
// size). Returns true if the relative dot-product error is within the
// expected ~5 % bound for symmetric int8 quantization on already-
// normalized data. Always reports the kernel it dispatched to.
//
// Pre-condition: called at process startup / once before hot paths.
// Deterministic given the seed (default 0xC0FFEE) so failures are
// reproducible.
// ---------------------------------------------------------------------------
inline bool self_test(std::size_t dim = 384,
                      std::uint32_t seed = 0xC0FFEEu) noexcept {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    // Build two unit-normalized random vectors.
    std::vector<float> va(dim), vb(dim);
    for (std::size_t i = 0; i < dim; ++i) {
        va[i] = dist(rng);
        vb[i] = dist(rng);
    }
    auto normalize = [](std::vector<float>& v) {
        float s = 0.0f;
        for (float x : v) s += x * x;
        s = std::sqrt(s);
        if (s == 0.0f) return;
        const float inv = 1.0f / s;
        for (float& x : v) x *= inv;
    };
    normalize(va);
    normalize(vb);

    // f32 reference dot product.
    float f32_dot = 0.0f;
    for (std::size_t i = 0; i < dim; ++i) f32_dot += va[i] * vb[i];

    // f32 reference L2 distance.
    float f32_l2 = 0.0f;
    for (std::size_t i = 0; i < dim; ++i) {
        const float d = va[i] - vb[i];
        f32_l2 += d * d;
    }

    // Quantize.
    QVector qa = quantize(va.data(), dim);
    QVector qb = quantize(vb.data(), dim);

    // Scalar int8 references.
    const float scalar_dot = dot_scalar(qa, qb, dim);
    const float scalar_l2  = l2_scalar(qa, qb, dim);

    // Pick VNNI kernel if available.
    const Int8DotFn kernel = pick_int8_dot_kernel();

    // Helper: relative error in [-, ∞); treats f32 == 0 specially.
    auto rel_err = [](float approx, float exact) {
        const float ae = std::abs(approx - exact);
        const float denom = std::max(std::abs(exact), 1e-12f);
        return ae / denom;
    };

    // Scalar must agree with itself exactly modulo int8 rounding error,
    // and the int8 dot must approximate f32 within the typical bound.
    const float dot_err_scalar_vs_f32 = rel_err(scalar_dot, f32_dot);
    const float l2_err_scalar_vs_f32  = rel_err(scalar_l2,  f32_l2);

    // 5 % is generous for int8 symmetric on unit vectors. Cosine
    // distances are in [-1, 1], so 5 % relative on |dot| < 0.01 still
    // leaves 0.0005 absolute — well below HNSW's tie-break threshold.
    constexpr float kTol = 0.05f;
    bool ok = (dot_err_scalar_vs_f32 < kTol) &&
              (l2_err_scalar_vs_f32  < kTol);

    if (kernel != nullptr) {
        const float vnni_dot = kernel(qa.codes.data(), qb.codes.data(),
                                      qb.sum_codes,
                                      qa.scale, qb.scale, dim);
        // VNNI must agree with scalar int8 within float ULP (the integer
        // result is the same; the float multiplication is identical).
        const float vnni_vs_scalar = rel_err(vnni_dot, scalar_dot);
        // Float multiplication can introduce ~1 ULP; allow 1e-5.
        ok = ok && (vnni_vs_scalar < 1e-5f);

        // And vs f32, same bound as scalar.
        ok = ok && (rel_err(vnni_dot, f32_dot) < kTol);
    }

    return ok;
}

}  // namespace bitcask::vec::int8
