// OKI（有序 Key 索引）run 与 manifest——S33-3。
// 设计：doc/ordered-key-index-design-zh.md §3。
//
// run（`kv.oki.seg-<gen>`，"BCOK" v1）是按 key 升序排列的不可变文件，条目
// 只存 (key, ord, tomb)——**不存位置信息**：range 查询逐 key 回查哈希
// keydir 取权威位置，因此 merge 搬迁与 OKI 零交互、run 允许陈旧（§2.2）。
//
// 磁盘布局（全小端）：
//   header  8B: magic "BCOK" u32 | version u32 = 1
//   数据块区（块目标 ~4KiB，块界由稀疏索引给出；**每块解码状态复位**
//   prev_key="" / prev_ord=0，块首条自然退化为全量 key + 绝对 ord）：
//     每条: [vbyte shared_len][vbyte suffix_len][suffix]
//           [vbyte ord_delta(u64 二补数回绕相对 prev_ord)][flags u8]
//     flags: bit0 = tomb；其余位保留（Level B 全字段扩展位），读端遇未知位
//     fail-fast（kCorrupt → 整个 run 弃用重建，绝不静默跳）。
//   稀疏索引区: [count u32] + count × { [vbyte klen][块首 key][block_off u64] }
//   trailer 24B: [entry_count u64][index_off u64][crc u32][magic "BCOE" u32]
//     CRC 覆盖 [0, size-8)（即除 crc 与尾 magic 外全部字节）。
//
// 有序 key 的公共前缀差分对 `prefix:id` 形态收益显著；ord 差分同 hint v5
// 的回绕语义（正确性不依赖单调性）。
//
// manifest（`kv.oki.manifest`，"BCOM" v1）是 OKI 的**唯一 commit point**：
//   [magic u32][ver u32 = 1][count u32] + count × [gen u64][cover_ord u64]
//   + [wm u64（联合覆盖水位）] + [crc u32 覆盖 [0, size-8)][magic u32]
// 读端任何校验不过 → nullopt = 整体弃用 OKI（派生缓存语义，重建兜底）。
// 写端 atomic_write_bytes(fsync_dir=true)——与 index.manifest 同款纪律。
//
// === 线程模型 ===
// Writer 单线程（构建期独占）。Reader open 后不可变，多线程可各持自己的
// Cursor 并发读（pread 无共享可变状态）；Cursor 自身非线程安全。

#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitcask/io.hpp"
#include "bitcask/detail/file_util.hpp"

namespace bitcask::oki {

inline constexpr std::uint32_t kRunMagic        = 0x4B4F4342;  // "BCOK" LE
inline constexpr std::uint32_t kRunTrailerMagic = 0x454F4342;  // "BCOE" LE
inline constexpr std::uint32_t kRunVersion      = 1;
inline constexpr std::size_t   kRunHeaderSize   = 8;
inline constexpr std::size_t   kRunTrailerSize  = 24;
inline constexpr std::size_t   kDefaultBlockBytes = 4096;

inline constexpr std::uint32_t kManifestMagic   = 0x4D4F4342;  // "BCOM" LE
inline constexpr std::uint32_t kManifestVersion = 1;
inline constexpr char kManifestName[] = "kv.oki.manifest";

// flags 位。v1 只认 bit0；其余位保留给 Level B（全字段扩展），读端遇
// 未知位 fail-fast。
inline constexpr std::uint8_t kFlagTomb       = 0x01;
inline constexpr std::uint8_t kKnownFlagsMask = 0x01;

enum class OkiError {
    kIo,          // 底层 I/O 失败
    kCorrupt,     // magic/版本/CRC/结构校验不过（含未知 flags 位）→ 弃用重建
    kOutOfOrder,  // writer：add 的 key 未严格升序（caller bug）
    kBadState,    // writer：finish 后继续 add / 重复 finish 等误用
};

// `<dir>/kv.oki.seg-<gen>`
[[nodiscard]] std::string mk_run_filename(std::string_view dir,
                                          std::uint64_t gen);
// `<dir>/kv.oki.manifest`
[[nodiscard]] std::string mk_manifest_filename(std::string_view dir);

// ---------------------------------------------------------------------------
// Writer：key 严格升序 add，finish 时写索引 + trailer 并原子 rename 就位
// （AtomicFileWriter：中途失败/析构自动清 tmp，最终路径不见半截文件）。
// ---------------------------------------------------------------------------
class OkiRunWriter {
public:
    // block_target：数据块目标字节数（超过即封块）。测试用小值逼出多块。
    [[nodiscard]] static std::expected<OkiRunWriter, OkiError>
    create(std::string path, std::size_t block_target = kDefaultBlockBytes);

    OkiRunWriter(OkiRunWriter&&) noexcept = default;
    OkiRunWriter& operator=(OkiRunWriter&&) noexcept = default;
    OkiRunWriter(const OkiRunWriter&) = delete;
    OkiRunWriter& operator=(const OkiRunWriter&) = delete;
    ~OkiRunWriter() = default;

