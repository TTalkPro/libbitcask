// OKI（有序 Key 索引）run 与 manifest——S33-3。
// 设计：doc/ordered-key-index-design-zh.md §3。
//
// run（`kv.oki.seg-<gen>`，"BCOK"）是按 key 升序排列的不可变文件。
// v1 条目只存 (key, ord, tomb)——range 查询逐 key 回查哈希 keydir 取权威
// 位置。**v2（S36 Level B，doc/keydir-disk-resident-design-zh.md §4）条目
// 可携全字段位置**（file_id/total_sz/offset/tstamp），并内嵌 bloom——
// 组合视图点查的权威载体。v1/v2 读端都支持；写端按 create 的 version 定。
//
// 磁盘布局（全小端）：
//   header  8B: magic "BCOK" u32 | version u32 = 1 或 2
//   数据块区（块目标 ~4KiB，块界由稀疏索引给出；**每块解码状态复位**
//   prev_key="" / prev_ord=0 / prev_tstamp=0，块首条自然退化为全量值）：
//     每条: [vbyte shared_len][vbyte suffix_len][suffix]
//           [vbyte ord_delta(u64 二补数回绕相对 prev_ord)][flags u8]
//     v2 且 flags.bit1(has_loc) 时追加:
//           [vbyte file_id][vbyte total_sz][vbyte offset]
//           [vbyte tstamp_delta(回绕相对 prev_tstamp)]
//     flags: bit0 = tomb；bit1 = has_loc（仅 v2；墓碑行免位置字段）；
//     其余位保留，读端遇未知位 fail-fast（kCorrupt → 整个 run 弃用重建）。
//   稀疏索引区: [count u32] + count × { [vbyte klen][块首 key][block_off u64] }
//   bloom 区（仅 v2）: [n_bits u64][k u8][位数组 ceil(n_bits/8) 字节]
//     稳定哈希（FNV-1a64 + splitmix64 双哈希），持久化格式的一部分。
//   trailer: v1 24B [entry_count u64][index_off u64][crc u32][magic "BCOE"]
//            v2 32B [entry_count u64][index_off u64][bloom_off u64][crc][magic]
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
inline constexpr std::uint32_t kRunVersion2     = 2;   // S36-1：全字段 + bloom
inline constexpr std::size_t   kRunHeaderSize   = 8;
inline constexpr std::size_t   kRunTrailerSize  = 24;   // v1
inline constexpr std::size_t   kRunTrailerSizeV2 = 32;  // v2（+bloom_off u64）
inline constexpr std::size_t   kDefaultBlockBytes = 4096;

inline constexpr std::uint32_t kManifestMagic   = 0x4D4F4342;  // "BCOM" LE
inline constexpr std::uint32_t kManifestVersion = 1;
inline constexpr std::uint32_t kManifestVersion2 = 2;  // S36-1：条目带 format_ver
inline constexpr char kManifestName[] = "kv.oki.manifest";

// flags 位。v1 只认 bit0；v2 认 bit0|bit1；其余位保留，读端遇未知位
// fail-fast。
inline constexpr std::uint8_t kFlagTomb       = 0x01;
inline constexpr std::uint8_t kFlagHasLoc     = 0x02;  // 仅 v2
inline constexpr std::uint8_t kKnownFlagsMask   = 0x01;
inline constexpr std::uint8_t kKnownFlagsMaskV2 = 0x03;

// S36-1 bloom 参数（持久化格式的一部分——改动即 run 版本演进）。
inline constexpr std::size_t kBloomBitsPerEntry = 10;  // FP ≈ 1%
inline constexpr std::uint8_t kBloomHashes      = 7;

