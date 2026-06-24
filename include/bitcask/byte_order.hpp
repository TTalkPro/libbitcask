// 小端 load/store 辅助。统一盘格式字节序为小端(LE)：全引擎 LE-only 主机
// (x86/ARM64)，原生零转换 + mmap 零拷贝友好。位移实现与主机字节序无关
// (LE 主机上编译器优化为单条 mov)。
//
// 历史：record/hint 曾为大端(对齐 Erlang <<X:N>>)；flag-day 全切 LE 后，
// 旧大端文件不可读(需重建)，见 doc/format-zh.md。
#pragma once

#include <cstddef>
#include <cstdint>

namespace bitcask {

constexpr void le_store_u16(std::byte* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::byte>(v & 0xFF);
    p[1] = static_cast<std::byte>((v >> 8) & 0xFF);
}
constexpr void le_store_u32(std::byte* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::byte>(v & 0xFF);
    p[1] = static_cast<std::byte>((v >> 8) & 0xFF);
    p[2] = static_cast<std::byte>((v >> 16) & 0xFF);
    p[3] = static_cast<std::byte>((v >> 24) & 0xFF);
}
constexpr void le_store_u64(std::byte* p, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[static_cast<std::size_t>(i)] =
            static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
    }
}
constexpr std::uint16_t le_load_u16(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(p[0]) |
        (static_cast<std::uint16_t>(p[1]) << 8));
}
constexpr std::uint32_t le_load_u32(const std::byte* p) noexcept {
    return  static_cast<std::uint32_t>(p[0])        |
           (static_cast<std::uint32_t>(p[1]) << 8)  |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
constexpr std::uint64_t le_load_u64(const std::byte* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[static_cast<std::size_t>(i)])
             << (8 * i);
    }
    return v;
}

}  // namespace bitcask
