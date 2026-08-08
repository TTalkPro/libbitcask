// cpu_features — 运行时 ISA 探测与档位钳制（S37-3 第一步）。
//
// === 为什么要自己写 ===
// 现有派发全部用 `__builtin_cpu_supports("avx2")` 一类的 GCC/Clang 内建，
// **MSVC 完全不支持**（既无该内建，也无 `__attribute__((target))`）。本模块
// 是那 18 处调用的统一替代品，用 CPUID/XGETBV 自实现，两边编译器通用。
//
// === 最容易写错的地方：OS 状态支持位 ===
// `__builtin_cpu_supports` 会**替调用方检查 XCR0**——CPU 报告支持 AVX 不等于
// 能用：还需要 OS 在 XSAVE 区里为 YMM（XCR0[2:1]）/ ZMM（XCR0[7:5]）分配了
// 保存空间。漏掉这步，代码会在「CPU 支持但 OS / hypervisor 未启用宽寄存器
// 保存」的机器上直接 #UD 崩溃，而且**只在特定虚拟化环境复现**——本地和常规
// CI 全绿，线上偶发。本模块显式做这层检查，并由 cpu_features_test 对拍。
//
// === AVX-512 的门比 avx512f 更严 ===
// 代码历史上只查 `avx512f`。但 MSVC 的 `/arch:AVX512` 隐含
// **F + CD + BW + DQ + VL**——编译器可能在该 TU 的胶水代码里自动生成
// BW/DQ/VL 指令，而运行时门只放行了 F ⇒ 在仅 F+CD 的 CPU（Xeon Phi
// KNL/KNM 一类）上 #UD。故本模块的 AVX-512 档要求整个集合齐备。
// 现代 Xeon（Skylake-X 起）均满足，实际不损失覆盖。
//
// === BITCASK_SIMD_MAX：强制降档 ===
// 环境变量，取值 scalar|sse42|avx2|avxvnni|avx512|avx512vnni。
// **只能下调、不能上调**（不可能凭空造出硬件没有的指令）。
//
// 存在理由：本仓库的 SIMD 覆盖此前完全取决于跑测试那台机器的 CPU 型号——
// 例如开发机与 CI runner 都没有 AVX-512，意味着 AVX-512 内核**从未被执行
// 过**，改错了也不会红。有了本开关，同一台机器就能把 scalar / sse42 / avx2
// 各档都跑一遍做对拍。
//
// 注意本开关**救不了上层档位**：它只能往下钳。要覆盖 AVX-512，仍须在带
// AVX-512 的机器上跑（S37-7 的 CI 矩阵）。

#ifndef BITCASK_DETAIL_CPU_FEATURES_HPP
#define BITCASK_DETAIL_CPU_FEATURES_HPP

#include <cstdint>
#include <string_view>

// ---------------------------------------------------------------------------
// 可移植宏（S37-3.b）。原先散在 25 处的
//   #if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
// 有两个问题：① MSVC 用 `_M_X64` 而非 `__x86_64__`，② 把「是不是 x86-64」
// 和「是不是 GCC/Clang」搅在一起——后者的存在只是因为内核用了 GCC 扩展，
// 扩展消除后这个条件就不该再有。
// ---------------------------------------------------------------------------
#if defined(__x86_64__) || defined(_M_X64)
#  define BITCASK_X86_64 1
#else
#  define BITCASK_X86_64 0
#endif

#if defined(_MSC_VER)
#  define BITCASK_NOINLINE __declspec(noinline)
#else
#  define BITCASK_NOINLINE __attribute__((noinline))
#endif

// 关闭指定 sanitizer 检查。MSVC 无对应物，展开为空。
#if defined(_MSC_VER)
#  define BITCASK_NO_SANITIZE(what)
#else
#  define BITCASK_NO_SANITIZE(what) __attribute__((no_sanitize(what)))
#endif

