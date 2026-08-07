// CRC32 PCLMULQDQ 折叠内核 —— SSE4.2 档（S37-3.b 自 hw_crc32.hpp 拆出）。
//
// ⚠️ 本 TU 由 bitcask_simd_tu(... sse42) 施加 -msse4.2 -mpclmul
// （MSVC 下 SSE4.2/PCLMUL intrinsic 无需 /arch 开关，故不加）。
// 只放门（has_pclmul_crc32()）后调用的内核，不得有静态初始化器。
//
// **算法逐字未动**——CRC 的每一步折叠常量与归约顺序都影响结果，且契约是
// 「与 zlib::crc32() 逐位相同」，任何改动都会破坏已落盘数据的校验。

#include "bitcask/hw_crc32.hpp"

#if BITCASK_X86_64
#include <immintrin.h>
#include <wmmintrin.h>   // _mm_clmulepi64_si128

namespace bitcask::hw::detail {


// ---------------------------------------------------------------------------
// PCLMULQDQ hardware path. Bit-reflected CRC32 IEEE 802.3 (poly 0xEDB88320).
//
// Constants are from Intel's paper "Fast CRC Computation for Generic
// Polynomials Using PCLMULQDQ Instruction" and match the Linux kernel
// arch/x86/crypto/crc32-pclmul_asm.S implementation (kernel 5.10+). All
// values are stored as 64-bit big-endian hex (low bit of exponent is at
// bit 63, after the implicit <<1 for the reflected polynomial).
//
//   k1 (R1): x^(4*128+32) mod P, <<32 reflected & xored = 0x0000000154442bd4
//   k2 (R2): x^(4*128-32) mod P, <<32 reflected & xored = 0x00000001c6e41596
//   k3 (R3): x^(  128+32) mod P, <<32 reflected & xored = 0x00000001751997d0
//   k4 (R4): x^(  128-32) mod P, <<32 reflected & xored = 0x00000000ccaa009e
//   k5 (R5): x^(   64+32) mod P, <<32 reflected & xored = 0x0000000163cd6124
//   mu:      floor(x^64 / P)         reflected          = 0x00000001f7011641
//   poly:    P(x) << 1 reflected                         = 0x00000001db710641
//
// We use 128-bit SSE (_mm_*) intrinsics — no AVX-512, so this works on every
// x86_64 with PCLMULQDQ (Westmere 2010+, Bulldozer 2011+, all Silvermont,
// all current server/consumer chips).
// ---------------------------------------------------------------------------
alignas(16) inline constexpr std::uint8_t kK1K2Bytes[16] = {
    // low qword  = 0x0000000154442bd4  (Intel's k2 = x^(4*128+32) mod P)
    0xd4, 0x2b, 0x44, 0x54, 0x01, 0x00, 0x00, 0x00,
    // high qword = 0x00000001c6e41596  (Intel's k1 = x^(4*128-32) mod P)
    0x96, 0x15, 0xe4, 0xc6, 0x01, 0x00, 0x00, 0x00,
};
alignas(16) inline constexpr std::uint8_t kK3K4Bytes[16] = {
    // low qword  = 0x00000001751997d0  (Intel's k3 = x^(128+32) mod P)
    0xd0, 0x97, 0x19, 0x75, 0x01, 0x00, 0x00, 0x00,
    // high qword = 0x00000000ccaa009e  (Intel's k4 = x^(128-32) mod P)
    0x9e, 0x00, 0xaa, 0xcc, 0x00, 0x00, 0x00, 0x00,
};
alignas(16) inline constexpr std::uint8_t kK5K0Bytes[16] = {
    // low qword  = 0x0000000163cd6124  (Intel's k5 = x^(64+32) mod P)
    0x24, 0x61, 0xcd, 0x63, 0x01, 0x00, 0x00, 0x00,
    // high qword = 0  (unused; k5 fits in the low qword)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
alignas(16) inline constexpr std::uint8_t kMask32Bytes[16] = {
    // mask of 0xFFFFFFFF in the low 32 bits — used both as the bit-mask for
    // the 64→32 fold (simd_crc uses x3 = {~0, 0, ~0, 0}; same mask shape).
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
alignas(16) inline constexpr std::uint8_t kMuPolyBytes[16] = {
    // low qword  = 0x00000001db710641  (P(x) << 1 reflected)
    0x41, 0x06, 0x71, 0xdb, 0x01, 0x00, 0x00, 0x00,
    // high qword = 0x00000001f7011641  (floor(x^64 / P) reflected = mu)
    0x41, 0x16, 0x01, 0xf7, 0x01, 0x00, 0x00, 0x00,
};

// Fold one __m128i lane with (k1, k2) and XOR the next 16 bytes of input.
// `xmm` is in/out; `next` is XOR'd in after the fold. Mirrors the kernel's
// loop_64 step (and simd_crc's main loop), with our constant layout where
// k1k2.low = k2 (high-exponent constant) and k1k2.high = k1 (low-exponent
// constant) — see kK1K2Bytes comment above.
__m128i fold_one(__m128i xmm, __m128i k1k2, __m128i next) noexcept {
    const __m128i lo = _mm_clmulepi64_si128(xmm, k1k2, 0x00);  // xmm.low × k2
    const __m128i hi = _mm_clmulepi64_si128(xmm, k1k2, 0x11);  // xmm.high × k1
    return _mm_xor_si128(_mm_xor_si128(lo, hi), next);
}

// PCLMULQDQ-based CRC32 on an aligned body of length len (must be >= 64 and
// a multiple of 16). Caller must have already processed any leading
// head-alignment bytes via crc32_bytewise. `crc_internal` is the
// non-inverted polynomial state from the previous call (or `~0` for the
// start of a fresh computation).
//
// Returns the new non-inverted internal state.
//
// Algorithm structure mirrors Chromium's crc32_sse42_simd_ (used in zlib):
//   1. Load 4 × 16 bytes; XOR seed into lane 0's low 32 bits.
//   2. Fold next 64-byte blocks in parallel (4 lanes × 16-byte fold).
//   3. Reduce 4 lanes → 1 lane (three 16-byte folds using k3/k4).
//   4. Reduce remaining 16-byte tail blocks to 1 lane.
//   5. 128 → 64 fold (one PCLMULQDQ + shift).
//   6. 64 → 32 fold (mask + PCLMULQDQ + XOR).
//   7. Barrett reduction 64 → 32.
//
// The 64-byte minimum matches the kernel's crc32_pclmul_le_16 contract:
// Step 1 unconditionally reads 4 × 16 bytes, so the body must contain at
// least that much.
std::uint32_t crc32_pclmul(std::uint32_t crc_internal,
                                  const std::byte* buf,
                                  std::size_t len) noexcept {
    const __m128i k1k2   = _mm_load_si128(reinterpret_cast<const __m128i*>(kK1K2Bytes));
    const __m128i k3k4   = _mm_load_si128(reinterpret_cast<const __m128i*>(kK3K4Bytes));
    const __m128i k5     = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(kK5K0Bytes));
    const __m128i mask32 = _mm_load_si128(reinterpret_cast<const __m128i*>(kMask32Bytes));
    const __m128i mupoly = _mm_load_si128(reinterpret_cast<const __m128i*>(kMuPolyBytes));

    // Step 1: load the first 4 × 16 bytes; XOR the seed into lane 0's low
    // 32 bits. (XORing into the low 32 bits, with the rest of the seed_vec
    // zeroed, leaves the rest of xmm1 = the original 16 bytes of input.)
    __m128i xmm1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(buf + 0x00));
    __m128i xmm2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(buf + 0x10));
    __m128i xmm3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(buf + 0x20));
    __m128i xmm4 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(buf + 0x30));
    xmm1 = _mm_xor_si128(xmm1, _mm_cvtsi32_si128(static_cast<int>(crc_internal)));

    const std::byte* p = buf + 0x40;
    std::size_t remaining = len - 0x40;

    // Step 2: 64-byte fold loop. Each iteration folds 4 × 16-byte blocks
    // (already in xmm1..xmm4) and pulls in the next 64 bytes from input.
    while (remaining >= 0x40) {
        xmm1 = fold_one(xmm1, k1k2, _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 0x00)));
        xmm2 = fold_one(xmm2, k1k2, _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 0x10)));
        xmm3 = fold_one(xmm3, k1k2, _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 0x20)));
        xmm4 = fold_one(xmm4, k1k2, _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 0x30)));
        p         += 0x40;
        remaining -= 0x40;
    }

    // Step 3: fold the remaining 3 lanes (xmm2..xmm4) into xmm1 using (k3, k4).
    {
        __m128i tmp;
        tmp  = _mm_clmulepi64_si128(xmm1, k3k4, 0x00);  // xmm1.low × k3
        xmm1 = _mm_clmulepi64_si128(xmm1, k3k4, 0x11);  // xmm1.high × k4
        xmm1 = _mm_xor_si128(xmm1, tmp);
        xmm1 = _mm_xor_si128(xmm1, xmm2);

        tmp  = _mm_clmulepi64_si128(xmm1, k3k4, 0x00);
        xmm1 = _mm_clmulepi64_si128(xmm1, k3k4, 0x11);
        xmm1 = _mm_xor_si128(xmm1, tmp);
        xmm1 = _mm_xor_si128(xmm1, xmm3);

        tmp  = _mm_clmulepi64_si128(xmm1, k3k4, 0x00);
        xmm1 = _mm_clmulepi64_si128(xmm1, k3k4, 0x11);
        xmm1 = _mm_xor_si128(xmm1, tmp);
        xmm1 = _mm_xor_si128(xmm1, xmm4);
    }

    // Step 4: 16-byte tail loop. Each remaining full 16-byte block gets
    // folded into xmm1 with (k3, k4).
    while (remaining >= 0x10) {
        __m128i tmp;
        tmp  = _mm_clmulepi64_si128(xmm1, k3k4, 0x00);
        xmm1 = _mm_clmulepi64_si128(xmm1, k3k4, 0x11);
        xmm1 = _mm_xor_si128(xmm1, tmp);
        xmm1 = _mm_xor_si128(
            xmm1, _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)));
        p         += 0x10;
        remaining -= 0x10;
    }

    // Step 5: 128 → 64 fold. Appends 32 zero bits to the stream; reduces
    // 128 bits to 64. PCLMULQDQ imm=0x10 = (xmm.low × k3k4.high) =
    // (xmm.low × k4); then shift xmm right by 64 bits and XOR the product
    // in. (Kernel's $0x01 form is equivalent under the k3k4 layout used
    // here — see kK3K4Bytes comment.)
    {
        __m128i tmp = _mm_clmulepi64_si128(xmm1, k3k4, 0x10);
        xmm1 = _mm_srli_si128(xmm1, 8);
        xmm1 = _mm_xor_si128(xmm1, tmp);
    }

    // Step 6: 64 → 32 fold. Mask low 32 bits, fold with k5 (R5 sits in the
    // low 64 bits of xmm loaded from k5k0), XOR with the upper 32 bits
    // (shifted into the low 32 of hi32) to get the 32-bit folded state.
    {
        __m128i hi32 = _mm_srli_si128(xmm1, 4);
        __m128i lo32 = _mm_and_si128(xmm1, mask32);
        __m128i fold = _mm_clmulepi64_si128(lo32, k5, 0x00);
        xmm1 = _mm_xor_si128(fold, hi32);
    }

    // Step 7: Barrett reduction 64 → 32. mupoly = {poly, mu}; the fold is
    //   tmp = (state & mask32)              // low 32 bits
    //   tmp = clmul(tmp, mupoly, 0x10)      // (tmp.low × mu)
    //   tmp &= mask32                       // keep low 32 bits of mu * low
    //   tmp = clmul(tmp, mupoly, 0x00)      // (tmp × poly)
    //   result = tmp ^ state
    //   final  = bits 32-63 of result
    {
        const __m128i state = xmm1;
        __m128i tmp = _mm_and_si128(state, mask32);
        tmp = _mm_clmulepi64_si128(tmp, mupoly, 0x10);
        tmp = _mm_and_si128(tmp, mask32);
        tmp = _mm_clmulepi64_si128(tmp, mupoly, 0x00);
        tmp = _mm_xor_si128(tmp, state);
        return static_cast<std::uint32_t>(
            _mm_extract_epi32(tmp, 1));
    }
}

