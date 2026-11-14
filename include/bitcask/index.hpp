// 向量库的内存索引侧表（Index）。
//
// Index 是 legacy KeyDir 的演化版（doc/vector-db-design-zh.md §3.1）。S42 起
// 改为 live-only 存储（docs/design/s42-docmap-live-store-design-zh.md）：
//   ext2ord_ : ext_id → 最新 ord（node-based，key 地址稳定，S40）
//   live_    : ord → LiveRow（仅当前存活行；ord 是 map key，不再做数组下标）
//   meta_    : ord → meta blob（仅 live；空 blob 即删除条目）
//   delta_live_ : 当前 checkpoint 窗口内仍 live 的 ord journal
// ord/LSN 仍全局单调、永不复用；覆盖写/删除在写路径同步淘汰旧行，
// 稳态内存 O(live docs) 而非 O(historical ords)。
//
// V1 范围：只承载身份映射 + 文档定位 + 软删，不含 BM25 倒排 / HNSW / fold MVCC。
//
// === 线程模型 ===
// 所有 public 方法线程安全：读取 shared_lock、写入 unique_lock。caller 不应
// 在外部预先持锁。组合操作（如 get 后 put）非原子。

#pragma once

#include "bitcask/live_checker.hpp"
#include "bitcask/doc_table.hpp"  // S16-3：Index 实现 DocTable（查询面只读身份表）
#include "bitcask/io.hpp"  // 6.6.0：deserialize_docmap 流式重载的 FileHandle
#include "bitcask/index_ids.hpp"  // S27-1：Lsn/DocId 角色别名
#include "bitcask/meta_filter.hpp"  // S13-P8：eval_meta 锁内求值
#include "bitcask/string_hash.hpp"

#include <ankerl/unordered_dense.h>  // S42：live_/meta_/delta_live_ 稠密 map

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
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

// 每文档元信息（S42 起挂在 LiveRow.slot，不再按 ord 下标展开）。
// S21-1：不含 ord——ord 是 live_ 的 map key，存槽里是每行 8B 死重。
// tstamp 随 64 位时间戳 flag-day 为 u64；sizeof 32B（含尾部 padding）。
struct DocSlot {
    DocLoc        loc;
    std::uint64_t tstamp  = 0;
    std::uint32_t doc_len = 0;   // BM25 token 数（V2 由 analyzer 填）
};

// get() 的返回形态：存储槽字段 + 查询时现填的 ord。聚合继承使既有
// 消费点（slot->loc / slot->tstamp / slot->ord）全部无感。
struct DocHit : DocSlot {
    std::uint64_t ord = 0;
};

// S42：live-only 行。ext 指向 ext2ord_ 节点键（node-based，地址稳定，S40）；
// slot 为文档定位与长度。ord 是 live_ 的 map key，不重复存储。
struct LiveRow {
    std::string_view ext;
    DocSlot slot;
};

// S42：升序遍历/checkpoint 快照的行引用。ext/slot 指向 Index 内部（live_ 值
// 与 ext2ord_ 节点键），仅在遍历回调的 shared lock 作用域内有效。
struct DocRowRef {
    std::uint64_t ord;
    std::string_view ext;
    const DocSlot* slot;
};

struct IndexInfo {
    std::uint64_t live_docs  = 0;   // 当前存活文档数（= ext2ord_.size()）
    std::uint64_t total_ords = 0;   // 历史分配 ord 数（含已死）
    std::uint64_t next_ord   = 0;
    std::uint64_t chunks_allocated = 0;   // S42 起冻结为 0（诊断兼容，保留字段）
    std::uint64_t chunks_freed     = 0;   // S42 起冻结为 0（诊断兼容，保留字段）
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
    // 的 live row/meta/journal 写路径同步淘汰（S42，不再等 compaction），
    // ext2ord 改指新 ord。内部把 next_ord 推到 max(next_ord, ord+1)，
    // 故恢复时按 ord 序回放亦走此方法。线程安全：unique_lock。
    void put_doc(std::string_view ext_id, DocId docid, const DocSlot& slot);

