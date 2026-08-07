#include "bitcask/data_file.hpp"


#include <algorithm>
#include <cstring>
#include <filesystem>
#include <optional>
#include <utility>

#include "bitcask/format.hpp"
#include "bitcask/detail/chunked_reader.hpp"       // T23
#include "bitcask/detail/thread_local_buffer.hpp"  // S9-P1-d

namespace bitcask::fileops {

namespace {
// fold(tolerate_crc_errors=true) 模式下，连续遇到 CRC 错的 record 上限。
// legacy 是 20 条——超过就认定文件损坏程度太重，放弃恢复整个文件。
constexpr int kCrcSkipLimit = 20;
}  // namespace

std::expected<DataFile, DataFileFault>
DataFile::open(std::string_view path, Mode mode, bool sync, bool mmap_enabled) {
    using io::OpenFlag;
    OpenFlag flags = OpenFlag::kNone;
    switch (mode) {
        case Mode::kRead:   flags = OpenFlag::kReadOnly; break;
        case Mode::kAppend: flags = OpenFlag::kNone; break;       // O_RDWR|O_APPEND default
        case Mode::kCreate: flags = OpenFlag::kCreate; break;     // O_EXCL
    }
    if (sync && mode != Mode::kRead) flags = flags | OpenFlag::kOSync;

    auto f = io::PosixFile::open(path, flags);
    if (!f) return std::unexpected(io_fault(f.error()));

    // 同时给 kRead（fold 要用 size 划界）和 kAppend（write 用 pwrite，
    // 不依赖 O_APPEND 的内核语义，要自己跟踪偏移）记录起始偏移。
    std::uint64_t initial_off = 0;
    if (mode != Mode::kCreate) {
        auto end = f->seek(0, SEEK_END);
        if (!end) return std::unexpected(io_fault(end.error()));
        initial_off = *end;
    }
    // S36-5 B1：o_sync（durable_writes）= 逐条持久；持久水位初值 =
    // 现有大小（盘上字节；kCreate 为 0）。
    DataFile df(std::move(*f), std::string(path), initial_off, mode,
                sync && mode != Mode::kRead);

    // P6:sealed 只读文件整文件 mmap(PROT_READ, MAP_SHARED)。只对 kRead
    // (sealed 不可变,无 torn-tail/SIGBUS)、64 位(地址空间)、非空文件。
    // read_mmap 直读映射(get 热路径零拷贝);read()/fold() 仍走 pread——
    // 故 **fd 保留不关**(关 fd 会让迭代器/恢复在 mmapped 句柄上的 pread 失效)。
    // fd 预算的回收(close + munmap)交给 P9(read_files_ LRU)。失败 → 纯 pread。
    // mmap_enabled=false(纯 fold 的恢复/merge/迭代器 pin)跳过,避免无谓映射。
    if (mode == Mode::kRead && mmap_enabled && sizeof(void*) >= 8 &&
        initial_off > 0) {
        // D3:get() 热路径按 offset 随机读 → advise_random 禁内核预读。
        // 失败 → 无效映射,纯 pread 回退（MappedFile 内部语义）。
        df.map_ = io::MappedFile::map_readonly(
            df.file_.fd(), static_cast<std::size_t>(initial_off),
            /*advise_random=*/true);
    }
    return df;
}

DataFile::DataFile(DataFile&& o) noexcept
    : file_(std::move(o.file_)), path_(std::move(o.path_)),
      current_offset_(o.current_offset_), mode_(o.mode_),
      durable_writes_(o.durable_writes_), durable_(std::move(o.durable_)),
      write_buf_(std::move(o.write_buf_)),
      map_(std::move(o.map_)) {}

DataFile& DataFile::operator=(DataFile&& o) noexcept {
    if (this != &o) {
        file_           = std::move(o.file_);
        path_           = std::move(o.path_);
        current_offset_ = o.current_offset_;
        mode_           = o.mode_;
        durable_writes_ = o.durable_writes_;
        durable_        = std::move(o.durable_);
        write_buf_      = std::move(o.write_buf_);
        map_            = std::move(o.map_);  // B3:双 munmap 防护在 MappedFile
    }
    return *this;
}

std::expected<codec::DataRecordView, DataFileFault>
DataFile::read_mmap(std::uint64_t offset, std::uint32_t total_size) const {
    if (!map_.valid()) {
        return std::unexpected(DataFileFault{DataFileError::kIo});
    }
    if (offset > map_.size() || total_size > map_.size() - offset) {
        return std::unexpected(DataFileFault{DataFileError::kShortRead});
    }
    auto rec = codec::decode_data_record(
        std::span<const std::byte>(map_.data() + offset, total_size));
    if (!rec) {
        switch (rec.error()) {
            case codec::DecodeError::kBadCrc:
                return std::unexpected(DataFileFault{DataFileError::kBadCrc});
            case codec::DecodeError::kBufferTooShort:
                return std::unexpected(DataFileFault{DataFileError::kShortRead});
            default:
                return std::unexpected(DataFileFault{DataFileError::kBadCrc});
        }
    }
    return *rec;
}

// ---------------------------------------------------------------------------
// 写入
// ---------------------------------------------------------------------------

// append 一条 record 到当前 offset。流程：
//   1. 校验权限 + 字段大小
//   2. 在内存里 encode 整条 record
//   3. pwrite 到 current_offset_，并把 current_offset_ 向前推
// 注意是 pwrite 不是 write——即使打开时带了 O_APPEND，我们也要明确指定
// offset 来支持「自己追踪写入位置」的语义（下一步 truncate_to 会用到）。
std::expected<WriteResult, DataFileFault>
DataFile::write(format::RecordType type,
                std::uint64_t tstamp,
                std::uint64_t ord,
                std::span<const std::byte> key,
                std::span<const std::byte> value) {
    if (mode_ == Mode::kRead) {
        return std::unexpected(DataFileFault{DataFileError::kIo, 0});
    }
    if (key.size()   > format::kMaxKeySize)   return std::unexpected(DataFileFault{DataFileError::kTooLarge});
    if (value.size() > format::kMaxValueSize) return std::unexpected(DataFileFault{DataFileError::kTooLarge});

    write_buf_.clear();  // 复用容量:稳态零分配(encode 是 append 语义)
    const std::size_t total =
        codec::encode_data_record(write_buf_, type, tstamp, ord, key, value);

    const std::uint64_t off = current_offset_;
    auto w = file_.pwrite(off, write_buf_);
    if (!w) return std::unexpected(io_fault(w.error()));
    current_offset_ += total;
    if (durable_writes_) {  // S36-5 B1：O_SYNC pwrite 即持久
        durable_->store(current_offset_, std::memory_order_release);
    }
    return WriteResult{off, static_cast<std::uint32_t>(total)};
}

// S29-7 铺垫：写入 caller 已编码的完整 record（编码在 write_mu_ 外完成，
// 本函数在锁内只做 pwrite + 偏移推进；每条立即 pwrite，WAL 语义同 write()）。
std::expected<WriteResult, DataFileFault>
DataFile::write_encoded(std::span<const std::byte> record) {
    if (mode_ == Mode::kRead) {
        return std::unexpected(DataFileFault{DataFileError::kIo, 0});
    }
    const std::uint64_t off = current_offset_;
    auto w = file_.pwrite(off, record);
    if (!w) return std::unexpected(io_fault(w.error()));
    current_offset_ += record.size();
    if (durable_writes_) {  // S36-5 B1：O_SYNC pwrite 即持久
        durable_->store(current_offset_, std::memory_order_release);
    }
    return WriteResult{off, static_cast<std::uint32_t>(record.size())};
}

// S2:批量 append——编码进 batch_buf_，越过阈值才一次 pwrite。offset 取
// current_offset_（逻辑位置，含缓冲），与落盘时机解耦。
std::expected<WriteResult, DataFileFault>
DataFile::write_buffered(format::RecordType type,
                         std::uint64_t tstamp,
                         std::uint64_t ord,
                         std::span<const std::byte> key,
                         std::span<const std::byte> value) {
    if (mode_ == Mode::kRead) {
        return std::unexpected(DataFileFault{DataFileError::kIo, 0});
    }
    if (key.size()   > format::kMaxKeySize)   return std::unexpected(DataFileFault{DataFileError::kTooLarge});
    if (value.size() > format::kMaxValueSize) return std::unexpected(DataFileFault{DataFileError::kTooLarge});

    const std::uint64_t off = current_offset_;
    // encode 是 append 语义:累积到 batch_buf_ 尾部,不清空。
    const std::size_t total =
        codec::encode_data_record(batch_buf_, type, tstamp, ord, key, value);
    current_offset_ += total;
    if (batch_buf_.size() >= kBatchFlushBytes) {
        if (auto f = flush_batch(); !f) return std::unexpected(f.error());
    }
    return WriteResult{off, static_cast<std::uint32_t>(total)};
}

// 把 batch_buf_ 一次 pwrite 到它对应的文件区间起点并清空。
std::expected<void, DataFileFault> DataFile::flush_batch() {
    if (batch_buf_.empty()) return {};
    // 缓冲首字节对应的文件偏移 = 逻辑末尾 − 缓冲长度。
    const std::uint64_t flush_off = current_offset_ - batch_buf_.size();
    auto w = file_.pwrite(flush_off, batch_buf_);
    if (!w) return std::unexpected(io_fault(w.error()));
    batch_buf_.clear();  // 复用容量
    if (durable_writes_) {  // S36-5 B1：O_SYNC pwrite 即持久
        durable_->store(current_offset_, std::memory_order_release);
    }
    return {};
}

// 截到当前写入位置：先 lseek 到 current_offset_，再 ftruncate 到那里。
// 用于 undo——caller 想撤销最近一次 write 的话，可以记下 write 前的
// current_offset_，写完发现不对就 truncate_to(那个旧值)。
std::expected<void, DataFileFault> DataFile::truncate_here() {
    auto s = file_.seek(static_cast<std::int64_t>(current_offset_), SEEK_SET);
    if (!s) return std::unexpected(io_fault(s.error()));
    auto t = file_.truncate_here();
    if (!t) return std::unexpected(io_fault(t.error()));
    return {};
}

// fsync 包装。cask 的 sync_strategy=o_sync 模式下不会调到这里
//（已经在 write 时直接落盘）；none 模式下由调用方在合适时机主动调。
std::expected<void, DataFileFault> DataFile::sync() {
    // S2:兜底——sync 前确保 batch_buf_ 已落盘（write() 路径恒空,no-op），
    // 防 caller 漏调 flush_batch() 导致缓冲数据未落盘却被采信。
    if (auto f = flush_batch(); !f) return std::unexpected(f.error());
    auto s = file_.sync();
    if (!s) return std::unexpected(io_fault(s.error()));
    // S36-5 B1：全部逻辑字节已持久。
    durable_->store(current_offset_, std::memory_order_release);
    return {};
}

// S36-5 B1：跨线程 fd 级持久化（语义见头文件）。
bool DataFile::sync_fd_only(std::uint64_t durable_floor) noexcept {
    if (!file_.sync()) return false;
    std::uint64_t cur = durable_->load(std::memory_order_relaxed);
    while (cur < durable_floor &&
           !durable_->compare_exchange_weak(cur, durable_floor,
                                            std::memory_order_release)) {
    }
    return true;
}

// ---------------------------------------------------------------------------
// 读取
// ---------------------------------------------------------------------------

// 在指定 offset 读 total_size 字节并 decode 一条 record。
//
// total_size 必须正好等于当时写入的字节数（keydir 存的就是这个值）；
// 短读或 EOF 都翻成 kShortRead——意味着 keydir 跟磁盘不一致（极端情况
// 下数据损坏或被截断）。CRC 不通过翻成 kBadCrc。
std::expected<ReadRecord, DataFileFault>
DataFile::read(std::uint64_t offset, std::uint32_t total_size) {
    // get 热路径:per-thread 复用读缓冲,稳态零分配。pread 线程安全,
    // 多线程可并发调 read(),故缓冲必须 thread_local 而非成员。
    // S9-P1-d：ensure+防膨胀收敛进 ThreadLocalBuffer（默认 retain 1 MiB）。
    static thread_local detail::ThreadLocalBuffer read_buf;
    std::byte* rb = read_buf.ensure(total_size);

    auto n = file_.pread_into(offset, std::span(rb, total_size));
    if (!n) return std::unexpected(io_fault(n.error()));
    if (*n < total_size) {  // 含 EOF(0 字节)
        return std::unexpected(DataFileFault{DataFileError::kShortRead});
    }
    auto rec = codec::decode_data_record(
        std::span<const std::byte>(rb, total_size));
    if (!rec) {
        // codec::DecodeError → DataFileFault
        switch (rec.error()) {
            case codec::DecodeError::kBadCrc:
                return std::unexpected(DataFileFault{DataFileError::kBadCrc});
            case codec::DecodeError::kBufferTooShort:
                return std::unexpected(DataFileFault{DataFileError::kShortRead});
            default:
                return std::unexpected(DataFileFault{DataFileError::kBadCrc});
        }
    }
    ReadRecord out;
    out.type       = rec->type;
    out.tstamp     = rec->tstamp;
    out.ord        = rec->ord;
    if (rec->total_size > format::kMaxValueSize + format::kHeaderSize + format::kMaxKeySize) {
        return std::unexpected(DataFileFault{DataFileError::kTooLarge});
    }
    out.total_size = static_cast<std::uint32_t>(rec->total_size);
    out.key.assign(rec->key.begin(), rec->key.end());
    out.value.assign(rec->value.begin(), rec->value.end());
    // key/value 已拷出,缓冲可以安全收缩(防超大 value 撑住线程内存)。
    read_buf.maybe_shrink();
    return out;
}

std::expected<void, DataFileFault>
DataFile::fold(FoldFn fn, bool tolerate_crc_errors,
                std::uint64_t* out_last_valid_end,
                std::uint64_t start_offset) {
    if (out_last_valid_end) *out_last_valid_end = start_offset;
    // 进入 fold 时拍个文件大小快照（防止 fold 期间被人 append），再 seek 回头。
    auto eof = file_.seek(0, SEEK_END);
    if (!eof) return std::unexpected(io_fault(eof.error()));
    const std::uint64_t total = *eof;
    if (auto r = file_.seek_bof(); !r) return std::unexpected(io_fault(r.error()));

    std::uint64_t offset = start_offset;
    int crc_errors = 0;
    // S35 原子批（doc/atomic-batch-design-zh.md §1.3）：非 0 = 正处于批头
    // 声明的成员区间 [.., batch_until) 内。区间内不推进 out_last_valid_end
    // ——推进到区间末端才一次性推到位；区间不完整（越 EOF / 批头畸形 /
    // 嵌套批头 / 记录跨区间边界）→ break，lve 停在批头起点 ⟹ 整批不可见，
    // 写打开恢复按既有语义截断。区间内单条 CRC 腐蚀走 tolerate 跳过（位
    // 腐蚀逐条降级，与全引擎一致——原子性只承诺崩溃故障）。
    std::uint64_t batch_until = 0;

    // S13-P6：流式 chunked pread——原实现每条 record 2 次 pread（header 23B
    // + 整条 body），百万条即两百万次 syscall；现 256 KiB 一块 + 跨 record
    // 复用，syscall 数降 3 个数量级。
    // 影响面：merge 全部输入文件扫描、搜索模式恢复（有 search 时 hint 快路径
    // 被跳过恒走 data fold）、纯 KV 无 hint 回退。buffer thread_local：fold
    // 可被多 reader 并发调同一 DataFile——pread 线程安全，buf 不能共享。
    // T23：refill 归并进 detail::ChunkedReader（与 hint fold 共享实现）。
    static thread_local detail::ThreadLocalBuffer buf;
    detail::ChunkedReader rd(file_, buf, total);

    while (offset + format::kHeaderSize <= total) {
        // 1) header（kHeaderSize 字节）就位。
        if (rd.avail() < format::kHeaderSize) {
            auto r = rd.refill(offset, format::kHeaderSize);
            if (!r) return std::unexpected(io_fault(r.error()));
            if (*r == 0) break;  // EOF
        }
        if (rd.avail() < format::kHeaderSize) break;  // 短读

        // 只读出长度字段，CRC 等整条 record 就位再校验。盘格式为小端
        // (全引擎 LE-only 主机,见 codec 顶部 static_assert)→ 直接 memcpy。
        const auto* p = rd.cursor();
        std::uint16_t key_sz;
        std::uint32_t value_sz;
        std::memcpy(&key_sz,  p + format::kKeySzOffset,  sizeof(key_sz));
        std::memcpy(&value_sz, p + format::kValueSzOffset, sizeof(value_sz));

        const std::uint32_t rec_total =
            static_cast<std::uint32_t>(format::kHeaderSize) +
            static_cast<std::uint32_t>(key_sz) + value_sz;

        // 文件尾被 torn write 截断——header 说有 N 字节 body 但磁盘上没那么多。
        // 静默 break，out_last_valid_end 保持上一条 record 的末尾偏移；caller
        // 拿这个值跟 size() 比较，不一致就 truncate_to(out_last_valid_end)。
        if (offset + rec_total > total) break;

        // 2) 整条 record 就位才 decode——缺就再 refill（含巨型 record 扩容）。
        if (rd.avail() < rec_total) {
            auto r = rd.refill(offset, rec_total);
            if (!r) return std::unexpected(io_fault(r.error()));
        }
        if (rd.avail() < rec_total) break;  // 短读当 EOF（同 hint fold）

        auto rec = codec::decode_data_record(
            std::span<const std::byte>(rd.cursor(), rec_total));
        if (!rec) {
            if (rec.error() == codec::DecodeError::kBadCrc && tolerate_crc_errors) {
                if (++crc_errors > kCrcSkipLimit) {
                    return std::unexpected(DataFileFault{DataFileError::kBadCrc});
                }
                offset += rec_total;
                rd.consume(rec_total);
                // S35：跳过的坏记录也参与区间收口（区间是纯字节账）。
                if (batch_until != 0 && offset >= batch_until) {
                    if (offset > batch_until) break;  // 跨界 = 区间账坏
                    batch_until = 0;
                    if (out_last_valid_end) *out_last_valid_end = offset;
                }
                continue;
            }
            switch (rec.error()) {
                case codec::DecodeError::kBadCrc:
                    return std::unexpected(DataFileFault{DataFileError::kBadCrc});
                default:
                    return std::unexpected(DataFileFault{DataFileError::kShortRead});
            }
        }
        if (rec->type == format::RecordType::kBatchHeader) {
            if (batch_until != 0) break;  // 嵌套批头 = 损坏，整批不可见
            auto bh = codec::decode_batch_header_value(rec->value);
            if (!bh) break;  // 批头 value 畸形：torn 语义
            const std::uint64_t until = offset + rec_total + bh->span_bytes;
            if (until > total) break;  // 区间越 EOF：未提交，整批不可见
            batch_until = until;
        }
        fn(*rec, offset, rec_total);
        offset += rec_total;
        rd.consume(rec_total);
        if (batch_until != 0) {
            if (offset >= batch_until) {
                if (offset > batch_until) break;  // 记录跨区间边界 = 账坏
                batch_until = 0;
                if (out_last_valid_end) *out_last_valid_end = offset;
            }
            // 区间未收口：不推进 lve
        } else if (out_last_valid_end) {
            *out_last_valid_end = offset;
        }
    }
    // 防线程内存膨胀：一次巨型 record 不应让 buffer 永久占住线程堆。
    rd.shrink();
    return {};
}

// 截到指定大小（torn-write 修复用）：seek 到 new_size 然后 ftruncate。
// 跟 truncate_here 区别：caller 显式给目标大小，不依赖当前 current_offset_。
// 必须处于 write 模式——只读 cask 不该乱动磁盘。
std::expected<void, DataFileFault>
DataFile::truncate_to(std::uint64_t new_size) {
    if (mode_ == Mode::kRead) {
        return std::unexpected(DataFileFault{DataFileError::kIo, 0});
    }
    auto s = file_.seek(static_cast<std::int64_t>(new_size), SEEK_SET);
    if (!s) return std::unexpected(io_fault(s.error()));
    auto t = file_.truncate_here();
    if (!t) return std::unexpected(io_fault(t.error()));
    // 截断后 seek 到文件尾，让下一次 pwrite(current_offset_) 紧接着写。
    // current_offset_ 一般由 caller 主动设（恢复路径调用 truncate_to 时
    // 会把 current_offset_ 同步到 new_size）。
    auto e = file_.seek(0, SEEK_END);
    if (!e) return std::unexpected(io_fault(e.error()));
    current_offset_ = *e;
    return {};
}

// ---------------------------------------------------------------------------
// 文件名工具
// ---------------------------------------------------------------------------

std::string mk_data_filename(std::string_view dirname, std::uint64_t tstamp) {
    std::filesystem::path p(dirname);
    p /= (std::to_string(tstamp) + ".bitcask.data");
    return p.string();
}

std::string mk_hint_filename(std::string_view data_path) {
    // 把后缀 ".data" 换成 ".hint"，跟 legacy hintfile_name 完全一致；
    // 没有 ".data" 后缀就直接 append（兜底，正常路径不会走到）。
    constexpr std::string_view kData = ".data";
    constexpr std::string_view kHint = ".hint";
    std::string out(data_path);
    if (out.size() >= kData.size() &&
        out.compare(out.size() - kData.size(), kData.size(), kData) == 0) {
        out.replace(out.size() - kData.size(), kData.size(), kHint);
    } else {
        out.append(kHint);
    }
    return out;
}

std::optional<std::uint64_t>
parse_data_tstamp(std::string_view filename) noexcept {
    // 去掉目录前缀；没斜杠就当 basename 用。
    auto slash = filename.find_last_of('/');
    std::string_view base =
        (slash == std::string_view::npos) ? filename : filename.substr(slash + 1);

    constexpr std::string_view kSuffix = ".bitcask.data";
    if (base.size() <= kSuffix.size()) return std::nullopt;
    if (base.compare(base.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
        return std::nullopt;
    }
    base = base.substr(0, base.size() - kSuffix.size());

    if (base.empty()) return std::nullopt;
    std::uint64_t tstamp = 0;
    for (char c : base) {
        if (c < '0' || c > '9') return std::nullopt;
        tstamp = tstamp * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return tstamp;
}

}  // namespace bitcask::fileops