    // key 必须严格大于上一条（升序、去重由上游负责）；违反返回 kOutOfOrder。
    [[nodiscard]] std::expected<void, OkiError>
    add(std::span<const std::byte> key, std::uint64_t ord, bool tomb);

    struct Stats {
        std::uint64_t entries = 0;
        std::uint64_t blocks = 0;
        std::uint64_t file_bytes = 0;
    };
    // 封口：flush 尾块 + 索引 + trailer + fdatasync + rename。
    [[nodiscard]] std::expected<Stats, OkiError> finish(bool fsync_dir = false);

private:
    OkiRunWriter(detail::AtomicFileWriter&& w, std::size_t block_target)
        : w_(std::move(w)), block_target_(block_target) {}

    [[nodiscard]] bool flush_block();  // 空块 no-op；fwrite + CRC + 记账

    struct IndexEntry {
        std::string first_key;
        std::uint64_t off = 0;
    };

    detail::AtomicFileWriter w_;
    std::size_t   block_target_;
    std::vector<std::byte> blk_;        // 攒块缓冲
    std::string   blk_first_key_;       // 当前块首 key（索引条目）
    std::string   prev_key_;            // 块内前缀差分状态
    std::uint64_t prev_ord_ = 0;        // 块内 ord 差分状态
    std::string   last_key_;            // 全局升序校验
    bool          has_last_ = false;
    std::vector<IndexEntry> index_;
    std::uint64_t file_off_ = kRunHeaderSize;  // 已落盘字节（含 header）
    std::uint32_t crc_ = 0;
    std::uint64_t entries_ = 0;
    bool          finished_ = false;
    bool          header_written_ = false;
};

// ---------------------------------------------------------------------------
// Reader：open 时全文件 CRC 校验（run 是派生缓存，eager 校验换绝对安全；
// 大 run 的惰性/分块校验属后续优化）+ 稀疏索引载入内存。
// ---------------------------------------------------------------------------
class OkiRunReader {
public:
    [[nodiscard]] static std::expected<OkiRunReader, OkiError>
    open(std::string path);

    OkiRunReader(OkiRunReader&&) noexcept = default;
    OkiRunReader& operator=(OkiRunReader&&) noexcept = default;
    OkiRunReader(const OkiRunReader&) = delete;
    OkiRunReader& operator=(const OkiRunReader&) = delete;
    ~OkiRunReader() = default;

    struct Entry {
        std::string   key;   // cursor 拥有的重建缓冲（跨 next 复用）
        std::uint64_t ord = 0;
        bool          tomb = false;
    };

    // 顺序游标。next：成功且有条目 → true（out 填充）；到尾 → false；
    // 损坏/IO → unexpected。非线程安全；一个 Reader 可开多个 Cursor 并发。
    class Cursor {
    public:
        [[nodiscard]] std::expected<bool, OkiError> next(Entry& out);

    private:
        friend class OkiRunReader;
        explicit Cursor(const OkiRunReader* r) : r_(r) {}
        [[nodiscard]] std::expected<bool, OkiError> load_block(std::size_t bi);

        const OkiRunReader* r_ = nullptr;
        std::size_t   bi_ = 0;       // 下一个待载入的块下标（已载则为当前+1）
        std::vector<std::byte> blk_; // 当前块数据
        std::size_t   pos_ = 0;      // 块内游标
        bool          block_loaded_ = false;
        std::string   prev_key_;     // 块内差分状态
        std::uint64_t prev_ord_ = 0;
        std::optional<Entry> pending_;  // seek 越位暂存
    };

    [[nodiscard]] Cursor begin() const { return Cursor(this); }
    // 首个 key ≥ lo 的位置；lo 为空 span 等价 begin()。
    [[nodiscard]] std::expected<Cursor, OkiError>
    seek(std::span<const std::byte> lo) const;

    [[nodiscard]] std::uint64_t entry_count() const noexcept {
        return entry_count_;
    }
    [[nodiscard]] std::size_t block_count() const noexcept {
        return blocks_.size();
    }

private:
    OkiRunReader() = default;

    struct BlockRef {
        std::string   first_key;
        std::uint64_t off = 0;
        std::uint64_t end = 0;  // 下块 off 或 index_off
    };

    mutable io::PosixFile file_;  // pread 线程安全、无共享游标
    std::vector<BlockRef> blocks_;
    std::uint64_t entry_count_ = 0;
};

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------
struct OkiManifestEntry {
    std::uint64_t gen = 0;        // run 文件代号（kv.oki.seg-<gen>）
    std::uint64_t cover_ord = 0;  // 该 run 覆盖到的 LSN 上界
};

struct OkiManifest {
    std::vector<OkiManifestEntry> runs;
    std::uint64_t wm = 0;  // 联合覆盖水位（oki_wm；tail 重放起点）
};

// 唯一 commit point：atomic_write_bytes(fsync_dir=true)。
[[nodiscard]] bool write_manifest(std::string_view dir, const OkiManifest& m);
// 任何校验不过 → nullopt（整体弃用 OKI，重建兜底）。文件不存在同样 nullopt。
[[nodiscard]] std::optional<OkiManifest> read_manifest(std::string_view dir);

}  // namespace bitcask::oki