// ---------------------------------------------------------------------------
// BITCASK_TSAN_ENABLED —— 是否在 TSan 插桩构建下（1/0）。
//
// S37-4：替代原先散在 8 个文件里的
//   #if defined(__SANITIZE_THREAD__)
//       || (defined(__has_feature) && __has_feature(thread_sanitizer))
//
// （引文刻意**去掉了原来的行尾续行反斜杠**：`\` 出现在 `//` 行末会把下一行
//  拼进本注释——即 `-Wcomment`，而 CI 的 werror-lib job 带 `-Werror`，于是这
//  条注释本身会让 Linux 库构建失败。本头经 bitcask_format PUBLIC 传播，一处
//  中招就是几十个 TU 中招。写成两行、`||` 前置，语义不变且不再需要反斜杠。）
//
// **那个写法在符合标准的预处理器下是语法错误**，不只是「MSVC 方言问题」：
// `__has_feature` 未定义时，标准要求先把整个 #if 表达式做宏替换、把剩余
// 标识符换成 0，于是右半边变成 `0(thread_sanitizer)` —— 一个不合法的表达式。
// GCC/Clang 对 `&&` 右侧宽容（不求值即不报错），MSVC 的 /Zc:preprocessor
// 严格按标准来，报 C1012「unmatched parenthesis」，且错误位置指向 #if 行，
// 与真实原因（探测宏不存在）毫无关联，极难追。
//
// 正确写法是**嵌套** #if：外层先确认 `__has_feature` 存在，内层才调用它。
// TSan 只在 GCC/Clang 上存在（设计稿 §5.3：MSVC 无 TSan），故 MSVC 恒 0。
// ---------------------------------------------------------------------------
#if defined(__SANITIZE_THREAD__)
#  define BITCASK_TSAN_ENABLED 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define BITCASK_TSAN_ENABLED 1
#  else
#    define BITCASK_TSAN_ENABLED 0
#  endif
#else
#  define BITCASK_TSAN_ENABLED 0
#endif

namespace bitcask::simd {

// ISA 档位。单调递增——BITCASK_SIMD_MAX 按序钳制。
enum class IsaTier : unsigned {
    kScalar     = 0,
    kSse42      = 1,  // SSE4.2 + PCLMULQDQ（CRC32 折叠内核）
    kAvx2       = 2,  // AVX2 + FMA
    kAvxVnni    = 3,  // + AVX-VNNI（256 位 int8 点积）
    kAvx512     = 4,  // AVX-512 F+CD+BW+DQ+VL
    kAvx512Vnni = 5,  // + AVX512-VNNI
};

// 已生效的能力位。**三重过滤后的结果**：
//   ① CPUID 报告支持 ② XCR0 表明 OS 已启用相应寄存器状态
//   ③ 未被 BITCASK_SIMD_MAX 钳掉
// 调用方直接用，无需再做任何 OS 支持检查。
struct Features {
    bool sse42       = false;
    bool pclmul      = false;
    bool avx2        = false;
    bool fma         = false;
    bool avx_vnni    = false;
    bool avx512f     = false;
    bool avx512cd    = false;
    bool avx512bw    = false;
    bool avx512dq    = false;
    bool avx512vl    = false;
    bool avx512vnni  = false;
};

// 首次调用时探测并缓存（线程安全：函数局部静态）。
[[nodiscard]] const Features& features() noexcept;

// 生效的最高档位（已含钳制）。
[[nodiscard]] IsaTier active_tier() noexcept;

// 档位名（诊断/测试用）。
[[nodiscard]] std::string_view tier_name(IsaTier t) noexcept;

// --- 派发点用的便捷谓词（语义 = 「这条码路现在可以走吗」）----------------
[[nodiscard]] inline bool have_sse42_pclmul() noexcept {
    const auto& f = features();
    return f.sse42 && f.pclmul;
}
[[nodiscard]] inline bool have_avx2() noexcept { return features().avx2; }
[[nodiscard]] inline bool have_avx2_fma() noexcept {
    const auto& f = features();
    return f.avx2 && f.fma;
}
[[nodiscard]] inline bool have_avx_vnni() noexcept {
    return features().avx_vnni;
}
// AVX-512：整集齐备（见文件头注释——比历史上的「只查 avx512f」更严）。
[[nodiscard]] inline bool have_avx512() noexcept {
    const auto& f = features();
    return f.avx512f && f.avx512cd && f.avx512bw && f.avx512dq && f.avx512vl;
}
[[nodiscard]] inline bool have_avx512_vnni() noexcept {
    return have_avx512() && features().avx512vnni;
}

// --- 测试专用：绕过缓存与钳制，拿裸 CPUID 结果 ---------------------------
// 供 cpu_features_test 与 __builtin_cpu_supports 对拍（验证 CPUID 位解析
// 与 XCR0 检查正确）。生产代码勿用——它不含 BITCASK_SIMD_MAX 钳制。
namespace testing {
[[nodiscard]] Features probe_raw() noexcept;
// XCR0 是否表明 OS 已启用 YMM / ZMM 状态保存。
[[nodiscard]] bool os_supports_avx() noexcept;
[[nodiscard]] bool os_supports_avx512() noexcept;
// 解析档位名；无法识别返回 false。
[[nodiscard]] bool parse_tier(std::string_view name, IsaTier* out) noexcept;
}  // namespace testing

}  // namespace bitcask::simd

#endif  // BITCASK_DETAIL_CPU_FEATURES_HPP
