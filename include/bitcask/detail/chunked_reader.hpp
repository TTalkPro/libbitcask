// T23：流式 chunked pread 的共享 refill 基建。
//
// 归并前有三份手抄的 refill（hint_file.cpp v2 fold / fold_v4、
// data_file.cpp fold），`need` 公式已出现漂移（data_file 版丢了
// `buf_len +`，当时无害但证明分别维护不可靠）。本类收敛为唯一实现，
// hint v5 fold 与 data fold 共用；唯一参数化点是文件读取上界 end_bound
// （data fold 用文件总长快照，hint fold 用 body_end = total − trailer）。
//
// 语义：buf 内有效数据区间 [pos, len)，对应文件 [file_off, file_off+avail)。
// refill 把残留字节 memmove 到 buf 头部，再续读一整块 kChunkBytes（或按
// need_hint 扩容——巨型 record 需要单缓冲装下整条）。
//
// 线程模型：非线程安全（独占持有传入的 ThreadLocalBuffer 与游标状态）；
// 典型用法是每个 fold 调用栈一个实例 + thread_local 缓冲。

#pragma once

#include <algorithm>
#include <cstring>
#include <expected>
#include <span>

#include "bitcask/io.hpp"
#include "bitcask/detail/thread_local_buffer.hpp"

namespace bitcask::detail {

class ChunkedReader {
public:
    static constexpr std::size_t kChunkBytes = 256 * 1024;  // 256 KiB

    // end_bound：文件读取上界（绝不读越过它——trailer/总长快照语义）。
    ChunkedReader(io::PosixFile& file, ThreadLocalBuffer& buf,
                  std::uint64_t end_bound) noexcept
        : file_(file), buf_(buf), end_(end_bound) {
        buf_.ensure(kChunkBytes);
    }

    [[nodiscard]] std::size_t avail() const noexcept { return len_ - pos_; }
    [[nodiscard]] const std::byte* cursor() const noexcept {
        return buf_.data() + pos_;
    }
    void consume(std::size_t n) noexcept { pos_ += n; }

    // refill：残留搬到头部 + 续读一块。file_off = cursor() 当前对应的文件
    // 偏移（caller 的消费游标）。need_hint：本轮至少需要的可用字节数
    // （超过 kChunkBytes 时触发缓冲扩容）。返回新读字节数（0 = 到界/EOF）。
    [[nodiscard]] std::expected<std::size_t, io::IoError>
    refill(std::uint64_t file_off, std::size_t need_hint) {
        const std::size_t leftover = len_ - pos_;
        if (leftover > 0 && pos_ > 0) {
            std::memmove(buf_.data(), buf_.data() + pos_, leftover);
        }
        pos_ = 0;
        len_ = leftover;

        // 统一后的公式：残留 + max(一块, 本轮所需)——T23 前 data_file 版
        // 丢了 `len_ +`，此处以 hint 版（正确覆盖巨型 record）为准。
        const std::size_t need =
            std::max(len_ + kChunkBytes, len_ + need_hint);
        buf_.ensure(need);

        const std::uint64_t file_remaining =
            end_ > (file_off + len_) ? end_ - (file_off + len_) : 0;
        if (file_remaining == 0) return static_cast<std::size_t>(0);
        const std::size_t to_read = static_cast<std::size_t>(
            std::min<std::uint64_t>(buf_.size() - len_, file_remaining));
        if (to_read == 0) return static_cast<std::size_t>(0);

        auto n = file_.pread_into(file_off + len_,
                                  std::span(buf_.data() + len_, to_read));
        if (!n) return std::unexpected(n.error());
        len_ += *n;
        return *n;
    }

    // 防线程内存膨胀：一次巨型 record 不应让缓冲永久占住线程堆。
    void shrink() { buf_.maybe_shrink(); }

private:
    io::PosixFile& file_;
    ThreadLocalBuffer& buf_;
    std::uint64_t end_;
    std::size_t pos_ = 0;
    std::size_t len_ = 0;
};

}  // namespace bitcask::detail
