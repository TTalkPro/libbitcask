// OKI（有序 Key 索引）run 与 manifest——S33-3。
// 设计：doc/ordered-key-index-design-zh.md §3。
//
// run（`kv.oki.seg-<gen>`，"BCOK"）是按 key 升序排列的不可变文件。
// v1 条目只存 (key, ord, tomb)——range 查询逐 key 回查哈希 keydir 取权威
// 位置。**v2（S36 Level B，doc/keydir-disk-resident-design-zh.md §4）条目
// 可携全字段位置**（file_id/total_sz/offset/tstamp），并内嵌 bloom——
// 组合视图点查的权威载体。v3（S43-1，D2 §4-C）在 v2 行格式上为每个数据
// 块追加独立 CRC32：开库只校验 header/索引/bloom/元数据 CRC，数据块延迟
// 到首读时校验。v1/v2/v3 读端都支持；v1/v2 保持 eager 整文件 CRC。
//
// 磁盘布局（全小端）：
//   header  8B: magic "BCOK" u32 | version u32 = 1、2 或 3
//   数据块区（块目标 ~4KiB，块界由稀疏索引给出；**每块解码状态复位**
//   prev_key="" / prev_ord=0 / prev_tstamp=0，块首条自然退化为全量值）：
//     每条: [vbyte shared_len][vbyte suffix_len][suffix]
//           [vbyte ord_delta(u64 二补数回绕相对 prev_ord)][flags u8]
//     v2 且 flags.bit1(has_loc) 时追加:
//           [vbyte file_id][vbyte total_sz][vbyte offset]
//           [vbyte tstamp_delta(回绕相对 prev_tstamp)]
//     flags: bit0 = tomb；bit1 = has_loc（v2+；墓碑行免位置字段）；
//     其余位保留，读端遇未知位 fail-fast（kCorrupt → 整个 run 弃用重建）。
//   稀疏索引区: [count u32] + count × { [vbyte klen][块首 key][block_off u64] }
//     v3 每条追加 [block_crc u32]。
//   bloom 区（v2/v3）: [n_bits u64][k u8][位数组 ceil(n_bits/8) 字节]
//     稳定哈希（FNV-1a64 + splitmix64 双哈希），持久化格式的一部分。
//   trailer: v1/v2 24/32B
//            [entry_count u64][index_off u64][(v2+) bloom_off u64][crc][magic]
//     v1/v2 CRC 覆盖 [0, size-8)；v3 CRC 覆盖小区域元数据（不含数据块）。
//
    // 有序 key 的公共前缀差分对 `prefix:id` 形态收益显著；ord 差分沿用
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

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bitcask/io.hpp"
#include "bitcask/detail/file_util.hpp"