    // 删除：软删 ext_id 当前文档（淘汰 live row）、erase ext2ord。tomb_ord 是
    // 墓碑 record 自身的 ord（仅用于推进 next_ord）→ 纯 LSN 角色。返回原本
    // 是否存在。线程安全：unique_lock。
    bool remove(std::string_view ext_id, Lsn tomb_ord);

    // S16-2：单独回填 doc_len（BM25 token 数）。写路径反转后 DocSlot 由
    // 宿主先落（doc_len=0，宿主不做分析拿不到 token 数），分析产物由
    // BM25 侧（reducer 单写者）经本方法补写。ord 未登记则 no-op（防御）。
    // S18-1：override bm25::DocLenWriter。线程安全：unique_lock。
    void set_doc_len(DocId docid, std::uint32_t len) override;

    // V5:存储 ord 的 meta blob(结构化 KV 二进制,可为空)。与 put_doc
    // 在同一 unique_lock 下调用——保证 meta 与定位/live 同写入原子点。
    // S42：meta 只挂 live 行——ord 未登记（或已死）时 no-op。blob 由
    // Index 内部拷贝(caller 可立即释放源缓冲)。线程安全:unique_lock。
    void set_meta(DocId docid, std::span<const std::byte> blob);

    // ---- S18-2：docmap 持久化记账（自记账原则：写它的人负责记账）----
    //
    // 脏位：put_doc/remove/set_doc_len/set_meta 内部置位；docmap 保存方
    // （宿主 Cask / legacy SearchLayer 路径）保存或载入成功后 clear_dirty()。
    // 初值 true（未知状态一律重序列化）。
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

    // S42-3：checkpoint 载入期抑制 delta journal（RAII；析构恢复）。
    // 载入重放的行会在收尾 begin_delta_window 时整体清空，逐行入账只是
    // 白付开库峰值内存——base 重放期间置位本 guard 直接跳过 journal 维护。
    class ReplayJournalGuard {
    public:
        explicit ReplayJournalGuard(Index& idx) : idx_(idx) {
            idx_.journal_suppressed_ = true;
        }
        ~ReplayJournalGuard() { idx_.journal_suppressed_ = false; }
        ReplayJournalGuard(const ReplayJournalGuard&) = delete;
        ReplayJournalGuard& operator=(const ReplayJournalGuard&) = delete;

    private:
        Index& idx_;
    };

    // S42：当前 delta 窗口 journal 的有序快照（诊断/测试用）。
    // 不变量：快照 ⊆ live（覆盖写/删除同步淘汰旧 ord）。
    [[nodiscard]] std::vector<std::uint64_t> delta_live_snapshot() const {
        std::shared_lock lk(mutex_);
        std::vector<std::uint64_t> ords;
        ords.reserve(delta_live_.size());
        for (auto ord : delta_live_) ords.push_back(ord);
        std::sort(ords.begin(), ords.end());
        return ords;
    }

    // ---- 读 ----
    // 取 ext_id 当前存活文档的定位（含现填的 ord）；不存在/已删返回 nullopt。
    // 线程安全：shared_lock。
    [[nodiscard]] std::optional<DocHit> get(std::string_view ext_id) const;

    // ord → ext_id（检索结果翻译用）。已死返回 nullopt（S40 契约）。
    // S16-3：override DocTable::ord_to_ext。
    [[nodiscard]] std::optional<std::string> ord_to_ext(DocId docid) const override;

    // 某 ord 是否存活。同时实现 LiveChecker::is_live。线程安全：shared_lock。
    [[nodiscard]] bool is_live(DocId docid) const override;

    // LiveChecker::doc_len — 返回 ord 对应文档的 token 数，未登记/已死返回 0。
    [[nodiscard]] std::uint32_t doc_len(DocId docid) const override;

    // S16-3：DocTable::ord_of — ext_id → ord（explain 等 key→ord 反查）。
    [[nodiscard]] std::optional<DocId>
    ord_of(std::string_view ext_id) const override;

