#include "bitcask/hint_file.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "bitcask/byte_order.hpp"  // S23-A1: le_load_u32/le_store_u32
#include "bitcask/format.hpp"
#include "bitcask/detail/chunked_reader.hpp"        // T23
#include "bitcask/detail/thread_local_buffer.hpp"   // S9-P1-d

namespace bitcask::fileops {

std::expected<HintFile, DataFileFault>
HintFile::open(std::string_view path, Mode mode, bool sync) {
    using io::OpenFlag;
    OpenFlag flags = OpenFlag::kNone;
    switch (mode) {
        case Mode::kRead:   flags = OpenFlag::kReadOnly; break;
        case Mode::kAppend: flags = OpenFlag::kNone; break;
        case Mode::kCreate: flags = OpenFlag::kCreate; break;
    }
    if (sync && mode != Mode::kRead) flags = flags | OpenFlag::kOSync;

    auto f = io::PosixFile::open(path, flags);
    if (!f) return std::unexpected(io_fault(f.error()));
    HintFile hf(std::move(*f), std::string(path), 0, mode);
    // 新建文件先在缓冲种入 v5 文件头 magic（随首批记录落盘），
    // running CRC 覆盖之（trailer CRC 语义 = [0, size-8) 全字节）。
    // kAppend 不种头（生产零调用；对既有文件追加本就不维护 CRC 连续性）。
    if (mode == Mode::kCreate) {
        const std::size_t base = hf.pending_.size();
        hf.pending_.resize(base + format::kHintHeader);
        le_store_u32(hf.pending_.data() + base, format::kHintMagicV5);
        hf.running_crc_ = codec::crc32_update(
            0, std::span<const std::byte>(hf.pending_.data() + base,
                                          format::kHintHeader));
    }
    return hf;
}

// ---------------------------------------------------------------------------
// 写入
// ---------------------------------------------------------------------------

// 把攒批缓冲一次性落盘（空缓冲 no-op）。
std::expected<void, DataFileFault> HintFile::flush_pending() {
    if (pending_.empty()) return {};
    auto w = file_.write(pending_);
    if (!w) return std::unexpected(io_fault(w.error()));
    pending_.clear();  // 复用容量
    return {};
}

// 追加一条 hint record（先进 pending_ 缓冲，攒满 kFlushBytes 才落盘）。同步把
// 刚编码的字节算进 running_crc_——finalize 用这个累计值生成 trailer，下次 open
// 验文件完整性。encode 是 append 语义，直接写进 pending_ 末尾免一次拷贝。
std::expected<void, DataFileFault>
HintFile::write(std::uint64_t tstamp, std::uint32_t total_sz,
                std::uint64_t offset, bool tombstone,
                std::span<const std::byte> key, std::uint64_t ord) {
    if (mode_ == Mode::kRead) {
        return std::unexpected(DataFileFault{DataFileError::kIo, 0});
    }
    const std::size_t before = pending_.size();
    // v5 变长编码（gap/ord_delta 差分串联经 prev_end_/prev_ord_）。
    codec::encode_hint_record_v5(pending_, tstamp, total_sz, offset,
                                 tombstone, key, ord, prev_end_, prev_ord_);
    running_crc_ = codec::crc32_update(
        running_crc_,
        std::span<const std::byte>(pending_.data() + before,
                                   pending_.size() - before));

    if (pending_.size() >= kFlushBytes) {
        if (auto r = flush_pending(); !r) return r;
    }
    return {};
}

// 写 trailer 并把 running_crc_ 嵌进去，连同缓冲里剩余 record 一次落盘。
// 这是 hint 文件的「封口」操作；没封口的 hint 文件下次 open 会被
// validate_trailer() 判失败，cask 会 fallback 到 fold(data) 重建——慢但可靠。
std::expected<void, DataFileFault> HintFile::finalize() {
    // trailer（8B：magic + running CRC；CRC 覆盖 [0, size-8)）。
    const std::size_t base = pending_.size();
    pending_.resize(base + format::kHintTrailer);
    le_store_u32(pending_.data() + base, format::kHintTrailerMagic);
    le_store_u32(pending_.data() + base + 4, running_crc_);
    return flush_pending();  // 缓冲 record + trailer 一次写
}

std::expected<void, DataFileFault> HintFile::sync() {
    auto s = file_.sync();
    if (!s) return std::unexpected(io_fault(s.error()));
    return {};
}

// ---------------------------------------------------------------------------
// 读取（仅 v5；BCH4 及更早无读端——fold 报 kBadCrc、validate 返回 false，
// caller 一律退 fold(data) 重建）
// ---------------------------------------------------------------------------

std::expected<void, DataFileFault> HintFile::fold(FoldFn fn) {
    auto end = file_.seek(0, SEEK_END);
    if (!end) return std::unexpected(io_fault(end.error()));
    const std::uint64_t total = *end;

    if (total < format::kHintHeader) {
        return std::unexpected(DataFileFault{DataFileError::kBadCrc});
    }
    auto h = file_.pread(0, format::kHintHeader);
    if (!h) return std::unexpected(io_fault(h.error()));
    if (std::holds_alternative<io::ReadEof>(*h) ||
        le_load_u32(std::get<io::ReadOk>(*h).data.data()) !=
            format::kHintMagicV5) {
        return std::unexpected(DataFileFault{DataFileError::kBadCrc});
    }
    return fold_v5(total, std::move(fn));
}

// v5 流式 fold（变长记录：try-decode，字节不足则 refill 重试）。
// body_end：有 trailer 时为 total-8；未封口（崩溃）文件读到 decode 短缺
// 即停（「短读当 EOF」语义）。
std::expected<void, DataFileFault>
HintFile::fold_v5(std::uint64_t total, FoldFn fn) {
    std::uint64_t body_end = total;
    if (total >= format::kHintHeader + format::kHintTrailer) {
        auto t = file_.pread(total - format::kHintTrailer, 4);
        if (!t) return std::unexpected(io_fault(t.error()));
        if (!std::holds_alternative<io::ReadEof>(*t) &&
            le_load_u32(std::get<io::ReadOk>(*t).data.data()) ==
                format::kHintTrailerMagic) {
            body_end = total - format::kHintTrailer;
        }
    }

    // T23：refill 归并进 detail::ChunkedReader（原 v2/v4 两份手抄删除）。
    static thread_local detail::ThreadLocalBuffer buf;
    detail::ChunkedReader rd(file_, buf, body_end);

    std::uint64_t offset = format::kHintHeader;  // 跳过文件头
    std::uint64_t prev_end = 0;
    std::uint64_t prev_ord = 0;
    while (offset < body_end) {
        const std::uint64_t region_left = body_end - offset;
        auto rec = codec::decode_hint_record_v5(
            std::span<const std::byte>(
                rd.cursor(),
                static_cast<std::size_t>(
                    std::min<std::uint64_t>(rd.avail(), region_left))),
            prev_end, prev_ord);
        if (!rec) {
            if (rec.error() != codec::DecodeError::kBufferTooShort) {
                return std::unexpected(DataFileFault{DataFileError::kShortRead});
            }
            auto r = rd.refill(offset, /*need_hint=*/64);
            if (!r) return std::unexpected(io_fault(r.error()));
            if (*r == 0) break;  // 尾部截断（未封口）→ 当 EOF
            continue;            // 重试解码
        }
        fn(*rec);
        offset += rec->consumed;
        rd.consume(rec->consumed);
    }
    rd.shrink();
    return {};
}

// 单独验 trailer CRC：先读末尾 trailer 拿到 expected_crc，再从头流式
// 算一遍前面所有字节的 CRC。两者一致才算 hint 文件健康，可以直接 fold；
// 否则 caller 应该 fall back 到 fold(data_file) 重建。
std::expected<bool, DataFileFault> HintFile::validate_trailer() {
    auto end = file_.seek(0, SEEK_END);
    if (!end) return std::unexpected(io_fault(end.error()));
    const std::uint64_t total = *end;

    if (total < format::kHintHeader) return false;
    auto h = file_.pread(0, format::kHintHeader);
    if (!h) return std::unexpected(io_fault(h.error()));
    if (std::holds_alternative<io::ReadEof>(*h) ||
        le_load_u32(std::get<io::ReadOk>(*h).data.data()) !=
            format::kHintMagicV5) {
        return false;  // 非 v5（含 BCH4 旧纪元）→ 不可用
    }
    if (total < format::kHintHeader + format::kHintTrailer) {
        return false;  // 未封口
    }
    auto t = file_.pread(total - format::kHintTrailer, format::kHintTrailer);
    if (!t) return std::unexpected(io_fault(t.error()));
    if (std::holds_alternative<io::ReadEof>(*t)) return false;
    const auto& tb = std::get<io::ReadOk>(*t).data;
    if (le_load_u32(tb.data()) != format::kHintTrailerMagic) {
        return false;
    }
    const std::uint32_t expected_crc = le_load_u32(tb.data() + 4);
    std::uint64_t remaining = total - format::kHintTrailer;
    std::uint64_t off = 0;
    std::uint32_t crc = 0;
    constexpr std::size_t kChunk = 65536;
    std::vector<std::byte> cbuf;
    while (remaining > 0) {
        const std::size_t n = static_cast<std::size_t>(
            std::min<std::uint64_t>(kChunk, remaining));
        if (cbuf.size() < n) cbuf.resize(n);
        auto r = file_.pread_into(off, std::span(cbuf.data(), n));
        if (!r) return std::unexpected(io_fault(r.error()));
        const std::size_t got = *r;
        if (got == 0) break;
        crc = codec::crc32_update(
            crc, std::span<const std::byte>(cbuf.data(), got));
        remaining -= got;
        off += got;
        if (got < n) break;
    }
    return crc == expected_crc;
}

// S13-P8：单遍校验 + fold（契约见头文件）。
std::expected<bool, DataFileFault> HintFile::fold_validated(FoldFn fn) {
    auto end = file_.seek(0, SEEK_END);
    if (!end) return std::unexpected(io_fault(end.error()));
    const std::uint64_t total = *end;
    if (total < format::kHintHeader + format::kHintTrailer) return false;

    // 整文件一次读入（单 I/O 遍 + 单 CRC 遍 + 内存解析）。
    std::vector<std::byte> buf(static_cast<std::size_t>(total));
    std::size_t got_total = 0;
    while (got_total < buf.size()) {
        auto r = file_.pread_into(
            got_total, std::span(buf.data() + got_total,
                                 buf.size() - got_total));
        if (!r) return std::unexpected(io_fault(r.error()));
        if (*r == 0) break;  // 短读（文件被并发截断）→ 按校验失败处理
        got_total += *r;
    }
    if (got_total < buf.size()) return false;

    if (le_load_u32(buf.data()) != format::kHintMagicV5) {
        return false;  // 非 v5（含 BCH4 旧纪元）→ 退 fold(data)
    }
    const std::size_t body_end = buf.size() - format::kHintTrailer;
    if (le_load_u32(buf.data() + body_end) != format::kHintTrailerMagic) {
        return false;
    }
    const std::uint32_t expected_crc =
        le_load_u32(buf.data() + body_end + 4);
    const std::uint32_t crc = codec::crc32(
        std::span<const std::byte>(buf.data(), body_end));
    if (crc != expected_crc) return false;

    std::size_t off = format::kHintHeader;
    std::uint64_t prev_end = 0;
    std::uint64_t prev_ord = 0;
    while (off < body_end) {
        auto rec = codec::decode_hint_record_v5(
            std::span<const std::byte>(buf.data() + off, body_end - off),
            prev_end, prev_ord);
        if (!rec) {
            // CRC 已过仍解不动 = 防御性短路（不应发生）。
            return std::unexpected(
                DataFileFault{DataFileError::kShortRead});
        }
        fn(*rec);
        off += rec->consumed;
    }
    return true;
}

}  // namespace bitcask::fileops
