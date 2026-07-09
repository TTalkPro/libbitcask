// 向量库的内存索引侧表（Index）。
//
// Index 是 legacy KeyDir 的演化版（doc/vector-db-design-zh.md §3.1）：主映射
// 从「key → location」改为「ext_id → 最新 ord」+ 一组「ord 下标的数组」
// （slots / ord2ext / live）。ord 是引擎单调分配的 per-write 序号，永不复用，
// 所以 ord→X 用数组而非 hashmap（O(1) 下标、省内存）。
//
// V1 范围：只承载身份映射 + 文档定位 + 软删，不含 BM25 倒排 / HNSW / fold MVCC。
//
// === 线程模型 ===
// 所有 public 方法线程安全：读取 shared_lock、写入 unique_lock。caller 不应
// 在外部预先持锁。组合操作（如 get 后 put）非原子。*_locked 后缀的
// 私有方法要求 caller 已持 unique_lock。

#pragma once

#include "bitcask/live_checker.hpp"
#include "bitcask/doc_table.hpp"  // S16-3：Index 实现 DocTable（查询面只读身份表）
#include "bitcask/index_ids.hpp"  // S27-1：Lsn/DocId 角色别名
#include "bitcask/meta_filter.hpp"  // S13-P8：eval_meta 锁内求值
#include "bitcask/string_hash.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bitcask::index {

// 透明 hash 已提到 bitcask/string_hash.hpp 与 KeyDir 共用。
using bitcask::StringHash;

// 一条文档在磁盘上的定位（pread 整条 kDoc 用）。
// S21-1：字段按对齐降序排（offset 在前），sizeof 24→16（消 8B padding）。
// 构造点一律 designated initializer——位置式聚合初始化在字段重排下会静默错位。
struct DocLoc {
    std::uint64_t offset   = 0;
    std::uint32_t file_id  = 0;
    std::uint32_t total_sz = 0;
};

// 按 ord 存的每文档元信息（slots[ord]）。
// S21-1：不含 ord——slots_ 本身按 ord 下标寻址，存 ord 是每槽 8B 死重
// （原注释自证「仅 get() 返回时填充」）。get() 返回带 ord 的 DocHit。
// sizeof 40→24（连同 DocLoc 重排），每 chunk 槽数组 2.5MB→1.5MB。
struct DocSlot {
    DocLoc        loc;
    std::uint32_t tstamp  = 0;
    std::uint32_t doc_len = 0;   // BM25 token 数（V2 由 analyzer 填）
};

// get() 的返回形态：存储槽字段 + 查询时现填的 ord。聚合继承使既有
// 消费点（slot->loc / slot->tstamp / slot->ord）全部无感。
struct DocHit : DocSlot {
    std::uint64_t ord = 0;
};

struct IndexInfo {
    std::uint64_t live_docs  = 0;   // 当前存活文档数（= ext2ord_.size()）
    std::uint64_t total_ords = 0;   // 历史分配 ord 数（含已死）
    std::uint64_t next_ord   = 0;
    std::uint64_t chunks_allocated = 0;   // 分配过的 chunk 总数
    std::uint64_t chunks_freed     = 0;   // 被 compact_chunks 释放的 chunk 数
};

// ---- 分块数组（Tiered Arrays, 方案 B）----
// slots_ 和 ord2ext_ 按 chunk 分块，live_count == 0 的 chunk 可在 merge 后释放。
// live_ 和 doc_lens_ 保持平坦（SIMD fill_is_live / fill_doc_lens 需要）。
// 设计详见 doc/ord-recycling-design-zh.md §5。
static constexpr std::size_t kChunkOrds = 65536;   // 每 chunk 64K 个 ord

struct Chunk {
    std::array<DocSlot,     kChunkOrds> slots;      // 24B × 64K = 1.5 MB
    std::array<std::string, kChunkOrds> ord2ext;    // ~32B × 64K = 2 MB (SSO)
    std::uint32_t live_count = 0;                    // chunk 内存活 ord 数；== 0 可释放
};