// S29-10：16..63 字节小块 PCLMULQDQ 内核。大内核（crc32_pclmul）Step 1 无条件
// 读 4×16 字节 → 有 64B 下限；流式增量场景（hint/WAL 帧,每次 16-48B）恒落
// bytewise 表路径。本内核处理 nblocks16 个 16 字节块（loadu,**无对齐要求**
// ——小块走头对齐反而会把整块拆碎成 bytewise）：首块 XOR seed,后续块各一次
// (k3,k4) 折叠,收尾与大内核 Step 5-7 完全一致（128→64→32 + Barrett）。
// 返回新的非取反内部状态（同 crc32_pclmul 契约）。
std::uint32_t crc32_pclmul_small(std::uint32_t crc_internal,
                                        const std::byte* buf,
                                        std::size_t nblocks16) noexcept {
    const __m128i k3k4   = _mm_load_si128(reinterpret_cast<const __m128i*>(kK3K4Bytes));
    const __m128i k5     = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(kK5K0Bytes));
    const __m128i mask32 = _mm_load_si128(reinterpret_cast<const __m128i*>(kMask32Bytes));
    const __m128i mupoly = _mm_load_si128(reinterpret_cast<const __m128i*>(kMuPolyBytes));

    __m128i xmm1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(buf));
    xmm1 = _mm_xor_si128(xmm1, _mm_cvtsi32_si128(static_cast<int>(crc_internal)));

    const std::byte* p = buf + 0x10;
    for (std::size_t i = 1; i < nblocks16; ++i) {
        __m128i tmp;
        tmp  = _mm_clmulepi64_si128(xmm1, k3k4, 0x00);
        xmm1 = _mm_clmulepi64_si128(xmm1, k3k4, 0x11);
        xmm1 = _mm_xor_si128(xmm1, tmp);
        xmm1 = _mm_xor_si128(
            xmm1, _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)));
        p += 0x10;
    }

    // 128 → 64 fold（同大内核 Step 5）。
    {
        __m128i tmp = _mm_clmulepi64_si128(xmm1, k3k4, 0x10);
        xmm1 = _mm_srli_si128(xmm1, 8);
        xmm1 = _mm_xor_si128(xmm1, tmp);
    }
    // 64 → 32 fold（同 Step 6）。
    {
        __m128i hi32 = _mm_srli_si128(xmm1, 4);
        __m128i lo32 = _mm_and_si128(xmm1, mask32);
        __m128i fold = _mm_clmulepi64_si128(lo32, k5, 0x00);
        xmm1 = _mm_xor_si128(fold, hi32);
    }
    // Barrett 64 → 32（同 Step 7）。
    {
        const __m128i state = xmm1;
        __m128i tmp = _mm_and_si128(state, mask32);
        tmp = _mm_clmulepi64_si128(tmp, mupoly, 0x10);
        tmp = _mm_and_si128(tmp, mask32);
        tmp = _mm_clmulepi64_si128(tmp, mupoly, 0x00);
        tmp = _mm_xor_si128(tmp, state);
        return static_cast<std::uint32_t>(_mm_extract_epi32(tmp, 1));
    }
}

// True iff CPU supports both sse4.2 and pclmul. Cached after first call.

}  // namespace bitcask::hw::detail

#endif  // BITCASK_X86_64
