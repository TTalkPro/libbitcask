// 求交内核 —— AVX-512 档（S37-3.b 自 intersect.cpp 拆出）。
//
// ⚠️ 本 TU 由 bitcask_simd_tu(... avx512) 施加
// -mavx512f -mavx512cd -mavx512bw -mavx512dq -mavx512vl（MSVC: /arch:AVX512）。
// 只放门后调用的内核，不得有静态初始化器。算法逐字未动。
//
// 注：kRot512 是 constexpr 常量表（无动态初始化），放在本 TU 安全。

#include "intersect_kernels.hpp"

#include <bit>   // S37-4：std::popcount。原靠 libstdc++ 的传递包含拿到，
                 // MSVC STL 不传递，须显式包含。

#if BITCASK_X86_64
#include <immintrin.h>

namespace bitcask::bm25::kernels {
namespace {

// ── AVX-512 内核（block=8，permutexvar + cmpeq_mask + compressstoreu）─────

alignas(64) static constexpr std::uint64_t kRot512[8][8] = {
    {0, 1, 2, 3, 4, 5, 6, 7},
    {1, 2, 3, 4, 5, 6, 7, 0},
    {2, 3, 4, 5, 6, 7, 0, 1},
    {3, 4, 5, 6, 7, 0, 1, 2},
    {4, 5, 6, 7, 0, 1, 2, 3},
    {5, 6, 7, 0, 1, 2, 3, 4},
    {6, 7, 0, 1, 2, 3, 4, 5},
    {7, 0, 1, 2, 3, 4, 5, 6},
};

std::uint64_t* exact_match_u64_avx512(const std::uint64_t* a,
                                      const std::uint64_t* b,
                                      std::uint64_t* cur) {
    const __m512i va = _mm512_loadu_si512(a);
    const __m512i vb = _mm512_loadu_si512(b);
    // 对8个int64逐个比较，返回的mask,如果bit设置为1，代表a[i] == b[i]
    __mmask8 cmp = _mm512_cmpeq_epi64_mask(va, vb);
    for (int r = 1; r < 8; ++r) {
        const __m512i ridx = _mm512_load_si512(kRot512[r]);
        const __m512i vbr = _mm512_permutexvar_epi64(ridx, vb);
        // 循环比较的时候A不动，只移动B，这样设置位掩码，就代表A中该位命中了
        cmp |= _mm512_cmpeq_epi64_mask(va, vbr);
    }

    // compressstoreu 按 mask 只写 popcount(cmp) 个元素，预分配缓冲下
    // 无需逐块 resize，也无需 cmp==0 早退分支。
    _mm512_mask_compressstoreu_epi64(cur, cmp, va);
    //通过std::popcount来计算2进制中的1的个数，然后移动指针
    return cur + std::popcount(static_cast<unsigned>(cmp));
}
}  // namespace

std::uint64_t* intersect_inoue_avx512(const std::uint64_t* a, std::size_t na,
                                      const std::uint64_t* b, std::size_t nb,
                                      std::uint64_t* cur) {
    constexpr std::size_t B = 8;
    std::size_t i = 0;
    std::size_t j = 0;

    while (i + B <= na && j + B <= nb) {
        if (a[i + B - 1] < b[j]) { i += B; continue; }
        if (b[j + B - 1] < a[i]) { j += B; continue; }

        cur = exact_match_u64_avx512(a + i, b + j, cur);

        const std::uint64_t amax = a[i + B - 1];
        const std::uint64_t bmax = b[j + B - 1];
        if (amax <= bmax) i += B;
        if (bmax <= amax) j += B;
    }

    return kernels::intersect_scalar(a + i, na - i, b + j, nb - j, cur);
}
}  // namespace bitcask::bm25::kernels

#endif  // BITCASK_X86_64
