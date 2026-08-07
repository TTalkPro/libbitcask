// 升序去重 u64 数组求交（Inoue 块过滤 + SIMD 精确匹配）。
// 接口语义见 intersect.hpp。
//
// 写出路径：入口一次性 resize 到结果上界 min(na, nb)，各内核通过裸指针
// 游标写出（消除热循环内 push_back 的容量检查与 size 读改写），结束后
// 一次 resize 截断到实际长度。见 doc/intersect-kernel-internals-zh.md §2。

#include "bitcask/intersect.hpp"
#include "bitcask/detail/cpu_features.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>

#include "intersect_kernels.hpp"  // S37-3.b：SIMD 内核已分 ISA TU

namespace bitcask::bm25 {
namespace {

// S37-3.b：intersect_scalar 已提到 intersect_kernels.hpp（SIMD 内核收尾也用它）。

std::uint64_t* intersect_galloping(const std::uint64_t* s, std::size_t ns,
                                   const std::uint64_t* l, std::size_t nl,
                                   std::uint64_t* cur) {
    std::size_t lo = 0;
    for (std::size_t i = 0; i < ns && lo < nl; ++i) {
        const std::uint64_t v = s[i];
        std::size_t step = 1;
        std::size_t hi = lo;
        while (hi < nl && l[hi] < v) {
            lo = hi + 1;
            hi += step;
            step <<= 1;
        }
        if (hi >= nl) hi = nl - 1;
        if (l[hi] < v) break;
        const auto* it = std::lower_bound(l + lo, l + hi + 1, v);
        lo = static_cast<std::size_t>(it - l);
        if (lo < nl && l[lo] == v) {
            *cur++ = v;
            ++lo;
        }
    }
    return cur;
}

}  // namespace

void intersect_u64(std::span<const std::uint64_t> a,
                   std::span<const std::uint64_t> b,
                   std::vector<std::uint64_t>& out) {
    out.clear();
    if (a.empty() || b.empty()) return;

    // 结果上界 = min(na, nb)。一次性 resize,内核经裸指针游标写出,
    // 末尾截断。resize 的零填充是一趟顺序写,远比每元素一次
    // push_back 容量检查 + size 读改写便宜。
    const std::size_t bound = std::min(a.size(), b.size());
    out.resize(bound);
    std::uint64_t* const base = out.data();
    std::uint64_t* cur = base;

    if (a.size() * 32 < b.size()) {
        cur = intersect_galloping(a.data(), a.size(), b.data(), b.size(), cur);
        out.resize(static_cast<std::size_t>(cur - base));
        return;
    }
    if (b.size() * 32 < a.size()) {
        cur = intersect_galloping(b.data(), b.size(), a.data(), a.size(), cur);
        out.resize(static_cast<std::size_t>(cur - base));
        return;
    }

#if BITCASK_X86_64
    static const bool kHasAvx512f = simd::have_avx512();  // S37-3：整集门
    if (kHasAvx512f) {
        cur = kernels::intersect_inoue_avx512(a.data(), a.size(), b.data(), b.size(),
                                     cur);
        out.resize(static_cast<std::size_t>(cur - base));
        return;
    }
    static const bool kHasAvx2 = simd::have_avx2();  // S37-3
    if (kHasAvx2) {
        cur = kernels::intersect_inoue_avx2(a.data(), a.size(), b.data(), b.size(),
                                   cur);
        out.resize(static_cast<std::size_t>(cur - base));
        return;
    }
#endif
    cur = kernels::intersect_scalar(a.data(), a.size(), b.data(), b.size(), cur);
    out.resize(static_cast<std::size_t>(cur - base));
}

}  // namespace bitcask::bm25
