// cpu_features 实现（S37-3）。契约与告警见 detail/cpu_features.hpp。

#include "bitcask/detail/cpu_features.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if BITCASK_X86_64
#  if defined(_MSC_VER)
#    include <intrin.h>     // __cpuidex / _xgetbv
#  else
#    include <cpuid.h>      // __cpuid_count
#  endif
#endif

namespace bitcask::simd {

namespace {

#if BITCASK_X86_64

struct Regs {
    std::uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
};

Regs cpuid(std::uint32_t leaf, std::uint32_t subleaf) noexcept {
    Regs r;
#if defined(_MSC_VER)
    int out[4] = {0, 0, 0, 0};
    __cpuidex(out, static_cast<int>(leaf), static_cast<int>(subleaf));
    r.eax = static_cast<std::uint32_t>(out[0]);
    r.ebx = static_cast<std::uint32_t>(out[1]);
    r.ecx = static_cast<std::uint32_t>(out[2]);
    r.edx = static_cast<std::uint32_t>(out[3]);
#else
    __cpuid_count(leaf, subleaf, r.eax, r.ebx, r.ecx, r.edx);
#endif
    return r;
}

std::uint32_t max_leaf() noexcept { return cpuid(0, 0).eax; }

// XGETBV(0) —— 读 XCR0。
//
// GCC/Clang 侧用内联汇编而非 _xgetbv 内建：后者要求 -mxsave，而本 TU 是
// 通用构建（无任何 -m 开关），加开关会让编译器有权在别处也生成 XSAVE 系
// 指令。内联汇编无此副作用。
std::uint64_t xgetbv0() noexcept {
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    std::uint32_t lo = 0, hi = 0;
    __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
#endif
}

constexpr bool bit(std::uint32_t v, unsigned n) noexcept {
    return ((v >> n) & 1u) != 0u;
}

// XCR0 位：1 = XMM(SSE)，2 = YMM(AVX)，5 = opmask，6 = ZMM_Hi256，7 = Hi16_ZMM
constexpr std::uint64_t kXcr0Ymm  = 0x6;   // bits 1|2
constexpr std::uint64_t kXcr0Zmm  = 0xE6;  // bits 1|2|5|6|7

#endif  // BITCASK_X86_64

// --- BITCASK_SIMD_MAX 解析 ------------------------------------------------

struct TierName {
    std::string_view name;
    IsaTier tier;
};
constexpr TierName kTierNames[] = {
    {"scalar",      IsaTier::kScalar},
    {"sse42",       IsaTier::kSse42},
    {"avx2",        IsaTier::kAvx2},
    {"avxvnni",     IsaTier::kAvxVnni},
    {"avx512",      IsaTier::kAvx512},
    {"avx512vnni",  IsaTier::kAvx512Vnni},
};

// 读环境变量的钳制上限。未设 → 不钳（返回最高档）。
//
// **无法识别的值一律 abort，不做「警告后忽略」**：本开关的主要用途是 CI 的
// ISA 矩阵，一个拼错的档位若被静默忽略，那个 job 就会在满档下跑却显示为
// 在测低档——即「测了个寂寞还报绿」。宁可让它红。
IsaTier read_clamp() noexcept {
    const char* env = std::getenv("BITCASK_SIMD_MAX");
    if (env == nullptr || env[0] == '\0') return IsaTier::kAvx512Vnni;
    IsaTier t{};
    if (testing::parse_tier(env, &t)) return t;
    std::fprintf(stderr,
                 "BITCASK_SIMD_MAX='%s' 无法识别。可选："
                 "scalar|sse42|avx2|avxvnni|avx512|avx512vnni\n", env);
    std::abort();
}

Features probe_and_clamp() noexcept {
    Features f = testing::probe_raw();
    const IsaTier cap = read_clamp();
    const auto lt = [cap](IsaTier need) {
        return static_cast<unsigned>(cap) < static_cast<unsigned>(need);
    };
    // 逐档下钳。高档蕴含低档，故从高到低依次清零即可。
    if (lt(IsaTier::kAvx512Vnni)) f.avx512vnni = false;
    if (lt(IsaTier::kAvx512)) {
        f.avx512f = f.avx512cd = f.avx512bw = f.avx512dq = f.avx512vl = false;
        f.avx512vnni = false;
    }
    if (lt(IsaTier::kAvxVnni)) f.avx_vnni = false;
    if (lt(IsaTier::kAvx2)) f.avx2 = f.fma = false;
    if (lt(IsaTier::kSse42)) f.sse42 = f.pclmul = false;
    return f;
}

}  // namespace

namespace testing {

bool parse_tier(std::string_view name, IsaTier* out) noexcept {
    for (const auto& e : kTierNames) {
        if (e.name == name) {
            *out = e.tier;
            return true;
        }
    }
    return false;
}

bool os_supports_avx() noexcept {
#if BITCASK_X86_64
    const Regs l1 = cpuid(1, 0);
    if (!bit(l1.ecx, 27)) return false;  // OSXSAVE：XGETBV 本身是否可用
    return (xgetbv0() & kXcr0Ymm) == kXcr0Ymm;
#else
    return false;
#endif
}

bool os_supports_avx512() noexcept {
#if BITCASK_X86_64
    const Regs l1 = cpuid(1, 0);
    if (!bit(l1.ecx, 27)) return false;
    return (xgetbv0() & kXcr0Zmm) == kXcr0Zmm;
#else
    return false;
#endif
}

Features probe_raw() noexcept {
    Features f;
#if BITCASK_X86_64
    const std::uint32_t maxl = max_leaf();
    if (maxl < 1) return f;

    const Regs l1 = cpuid(1, 0);
    f.pclmul = bit(l1.ecx, 1);
    f.sse42  = bit(l1.ecx, 20);
    // SSE4.2 / PCLMULQDQ 用 XMM——x86-64 基线即含 SSE2，OS 必然保存 XMM，
    // 无需 XCR0 检查（这也是 __builtin_cpu_supports 的行为）。

    // FMA 与 AVX2 走 YMM ⇒ 必须过 OS 状态门。
    const bool osavx = os_supports_avx();
    f.fma = osavx && bit(l1.ecx, 12);

    if (maxl >= 7) {
        const Regs l7 = cpuid(7, 0);
        f.avx2 = osavx && bit(l7.ebx, 5);

        // AVX-512 走 ZMM + opmask ⇒ 更严的 OS 状态门。
        const bool osavx512 = os_supports_avx512();
        f.avx512f  = osavx512 && bit(l7.ebx, 16);
        f.avx512dq = osavx512 && bit(l7.ebx, 17);
        f.avx512cd = osavx512 && bit(l7.ebx, 28);
        f.avx512bw = osavx512 && bit(l7.ebx, 30);
        f.avx512vl = osavx512 && bit(l7.ebx, 31);
        f.avx512vnni = osavx512 && bit(l7.ecx, 11);

        // AVX-VNNI 在 leaf 7 **subleaf 1** 的 EAX bit 4——与 AVX512-VNNI
        // 完全不同的位置，是本模块最容易抄错的一处。
        if (cpuid(7, 0).eax >= 1) {
            const Regs l71 = cpuid(7, 1);
            f.avx_vnni = osavx && bit(l71.eax, 4);
        }
    }
#endif
    return f;
}

}  // namespace testing

const Features& features() noexcept {
    static const Features f = probe_and_clamp();
    return f;
}

IsaTier active_tier() noexcept {
    const Features& f = features();
    if (f.avx512vnni && f.avx512f && f.avx512cd && f.avx512bw && f.avx512dq &&
        f.avx512vl) {
        return IsaTier::kAvx512Vnni;
    }
    if (f.avx512f && f.avx512cd && f.avx512bw && f.avx512dq && f.avx512vl) {
        return IsaTier::kAvx512;
    }
    if (f.avx_vnni) return IsaTier::kAvxVnni;
    if (f.avx2 && f.fma) return IsaTier::kAvx2;
    if (f.sse42 && f.pclmul) return IsaTier::kSse42;
    return IsaTier::kScalar;
}

std::string_view tier_name(IsaTier t) noexcept {
    for (const auto& e : kTierNames) {
        if (e.tier == t) return e.name;
    }
    return "unknown";
}

}  // namespace bitcask::simd