class Index : public bm25::DocTable,
              public bm25::DocLenWriter,
              public bm25::CompactionStats {
public:
    Index() = default;
    Index(const Index&) = delete;
    Index& operator=(const Index&) = delete;

    // ---- ord 分配 ----
    // 拿下一个 ord（写 record header 前调用）。S27-1：产出 LSN。线程安全：unique_lock。
    Lsn alloc_ord();

    // ---- 写 ----
    // 登记一条文档（append 落盘后调用）。若 ext_id 已存在 → update：旧 ord
    // 在 live 中清 0（软删），ext2ord 改指新 ord。内部把 next_ord 推到
    // max(next_ord, ord+1)，故恢复时按 ord 序回放亦走此方法。
    // S27-1：`docid` 既是数组下标又推进 LSN 水位（当前 docid==lsn）；分段化后
    // 数组下标语义归 DocId、水位推进另由 LSN 承担。线程安全：unique_lock。
    void put_doc(std::string_view ext_id, DocId docid, const DocSlot& slot);

    // 删除：软删 ext_id 当前文档（清 live）、erase ext2ord。tomb_ord 是墓碑
    // record 自身的 ord（仅用于推进 next_ord）→ 纯 LSN 角色。返回原本是否存在。
    // 线程安全：unique_lock。
    bool remove(std::string_view ext_id, Lsn tomb_ord);

    // S16-2：单独回填 doc_len（BM25 token 数）。写路径反转后 DocSlot 由
    // 宿主先落（doc_len=0，宿主不做分析拿不到 token 数），分析产物由
    // BM25 侧（reducer 单写者）经本方法补写。同时更新 slots_ AoS 与
    // doc_lens_ SoA（序列化读前者，SIMD gather 读后者）。ord 未登记则
    // no-op（防御：空 job 守卫路径）。线程安全：unique_lock。
    // S18-1：override bm25::DocLenWriter——P4 起 TextPlugin 经窄接口回填。
    // S27-1：按 DocId(数组下标) 定位。
    void set_doc_len(DocId docid, std::uint32_t len) override;

    // V5:存储 ord 的 meta blob(结构化 KV 二进制,可为空)。与 put_doc
    // 在同一 unique_lock 下调用——保证 meta 与定位/live 同写入原子点,
    // 后续读路径不必额外同步。blob 由 Index 内部拷贝(caller 可立即
    // 释放源缓冲)。线程安全:unique_lock。
    void set_meta(DocId docid, std::span<const std::byte> blob);

    // ---- S18-2：docmap 持久化记账（自记账原则：写它的人负责记账）----
    //
    // 脏位：put_doc/remove/set_doc_len/set_meta/compact_chunks 内部置位；
    // docmap 保存方（宿主 Cask / legacy SearchLayer 路径）保存或载入成功后
    // clear_dirty()。初值 true（未知状态一律重序列化）。
    [[nodiscard]] bool dirty() const noexcept {
        return dirty_.load(std::memory_order_relaxed);
    }
    void clear_dirty() noexcept {
        dirty_.store(false, std::memory_order_relaxed);
    }

    // delta 窗口删除日志（docmap delta 的 remove 半边）。remove() 在
    // tomb_ord ≥ 窗口水位时内部入账（S14-4 门限语义：fold 重叠区旧墓碑
    // 不入，防跨文件 stale removal 重放误杀复活文档）。窗口水位由 docmap
    // 保存方在 base/delta 落成或载入完成时经 begin_delta_window 推进；
    // ckpt 载入重放产生的污染条目由载入方在收尾 clear_removals() 清除。
    void begin_delta_window(std::uint64_t wm);
    // 静止点快照（save 路径消费；拷贝返回，窗口内条目量小）。
    [[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>>
    removals_snapshot() const;
    void clear_removals();

    // ---- 读 ----
    // 取 ext_id 当前存活文档的定位（含现填的 ord）；不存在/已删返回 nullopt。
    // 线程安全：shared_lock。
    [[nodiscard]] std::optional<DocHit> get(std::string_view ext_id) const;

    // ord → ext_id（检索结果翻译用；V1 主要给调试/恢复）。越界返回 nullopt。
    // S16-3：override DocTable::ord_to_ext。S27-1：按 DocId 定位。
    [[nodiscard]] std::optional<std::string> ord_to_ext(DocId docid) const override;

    // 某 ord 是否存活。越界返回 false。线程安全：shared_lock。
    // 同时实现 LiveChecker::is_live。S27-1：按 DocId 定位。
    [[nodiscard]] bool is_live(DocId docid) const override;

    // LiveChecker::doc_len — 返回 ord 对应文档的 token 数，越界返回 0。
    [[nodiscard]] std::uint32_t doc_len(DocId docid) const override;

    // S16-3：DocTable::ord_of — ext_id → ord（explain 等 key→ord 反查）。
    // get() 的窄投影，避免查询面暴露完整 DocSlot。S27-1：返回当前版本 DocId。
    [[nodiscard]] std::optional<DocId>
    ord_of(std::string_view ext_id) const override;

    // V5:取 ord 的原始 meta blob(结构化 KV 二进制)。越界或空 → 空 vector,
    // 让上层 filter 直接判 false 跳过(无 meta = 不通过过滤)。
    // 线程安全:shared_lock。返回**拷贝**而非 span——读路径无锁并发,而
    // set_meta(worker 线程)会重分配底层 vector;若返回内部 span 会在锁外
    // 被并发 set_meta 释放(use-after-free)。锁内拷贝杜绝逃逸。
    [[nodiscard]] std::vector<std::byte> meta_blob(DocId docid) const;

    // S13-P8：meta filter 锁内求值——省去 meta_blob 的锁内堆拷贝（过滤查询
    // 每候选一次，overfetch K'=4k 时最多 4k 次拷贝/查询）。evaluate 是纯读
    // 无 IO（meta_filter.hpp），shared_lock 内直接跑安全。
    // 语义与「meta_blob 后 evaluate」一致：无 meta（空 blob）恒 false。
    [[nodiscard]] bool eval_meta(DocId docid,
                                 const meta::MetaFilter& filter) const override;

    // P2.1 批量版本：一次 shared_lock 完成整个数组（逐 posting 版本每条
    // posting 一次锁 + 一次虚调用，热词查询 = 数十万次锁操作且阻断评分
    // 循环的自动向量化）。
    void fill_is_live(std::span<const std::uint64_t> ords,
                      std::span<char> out) const override;
    void fill_doc_lens(std::span<const std::uint64_t> ords,
                       std::span<std::uint32_t> out) const override;

    // 释放所有 live_count == 0 的 chunk（merge 后调用）。
    // 返回释放的 chunk 数。线程安全：unique_lock。
    std::uint64_t compact_chunks();

    // ---- 内省 ----
    [[nodiscard]] IndexInfo info() const;

    // 当前存活文档数（= ext2ord_.size()）。线程安全：shared_lock。
    // S18-4：override bm25::CompactionStats。
    [[nodiscard]] std::uint64_t live_docs() const override {
        std::shared_lock lk(mutex_);
        return live_docs_;
    }

    // S12-2：自上次 reset 以来退休（软删/覆盖/删除）的文档版本数——put_doc 覆盖旧版本
    // 与 remove 各计 1。用于后台自动 compaction 的节流触发。仅 reducer 线程读写
    // （put_doc/remove 在写路径、maybe_auto_compact 在同线程末尾），故非并发热点。
    // 线程安全：shared_lock 读 / unique_lock 重置（与 put_doc/remove 的 unique_lock 一致）。
    [[nodiscard]] std::uint64_t retired_since_compact() const override {
        std::shared_lock lk(mutex_);
        return retired_since_compact_;
    }
    void reset_retired_since_compact() override {
        std::unique_lock lk(mutex_);
        retired_since_compact_ = 0;
    }

    // 遍历所有 live 文档，对每个调用 fn(ord, ext_id, slot)。
    // 线程安全：持 shared_lock。
    template <typename Fn>
    void for_each_live(Fn&& fn) const {
        std::shared_lock lk(mutex_);
        for (std::uint64_t ord = 0; ord < live_.size(); ++ord) {
            if (live_[ord]) {
                const auto ci = ord / kChunkOrds;
                const auto si = ord % kChunkOrds;
                fn(ord, chunks_[ci]->ord2ext[si], chunks_[ci]->slots[si]);
            }
        }
    }

    // S14-4：范围版——只遍历 ord ∈ [from, to) 的 live 文档（docmap delta
    // 行提取用）。语义/锁与 for_each_live 一致。
    template <typename Fn>
    void for_each_live_in(std::uint64_t from, std::uint64_t to, Fn&& fn) const {
        std::shared_lock lk(mutex_);
        const std::uint64_t hi =
            std::min<std::uint64_t>(to, live_.size());
        for (std::uint64_t ord = from; ord < hi; ++ord) {
            if (live_[ord]) {
                const auto ci = ord / kChunkOrds;
                const auto si = ord % kChunkOrds;
                fn(ord, chunks_[ci]->ord2ext[si], chunks_[ci]->slots[si]);
            }
        }
    }

    // ---- S18-2：docmap sidecar（"BCIS"）序列化（自 SearchLayer 平移）----
    // 把 ord → (ext_id, DocSlot) 活映射落进 checkpoint，避免冷启动全量 fold
    // 重建。covers_next_ord 记录快照覆盖的 ord 水位（与后续 WAL/data 的衔接
    // 点）。布局：header(magic+version+covers+行数) + 每活文档一行（固定
    // 34B + 变长 ext_id）+ CRC。仅遍历 live（死文档由 keydir LWW 过滤）。
    // serialize 返回 false 仅当某 ext 超 64KiB；deserialize 校验失败返回
    // nullopt，成功返回 covers。
    [[nodiscard]] bool serialize_docmap(std::vector<std::uint8_t>& out,
                                        std::uint64_t covers_next_ord) const;
    [[nodiscard]] std::optional<std::uint64_t>
    deserialize_docmap(std::span<const std::uint8_t> bytes);

private:
    mutable std::shared_mutex mutex_;

    std::unordered_map<std::string, std::uint64_t,
                       StringHash, std::equal_to<>> ext2ord_;  // ext_id → 最新 ord

    std::vector<std::unique_ptr<Chunk>> chunks_;               // chunk N 覆盖 [N*64K, (N+1)*64K)

    std::vector<std::uint8_t>  live_;       // 下标 = ord;0/1。平坦保持以兼容 SIMD gather。
    std::vector<std::uint32_t> doc_lens_;   // P2.4 SoA 副本;平坦保持以兼容 SIMD gather。
    // V5 per-ord meta。S21-1 惰性化：首个非空 set_meta 前恒 empty（不随 ord
    // 增长），无 meta 部署零常驻（改前每 ord 白付 24B 空 vector 头）。启用后
    // 与 live_ 同步跟平。读路径经 ord >= size() 门禁天然得到「空 blob」语义。
    std::vector<std::vector<std::byte>> meta_blobs_;

    std::uint64_t next_ord_       = 0;
    std::uint64_t live_docs_      = 0;
    std::uint64_t retired_since_compact_ = 0;  // S12-2：自上次 compact 起退休的文档版本数
    std::uint64_t chunks_alloc_   = 0;      // 历史分配 chunk 数（内省用）
    std::uint64_t chunks_freed_   = 0;      // 被 compact_chunks 释放的 chunk 数

    // S18-2：docmap 持久化记账（见 public 段注释）。removals_ 受 mutex_
    // 保护（remove 已持 unique_lock）；dirty_ 为 relaxed 原子（与原
    // SearchLayer::dirty_docmap_ 语义一致：写点已被 reducer/静止点串行化）。
    std::atomic<bool> dirty_{true};
    std::uint64_t delta_window_wm_ = 0;
    std::vector<std::pair<std::string, std::uint64_t>> removals_;

    void ensure_capacity_locked(std::uint64_t ord);
};

}  // namespace bitcask::index
