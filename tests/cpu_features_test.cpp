// cpu_features 单测（S37-3）。
//
// 本文件的价值集中在一件事：**把手写 CPUID 与 `__builtin_cpu_supports`
// 对拍**。后者是 GCC/Clang 帮我们做过 XCR0 检查的权威实现，MSVC 上没有——
// 所以只有在 GCC/Clang 构建里对拍通过，才有底气在 MSVC 上只留手写版。
//
// 对拍覆盖不到的部分（本机没有的 ISA）由档位钳制测试与 CI 矩阵补。

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "bitcask/detail/cpu_features.hpp"

namespace {

using bitcask::simd::Features;
using bitcask::simd::IsaTier;
namespace simd = bitcask::simd;

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#  define BITCASK_HAVE_CPU_SUPPORTS_BUILTIN 1
#else
#  define BITCASK_HAVE_CPU_SUPPORTS_BUILTIN 0
#endif

}  // namespace

// ---------------------------------------------------------------------------
// ① 与 __builtin_cpu_supports 对拍（GCC/Clang x86-64 上才有参照物）
// ---------------------------------------------------------------------------
#if BITCASK_HAVE_CPU_SUPPORTS_BUILTIN

TEST(CpuFeatures, MatchesCompilerBuiltin) {
    __builtin_cpu_init();
    const Features raw = simd::testing::probe_raw();

    // 逐位对拍。任一条失败都意味着 CPUID 位号抄错，或 XCR0 门写漏/写多。
    EXPECT_EQ(raw.sse42,      __builtin_cpu_supports("sse4.2") != 0)   << "sse4.2";
    EXPECT_EQ(raw.pclmul,     __builtin_cpu_supports("pclmul") != 0)   << "pclmul";
    EXPECT_EQ(raw.avx2,       __builtin_cpu_supports("avx2") != 0)     << "avx2";
    EXPECT_EQ(raw.fma,        __builtin_cpu_supports("fma") != 0)      << "fma";
    EXPECT_EQ(raw.avx512f,    __builtin_cpu_supports("avx512f") != 0)  << "avx512f";
    EXPECT_EQ(raw.avx512cd,   __builtin_cpu_supports("avx512cd") != 0) << "avx512cd";
    EXPECT_EQ(raw.avx512bw,   __builtin_cpu_supports("avx512bw") != 0) << "avx512bw";
    EXPECT_EQ(raw.avx512dq,   __builtin_cpu_supports("avx512dq") != 0) << "avx512dq";
    EXPECT_EQ(raw.avx512vl,   __builtin_cpu_supports("avx512vl") != 0) << "avx512vl";
    EXPECT_EQ(raw.avx512vnni, __builtin_cpu_supports("avx512vnni") != 0)
        << "avx512vnni";
    // AVX-VNNI 在 leaf 7 subleaf 1 EAX bit 4——与 avx512vnni 位置迥异，
    // 是最容易抄错的一处，单列强调。
    EXPECT_EQ(raw.avx_vnni,   __builtin_cpu_supports("avxvnni") != 0)  << "avxvnni";
}

// XCR0 门的方向性：CPU 报告支持 AVX 但 OS 未启用 YMM 保存时，
// __builtin_cpu_supports("avx2") 必为 false——我们不能比它更宽松。
// （无法构造「OS 关掉 AVX」的环境，故只能验蕴含关系。）
TEST(CpuFeatures, OsGateNeverMorePermissiveThanBuiltin) {
    __builtin_cpu_init();
    const Features raw = simd::testing::probe_raw();
    if (raw.avx2) {
        EXPECT_TRUE(simd::testing::os_supports_avx())
            << "报告 avx2 可用却未过 OS YMM 门——XCR0 检查漏了";
    }
    if (raw.avx512f) {
        EXPECT_TRUE(simd::testing::os_supports_avx512())
            << "报告 avx512f 可用却未过 OS ZMM 门——XCR0 检查漏了";
    }
}

#endif  // BITCASK_HAVE_CPU_SUPPORTS_BUILTIN

