// VByte 变长字节编码（Variable Byte Encoding / Varint）。
// Williams & Zobel 1999, "Compressing integers for fast file access".
// 每个字节低 7 位为数据，最高位为延续标记（1=最后一个字节，0=还有更多）。
//
// 编码示例：
//   encode(127) → [0xFF]           // 1 byte：127 < 128，最高位设 1 表示结束
//   encode(128) → [0x00, 0x01]     // 2 bytes：128 >= 128，先输出低 7 位 0x00，继续
//   encode(300) → [0xAC, 0x02]     // 2 bytes：300 = 44*7 + 0xAC(172)，44 = 0x01<<(7-1)
//
// 差值编码（Gap Encoding）：
//   对已排序的 ord 列表应用差分编码后再 VByte 压缩——相邻 ord 的差值通常远小于
//   绝对值，压缩率显著提升。
//   输入：sorted ords [3, 7, 15, 20]（升序）
//   差值：[3, 4, 8, 5]（第一个是绝对值，后续是相邻ord的差）
//   VByte([3, 4, 8, 5]) → 压缩字节序列

#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bitcask::codec {

// VByte 编码：将无符号整数压缩为变长字节序列，追加到 buf 末尾。
// val 必须为无符号整数。S9-P1-b：模板化单字节元素类型，统一服务
// `std::vector<std::uint8_t>`（bm25 WAL/落盘）与 `std::vector<std::byte>`
// （codec DocValue 段）两种缓冲——消除 codec.cpp 原匿名 vbyte_append 的重复。
template <typename Byte>
inline void vbyte_encode(std::uint64_t val, std::vector<Byte>& buf) {
    static_assert(sizeof(Byte) == 1, "vbyte 缓冲元素必须为单字节类型");
    while (val >= 128) {
        buf.push_back(static_cast<Byte>(val & 0x7F));
        val >>= 7;
    }
    buf.push_back(static_cast<Byte>(val | 0x80));
}

// VByte 解码：从 data[pos] 开始读取一个 VByte 编码的无符号整数。
// 返回 {解码值, 新位置}。
//
// **无边界检查**（热路径契约）：caller 须保证 data 在编码完整范围内。
// 用于 bm25 posting 落盘 / 段 mmap 解码等已由段级 CRC 背书的数据；损坏由
// 界检查兜住，单字节无开销。S21-2 A3 既定。
inline std::pair<std::uint64_t, std::size_t> vbyte_decode(const std::uint8_t* data, std::size_t pos) {
    std::uint64_t result = 0;
    std::uint64_t shift = 0;
    while (true) {
        auto byte = data[pos++];
        result |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if (byte & 0x80) break;  // 最高位为 1 表示最后一个字节
        shift += 7;
    }
    return {result, pos};
}

// VByte 解码（带边界检查 + 溢出防御）：从 buf[pos] 读一个 VByte。
// 成功返回 {value, new_pos}；越界或编码过长（shift ≥ 64）返回 std::nullopt。
//
// 与 vbyte_decode 的关系：本函数是**带检查**权威实现，服务 DocValue/meta
// 等不可信输入的解码路径；vbyte_decode 是**无检查**热路径版（段内已 CRC 背书）。
// 两套契约各有定位，不要合并到单一函数（详见 S21-2 A3）。
[[nodiscard]] inline std::optional<std::pair<std::uint64_t, std::size_t>>
vbyte_read_checked(std::span<const std::byte> buf, std::size_t pos) noexcept {
    std::uint64_t result = 0;
    std::uint64_t shift  = 0;
    while (true) {
        if (pos >= buf.size()) return std::nullopt;
        const auto b = static_cast<std::uint8_t>(buf[pos++]);
        result |= static_cast<std::uint64_t>(b & 0x7Fu) << shift;
        if (b & 0x80u) break;
        shift += 7;
        if (shift >= 64) return std::nullopt;  // 防御：超过 u64 的非法编码
    }
    return std::make_pair(result, pos);
}

// Gap 编码：对已排序的 ord 数组做差值编码后 VByte 压缩。
// 输入：sorted ords（升序）
// 输出：VByte 压缩的字节序列
inline std::vector<std::uint8_t> gap_encode(const std::vector<std::uint64_t>& ords) {
    std::vector<std::uint8_t> buf;
    if (ords.empty()) return buf;
    std::uint64_t prev = 0;
    for (auto ord : ords) {
        vbyte_encode(ord - prev, buf);
        prev = ord;
    }
    return buf;
}

// Gap 解码：从 VByte 压缩字节还原 ord 数组。
inline std::vector<std::uint64_t> gap_decode(const std::vector<std::uint8_t>& compressed) {
    std::vector<std::uint64_t> ords;
    if (compressed.empty()) return ords;
    std::size_t pos = 0;
    std::uint64_t prev = 0;
    while (pos < compressed.size()) {
        auto [delta, new_pos] = vbyte_decode(compressed.data(), pos);
        pos = new_pos;
        prev += delta;
        ords.push_back(prev);
    }
    return ords;
}

}  // namespace bitcask::codec