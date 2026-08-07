// bitcask/hw_crc32.hpp — CRC32 IEEE 802.3 (polynomial 0xEDB88320) with
// PCLMULQDQ hardware acceleration.
//
// Three tiers, picked at runtime via bitcask::simd::cpu_features (S37-3;
// 自实现 CPUID + XCR0 门，MSVC 通用，受 BITCASK_SIMD_MAX 钳制):
//   1. SSE4.2 + PCLMULQDQ: 16-byte-at-a-time carryless folding. ~10 GB/s on
//      modern x86. Used for inputs >= 16 bytes (after head-alignment).
//   2. Slice-by-1 bytewise table: head-alignment bytes + tail < 16 bytes.
//      Same polynomial as zlib; always bit-identical to zlib::crc32().
//   3. zlib ::crc32() — portable scalar fallback for non-x86.
//
// === Streaming / incrementality ===
// Public API matches bitcask::codec::crc32_update:
//   seed is the previous CRC32 result (zlib format: invert at start, invert
//   at end). Returns the CRC32 of (seed_so_far || data) in zlib format.
//
// Internal computation works on the non-inverted polynomial state, because
// that's what PCLMULQDQ folding expects. The wrapper does the initial/final
// inversion around the call.
//
// === Correctness ===
// All three paths produce bit-identical results to zlib's ::crc32() on every
// length from 0 to a few KiB. The benchmark's startup self-test enforces this.
//
// === Thread safety ===
// All functions are pure (read-only on the input span) and noexcept. Safe to
// call concurrently from multiple threads.

#pragma once

#include <algorithm>
#include "bitcask/detail/cpu_features.hpp"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "bitcask/detail/cpu_features.hpp"

#include <zlib.h>

