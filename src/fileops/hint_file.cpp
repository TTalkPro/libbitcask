#include "bitcask/hint_file.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "bitcask/format.hpp"

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
    return HintFile(std::move(*f), std::string(path), 0, mode);
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
    codec::encode_hint_record(pending_, tstamp, total_sz, offset, tombstone, key);
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
    codec::encode_hint_eof(pending_, running_crc_);  // 追加 sentinel
    return flush_pending();                          // 缓冲 record + sentinel 一次写
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

    std::uint64_t offset = 0;

    // 流式 chunked pread：每 256 KiB 一次 syscall，buffer 跨 record 复用。
    // 1M 条 record → 2M syscalls（原）→ ~几百次 chunked read，fold 主导
    // 路径 syscalls 减 1000+ 倍。buffer thread_local 是因为 fold 可在多
    // reader 并发调同一 HintFile——preade 线程安全，但 buf 不能共享。
    static thread_local std::vector<std::byte> buf;
    constexpr std::size_t kChunkBytes = 256 * 1024;  // 256 KiB
    constexpr std::size_t kBufRetain   = 1u << 20;   // 1 MiB 防膨胀阈值
    if (buf.size() < kChunkBytes) buf.resize(kChunkBytes);

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
        if (buf.size() < need) buf.resize(need);

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
    if (buf.size() > kBufRetain) {
        buf.clear();
        buf.shrink_to_fit();
    }
    return {};
}

// 单独验 trailer CRC：先读末尾的 sentinel 拿到 expected_crc，再从头流式
// 算一遍前面所有字节的 CRC。两者一致才算 hint 文件健康，可以直接 fold；
// 否则 caller 应该 fall back 到 fold(data_file) 重建。
std::expected<bool, DataFileFault> HintFile::validate_trailer() {
    auto end = file_.seek(0, SEEK_END);
    if (!end) return std::unexpected(io_fault(end.error()));
    const std::uint64_t total = *end;
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

}  // namespace bitcask::fileops
