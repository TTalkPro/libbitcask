// segment_v2.hpp — 封口段 v2 盘格式(mmap-native)+ 只读 MmapSegment(S30-P1)。
// 设计:docs/design/s30-mmap-segments.md §1/§2。
//
// 与 v1(SearchCheckpoint 内嵌 InvertedIndex::serialize 顺序流)的本质区别:
// **可点查**——排序词典 + 定长目录记录直接在 mmap 上二分,posting 按 128 块
// 位打包(u32 段内 docid)可随机跳块;查询按需解码,段数据不驻留内存。
//
// === 文件版式 ===
//   [Header 64B] magic/version/seg_id/doc_count/total_doc_len/field_count/flags
//   [sections…]  每字段:kPostings(流式,posting 块 + positions 内联)→
//                kBlocks(BlockMeta 表)→ kTermDict(TermRec 表)→ kTermBlob
//                → kFieldStats → kFieldName;全局:kDocStore(定长行+key blob)
//   [Footer]     节表 {kind, field_idx, off, len, crc32} × n + footer crc
//   [Tail 16B]   footer_off/footer_len/kTailMagic——定位 footer 的锚
//
// 段文件**一次写永不改**;live 位不进段文件,走独立 sidecar(<seg>.live,
// tmp+rename 重写,见 save_live_sidecar)。
//
// === 并发契约 ===
//   writer:单线程(封口点调用)。
//   MmapSegment:打开后只读(mmap PROT_READ);查询线程并发安全(纯读 +
//   thread_local 解码 scratch);live 位图为原子数组(mark_dead vs 查询)。
//   生命周期由 shared_ptr pin(munmap 在析构),沿用段对象既有模式。

#pragma once