    // V5:取 ord 的原始 meta blob(结构化 KV 二进制)。未登记/已死/空 → 空
    // vector, 让上层 filter 直接判 false 跳过。线程安全:shared_lock。返回
    // **拷贝**而非 span——读路径无锁并发,set_meta(worker 线程)会重分配
    // 底层 vector;锁内拷贝杜绝逃逸。
    [[nodiscard]] std::vector<std::byte> meta_blob(DocId docid) const;

    // S13-P8：meta filter 锁内求值——省去 meta_blob 的锁内堆拷贝。
    // 语义：死 ord / 无 meta（空 blob）恒 false。
    [[nodiscard]] bool eval_meta(DocId docid,
                                 const meta::MetaFilter& filter) const override;

    // P2.1 批量版本：一次 shared_lock 完成整个数组。S42：平坦数组退役后
    // 退化为逐 candidate hash lookup（放弃全局 SIMD gather；BM25 主查询
    // 已分段化，段内用 segment-local live checker，不走本接口）。
    void fill_is_live(std::span<const std::uint64_t> ords,
                      std::span<char> out) const override;
    void fill_doc_lens(std::span<const std::uint64_t> ords,
                       std::span<std::uint32_t> out) const override;

    // S42 起为兼容 no-op：live-only 结构在写路径同步淘汰死行，
    // 不存在可释放的 chunk。保留 API 减少调用方（Cask merge / 测试 shim）
    // 改动面。返回恒 0。线程安全：unique_lock。
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
    // 与 remove 各计 1。用于后台自动 compaction 的节流触发。
    // 线程安全：shared_lock 读 / unique_lock 重置。
    [[nodiscard]] std::uint64_t retired_since_compact() const override {
        std::shared_lock lk(mutex_);
        return retired_since_compact_;
    }
    void reset_retired_since_compact() override {
        std::unique_lock lk(mutex_);
        retired_since_compact_ = 0;
    }

    // 遍历所有 live 文档，对每个调用 fn(ord, ext_id, slot)。
    // 线程安全：持 shared_lock。S40：ext_id 以 std::string_view 传入（零拷贝，
    // 仅在回调内有效）。S42：hash 迭代无序——checkpoint/测试兼容要求按
    // ord 升序回调，锁内收集 RowRef 排序后再回调。
    template <typename Fn>
    void for_each_live(Fn&& fn) const {
        std::shared_lock lk(mutex_);
        std::vector<DocRowRef> refs;
        refs.reserve(live_.size());
        for (const auto& [ord, row] : live_) {
            refs.push_back(DocRowRef{ord, row.ext, &row.slot});
        }
        std::sort(refs.begin(), refs.end(),
                  [](const DocRowRef& a, const DocRowRef& b) {
                      return a.ord < b.ord;
                  });
        for (const auto& r : refs) fn(r.ord, r.ext, *r.slot);
    }

    // S14-4：范围版——只遍历 ord ∈ [from, to) 的 live 文档（docmap delta
    // 行提取用）。语义/锁/升序保证与 for_each_live 一致。
    template <typename Fn>
    void for_each_live_in(std::uint64_t from, std::uint64_t to, Fn&& fn) const {
        std::shared_lock lk(mutex_);
        std::vector<DocRowRef> refs;
        for (const auto& [ord, row] : live_) {
            if (ord >= from && ord < to) {
                refs.push_back(DocRowRef{ord, row.ext, &row.slot});
            }
        }
        std::sort(refs.begin(), refs.end(),
                  [](const DocRowRef& a, const DocRowRef& b) {
                      return a.ord < b.ord;
                  });
        for (const auto& r : refs) fn(r.ord, r.ext, *r.slot);
    }