namespace bitcask::oki {

inline constexpr std::uint32_t kRunMagic        = 0x4B4F4342;  // "BCOK" LE
inline constexpr std::uint32_t kRunTrailerMagic = 0x454F4342;  // "BCOE" LE
inline constexpr std::uint32_t kRunVersion      = 1;
inline constexpr std::uint32_t kRunVersion2     = 2;   // S36-1：全字段 + bloom
inline constexpr std::uint32_t kRunVersion3     = 3;   // S43-1：per-block CRC
inline constexpr std::size_t   kRunHeaderSize   = 8;
inline constexpr std::size_t   kRunTrailerSize  = 24;   // v1
inline constexpr std::size_t   kRunTrailerSizeV2 = 32;  // v2（+bloom_off u64）
inline constexpr std::size_t   kDefaultBlockBytes = 4096;

inline constexpr std::uint32_t kManifestMagic   = 0x4D4F4342;  // "BCOM" LE
inline constexpr std::uint32_t kManifestVersion = 1;
inline constexpr std::uint32_t kManifestVersion2 = 2;  // S36-1：条目带 format_ver
// S36-4：v2 布局 + 头部 flags 字节（bit0 = level_b——「由挂钩在线的
// Level B 写者维护，run 内位置字段可信」的模式戳；Level B 开启对未带戳
// 的目录必须全量重建起步，Level A 写者 open 时清戳）。
inline constexpr std::uint32_t kManifestVersion3 = 3;
// S43-2：v3 布局 + 结构化全局/文件元数据段。v4 是索引元数据迁移到
// OKI 提交点的第一步；旧 v3 读端拒收新 manifest 后只能重建，因此写端
// 在元数据首次可用时才升 v4，读端则同时支持 v1/v2/v3/v4。
inline constexpr std::uint32_t kManifestVersion4 = 4;
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
// S36-3：块 LRU 缓存（设计 §2/§5.1——冷点查的热块驻留）。
//
// key = (run gen, 块下标)。gen 由 manifest 单调分配、永不重用 ⟹ 缓存条目
// 跨 run 删除/重开天然无别名（旧 gen 条目只是等 LRU 淘汰或 purge 的死重）。
// 分片小锁（**独立锁域，不进 keydir 锁序链**——设计 §9）；块 shared_ptr
// 共享所有权，读者拿到后锁外解码，淘汰不影响在途读者。
// 线程模型：全方法线程安全；loader 在锁外执行（IO 不占锁），并发同块
// double-load 允许（后到者复用先到者已插入的块）。
// ---------------------------------------------------------------------------
class OkiBlockCache {
public:
    using Block = std::shared_ptr<const std::vector<std::byte>>;

    explicit OkiBlockCache(std::size_t capacity_bytes) noexcept
        : shard_cap_(capacity_bytes / kShards) {}

    // 命中返回块并升温；miss 调 loader 装载 + 插入（loader 返回 nullopt =
    // IO 失败，不入缓存，本调用返回 nullopt）。
    [[nodiscard]] std::optional<Block> get_or_load(
        std::uint64_t gen, std::uint64_t block_idx,
        const std::function<std::optional<std::vector<std::byte>>()>& loader);

    // 丢弃 gen 不在 keep 集里的全部条目（compact/rebuild 删旧 run 后调，
    // 免死重占位）。
    void purge_except(std::span<const std::uint64_t> keep_gens);

    // 测试/bench：重设容量（0 = 关缓存）；顺带清空。
    void reset_capacity(std::size_t capacity_bytes);

    struct Stats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t bytes = 0;   // 当前驻留字节
        std::uint64_t blocks = 0;  // 当前驻留块数
    };
    [[nodiscard]] Stats stats() const;

private:
    static constexpr std::size_t kShards = 16;
    struct Key {
        std::uint64_t gen = 0;
        std::uint64_t idx = 0;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            // splitmix 风格混合（gen/idx 都是小整数，直接异或会碰撞成灾）。
            std::uint64_t x = k.gen * 0x9E3779B97F4A7C15ull ^ k.idx;
            x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 27;
            return static_cast<std::size_t>(x);
        }
    };
    struct Node {
        Key key;
        Block block;
    };
    struct Shard {
        std::mutex mu;
        std::list<Node> lru;  // front = 最新
        std::unordered_map<Key, std::list<Node>::iterator, KeyHash> map;
        std::size_t bytes = 0;
    };
    [[nodiscard]] Shard& shard_for(const Key& k) noexcept {
        return shards_[KeyHash{}(k) % kShards];
    }
    void evict_over_cap_locked(Shard& sh) noexcept;

    mutable std::array<Shard, kShards> shards_;
    std::atomic<std::size_t> shard_cap_;  // reset_capacity 并发安全
    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};
};

// S36-3：块缓存默认容量（设计 §2 预算表；S36-4/6 时经 CaskOptions 透出）。
inline constexpr std::size_t kDefaultBlockCacheBytes = 256u << 20;

