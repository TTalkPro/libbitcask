// Index 的 SoA gather 内核声明（S37-3.b 自 index.cpp 拆出）。
//
// 拆分前这两段 AVX2 代码内联在 `Index::fill_is_live` / `Index::fill_doc_lens`
// 里，而**那两个成员函数整体带 `__attribute__((target("avx2")))` 且被无条件
// 调用**。该形态在原理上不成立：target 属性允许编译器在函数的**任何位置**
// 使用 AVX2，包括运行时门之后才该走的 scalar 回退路径——在无 AVX2 的 CPU
// 上就是 SIGILL。（实测当前 GCC -O3 未真的越界生成，属潜在风险而非现行 bug；
// 但结构上不该依赖编译器的克制。）
//
// 拆到独立 TU 后：宽指令只可能出现在本组内核里，调用方是普通 TU、
// 编译器无权在其中生成 AVX2，风险从「靠编译器自觉」变成「结构上不可能」。
//
// 调用方**必须先过 simd::have_avx2() 门**。

#pragma once

#include <cstddef>
#include <cstdint>

#include "bitcask/detail/cpu_features.hpp"

namespace bitcask::index::kernels {

#if BITCASK_X86_64

// live_arr[ords[i]] 的低字节写入 out[i]，i ∈ [0, n)。
// 前置：所有 ords[i] < live 数组长度（调用方已用 ords.back() < bound 判定）。
void fill_is_live_avx2(const std::uint8_t* live_arr, const std::uint64_t* ords,
                       char* out, std::size_t n) noexcept;

// dls_arr[ords[i]] 写入 out[i]，i ∈ [0, n)。前置同上。
void fill_doc_lens_avx2(const std::uint32_t* dls_arr,
                        const std::uint64_t* ords, std::uint32_t* out,
                        std::size_t n) noexcept;

#endif  // BITCASK_X86_64

}  // namespace bitcask::index::kernels
