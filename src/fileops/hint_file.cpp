#include "bitcask/hint_file.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "bitcask/byte_order.hpp"  // S23-A1: le_load_u32/le_store_u32
#include "bitcask/format.hpp"
#include "bitcask/detail/thread_local_buffer.hpp"  // S9-P1-d

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
    // S23-A1：新建文件先在缓冲种入 v3 文件头 magic（随首批记录落盘），
    // running CRC 覆盖之（trailer CRC 语义 = [0, size-8) 全字节）。
    // kAppend 不种头（生产零调用；对既有文件追加本就不维护 CRC 连续性）。
    if (mode == Mode::kCreate) {
        const std::size_t base = hf.pending_.size();
        hf.pending_.resize(base + format::kHintHeaderV3);
        le_store_u32(hf.pending_.data() + base, format::kHintMagicV3);
        hf.running_crc_ = codec::crc32_update(
            0, std::span<const std::byte>(hf.pending_.data() + base,
                                          format::kHintHeaderV3));
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
HintFile::write(std::uint32_t tstamp, std::uint32_t total_sz,
                std::uint64_t offset, bool tombstone,
                std::span<const std::byte> key) {
    if (mode_ == Mode::kRead) {
        return std::unexpected(DataFileFault{DataFileError::kIo, 0});
    }
    if (offset > format::kMaxOffsetV2) {
        return std::unexpected(DataFileFault{DataFileError::kTooLarge});
    }
    const std::size_t before = pending_.size();
    // S23-A1：v3 变长编码（gap 差分串联经 prev_end_）。
    prev_end_ = codec::encode_hint_record_v3(pending_, tstamp, total_sz,
                                             offset, tombstone, key,
                                             prev_end_);
    running_crc_ = codec::crc32_update(
        running_crc_,
        std::span<const std::byte>(pending_.data() + before,
                                   pending_.size() - before));

    if (pending_.size() >= kFlushBytes) {
        if (auto r = flush_pending(); !r) return r;
    }
    return {};
}

// 写 EOF sentinel 并把 running_crc_ 嵌进去，连同缓冲里剩余 record 一次落盘。
// 这是 hint 文件的「封口」操作；没封口的 hint 文件下次 open 会被
// validate_trailer() 判失败，cask 会 fallback 到 fold(data) 重建——慢但可靠。
std::expected<void, DataFileFault> HintFile::finalize() {
    // S23-A1：v3 trailer（8B：magic + running CRC；CRC 覆盖 [0, size-8)）。
    const std::size_t base = pending_.size();
    pending_.resize(base + format::kHintTrailerV3);
    le_store_u32(pending_.data() + base, format::kHintTrailerMagicV3);
    le_store_u32(pending_.data() + base + 4, running_crc_);
    return flush_pending();  // 缓冲 record + trailer 一次写
}

std::expected<void, DataFileFault> HintFile::sync() {
    auto s = file_.sync();
    if (!s) return std::unexpected(io_fault(s.error()));
    return {};
}

// ---------------------------------------------------------------------------
// 读取
// ---------------------------------------------------------------------------

std::expected<void, DataFileFault> HintFile::fold(FoldFn fn) {
    auto end = file_.seek(0, SEEK_END);
    if (!end) return std::unexpected(io_fault(end.error()));
    const std::uint64_t total = *end;

    // S23-A1：按文件头 magic 分派 v3（变长记录流）。
    if (total >= format::kHintHeaderV3) {
        auto h = file_.pread(0, format::kHintHeaderV3);
        if (!h) return std::unexpected(io_fault(h.error()));
        if (!std::holds_alternative<io::ReadEof>(*h) &&
            le_load_u32(std::get<io::ReadOk>(*h).data.data()) ==
                format::kHintMagicV3) {
            return fold_v3(total, std::move(fn));
        }
    }

    std::uint64_t offset = 0;

    // 流式 chunked pread：每 256 KiB 一次 syscall，buffer 跨 record 复用。
    // 1M 条 record → 2M syscalls（原）→ ~几百次 chunked read，fold 主导
    // 路径 syscalls 减 1000+ 倍。buffer thread_local 是因为 fold 可在多
    // reader 并发调同一 HintFile——preade 线程安全，但 buf 不能共享。
    // S9-P1-d：ensure+防膨胀收敛进 ThreadLocalBuffer（默认 retain 1 MiB）。
    static thread_local detail::ThreadLocalBuffer buf;
    constexpr std::size_t kChunkBytes = 256 * 1024;  // 256 KiB
    buf.ensure(kChunkBytes);

    // buf 内的有效数据区间：[buf_pos, buf_len)。每次消费完一段记录就
    // 推进 buf_pos；不够一条新 record 时先把残留 memmove 到 buf 头部，
    // 再从 file 续读剩余字节。
    std::size_t buf_pos = 0;
    std::size_t buf_len = 0;

    // refill 把残留字节搬到 buf 头部，再读一整块 256 KiB（或剩余文件字节）
    // 进去。返回从磁盘新读的字节数（0 = EOF）。read_size_hint 用来告诉
    // refill 这次至少要多少字节（巨型 record case 下需要扩容 buf）。
    auto refill = [&](std::uint64_t file_off,
                      std::size_t read_size_hint)
        -> std::expected<std::size_t, DataFileFault> {
        const std::size_t leftover = buf_len - buf_pos;
        if (leftover > 0 && buf_pos > 0) {
            std::memmove(buf.data(), buf.data() + buf_pos, leftover);
        }
        buf_pos = 0;
        buf_len = leftover;

        // 缓冲扩容到能装下「残留 + 一块」——巨型 record case（key_sz > 256K-18）
        // 需要把 buf 撑大，单次 read 才能装下。
        const std::size_t desired = buf_len + kChunkBytes;
        const std::size_t need = std::max(desired, buf_len + read_size_hint);
        buf.ensure(need);

        // 一次 pread 尽量读满 kChunkBytes；不要短读时多调一次。
        // 截到文件总长，避免 read 出 EOF 部分徒增 syscalls。
        const std::uint64_t file_remaining =
            total > (file_off + buf_len) ? total - (file_off + buf_len) : 0;
        if (file_remaining == 0) return static_cast<std::size_t>(0);
        const std::size_t to_read = static_cast<std::size_t>(
            std::min<std::uint64_t>(buf.size() - buf_len, file_remaining));
        if (to_read == 0) return static_cast<std::size_t>(0);

        auto n = file_.pread_into(
            file_off + buf_len, std::span(buf.data() + buf_len, to_read));
        if (!n) return std::unexpected(io_fault(n.error()));
        buf_len += *n;
        return *n;
    };

    while (offset + format::kHintRecordSize <= total) {
        // 1) 至少 18 字节 header 要就位
        if (buf_len - buf_pos < format::kHintRecordSize) {
            auto r = refill(offset, format::kHintRecordSize);
            if (!r) return std::unexpected(r.error());
            if (*r == 0) break;  // EOF
        }
        if (buf_len - buf_pos < format::kHintRecordSize) break;  // 短读

        // 2) 从 header 拿 key_sz（offset 4..5, P:小端 u16）
        const auto* p = buf.data() + buf_pos;
        const std::uint16_t key_sz =
            static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(p[4]) |
                (static_cast<std::uint16_t>(p[5]) << 8));
        const std::uint64_t rec_size =
            format::kHintRecordSize + static_cast<std::uint64_t>(key_sz);

        if (offset + rec_size > total) break;  // 文件尾被截断

        // 3) 整条 record 就位才 decode——缺就再 refill 一块。短读当 EOF 处理
        if (buf_len - buf_pos < rec_size) {
            auto r = refill(offset, static_cast<std::size_t>(rec_size));
            if (!r) return std::unexpected(r.error());
        }
        if (buf_len - buf_pos < rec_size) break;  // 短读 / EOF

        // 4) 复用 codec 解码——不重写解析逻辑
        auto rec = codec::decode_hint_record(
            std::span<const std::byte>(buf.data() + buf_pos,
                                       static_cast<std::size_t>(rec_size)));
        if (!rec) return std::unexpected(DataFileFault{DataFileError::kShortRead});

        // 遇到 EOF sentinel 收工——不调 fn（它不是真正的 entry）。
        if (codec::is_hint_eof(*rec)) break;

        fn(*rec);
        offset += rec_size;
        buf_pos += static_cast<std::size_t>(rec_size);
    }

    // 防线程内存膨胀：一次巨型 hint 文件不应让 buffer 永久占住线程栈/堆
    buf.maybe_shrink();
    return {};
}