    // V5 meta 持久化：遍历 ord ∈ [from, to) 且 live 且 meta 非空的文档，
    // 对每个调用 fn(ord, blob)。docmap ckpt（base 全量 / delta 窗口）用。
    // 语义/锁/升序保证与 for_each_live_in 一致。
    template <typename Fn>
    void for_each_meta_in(std::uint64_t from, std::uint64_t to, Fn&& fn) const {
        struct MetaRef {
            std::uint64_t ord;
            const std::vector<std::byte>* blob;
        };
        std::shared_lock lk(mutex_);
        std::vector<MetaRef> refs;
        for (const auto& [ord, blob] : meta_) {
            if (blob.empty() || ord < from || ord >= to) continue;
            if (!live_.contains(ord)) continue;  // 不变量防御
            refs.push_back(MetaRef{ord, &blob});
        }
        std::sort(refs.begin(), refs.end(),
                  [](const MetaRef& a, const MetaRef& b) {
                      return a.ord < b.ord;
                  });
        for (const auto& r : refs) {
            fn(r.ord, std::span<const std::byte>(*r.blob));
        }
    }

    // ---- S18-2：docmap sidecar（"BCIS"）序列化 ----
    // 把 ord → (ext_id, DocSlot) 活映射落进 checkpoint，避免冷启动全量 fold
    // 重建。covers_next_ord 记录快照覆盖的 ord 水位。S42：内存形态换
    // live-only map，磁盘格式逐字节不变（行按 ord 升序 gap+vbyte）。
    [[nodiscard]] bool serialize_docmap(std::vector<std::uint8_t>& out,
                                        std::uint64_t covers_next_ord) const;
    [[nodiscard]] std::optional<std::uint64_t>
    deserialize_docmap(std::span<const std::uint8_t> bytes);
    // 6.6.0：流式载入重载（分块 CRC + 分块解析，不把整段读进堆）。
    [[nodiscard]] std::optional<std::uint64_t>
    deserialize_docmap(io::FileHandle fd, std::uint64_t off,
                       std::uint64_t len);

private:
    mutable std::shared_mutex mutex_;

    // S40：node-based，key 地址稳定——LiveRow.ext 指向节点键，覆盖写只改
    // value 不动节点，删除时必须先淘汰 live row 再 extract 节点（防悬垂）。
    std::unordered_map<std::string, std::uint64_t,
                       StringHash, std::equal_to<>> ext2ord_;  // ext_id → 最新 ord

    // S42 live-only：ord → 行（仅当前存活）。ankerl 稠密 map：值连续存储，
    // erase 交换回收，稳态容量随 live_docs 而非历史 ord 增长。
    ankerl::unordered_dense::map<std::uint64_t, LiveRow> live_;
    // V5 meta：仅 live ord 有条目；空 blob 语义 = 删除条目（map 中不存在）。
    ankerl::unordered_dense::map<std::uint64_t, std::vector<std::byte>> meta_;
    // S42-2：当前 delta 窗口内仍 live 的 ord journal。put 插入 / 覆盖写与
    // 删除淘汰旧 ord / begin_delta_window 清空。delta checkpoint 消费。
    ankerl::unordered_dense::set<std::uint64_t> delta_live_;

    std::uint64_t next_ord_       = 0;
    std::uint64_t live_docs_      = 0;
    std::uint64_t retired_since_compact_ = 0;  // S12-2：自上次 compact 起退休的版本数
    std::uint64_t chunks_alloc_   = 0;      // S42 起冻结为 0（IndexInfo 兼容）
    std::uint64_t chunks_freed_   = 0;      // S42 起冻结为 0（IndexInfo 兼容）

    // S18-2：docmap 持久化记账（见 public 段注释）。removals_ 受 mutex_
    // 保护；dirty_ 为 relaxed 原子（写点已被 reducer/静止点串行化）。
    std::atomic<bool> dirty_{true};
    std::uint64_t delta_window_wm_ = 0;
    std::vector<std::pair<std::string, std::uint64_t>> removals_;
    // S42-3：载入期抑制 journal 增长（ReplayJournalGuard 置位）。
    bool journal_suppressed_ = false;
};

}  // namespace bitcask::index