// v2 行的位置字段（= keydir SingleEntry 的盘上子集；epoch 刻意不落盘，
// 同 ord 冲突用 (ord, run gen) 胜出——设计 §D2）。
struct RowLoc {
    std::uint32_t file_id  = 0;
    std::uint32_t total_sz = 0;
    std::uint64_t offset   = 0;
    std::uint64_t tstamp   = 0;
};

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
    // version：kRunVersion（v1，行不带位置）或 kRunVersion2（全字段+bloom）。
    // expected_entries：v2 bloom 预估容量（bloom 需先定尺寸再流式置位；
    // 实际条数超出仅推高误判率，不影响正确性——0 = 最小 bloom）。
    [[nodiscard]] static std::expected<OkiRunWriter, OkiError>
    create(std::string path, std::size_t block_target = kDefaultBlockBytes,
           std::uint32_t version = kRunVersion,
           std::uint64_t expected_entries = 0);

    OkiRunWriter(OkiRunWriter&&) noexcept = default;
    OkiRunWriter& operator=(OkiRunWriter&&) noexcept = default;
    OkiRunWriter(const OkiRunWriter&) = delete;
    OkiRunWriter& operator=(const OkiRunWriter&) = delete;
    ~OkiRunWriter() = default;

    // key 必须严格大于上一条（升序、去重由上游负责）；违反返回 kOutOfOrder。
    // loc：v2 位置字段（v1 传非空 → kBadState；v2 传空 = 免位置行，如墓碑）。
    [[nodiscard]] std::expected<void, OkiError>
    add(std::span<const std::byte> key, std::uint64_t ord, bool tomb,
        const RowLoc* loc = nullptr);

    struct Stats {
        std::uint64_t entries = 0;
        std::uint64_t blocks = 0;
        std::uint64_t file_bytes = 0;
    };
    // 封口：flush 尾块 + 索引 + trailer + fdatasync + rename。
    [[nodiscard]] std::expected<Stats, OkiError> finish(bool fsync_dir = false);

private:
    OkiRunWriter(detail::AtomicFileWriter&& w, std::size_t block_target,
                 std::uint32_t version, std::uint64_t expected_entries)
        : w_(std::move(w)), block_target_(block_target), version_(version) {
        if (version_ == kRunVersion2) init_bloom(expected_entries);
    }

    void init_bloom(std::uint64_t expected_entries);
    void bloom_insert(std::string_view key);

    [[nodiscard]] bool flush_block();  // 空块 no-op；fwrite + CRC + 记账

    struct IndexEntry {
        std::string first_key;
        std::uint64_t off = 0;
    };

    detail::AtomicFileWriter w_;
    std::size_t   block_target_;
    std::uint32_t version_ = kRunVersion;
    std::vector<std::byte> blk_;        // 攒块缓冲
    std::string   blk_first_key_;       // 当前块首 key（索引条目）
    std::string   prev_key_;            // 块内前缀差分状态
    std::uint64_t prev_ord_ = 0;        // 块内 ord 差分状态
    std::uint64_t prev_tstamp_ = 0;     // 块内 tstamp 差分状态（v2）
    std::vector<std::uint64_t> bloom_bits_;  // v2；64-bit word 数组
    std::uint64_t bloom_nbits_ = 0;
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
        // S36-1：v2 全字段（has_loc=false 时 loc 无意义；v1 行恒 false）。
        bool          has_loc = false;
        RowLoc        loc{};
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
        std::uint64_t prev_tstamp_ = 0;  // v2 块内差分状态
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
    [[nodiscard]] std::uint32_t version() const noexcept { return version_; }
    // v2：bloom 试探（false = 必不存在；true = 可能存在）。v1 恒 true。
    [[nodiscard]] bool may_contain(std::span<const std::byte> key) const noexcept;

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
    std::uint32_t version_ = kRunVersion;
    std::vector<std::uint64_t> bloom_bits_;  // v2；open 时整载
    std::uint64_t bloom_nbits_ = 0;
    std::uint8_t  bloom_k_ = 0;
};

// ---------------------------------------------------------------------------
// S36-1 外排构建器：无序流式喂行 → 每 spill_bytes 排序落临时 spill run
// （`kv.oki.spill-<gen>-<n>`，sweep 顺带清理崩溃残留）→ finish 时 k 路归并
// 成单个正式 run。同 key 胜出 = max (ord, 来源序)——来源序 = spill 序号
// （内存尾批最高），与 Level B 的 (ord, run gen) 格同构（设计 §D2）。
// 取代 rebuild 的全内存 sort（100M 档 GB 级峰值 → 有界 spill_bytes）。
// ---------------------------------------------------------------------------
class SpillingRunBuilder {
public:
    static constexpr std::size_t kDefaultSpillBytes = 64u << 20;  // 64 MiB