// S23-A1：v3 流式 fold（变长记录：try-decode，字节不足则 refill 重试）。
// body_end：有 trailer 时为 total-8；未封口（崩溃）文件读到 decode 短缺
// 即停（与 v2「短读当 EOF」语义一致）。
std::expected<void, DataFileFault>
HintFile::fold_v3(std::uint64_t total, FoldFn fn) {
    std::uint64_t body_end = total;
    if (total >= format::kHintHeaderV3 + format::kHintTrailerV3) {
        auto t = file_.pread(total - format::kHintTrailerV3, 4);
        if (!t) return std::unexpected(io_fault(t.error()));
        if (!std::holds_alternative<io::ReadEof>(*t) &&
            le_load_u32(std::get<io::ReadOk>(*t).data.data()) ==
                format::kHintTrailerMagicV3) {
            body_end = total - format::kHintTrailerV3;
        }
    }

    static thread_local detail::ThreadLocalBuffer buf;
    constexpr std::size_t kChunkBytes = 256 * 1024;
    buf.ensure(kChunkBytes);
    std::size_t buf_pos = 0;
    std::size_t buf_len = 0;

    // refill 语义同 v2 fold（残留 memmove 到头部 + 续读一块）。
    auto refill = [&](std::uint64_t file_off, std::size_t read_size_hint)
        -> std::expected<std::size_t, DataFileFault> {
        const std::size_t leftover = buf_len - buf_pos;
        if (leftover > 0 && buf_pos > 0) {
            std::memmove(buf.data(), buf.data() + buf_pos, leftover);
        }
        buf_pos = 0;
        buf_len = leftover;
        const std::size_t desired = buf_len + kChunkBytes;
        const std::size_t need = std::max(desired, buf_len + read_size_hint);
        buf.ensure(need);
        const std::uint64_t file_remaining =
            body_end > (file_off + buf_len) ? body_end - (file_off + buf_len)
                                            : 0;
        if (file_remaining == 0) return static_cast<std::size_t>(0);
        const std::size_t to_read = static_cast<std::size_t>(
            std::min<std::uint64_t>(buf.size() - buf_len, file_remaining));
        if (to_read == 0) return static_cast<std::size_t>(0);
        auto n = file_.pread_into(
            file_off + buf_len, std::span(buf.data() + buf_len, to_read));
        if (!n) return std::unexpected(io_fault(n.error()));
        buf_len += *n;
        return *n;
    };

    std::uint64_t offset = format::kHintHeaderV3;  // 跳过文件头
    std::uint64_t prev_end = 0;
    while (offset < body_end) {
        const std::size_t avail = buf_len - buf_pos;
        const std::uint64_t region_left = body_end - offset;
        auto rec = codec::decode_hint_record_v3(
            std::span<const std::byte>(
                buf.data() + buf_pos,
                static_cast<std::size_t>(
                    std::min<std::uint64_t>(avail, region_left))),
            prev_end);
        if (!rec) {
            if (rec.error() != codec::DecodeError::kBufferTooShort) {
                return std::unexpected(DataFileFault{DataFileError::kShortRead});
            }
            auto r = refill(offset, /*read_size_hint=*/64);
            if (!r) return std::unexpected(r.error());
            if (*r == 0) break;  // 尾部截断（未封口）→ 当 EOF
            continue;            // 重试解码
        }
        fn(*rec);
        offset += rec->consumed;
        buf_pos += rec->consumed;
    }
    buf.maybe_shrink();
    return {};
}

