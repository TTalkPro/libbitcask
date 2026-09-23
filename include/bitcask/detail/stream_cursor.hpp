// 6.6.0：开库期「整份读进缓冲」的流式替身（feedback 2026-09-23 / keel 转
// coxswain：search 库开库峰值 ≈ 盘上 1.8 倍）。
//
// 两件东西：
//   · crc32_file_range —— 分块定位读算一段文件的 CRC32；
//   · StreamCursor     —— 有界区间 [off, off+len) 上的顺序游标，接口与
//                         keydir 的 SnapCursor 同形（u32/u64/vb/bytes/fail），
//                         解析代码可原样换底座。
//
// 为什么不用 mmap：映射页一经触碰就进本进程工作集（Windows 任务管理器与
// 峰值 RSS 都算它），整段校验 = 整段常驻，直到 unmap 或被 OS 回收。定位读
// 的数据走 OS 文件缓存进一块定长缓冲，进程侧只多这一块。代价是一次
// memcpy——对 CRC / 反序列化这类本就逐字节扫的活可忽略。
//
// 与 chunked_reader.hpp 的分工：那边是 fold 热路径的 refill 原语（调用方
// 自管游标、缓冲是 thread_local），这边是冷启动一次性载入的整套游标，
// 缓冲随对象释放（开库完不留线程堆）。
//
// 线程模型：非线程安全（每次载入一个实例）。

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "bitcask/codec.hpp"
#include "bitcask/io.hpp"

namespace bitcask::detail {

inline constexpr std::size_t kStreamChunkBytes = 1024 * 1024;  // 1 MiB

// 对文件区间 [off, off+len) 分块算 CRC32（seed 语义同 codec::crc32_update；
// seed=0 时结果与 codec::crc32 对整段一次算逐位相同）。读失败 / 短读 → nullopt。
[[nodiscard]] inline std::optional<std::uint32_t>
crc32_file_range(io::FileHandle fd, std::uint64_t off, std::uint64_t len,
                 std::uint32_t seed = 0) {
    std::vector<std::byte> buf(static_cast<std::size_t>(
        std::min<std::uint64_t>(len, kStreamChunkBytes)));
    std::uint32_t crc = seed;
    while (len > 0) {
        const auto n = static_cast<std::size_t>(
            std::min<std::uint64_t>(len, buf.size()));
        if (!io::pread_all(fd, buf.data(), n, off)) return std::nullopt;
        crc = codec::crc32_update(crc, std::span<const std::byte>(buf.data(), n));
        off += n;
        len -= n;
    }
    return crc;
}

class StreamCursor {
public:
    // 句柄借用：cursor 存活期间 fd 须保持打开。
    StreamCursor(io::FileHandle fd, std::uint64_t off, std::uint64_t len,
                 std::size_t chunk = kStreamChunkBytes)
        : fd_(fd), next_(off), left_(len),
          buf_(static_cast<std::size_t>(std::min<std::uint64_t>(len, chunk))) {}

    bool fail = false;  // 越界 / 读错 / vbyte 超长——一次置位，此后读出一律为 0

    // 区间内是否已读尽（缓冲与文件两头都空）。
    [[nodiscard]] bool at_end() const noexcept {
        return pos_ == len_ && left_ == 0;
    }

    std::uint16_t u16() { std::uint16_t v = 0; bytes(&v, 2); return v; }
    std::uint32_t u32() { std::uint32_t v = 0; bytes(&v, 4); return v; }
    std::uint64_t u64() { std::uint64_t v = 0; bytes(&v, 8); return v; }

    // 边界安全 vbyte（同 SnapCursor::vb：超 10 字节 = 损坏）。
    std::uint64_t vb() {
        std::uint64_t v = 0, shift = 0;
        while (true) {
            std::uint8_t byte = 0;
            if (!bytes(&byte, 1)) return 0;
            v |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if (byte & 0x80) return v;
            shift += 7;
            if (shift > 63) { fail = true; return 0; }
        }
    }

    // 读 n 字节到 dst（可跨块）。不足 → fail 且返回 false。
    bool bytes(void* dst, std::size_t n) {
        if (fail) return false;
        auto* out = static_cast<std::byte*>(dst);
        while (n > 0) {
            if (pos_ == len_ && !refill()) { fail = true; return false; }
            const std::size_t k = std::min(n, len_ - pos_);
            std::memcpy(out, buf_.data() + pos_, k);
            pos_ += k;
            out += k;
            n -= k;
        }
        return true;
    }

private:
    bool refill() {
        if (left_ == 0 || buf_.empty()) return false;
        const auto n = static_cast<std::size_t>(
            std::min<std::uint64_t>(left_, buf_.size()));
        if (!io::pread_all(fd_, buf_.data(), n, next_)) return false;
        next_ += n;
        left_ -= n;
        pos_ = 0;
        len_ = n;
        return true;
    }

    io::FileHandle         fd_;
    std::uint64_t          next_;   // 下一次 refill 的文件偏移
    std::uint64_t          left_;   // 区间内尚未读进缓冲的字节数
    std::vector<std::byte> buf_;
    std::size_t            pos_ = 0;
    std::size_t            len_ = 0;
};

}  // namespace bitcask::detail