namespace bitcask::hw {

// ---------------------------------------------------------------------------
// Scalar (bytewise) CRC32 — IEEE 802.3 reflected polynomial 0xEDB88320.
// Used for short inputs (< 16 bytes) and for head/tail bytes around the
// PCLMULQDQ fast path. Bit-identical to zlib::crc32().
// ---------------------------------------------------------------------------
namespace detail {

// Standard reflected CRC32 lookup table: t[i] = CRC32(i). Generated at
// compile time from the polynomial 0xEDB88320 (see gen_crc_table logic in
// the test setup). alignas(64) so an aggressive compiler can vectorize a
// table-driven inner loop if it ever wants to.
inline constexpr std::array<std::uint32_t, 256> kCrc32Table = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu, 0xE963A535u, 0x9E6495A3u,
    0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u, 0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u,
    0x1DB71064u, 0x6AB020F2u, 0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u, 0xFA0F3D63u, 0x8D080DF5u,
    0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u, 0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu,
    0x35B5A8FAu, 0x42B2986Cu, 0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u, 0xCFBA9599u, 0xB8BDA50Fu,
    0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u, 0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du,
    0x76DC4190u, 0x01DB7106u, 0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du, 0x91646C97u, 0xE6635C01u,
    0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu, 0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u,
    0x65B0D9C6u, 0x12B7E950u, 0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u, 0xA4D1C46Du, 0xD3D6F4FBu,
    0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u, 0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u,
    0x5005713Cu, 0x270241AAu, 0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u, 0xB7BD5C3Bu, 0xC0BA6CADu,
    0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au, 0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u,
    0xE3630B12u, 0x94643B84u, 0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu, 0x196C3671u, 0x6E6B06E7u,
    0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu, 0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u,
    0xD6D6A3E8u, 0xA1D1937Eu, 0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u, 0x316E8EEFu, 0x4669BE79u,
    0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u, 0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu,
    0xC5BA3BBEu, 0xB2BD0B28u, 0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu, 0x72076785u, 0x05005713u,
    0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u, 0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u,
    0x86D3D2D4u, 0xF1D4E242u, 0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u, 0x616BFFD3u, 0x166CCF45u,
    0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u, 0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu,
    0xAED16A4Au, 0xD9D65ADCu, 0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u, 0x54DE5729u, 0x23D967BFu,
    0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u, 0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
};

// Apply CRC32 IEEE 802.3 to (buf, len) starting from internal state
// `crc_internal` (the polynomial state zlib would see as `seed ^ 0xFFFFFFFF`
// before processing its first byte — i.e., `0xFFFFFFFF` for a fresh call).
// Returns the new internal state in the same convention.
//
// Matches zlib's table-driven base (crc32_l / crc32_z) modulo the inversion
// convention. zlib inverts input and output around its base, so its internal
// `c` accumulator sees:
//   c = (zlib_seed ^ 0xFFFFFFFF) initially;
//   for each byte b: c = table[(c ^ b) & 0xFF] ^ (c >> 8);
//   return c ^ 0xFFFFFFFF;
// Our `crc_internal` IS zlib's internal `c` — no further inversion here.
inline std::uint32_t crc32_bytewise(std::uint32_t crc_internal,
                                    const std::byte* buf,
                                    std::size_t len) noexcept {
    std::uint32_t c = crc_internal;
    const auto* p = reinterpret_cast<const std::uint8_t*>(buf);
    // Process 8 bytes per iteration when possible — better i-cache behavior
    // on the small 1KB table (still tiny but halves loop overhead).
    while (len >= 8) {
        c = kCrc32Table[(c ^ p[0]) & 0xFF] ^ (c >> 8);
        c = kCrc32Table[(c ^ p[1]) & 0xFF] ^ (c >> 8);
        c = kCrc32Table[(c ^ p[2]) & 0xFF] ^ (c >> 8);
        c = kCrc32Table[(c ^ p[3]) & 0xFF] ^ (c >> 8);
        c = kCrc32Table[(c ^ p[4]) & 0xFF] ^ (c >> 8);
        c = kCrc32Table[(c ^ p[5]) & 0xFF] ^ (c >> 8);
        c = kCrc32Table[(c ^ p[6]) & 0xFF] ^ (c >> 8);
        c = kCrc32Table[(c ^ p[7]) & 0xFF] ^ (c >> 8);
        p += 8;
        len -= 8;
    }
    for (std::size_t i = 0; i < len; ++i) {
        c = kCrc32Table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c;
}

#if BITCASK_X86_64
// S37-3.b：PCLMULQDQ 折叠内核的**定义**已移出本头，进 src/simd/crc32_sse42.cpp
// （由 bitcask_simd_tu(... sse42) 施加 -msse4.2 -mpclmul；MSVC 下 SSE4.2/PCLMUL
// intrinsic 无需开关，故不加）。
//
// 移出原因同 bm25_kernels：内核原带 [[gnu::target("sse4.2,pclmul")]]，MSVC
// 不支持；而本头被 codec.cpp 等多个 TU 包含，内联在头里就无法对它们分别
// 施加 ISA 开关。
//
// 调用方必须先过 has_pclmul_crc32() 门。
//
// 语义契约不变：返回**非取反**的内部 CRC 状态；结果与 zlib::crc32() 逐位相同。
std::uint32_t crc32_pclmul(std::uint32_t crc_internal, const std::byte* buf,
                           std::size_t len) noexcept;
// nblocks16 = 16 字节块数（**不是**字节数）——与拆分前签名一致。
std::uint32_t crc32_pclmul_small(std::uint32_t crc_internal,
                                 const std::byte* buf,
                                 std::size_t nblocks16) noexcept;

// True iff CPU supports both sse4.2 and pclmul. Cached after first call.
inline bool has_pclmul_crc32() noexcept {
    // S37-3：经 simd::cpu_features（自实现 CPUID，MSVC 通用；受
    // BITCASK_SIMD_MAX 钳制，可强制降档做跨档对拍）。
    return simd::have_sse42_pclmul();
}
#endif  // BITCASK_X86_64

}  // namespace detail

// ---------------------------------------------------------------------------
// Public API (matches bitcask::codec::crc32 / crc32_update signatures).
// ---------------------------------------------------------------------------

// Streaming CRC32. `seed` is the previous result (zlib format: invert-at-
// start, invert-at-end). Returns CRC32(seed||data) in zlib format.
inline std::uint32_t crc32_update(std::uint32_t seed,
                                  std::span<const std::byte> data) noexcept {
    if (data.empty()) return seed;

    // zlib's ::crc32 handles the inversion in/out for us. Use it for very
    // short inputs to avoid the PCLMULQDQ setup overhead.
    if (data.size() < 16) {
        return static_cast<std::uint32_t>(
            ::crc32(static_cast<uLong>(seed),
                    reinterpret_cast<const Bytef*>(data.data()),
                    static_cast<uInt>(data.size())));
    }

    const std::byte* p   = data.data();
    std::size_t      len = data.size();

    // Internal (non-inverted) state corresponding to zlib's seed.
    const std::uint32_t inv_seed = ~seed;

#if BITCASK_X86_64
    if (detail::has_pclmul_crc32()) {
        std::uint32_t c = inv_seed;

        // S29-10：16..63 字节走小块内核——无对齐步骤（loadu 不要求对齐;
        // 原实现头对齐会把小块拆碎到全 bytewise），整 16B 块单折叠 CLMUL,
        // 尾部 <16B bytewise。流式增量（hint/WAL 帧）的主命中路径。
        if (len < 64) {
            const std::size_t nblk = len / 16;  // ≥1（<16 已在上面走 zlib）
            c = detail::crc32_pclmul_small(c, p, nblk);
            p   += nblk * 16;
            len -= nblk * 16;
            if (len) c = detail::crc32_bytewise(c, p, len);
            return ~c;
        }

        // Step A: process 0..15 leading bytes bytewise to align p to 16.
        // Mirrors the Linux kernel's crc32_le_arch() head-alignment step.
        const std::uintptr_t misalign =
            (16u - (reinterpret_cast<std::uintptr_t>(p) & 15u)) & 15u;
        if (misalign) {
            const std::size_t take = std::min<std::size_t>(misalign, len);
            c = detail::crc32_bytewise(c, p, take);
            p   += take;
            len -= take;
        }
        // Step B: PCLMULQDQ on the aligned body. The hardware kernel
        // requires at least 64 bytes (Step 1 reads 4 × 16 unconditionally).
        // 对齐消耗后小于 64 的 body（48..63）走小块内核（同上）。
        if (len >= 64) {
            const std::size_t body = len & ~std::size_t{15};
            c = detail::crc32_pclmul(c, p, body);
            p   += body;
            len -= body;
        } else if (len >= 16) {
            const std::size_t nblk = len / 16;
            c = detail::crc32_pclmul_small(c, p, nblk);
            p   += nblk * 16;
            len -= nblk * 16;
        }
        // Step C: trailing bytes (< 16) handled bytewise.
        if (len) {
            c = detail::crc32_bytewise(c, p, len);
        }
        return ~c;
    }
#endif

    // Generic x86 / non-x86: bytewise path. Equivalent to zlib for all sizes.
    return static_cast<std::uint32_t>(
        ::crc32(static_cast<uLong>(seed),
                reinterpret_cast<const Bytef*>(data.data()),
                static_cast<uInt>(data.size())));
}

// One-shot CRC32 of a single span.
inline std::uint32_t crc32(std::span<const std::byte> data) noexcept {
    return crc32_update(0u, data);
}

}  // namespace bitcask::hw