// ---------------------------------------------------------------------------
// ② 档位单调性与谓词自洽（与本机 CPU 无关，任何机器都跑得动）
// ---------------------------------------------------------------------------

TEST(CpuFeatures, TierImpliesLowerTiers) {
    const auto tier = simd::active_tier();
    // 高档必然蕴含低档的谓词为真——否则派发链会出现「跳档」空洞。
    if (tier >= IsaTier::kAvx512Vnni) { EXPECT_TRUE(simd::have_avx512_vnni()); }
    if (tier >= IsaTier::kAvx512)     { EXPECT_TRUE(simd::have_avx512()); }
    if (tier >= IsaTier::kAvx2)       { EXPECT_TRUE(simd::have_avx2_fma()); }
}

// AVX-512 谓词要求整集齐备（F+CD+BW+DQ+VL），而非只看 avx512f。
// 见 cpu_features.hpp：MSVC 的 /arch:AVX512 隐含整集，门若只查 F 会 #UD。
TEST(CpuFeatures, Avx512GateRequiresFullSet) {
    const Features& f = simd::features();
    if (simd::have_avx512()) {
        EXPECT_TRUE(f.avx512f && f.avx512cd && f.avx512bw && f.avx512dq &&
                    f.avx512vl);
    }
    // 反向：缺任何一位都不得放行。
    if (!f.avx512bw || !f.avx512dq || !f.avx512vl || !f.avx512cd) {
        EXPECT_FALSE(simd::have_avx512());
    }
    EXPECT_FALSE(simd::have_avx512_vnni() && !simd::have_avx512());
}

TEST(CpuFeatures, TierNamesRoundTrip) {
    for (const auto* n : {"scalar", "sse42", "avx2", "avxvnni", "avx512",
                          "avx512vnni"}) {
        IsaTier t{};
        ASSERT_TRUE(simd::testing::parse_tier(n, &t)) << n;
        EXPECT_EQ(simd::tier_name(t), std::string(n));
    }
    IsaTier ignored{};
    EXPECT_FALSE(simd::testing::parse_tier("avx3", &ignored));
    EXPECT_FALSE(simd::testing::parse_tier("", &ignored));
    EXPECT_FALSE(simd::testing::parse_tier("AVX2", &ignored));  // 大小写敏感
}

// BITCASK_SIMD_MAX 只能下调、不能上调。
// features() 缓存在函数局部静态里，进程内只探测一次，故本用例不改环境变量
// 重测（那需要子进程）——改由 simd_tier_matrix.cmake 在 ctest 层跑多档。
// 这里只验「钳制结果不会超过硬件真实能力」这条不变量。
TEST(CpuFeatures, ClampNeverExceedsHardware) {
    const Features raw = simd::testing::probe_raw();
    const Features& eff = simd::features();
    const auto le = [](bool effective, bool hardware) {
        return !effective || hardware;  // effective ⇒ hardware
    };
    EXPECT_TRUE(le(eff.sse42,      raw.sse42));
    EXPECT_TRUE(le(eff.pclmul,     raw.pclmul));
    EXPECT_TRUE(le(eff.avx2,       raw.avx2));
    EXPECT_TRUE(le(eff.fma,        raw.fma));
    EXPECT_TRUE(le(eff.avx_vnni,   raw.avx_vnni));
    EXPECT_TRUE(le(eff.avx512f,    raw.avx512f));
    EXPECT_TRUE(le(eff.avx512vnni, raw.avx512vnni));
}

// 诊断：把本机实际生效的档位打出来。CI 日志里一眼看出这次跑覆盖了哪条码路
// ——此前「测了哪条 ISA 路径」完全不可见，正是 AVX-512 内核长期无人执行
// 却无人察觉的原因。
TEST(CpuFeatures, ReportActiveTier) {
    const char* env = std::getenv("BITCASK_SIMD_MAX");
    std::printf("[cpu_features] active tier = %s (BITCASK_SIMD_MAX=%s)\n",
                std::string(simd::tier_name(simd::active_tier())).c_str(),
                env != nullptr ? env : "<unset>");
    SUCCEED();
}