#include "bitcask/index.hpp"      // DocSlot
#include "bitcask/inverted.hpp"   // FlatPostings / SearchResult / ExtStats / LiveChecker

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bitcask::search {

namespace segv2 {

inline constexpr std::uint32_t kMagic       = 0x42534732;  // 'BSG2'
inline constexpr std::uint32_t kFooterMagic = 0x42534746;  // 'BSGF'
inline constexpr std::uint32_t kTailMagic   = 0x42534754;  // 'BSGT'
inline constexpr std::uint32_t kLiveMagic   = 0x42534C56;  // 'BSLV'
inline constexpr std::uint32_t kVersion     = 1;
inline constexpr std::size_t   kBlockSize   = 128;  // == PostingList::kBlockSize

enum class Section : std::uint32_t {
    kPostings   = 1,   // 流式:per term [posting 块…][positions(可选)]
    kBlocks     = 2,   // BlockMeta 表(per term 连续)
    kTermDict   = 3,   // TermRec 表(按 term 字节序升序,mmap 二分)
    kTermBlob   = 4,   // term 字节(len 由 TermRec 给出)
    kFieldStats = 5,
    kFieldName  = 6,
    kDocStore   = 7,   // 全局(field_idx = kGlobalField)
};
inline constexpr std::uint32_t kGlobalField = 0xFFFFFFFFu;

// 词典目录记录(定长 48B,POD,memcpy 读写;数组按 term 字节序升序)。
struct TermRec {
    std::uint64_t term_off;      // kTermBlob 节内偏移
    std::uint32_t term_len;
    std::uint32_t df;            // posting 条数(含死点)
    std::uint32_t max_tf;
    std::uint32_t block_count;   // ⌈df/128⌉
    std::uint64_t postings_off;  // kPostings 节内偏移(本 term 块数据起点)
    std::uint64_t blocks_off;    // kBlocks 节内偏移(本 term BlockMeta 起点)
    std::uint64_t pos_off;       // kPostings 节内偏移(positions;无=u64(-1))
};
static_assert(sizeof(TermRec) == 48);

// 每 128-posting 块元数据(定长 20B;末块可不满,count 由 df 推出)。
struct BlockMeta {
    std::uint32_t first_docid;
    std::uint32_t last_docid;
    std::uint32_t max_tf;
    std::uint32_t min_dl;        // 块内最小非零 dl;全未知=1(admissible 回退)
    std::uint32_t data_off;      // 相对本 term postings_off 的块数据偏移
};
static_assert(sizeof(BlockMeta) == 20);

struct FieldStats {
    std::uint64_t live_doc_count;  // 写出时的 N(BM25 本地统计回退用)
    std::uint64_t sum_doc_len;
    std::uint64_t term_count;
    std::uint32_t has_positions;
    std::uint32_t pad = 0;
};
static_assert(sizeof(FieldStats) == 32);

// doc_store 定长行(48B)。节版式:[u64 doc_count][u64 key_blob_len]
// [rows…][key blob]。
struct DocRow {
    std::uint64_t lsn;
    std::uint64_t key_off;       // key blob 内偏移
    std::uint64_t loc_offset;    // DocSlot.loc.offset
    std::uint32_t loc_file_id;
    std::uint32_t loc_total_sz;
    std::uint32_t tstamp;
    std::uint32_t doc_len;
    std::uint32_t key_len;
    std::uint32_t pad = 0;
};
static_assert(sizeof(DocRow) == 48);

}  // namespace segv2

// writer 的每文档输入(docid 稠密 0..doc_count-1)。
struct SegV2DocRow {
    std::string_view key;
    std::uint64_t   lsn = 0;
    index::DocSlot  slot{};
};

// S30-P3:字段级流式输入源(writer 泛化)——InvertedIndex 包装与段合并
// (k-way 归并现产 posting)共用同一 writer。
struct SegV2FieldSource {
    std::string_view name;
    // 单次遍历:按 term 字节序升序逐个回调(term, 完整 PostingList)。
    // 空 PostingList 会被 writer 跳过(不入词典)。
    std::function<void(
        const std::function<void(std::string_view, const bm25::PostingList&)>&)>
        visit;
    // 字段统计 {live_doc_count(N), sum_doc_len}——在 visit 完成**之后**调用
    // (合并场景统计在归并中现算;term_count/has_positions 由 writer 自记)。
    std::function<std::pair<std::uint64_t, std::uint64_t>()> stats;
};

// 流式写 v2 段文件(tmp+rename 原子)。
// - doc_row:按 docid 取行,**会被调用两遍**(行 + key blob 两趟流式,免
//   key 缓冲),须纯函数;
// - 内存占用:O(词典 + 块表)瞬态(posting/positions/doc_store 全流式),
//   无 O(postings) 缓冲;
// - 约束:所有 posting docid 必须 < 2^32(段内本地 docid;越界返回 false)。
[[nodiscard]] bool write_segment_v2_streams(
    const std::string& path,
    std::uint64_t seg_id,
    std::span<const SegV2FieldSource> fields,
    std::uint32_t doc_count,
    const std::function<SegV2DocRow(std::uint32_t docid)>& doc_row,
    std::uint64_t total_doc_len);

// 便捷包装:字段来自内存 InvertedIndex(封口路径)。
[[nodiscard]] bool write_segment_v2(
    const std::string& path,
    std::uint64_t seg_id,
    std::span<const std::pair<std::string_view, const bm25::InvertedIndex*>> fields,
    std::uint32_t doc_count,
    const std::function<SegV2DocRow(std::uint32_t docid)>& doc_row,
    std::uint64_t total_doc_len);

// 只读 mmap 段。IS-A LiveChecker(段内 docid;live 位图 RAM 常驻,
// doc_len 走 doc_store 行)。
class MmapSegment : public bm25::LiveChecker {
public:
    // 打开 + (默认)整文件节级 CRC 校验。失败(IO/魔数/版本/CRC/越界节)
    // 返回 nullptr。verify_crc=false 为 S21-A6 可信读 opt-in。
    [[nodiscard]] static std::unique_ptr<MmapSegment> open(
        const std::string& path, bm25::Bm25Params params = {},
        bool verify_crc = true);
    ~MmapSegment() override;
    MmapSegment(const MmapSegment&) = delete;
    MmapSegment& operator=(const MmapSegment&) = delete;

    // ---- 元信息 ----
    [[nodiscard]] std::uint64_t seg_id() const noexcept { return seg_id_; }
    [[nodiscard]] std::uint32_t doc_count() const noexcept { return doc_count_; }
    [[nodiscard]] std::uint64_t total_doc_len() const noexcept { return total_doc_len_; }
    [[nodiscard]] std::vector<std::string_view> field_names() const;

    // ---- 查询面(镜像 InvertedIndex 语义;field = kDefaultField/命名字段) ----
    // 语义同 InvertedIndex::doc_freq(posting 列长,含死点)。
    [[nodiscard]] std::uint64_t doc_freq(std::string_view field,
                                         std::string_view term) const;
    // 语义/评分与 InvertedIndex::search **逐位一致**(共用
    // detail::score_bow_topk / search_wand_impl;见 bm25_search_impl.hpp 契约)。
    [[nodiscard]] std::vector<bm25::SearchResult> search(
        std::string_view field,
        const std::vector<std::string>& query_terms,
        std::size_t k,
        const bm25::LiveChecker& live_checker,
        const bm25::Bm25Params* params_override = nullptr,
        const bm25::ExtStats* ext = nullptr) const;
    // ---- S30-P3:词典枚举(段合并的 k-way 归并输入) ----
    [[nodiscard]] std::uint64_t term_count(std::string_view field) const;
    // 第 i 个词(词典升序;返回视图借 mmap 区,段 pin 期间稳定)。
    [[nodiscard]] std::string_view term_at(std::string_view field,
                                           std::uint64_t idx) const;

    // 解码单 term 的扁平快照(测试/上层组合用)。term 不存在返回 false。
    [[nodiscard]] bool decode_postings(std::string_view field,
                                       std::string_view term,
                                       bm25::FlatPostings& out) const;
    // 完整解码(含 tf/dl/positions 列)——phrase/near 与将来 merge 用。
    [[nodiscard]] bool decode_postings_list(std::string_view field,
                                            std::string_view term,
                                            bm25::PostingList& out) const;

    // ---- 其余查询面(语义与 InvertedIndex 同名方法逐位一致,共享
    //      bm25_search_impl 核;wildcard/fuzzy 因采集顺序不同,浮点累加序
    //      可有末位差,见实现注) ----
    [[nodiscard]] std::vector<bm25::SearchResult> search_phrase(
        std::string_view field, const std::vector<std::string>& query_terms,
        std::size_t k, const bm25::LiveChecker& live_checker,
        const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::vector<bm25::SearchResult> search_near(
        std::string_view field, const std::vector<std::string>& query_terms,
        std::size_t k, std::uint32_t slop,
        const bm25::LiveChecker& live_checker,
        const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] bm25::ScoreExplanation explain(
        std::string_view field, const std::vector<std::string>& query_terms,
        std::uint64_t docid, const bm25::LiveChecker& live_checker,
        const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::vector<bm25::SearchResult> search_wildcard(
        std::string_view field, const std::string& pattern, std::size_t k,
        const bm25::LiveChecker& live_checker,
        const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::vector<bm25::SearchResult> search_fuzzy(
        std::string_view field, const std::vector<std::string>& query_terms,
        std::size_t k, std::uint32_t max_edit_distance,
        const bm25::LiveChecker& live_checker,
        const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::vector<bm25::SearchResult> bool_search(
        std::string_view field, const bm25::QueryNode& query, std::size_t k,
        const bm25::LiveChecker& live_checker,
        const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::vector<bm25::SearchResult> bool_search_tree(
        std::string_view field, const bm25::QueryNode& root, std::size_t k,
        const bm25::LiveChecker& live_checker,
        const bm25::Bm25Params* params_override = nullptr) const;

    // ---- 字段统计(写出时快照) ----
    [[nodiscard]] std::uint64_t live_doc_count(std::string_view field) const;
    [[nodiscard]] std::uint64_t sum_doc_len(std::string_view field) const;

    // ---- doc_store ----
    [[nodiscard]] std::string_view key_of(std::uint32_t docid) const;
    [[nodiscard]] std::uint64_t lsn_of(std::uint32_t docid) const;
    [[nodiscard]] index::DocSlot slot_of(std::uint32_t docid) const;

    // ---- LiveChecker / live 位图 ----
    [[nodiscard]] bool is_live(std::uint64_t docid) const override;
    [[nodiscard]] std::uint32_t doc_len(std::uint64_t docid) const override;
    void mark_dead(std::uint32_t docid) noexcept;
    [[nodiscard]] std::uint64_t live_count() const noexcept;
    // sidecar:<path> 处读/写 live 位图(tmp+rename;doc_count 不符拒载)。
    [[nodiscard]] bool load_live_sidecar(const std::string& path);
    [[nodiscard]] bool save_live_sidecar(const std::string& path) const;

private:
    MmapSegment() = default;

    struct Field {
        std::string name;
        segv2::FieldStats stats{};
        const std::byte* dict = nullptr;      // TermRec 数组
        std::uint64_t    dict_count = 0;
        const std::byte* blob = nullptr;      // term 字节
        std::uint64_t    blob_len = 0;
        const std::byte* postings = nullptr;
        std::uint64_t    postings_len = 0;
        const std::byte* blocks = nullptr;
        std::uint64_t    blocks_len = 0;
    };

    [[nodiscard]] const Field* field_of(std::string_view name) const;
    // 词典二分:命中填 rec 返回 true。
    [[nodiscard]] bool find_term(const Field& f, std::string_view term,
                                 segv2::TermRec& rec) const;
    [[nodiscard]] bool decode_rec(const Field& f, const segv2::TermRec& rec,
                                  bm25::FlatPostings& out) const;
    [[nodiscard]] bool decode_rec_list(const Field& f,
                                       const segv2::TermRec& rec,
                                       bm25::PostingList& out) const;
    // 词典有序区间端点(term 字节序;二分)。
    [[nodiscard]] std::size_t dict_lower_bound(const Field& f,
                                               std::string_view key) const;
    [[nodiscard]] std::size_t dict_upper_bound(const Field& f,
                                               std::string_view key) const;
    [[nodiscard]] std::vector<bm25::SearchResult> phrase_common(
        std::string_view field, const std::vector<std::string>& query_terms,
        std::size_t k, std::uint32_t slop,
        const bm25::LiveChecker& live_checker,
        const bm25::Bm25Params* params_override) const;

    // mmap 区
    const std::byte* base_ = nullptr;
    std::size_t      len_ = 0;
    // header
    std::uint64_t seg_id_ = 0;
    std::uint32_t doc_count_ = 0;
    std::uint64_t total_doc_len_ = 0;
    // fields(open 时构建,查询期只读)
    std::vector<Field> fields_;
    // doc_store
    const std::byte* rows_ = nullptr;       // DocRow 数组
    const std::byte* key_blob_ = nullptr;
    std::uint64_t    key_blob_len_ = 0;
    // live 位图(byte/doc;原子:mark_dead vs 并发查询)
    std::unique_ptr<std::atomic<std::uint8_t>[]> live_;
    std::atomic<std::uint64_t> dead_count_{0};
    bm25::Bm25Params params_{};
};

// per-field 查询适配器:把 MmapSegment 的 (field, ...) 查询面折成 TermIndex
// ——SegmentView/FieldSegmentView 经同一接口指内存段与 mmap 段(Slice 4)。
// 生命周期:持 MmapSegment 裸指针,caller 保证段活过本对象(段对象内嵌或
// view pin 连带)。
class MmapFieldIndex final : public bm25::TermIndex {
public:
    MmapFieldIndex(const MmapSegment* seg, std::string field)
        : seg_(seg), field_(std::move(field)) {}

    [[nodiscard]] std::uint64_t live_doc_count() const override {
        return seg_->live_doc_count(field_);
    }
    [[nodiscard]] std::uint64_t sum_doc_len() const override {
        return seg_->sum_doc_len(field_);
    }
    [[nodiscard]] std::uint64_t doc_freq(std::string_view term) const override {
        return seg_->doc_freq(field_, term);
    }
    [[nodiscard]] auto search(const std::vector<std::string>& query_terms,
                              std::size_t k,
                              const bm25::LiveChecker& live_checker,
                              const bm25::Bm25Params* params_override = nullptr,
                              const bm25::ExtStats* ext = nullptr) const
        -> std::vector<bm25::SearchResult> override {
        return seg_->search(field_, query_terms, k, live_checker,
                            params_override, ext);
    }
    [[nodiscard]] auto search_phrase(
        const std::vector<std::string>& query_terms, std::size_t k,
        const bm25::LiveChecker& live_checker,
        const bm25::Bm25Params* params_override = nullptr) const
        -> std::vector<bm25::SearchResult> override {
        return seg_->search_phrase(field_, query_terms, k, live_checker,
                                   params_override);
    }
    [[nodiscard]] auto search_near(
        const std::vector<std::string>& query_terms, std::size_t k,
        std::uint32_t slop, const bm25::LiveChecker& live_checker,
        const bm25::Bm25Params* params_override = nullptr) const
        -> std::vector<bm25::SearchResult> override {
        return seg_->search_near(field_, query_terms, k, slop, live_checker,
                                 params_override);
    }
    [[nodiscard]] auto bool_search(
        const bm25::QueryNode& query, std::size_t k,
        const bm25::LiveChecker& live_checker,
        const bm25::Bm25Params* params_override = nullptr) const
        -> std::vector<bm25::SearchResult> override {
        return seg_->bool_search(field_, query, k, live_checker,
                                 params_override);
    }
    [[nodiscard]] auto bool_search_tree(
        const bm25::QueryNode& root, std::size_t k,
        const bm25::LiveChecker& live_checker,
        const bm25::Bm25Params* params_override = nullptr) const
        -> std::vector<bm25::SearchResult> override {
        return seg_->bool_search_tree(field_, root, k, live_checker,
                                      params_override);
    }
    [[nodiscard]] auto search_fuzzy(
        const std::vector<std::string>& query_terms, std::size_t k,
        std::uint32_t max_edit_distance,
        const bm25::LiveChecker& live_checker,
        const bm25::Bm25Params* params_override = nullptr) const
        -> std::vector<bm25::SearchResult> override {
        return seg_->search_fuzzy(field_, query_terms, k, max_edit_distance,
                                  live_checker, params_override);
    }
    [[nodiscard]] auto search_wildcard(
        const std::string& pattern, std::size_t k,
        const bm25::LiveChecker& live_checker,
        const bm25::Bm25Params* params_override = nullptr) const
        -> std::vector<bm25::SearchResult> override {
        return seg_->search_wildcard(field_, pattern, k, live_checker,
                                     params_override);
    }
    [[nodiscard]] auto explain(const std::vector<std::string>& query_terms,
                               std::uint64_t ord,
                               const bm25::LiveChecker& live_checker,
                               const bm25::Bm25Params* params_override =
                                   nullptr) const
        -> bm25::ScoreExplanation override {
        return seg_->explain(field_, query_terms, ord, live_checker,
                             params_override);
    }

    [[nodiscard]] const MmapSegment* segment() const noexcept { return seg_; }
    [[nodiscard]] std::string_view field() const noexcept { return field_; }

private:
    const MmapSegment* seg_;
    std::string field_;
};

}  // namespace bitcask::search