// ---------------------------------------------------------------------------
// Writer：key 严格升序 add，finish 时写索引 + trailer 并原子 rename 就位
// （AtomicFileWriter：中途失败/析构自动清 tmp，最终路径不见半截文件）。
// ---------------------------------------------------------------------------
class OkiRunWriter {
public:
    // block_target：数据块目标字节数（超过即封块）。测试用小值逼出多块。
    // version：v1（无位置）；v2（全字段+bloom）；v3（v2 + per-block CRC）。
    // expected_entries：v2/v3 bloom 预估容量（bloom 需先定尺寸再流式置位；
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
    // loc：v2+ 位置字段（v1 传非空 → kBadState；v2/v3 传空 = 免位置行）。
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
        if (version_ != kRunVersion) init_bloom(expected_entries);
    }

    void init_bloom(std::uint64_t expected_entries);
    void bloom_insert(std::string_view key);

    [[nodiscard]] bool flush_block();  // 空块 no-op；fwrite + CRC + 记账

    struct IndexEntry {
        std::string first_key;
        std::uint64_t off = 0;
        // 仅 v3 落盘；v1/v2 恒 0。
        std::uint32_t crc = 0;
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
// Reader：v1/v2 open 时全文件 CRC 校验；v3 open 只校验小区域元数据，
// 数据块 CRC 由 Cursor::load_block 两条加载路径在首读时校验。
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
        // S36-1：v2+ 全字段（has_loc=false 时 loc 无意义；v1 行恒 false）。
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
        // 当前块字节（缓存块或自有缓冲）。
        [[nodiscard]] std::span<const std::byte> block_span() const noexcept {
            return blk_hold_ ? std::span<const std::byte>(*blk_hold_)
                             : std::span<const std::byte>(blk_);
        }

        const OkiRunReader* r_ = nullptr;
        std::size_t   bi_ = 0;       // 下一个待载入的块下标（已载则为当前+1）
        std::vector<std::byte> blk_; // 当前块数据（无缓存路径）
        // S36-3：块缓存路径——持共享块所有权，blk_ 不用。
        OkiBlockCache* cache_ = nullptr;
        std::uint64_t  cache_gen_ = 0;
        OkiBlockCache::Block blk_hold_;
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

    // S36-3：点查——稀疏索引二分定位块 → 块经 cache 取（nullptr = 直接
    // pread）→ 块内线性扫。命中返回行；不存在返回空 optional（caller 先过
    // bloom，此处的空 = bloom 假阳性或真缺席）。cache_gen = 本 run 的
    // manifest gen（缓存 key 的一半；gen 永不重用 ⟹ 无别名）。
    [[nodiscard]] std::expected<std::optional<Entry>, OkiError>
    find(std::span<const std::byte> key, OkiBlockCache* cache = nullptr,
         std::uint64_t cache_gen = 0) const;

    [[nodiscard]] std::uint64_t entry_count() const noexcept {
        return entry_count_;
    }
    [[nodiscard]] std::size_t block_count() const noexcept {
        return blocks_.size();
    }
    [[nodiscard]] std::uint32_t version() const noexcept { return version_; }
    // v2/v3：bloom 试探（false = 必不存在；true = 可能存在）。v1 恒 true。
    [[nodiscard]] bool may_contain(std::span<const std::byte> key) const noexcept;

private:
    OkiRunReader() = default;

    // seek 本体（S36-3：带可选块缓存——seek()/find() 共用一份定位逻辑）。
    [[nodiscard]] std::expected<Cursor, OkiError>
    seek_impl(std::span<const std::byte> lo, OkiBlockCache* cache,
              std::uint64_t cache_gen) const;

    struct BlockRef {
        std::string   first_key;
        std::uint64_t off = 0;
        std::uint64_t end = 0;  // 下块 off 或 index_off
        // 仅 v3 有效；加载块后与块字节 CRC 比对。
        std::uint32_t crc = 0;
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
    // run 格式版本（1/2/3）。写端惰性选 manifest 版本：全 v1 → BCOM v1
    //（老读端可读）；含 v2/v3 → BCOM v2+（老读端拒收 → 弃用重建，自愈）。
    std::uint8_t  format_ver = 1;
};

// S43-2：随 BCOM v4 原子提交的索引权威派生元数据快照。
//
// 这是提交点上的绝对值，不是运行期热路径缓存。next_ord 允许大于 wm：
// wm 只表示 runs 已覆盖到的 LSN 下界，memdelta/tail 中的行仍属于同一
// 提交快照。next_file_id 显式建模为 biggest_file_id 的下一个分配值。
// oldest_tstamp=0 表示没有 live key；oldest=0 且 newest!=0 是非法快照。
struct OkiManifestMetadata {
    std::uint64_t next_ord = 0;
    std::uint32_t biggest_file_id = 0;
    std::uint32_t next_file_id = 0;
    std::uint64_t active_keys = 0;
    std::uint64_t live_bytes = 0;
    std::uint64_t oldest_tstamp = 0;
    std::uint64_t newest_tstamp = 0;
    // 版本演进保留位：读写端都必须要求 0，避免静默复用语义。
    std::uint64_t reserved = 0;
    bool operator==(const OkiManifestMetadata&) const = default;
};

// 定长 64B：4B file_id + 1B pending_delete + 3B pad + 7×8B 统计。
// expiration_epoch 沿用 BCKS 语义（kMaxEpoch = 无 pending delete）。
struct OkiManifestFStats {
    std::uint32_t file_id = 0;
    bool pending_delete = false;
    std::uint64_t live_keys = 0;
    std::uint64_t total_keys = 0;
    std::uint64_t live_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t oldest_tstamp = 0;
    std::uint64_t newest_tstamp = 0;
    std::uint64_t expiration_epoch = ~std::uint64_t{0};
    bool operator==(const OkiManifestFStats&) const = default;
};

// S43-2：caller 在写者静止点捕获的完整 v4 元数据；OkiState 只透传，
// 不在 flush/rebuild 内部自己感知 IndexAuthority。
struct OkiCommitSnapshot {
    OkiManifestMetadata metadata;
    std::vector<OkiManifestFStats> fstats;
};

struct OkiManifest {
    std::vector<OkiManifestEntry> runs;
    // 联合覆盖水位（oki_wm，**排他上界** = 尚未覆盖的最小 ord；tail 重放
    // 收 ord ≥ wm 的行）。排他语义是刻意的：alloc_ord 首个 LSN 为 0，
    // 含上界表示不了「已覆盖 ord 0」与「什么都没覆盖」的区别。
    std::uint64_t wm = 0;
    // S36-4：Level B 模式戳（BCOM v3；见 kManifestVersion3 注释）。
    bool level_b = false;
    // S43-2：v4 才落盘；v1-v3 双读时保持 nullopt，由 caller 迁移计算。
    std::optional<OkiManifestMetadata> metadata;
    std::vector<OkiManifestFStats> fstats;
    // S43-3：各 data file 的已提交字节水位（file_id → 排他 EOF）。这是
    // 「ord 水位 wm」在物理 data 文件上的落点：tail replay 直接从该字节
    // 之后 fold，不再为找出 ord>=wm 的尾部而读全库。旧 v4 manifest 没有
    // 本段时为空——recovery 退化为全量 data 扫描 + ord 门，方向仍安全。
    // v1-v3 恒为空。
    std::vector<std::pair<std::uint32_t, std::uint64_t>> data_wms;
};

// 唯一 commit point：atomic_write_bytes(fsync_dir=true)。
[[nodiscard]] bool write_manifest(std::string_view dir, const OkiManifest& m);
// 任何校验不过 → nullopt（整体弃用 OKI，重建兜底）。文件不存在同样 nullopt。
[[nodiscard]] std::optional<OkiManifest> read_manifest(std::string_view dir);

}  // namespace bitcask::oki
