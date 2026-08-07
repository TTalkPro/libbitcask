// 求交内核 —— AVX2 档（S37-3.b 自 intersect.cpp 拆出）。
//
// ⚠️ 本 TU 由 bitcask_simd_tu(... avx2) 施加 -mavx2 -mfma（MSVC: /arch:AVX2）。
// 只放门后调用的内核，不得有静态初始化器。算法逐字未动。

#include "intersect_kernels.hpp"

#if BITCASK_X86_64
#include <immintrin.h>

namespace bitcask::bm25::kernels {
namespace {
// ── AVX2 内核（block=4，permute4x64 立即数旋转 + 条件提取）──────────────
// 压缩段维持分支形式；分支 vs PairLut 无分支版的选型待实测
// （doc/intersect-kernel-internals-zh.md §3）。

std::uint64_t* exact_match_u64_avx2(const std::uint64_t* a,
                                    const std::uint64_t* b,
                                    std::uint64_t* cur) {
    // S37-4：原为 `const __m256i_u*`。`__m256i_u` 是 GCC/Clang 私有的
    // 「对齐要求为 1 的 __m256i」别名，MSVC 无此类型。改用标准形参类型
    // `const __m256i*`——_mm256_loadu_si256 的**语义本就是非对齐装载**
    // （Intel intrinsics guide 的原型即为此），对齐豁免由 intrinsic 自身
    // 保证，不靠指针类型。GCC/Clang 侧生成的指令不变（同为 vmovdqu）。
    const __m256i va =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a));
    const __m256i vb =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b));

    const __m256i cmp01 = _mm256_or_si256(
        _mm256_cmpeq_epi64(va, vb),
        _mm256_cmpeq_epi64(va,
                           _mm256_permute4x64_epi64(vb, 0x39)));
    const __m256i cmp23 = _mm256_or_si256(
        _mm256_cmpeq_epi64(
            va, _mm256_permute4x64_epi64(vb, 0x4E)),
        _mm256_cmpeq_epi64(
            va, _mm256_permute4x64_epi64(vb, 0x93)));
    const __m256i cmp = _mm256_or_si256(cmp01, cmp23);

    const unsigned mask = static_cast<unsigned>(
        _mm256_movemask_pd(_mm256_castsi256_pd(cmp)));

    if (mask & 1u) *cur++ = a[0];
    if (mask & 2u) *cur++ = a[1];
    if (mask & 4u) *cur++ = a[2];
    if (mask & 8u) *cur++ = a[3];
    return cur;
}

}  // namespace
std::uint64_t* intersect_inoue_avx2(const std::uint64_t* a, std::size_t na,
                                    const std::uint64_t* b, std::size_t nb,
                                    std::uint64_t* cur) {
    constexpr std::size_t B = 4;
    std::size_t i = 0;
    std::size_t j = 0;

    while (i + B <= na && j + B <= nb) {
        if (a[i + B - 1] < b[j]) { i += B; continue; }
        if (b[j + B - 1] < a[i]) { j += B; continue; }

        cur = exact_match_u64_avx2(a + i, b + j, cur);

        const std::uint64_t amax = a[i + B - 1];
        const std::uint64_t bmax = b[j + B - 1];
        if (amax <= bmax) i += B;
        if (bmax <= amax) j += B;
    }

    return kernels::intersect_scalar(a + i, na - i, b + j, nb - j, cur);
}
}  // namespace bitcask::bm25::kernels

#endif  // BITCASK_X86_64