// 单独验 trailer CRC：先读末尾的 sentinel 拿到 expected_crc，再从头流式
// 算一遍前面所有字节的 CRC。两者一致才算 hint 文件健康，可以直接 fold；
// 否则 caller 应该 fall back 到 fold(data_file) 重建。
std::expected<bool, DataFileFault> HintFile::validate_trailer() {
    auto end = file_.seek(0, SEEK_END);
    if (!end) return std::unexpected(io_fault(end.error()));
    const std::uint64_t total = *end;

    // S23-A1：v3 分派（文件头 magic）。CRC 覆盖 [0, total-8)。
    if (total >= format::kHintHeaderV3) {
        auto h = file_.pread(0, format::kHintHeaderV3);
        if (!h) return std::unexpected(io_fault(h.error()));
        if (!std::holds_alternative<io::ReadEof>(*h) &&
            le_load_u32(std::get<io::ReadOk>(*h).data.data()) ==
                format::kHintMagicV3) {
            if (total < format::kHintHeaderV3 + format::kHintTrailerV3) {
                return false;  // 未封口
            }
            auto t = file_.pread(total - format::kHintTrailerV3,
                                 format::kHintTrailerV3);
            if (!t) return std::unexpected(io_fault(t.error()));
            if (std::holds_alternative<io::ReadEof>(*t)) return false;
            const auto& tb = std::get<io::ReadOk>(*t).data;
            if (le_load_u32(tb.data()) != format::kHintTrailerMagicV3) {
                return false;
            }
            const std::uint32_t expected_crc = le_load_u32(tb.data() + 4);
            std::uint64_t remaining = total - format::kHintTrailerV3;
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
    }
    if (total < format::kHintRecordSize) return false;

    // 1) 先读末尾的 sentinel record 拿 expected_crc。
    auto t = file_.pread(total - format::kHintRecordSize,
                         format::kHintRecordSize);
    if (!t) return std::unexpected(io_fault(t.error()));
    if (std::holds_alternative<io::ReadEof>(*t)) return false;
    auto& tb = std::get<io::ReadOk>(*t);
    auto trailer = codec::decode_hint_record(tb.data);
    if (!trailer) return false;
    if (!codec::is_hint_eof(*trailer)) return false;
    const std::uint32_t expected_crc = trailer->total_sz;

    // 2) 流式扫 trailer 之前的全部字节算 CRC，64 KiB 一块。
    // ⑮:pread_into + 复用缓冲（容量只增），替代 file_.read(n) 每块一次堆
    // 分配（open 路径，O(filesize/64K) 次分配 → 1 次）。
    std::uint64_t remaining = total - format::kHintRecordSize;
    std::uint64_t off = 0;
    std::uint32_t crc = 0;
    constexpr std::size_t kChunk = 65536;
    std::vector<std::byte> buf;
    while (remaining > 0) {
        const std::size_t n =
            static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, remaining));
        if (buf.size() < n) buf.resize(n);
        auto r = file_.pread_into(off, std::span(buf.data(), n));
        if (!r) return std::unexpected(io_fault(r.error()));
        const std::size_t got = *r;
        if (got == 0) break;
        crc = codec::crc32_update(crc, std::span<const std::byte>(buf.data(), got));
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
    if (total < format::kHintHeaderV3) return false;

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

    // S23-A1：v3 分派（文件头 magic）。CRC 覆盖 [0, size-8)。
    if (le_load_u32(buf.data()) == format::kHintMagicV3) {
        if (total < format::kHintHeaderV3 + format::kHintTrailerV3) {
            return false;  // 未封口
        }
        const std::size_t body_end =
            buf.size() - format::kHintTrailerV3;
        if (le_load_u32(buf.data() + body_end) !=
            format::kHintTrailerMagicV3) {
            return false;
        }
        const std::uint32_t expected_crc =
            le_load_u32(buf.data() + body_end + 4);
        const std::uint32_t crc = codec::crc32(
            std::span<const std::byte>(buf.data(), body_end));
        if (crc != expected_crc) return false;

        std::size_t off = format::kHintHeaderV3;
        std::uint64_t prev_end = 0;
        while (off < body_end) {
            auto rec = codec::decode_hint_record_v3(
                std::span<const std::byte>(buf.data() + off, body_end - off),
                prev_end);
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
    if (total < format::kHintRecordSize) return false;

    // trailer sentinel + CRC 判定（与 validate_trailer 逐字节一致）。
    const std::size_t body = buf.size() - format::kHintRecordSize;
    auto trailer = codec::decode_hint_record(
        std::span<const std::byte>(buf.data() + body, format::kHintRecordSize));
    if (!trailer) return false;
    if (!codec::is_hint_eof(*trailer)) return false;
    const std::uint32_t expected_crc = trailer->total_sz;
    const std::uint32_t crc = codec::crc32(
        std::span<const std::byte>(buf.data(), body));
    if (crc != expected_crc) return false;

    // 内存解析逐条回调（与 fold 的记录界定一致：header 拿 key_sz 推进）。
    std::size_t off = 0;
    while (off + format::kHintRecordSize <= body) {
        const auto* pb = buf.data() + off;
        const std::uint16_t key_sz = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(pb[4]) |
            (static_cast<std::uint16_t>(pb[5]) << 8));
        const std::size_t rec_size =
            format::kHintRecordSize + static_cast<std::size_t>(key_sz);
        if (off + rec_size > body) break;  // 尾部截断（CRC 已过，防御性）
        auto rec = codec::decode_hint_record(
            std::span<const std::byte>(pb, rec_size));
        if (!rec) return std::unexpected(DataFileFault{DataFileError::kShortRead});
        if (codec::is_hint_eof(*rec)) break;
        fn(*rec);
        off += rec_size;
    }
    return true;
}

}  // namespace bitcask::fileops
