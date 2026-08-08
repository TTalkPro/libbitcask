// ---------------------------------------------------------------------------
// 窄路径 ↔ std::filesystem::path 的编码桥（P0 / 遗留项 W4）
//
// 库级约定：**窄路径一律是 UTF-8**。io:: seam 就是这么解的——`win32_file.cpp`
// 的 `widen()` 拿 `CP_UTF8 | MB_ERR_INVALID_CHARS` 严格解码，非法序列直接失败
// 而不退回 ANSI 猜测（设计稿 C8）。
//
// 但 `std::filesystem::path` 在 Windows 上内部是 UTF-16，而它的**窄字符构造**与
// **`string()`** 走的都是 `__std_fs_code_page()`，默认是**系统 ANSI 代码页**。
// 简中 Windows 上实测 `GetACP() == 936`（GBK）。于是这两个方向各错一次、
// 方向相反，实测有三种形态（P0.0，2026-08-08，VS 18 / MSVC 14.51 / CP936）：
//
//   1. UTF-8 字节碰巧也是合法 GBK（如「测试」= E6 B5 8B E8 AF 95）
//      → `fs::path` 静默解成别的宽字符，但 `.string()` 又编回原字节，
//        **两次错误抵消**。所以纯「窄 → path → 窄 → seam」的链路看着是好的。
//        代价是这段路径在 `fs::exists` / `ifstream` 眼里指向的是**另一个名字**。
//   2. UTF-8 字节不是合法 GBK（如「测试库」）
//      → **`fs::path` 构造直接抛 `std::system_error`**
//        （`ERROR_NO_UNICODE_TRANSLATION`）。若构造点在 `noexcept` 函数里，
//        就是 `std::terminate`。
//   3. 路径里混进真·宽来源（`directory_iterator` 的 entry）
//      → `.string()` 编出 GBK，喂给 seam 被 `MB_ERR_INVALID_CHARS` 判非法
//        → `EINVAL`，且 `remove_file` 一类返回 false 而文件仍在。
//
// 本对函数把两个方向都钉死在 UTF-8 上：走 `char8_t` 重载，**由标准保证按
// UTF-8 解释**，既不碰代码页也不需要 `windows.h`。
//
// POSIX 下 `path` 的 value_type 就是 char，`u8string()` 与 `string()` 是同一批
// 字节，两个函数都退化成一次拷贝，行为逐字不变。
//
// ⚠️ 成对使用。只改一头会把「两次错误抵消」拆散，反而把形态 1 从「能用」
// 变成「不能用」——见上表。新代码不要再直接写 `fs::path(窄串)` 或 `.string()`。
//
// ---------------------------------------------------------------------------
// 为什么两个都是 noexcept
//
// 转换本身是会失败的：`from_utf8` 收到非法 UTF-8（孤立续字节、截断序列、
// 超长编码 C0 80、代理区 ED A0 80…）时标准转换会抛 `std::system_error`；
// `to_utf8` 在路径含**非配对代理**时同理——而 Windows 的文件名**允许**
// 非配对代理，这不是理论情况。
//
// 但本库的调用方几乎都是 `noexcept` 或返 `bool`/`expected` 的风格，最典型的
// 是 `detail::fsync_parent_dir`（`noexcept`，且是全库 9 个原子写站点的公共
// 收尾）。让转换抛穿过去就是 `std::terminate`——**换掉了触发条件，没换掉
// 失败形态**。所以这里把失败收敛成「空值」：
//
//   from_utf8(非法) -> 空 path    -> 下游 open/remove 拿空路径，照常失败返错
//   to_utf8(非法)   -> 空 string  -> 同上
//
// 代价是「空输入」与「非法输入」不可区分。对本库无妨——两者下游都是同一
// 条失败路径。若将来某处（如 c_api 的入参校验）需要分辨，再加一个返
// `std::optional` 的 `try_*` 变体，不要把 throw 放回来。
//
// `catch (...)` 也一并吞掉 `bad_alloc`：在 `noexcept` 函数里，返回空值总好过
// 直接 terminate。
// ---------------------------------------------------------------------------

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace bitcask::detail {

// UTF-8 窄串 → path。空串或非法 UTF-8 → 空 path（见上）。
[[nodiscard]] inline std::filesystem::path from_utf8(std::string_view s) noexcept {
    const auto* first = reinterpret_cast<const char8_t*>(s.data());
    try {
        return std::filesystem::path(first, first + s.size());
    } catch (...) {
        return {};
    }
}

// path → UTF-8 窄串。空 path 或不可转换（非配对代理）→ 空串（见上）。
[[nodiscard]] inline std::string to_utf8(const std::filesystem::path& p) noexcept {
    try {
        const std::u8string s = p.u8string();
        return std::string(s.begin(), s.end());
    } catch (...) {
        return {};
    }
}

}  // namespace bitcask::detail
