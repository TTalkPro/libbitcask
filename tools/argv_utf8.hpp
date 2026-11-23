// argv_utf8 — 控制台工具的 argv → UTF-8（2026-11-23 反馈：Windows 上非 ASCII
// 目录下 vec_engine_migrate 报「store in use」）。
//
// 库级约定是「窄路径一律 UTF-8」（include/bitcask/detail/path_utf8.hpp 头注），
// 但 Windows 上 `main(int, char**)` 的窄 argv 是 CRT 按**系统 ANSI 代码页**转出
// 来的（简中 = 936 / GBK）。工具把它原样交给库，io 层按 UTF-8 严格解码
// （`MB_ERR_INVALID_CHARS`）⇒ EINVAL：锁拿不到、meta 读不了，而报文往往指向
// 别的方向。
//
// 所以凡是「argv 里的路径会进库」的工具，入口都过一遍这里：把 ACP 字节按 ACP
// 解回宽字符，再编成 UTF-8。之后工具内部与库之间只有一种窄编码。
//
// 取舍：
//   - 只在 Windows 上做转换；POSIX 的 argv 本来就是字节串（通常即 UTF-8），
//     原样拷贝，行为逐字不变。
//   - 不走 `wmain` / `CommandLineToArgvW`：前者要改入口签名、后者要多链
//     shell32；而 CRT 已经做过一次 宽 → 窄(ACP)，这里精确地做逆变换即可。
//   - CRT 那一步已经把 ACP 表示不了的字符换成了 `?`，这里救不回来——
//     路径里有这类字符的工具调用本来就没法经窄 argv 传对。
//   - 解不出来（不应发生）时退回原字节：宁可后面报「路径打不开」，也不在这
//     里悄悄换一个参数。
//
// 用法：
//
//     int main(int argc, char** argv) {
//         const auto args = bitcask_tools::args_as_utf8(argc, argv);
//         // args[i] 与 argv[i] 一一对应（含 argv[0]）；argv 本身不动。
//         const std::string_view dir = args[2];
//
// 打印给控制台看的 argv[0] 仍用原值——它只在用法说明里出现，转成 UTF-8 反而
// 会在 GBK 控制台上变乱码。

#pragma once

#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bitcask_tools {

namespace detail {

#if defined(_WIN32)
// ACP 窄串 → UTF-8。解不出来返回 nullopt（不应发生：CRT 刚从宽字符转过来）。
[[nodiscard]] inline std::optional<std::string> acp_to_utf8(const char* s) {
    const int wlen = ::MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (wlen <= 0) return std::nullopt;
    std::wstring w(static_cast<std::size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), wlen);
    const int ulen = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                           w.c_str(), -1, nullptr, 0, nullptr,
                                           nullptr);
    if (ulen <= 0) return std::nullopt;
    std::string u(static_cast<std::size_t>(ulen), '\0');
    ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.c_str(), -1,
                          u.data(), ulen, nullptr, nullptr);
    u.resize(static_cast<std::size_t>(ulen - 1));  // 去掉结尾的 '\0'
    return u;
}
#endif

}  // namespace detail

// 与 argv 一一对应的 UTF-8 拷贝（含 argv[0]）。不抛：每个元素要么是转换
// 结果，要么是原字节的拷贝。
[[nodiscard]] inline std::vector<std::string> args_as_utf8(int argc,
                                                           char** argv) {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(argc > 0 ? argc : 0));
    for (int i = 0; i < argc; ++i) {
        const char* a = argv[i] != nullptr ? argv[i] : "";
#if defined(_WIN32)
        if (auto u = detail::acp_to_utf8(a)) {
            out.push_back(std::move(*u));
        } else {
            out.emplace_back(a);
        }
#else
        out.emplace_back(a);
#endif
    }
    return out;
}

}  // namespace bitcask_tools
