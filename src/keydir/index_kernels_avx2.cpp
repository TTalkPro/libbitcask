// Index SoA gather 内核 —— AVX2 档（S37-3.b 自 index.cpp 拆出）。
//
// ⚠️ 本 TU 由 bitcask_simd_tu(... avx2) 施加 -mavx2 -mfma（MSVC: /arch:AVX2）。
// 编译器有权在本文件任何函数里生成 AVX2 指令 ⇒ 只放门后调用的内核，
// **不得有静态初始化器**。契约见 CMakeLists.txt 的 bitcask_simd_tu 注释。
//
// 算法逐字未动（gather 宽度、lane 提取方式、尾部标量循环均照搬）。

#include "index_kernels.hpp"

#if BITCASK_X86_64
#include <immintrin.h>

namespace bitcask::index::kernels {

// AVX2 vpgatherdq(_mm256_i64gather_epi64) 一次取 __m256i 索引(8 × 64-bit)
// 但仅消费低 4 个索引、返 4 × 64-bit 值(__m256i 高 128 bit 是无定义垃圾，
// 绝不读)。每轮 4 ords = 1 gather；低 lane 提取低字节写入 out。
// 注：LTO 模式下 _mm256_extract_epi64 会被拆成对 _mm256_extractf128_si256
// 的非立即数调用失败，故走 store + 数组索引（编译为 vmovq）。
void fill_is_live_avx2(const std::uint8_t* live_arr, const std::uint64_t* ords,
                       char* out, std::size_t n) noexcept {
    std::size_t i = 0;
    alignas(32) std::uint64_t lanes[4];
    for (; i + 4 <= n; i += 4) {
        __m256i idx =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ords + i));
        __m256i b = _mm256_i64gather_epi64(
            reinterpret_cast<const long long*>(live_arr), idx, 1);
        _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), b);
        out[i + 0] = static_cast<char>(lanes[0] & 0xFF);
        out[i + 1] = static_cast<char>(lanes[1] & 0xFF);
        out[i + 2] = static_cast<char>(lanes[2] & 0xFF);
        out[i + 3] = static_cast<char>(lanes[3] & 0xFF);
    }
    for (; i < n; ++i) out[i] = static_cast<char>(live_arr[ords[i]]);
}

// AVX2 vpgatherqd(_mm256_i64gather_epi32) 一次取 8 个 64-bit 索引但仅消费
// 低 4 个、返 4 个 32-bit 值(__m128i)。每轮 4 ords 一次 gather。
//
// 拆分前尾部标量循环在调用方（fill_doc_lens）里，与 SIMD 主循环共享游标 i；
// 拆出后尾部一并搬进来——否则调用方仍需知道内核消费到哪一条，接口会渗漏
// SIMD 步长这种实现细节。语义完全等价。
void fill_doc_lens_avx2(const std::uint32_t* dls_arr,
                        const std::uint64_t* ords, std::uint32_t* out,
                        std::size_t n) noexcept {
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256i idx =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ords + i));
        __m128i v = _mm256_i64gather_epi32(
            reinterpret_cast<const int*>(dls_arr), idx, 4);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), v);
    }
    for (; i < n; ++i) out[i] = dls_arr[ords[i]];
}

}  // namespace bitcask::index::kernels

#endif  // BITCASK_X86_64