    // 产出 `<dir>/kv.oki.seg-<gen>`。drop_tombstones 仅在「本 run 将是全量
    // 唯一 run」时可开（全归并前提，同 compact_all 的约束）。
    [[nodiscard]] static std::expected<SpillingRunBuilder, OkiError>
    create(std::string dir, std::uint64_t gen,
           std::uint32_t version = kRunVersion,
           std::size_t spill_bytes = kDefaultSpillBytes,
           bool drop_tombstones = false,
           std::size_t block_target = kDefaultBlockBytes);

    SpillingRunBuilder(SpillingRunBuilder&&) noexcept = default;
    SpillingRunBuilder& operator=(SpillingRunBuilder&&) noexcept = default;
    SpillingRunBuilder(const SpillingRunBuilder&) = delete;
    SpillingRunBuilder& operator=(const SpillingRunBuilder&) = delete;
    ~SpillingRunBuilder();  // 未 finish → best-effort 清 spill 文件

    // 无序、可重 key（胜出规则见类注释）。
    [[nodiscard]] std::expected<void, OkiError>
    add(std::span<const std::byte> key, std::uint64_t ord, bool tomb,
        const RowLoc* loc = nullptr);

    [[nodiscard]] std::expected<OkiRunWriter::Stats, OkiError>
    finish(bool fsync_dir = false);

private:
    SpillingRunBuilder() = default;

    struct Row {
        std::string key;
        std::uint64_t ord = 0;
        RowLoc loc{};
        bool tomb = false;
        bool has_loc = false;
        std::uint64_t seq = 0;  // 到达序（同 key 同 ord 后到者胜）
    };
    [[nodiscard]] std::expected<void, OkiError> spill();
    [[nodiscard]] std::string spill_path(std::size_t n) const;
    static void sort_dedup(std::vector<Row>& rows);

    std::string dir_;
    std::uint64_t gen_ = 0;
    std::uint32_t version_ = kRunVersion;
    std::size_t spill_bytes_ = kDefaultSpillBytes;
    bool drop_tombstones_ = false;
    std::size_t block_target_ = kDefaultBlockBytes;
    std::vector<Row> buf_;
    std::size_t buf_bytes_ = 0;
    std::uint64_t seq_ = 0;
    std::uint64_t total_rows_ = 0;
    std::vector<std::string> spill_paths_;
    bool finished_ = false;
};

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------
struct OkiManifestEntry {
    std::uint64_t gen = 0;        // run 文件代号（kv.oki.seg-<gen>）
    std::uint64_t cover_ord = 0;  // 该 run 的覆盖上界（**排他**：run 内最大 ord + 1）
    // S36-1：run 格式版本（1/2）。写端惰性选 manifest 版本：全 v1 → BCOM v1
    //（老读端可读）；含 v2 → BCOM v2（老读端拒收 → 弃用重建，自愈）。
    std::uint8_t  format_ver = 1;
};

struct OkiManifest {
    std::vector<OkiManifestEntry> runs;
    // 联合覆盖水位（oki_wm，**排他上界** = 尚未覆盖的最小 ord；tail 重放
    // 收 ord ≥ wm 的行）。排他语义是刻意的：alloc_ord 首个 LSN 为 0，
    // 含上界表示不了「已覆盖 ord 0」与「什么都没覆盖」的区别。
    std::uint64_t wm = 0;
};

// 唯一 commit point：atomic_write_bytes(fsync_dir=true)。
[[nodiscard]] bool write_manifest(std::string_view dir, const OkiManifest& m);
// 任何校验不过 → nullopt（整体弃用 OKI，重建兜底）。文件不存在同样 nullopt。
[[nodiscard]] std::optional<OkiManifest> read_manifest(std::string_view dir);

}  // namespace bitcask::oki
