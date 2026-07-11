// 封口段（SealedSegment，S27-2 Slice 3 + S27-3 Slice A 多字段扩展）。
// 见 doc/segment-index-design-zh.md §2.2/§3.4/§8。
//
// 一个不可变、自包含的段：段内倒排（本地 docid）+ 平坦 doc_store（docid→{key, lsn,
// DocSlot, live}）+ 段统计。落盘复用 SearchCheckpoint 段级 CRC 容器（§8）：
//   section kBm25Default = 默认字段 InvertedIndex::serialize 字节
//   section kSegFields   = 命名字段倒排：多字段 round-trip 扩展（S27-3 Slice A）
//   section kSegDocStore = 本头 encode 的平坦 doc_store（段级 total doc_len）
//
// 段级 doc_lens_[docid] 是**整文档总词数**（Σ 所有字段 dl），与 TextPlugin::docs_
// 语义一致——LiveChecker::doc_len(docid) 返回段级 total，BM25 dl 归一化即按此值。
//
// 此 slice 扩展默认字段之外的**命名字段**：每字段一个独立 InvertedIndex。
// 字段 map 由 `fields_mu_`（shared_mutex）保护——只护 map 结构（写线程首次
// emplace vs 查询 find），InvertedIndex 本体地址稳定且自带分片并发 →
// 引用可出锁用（对齐 TextPlugin::fields_ 的 C1 约定）。
//
// SealedSegment IS-A LiveChecker（段内 docid）。

#pragma once

#include "bitcask/index_ids.hpp"
#include "bitcask/index.hpp"           // DocSlot / DocLoc
#include "bitcask/inverted.hpp"        // InvertedIndex / TermPositions / LiveChecker
#include "bitcask/search_checkpoint.hpp"
#include "bitcask/search_types.hpp"    // kDefaultField
#include "bitcask/row_chunks.hpp"      // RowChunks（并发 doc_store 底座）
#include "bitcask/segment_query.hpp"   // SegmentView
#include "bitcask/string_hash.hpp"     // StringHash（透明 hash）

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bitcask::search {

class SealedSegment;  // 前向声明（multi_field_segment_search 视图需要）

// ---- 多字段视图（与 multi_field_segment_search 配套）----

// 单字段视图（multi-field 形态）：指向一个 InvertedIndex（段内某字段）+ 字段名。
// LiveChecker / key / lsn 由所属段（MultiFieldSegmentView::seg）统一提供。
struct FieldSegmentView {
    std::string_view       field_name;   // 含 kDefaultField
    // S30-P1 Slice 4:查询接口指针(同 SegmentView::inv)。
    const bm25::TermIndex* inv;          // 段内该字段倒排查询面（本地 docid）
};

// 多字段段视图：把 SealedSegment 的多字段状态暴露给 multi_field_segment_search。
// 段提供 LiveChecker + key_of + lsn_of；fields 列出本段所有参与查询的字段倒排
// （默认字段若有写入也应列入，对应 `field_name == kDefaultField`）。
struct MultiFieldSegmentView {
    const SealedSegment*         seg;        // 段（LiveChecker + docid→key/lsn）
    std::vector<FieldSegmentView> fields;    // 该段所有字段倒排
    std::shared_ptr<const void>  pin;        // S27-3 步骤 5:段生命周期钉住
};

class SealedSegment : public bm25::LiveChecker {
public:
    SealedSegment() = default;
    SealedSegment(const SealedSegment&) = delete;
    SealedSegment& operator=(const SealedSegment&) = delete;

    // ---- 单字段 add（向后兼容，默认字段）----

    // 构建（内存段）：追加一篇文档到默认字段，返回段内本地 docid（自增）。
    // doc_len 取 Σtf，与 inv_ 内部统计一致（BM25 打分从 LiveChecker 读 doc_len）。
    DocId add(std::string key, Lsn lsn, const bm25::TermPositions& terms,
              index::DocLoc loc = {}, std::uint32_t tstamp = 0) {
        const DocId docid = static_cast<DocId>(keys_.size());
        std::uint32_t dl = 0;
        for (const auto& [t, d] : terms) dl += d.first;
        // 写序(并发契约,见 live_/count_pub_ 注释):行 → 发布 → 倒排。
        keys_.push_back(std::move(key));
        lsns_.push_back(lsn);
        slots_.push_back(index::DocSlot{.loc = loc, .tstamp = tstamp, .doc_len = dl});
        doc_lens_.push_back(dl);
        live_.push_back(std::uint8_t{1});
        count_pub_.store(docid + 1, std::memory_order_release);
        inv_.add_doc(docid, terms);
        return docid;
    }

    // ---- 多字段 add（S27-3 Slice A，Slice B 由 TextPlugin 把 ReduceJob 转此）----

    // 一篇文档的多字段 add 输入。`field_name` 已规范化（空 → kDefaultField，由
    // caller 负责；本方法不再二次映射，避免与 TextPlugin::map_analyze 的归一化点
    // 出现分叉）。`terms` 借用：调用期间存活即可（add_doc 内部消化后不再读）。
    // **不**为字段名空 / terms 为空做防御——这是契约的一部分（caller 已规范化，
    // 空 terms 意味着该字段无 token，写了也零 posting，省一遍 add_doc 路径）。
    struct FieldInput {
        std::string_view                  field_name;
        const bm25::TermPositions*        terms;
    };

    // 多字段构建：所有字段（默认 + 命名）的 posting 落到各自 InvertedIndex；
    // doc_lens_[docid] 存**段级 total**（Σ 所有字段 dl）— 与 TextPlugin::docs_
    // 语义一致。first-use 字段 emplace 创建 InvertedIndex（与 TextPlugin 的
    // field_index 双检创建同款）。
    DocId add(std::string key, Lsn lsn,
              std::span<const FieldInput> fields,
              std::uint32_t total_doc_len,
              index::DocLoc loc = {}, std::uint32_t tstamp = 0) {
        const DocId docid = static_cast<DocId>(keys_.size());
        // 写序(并发契约):行 → 发布 → 各字段倒排。
        keys_.push_back(std::move(key));
        lsns_.push_back(lsn);
        slots_.push_back(index::DocSlot{.loc = loc, .tstamp = tstamp,
                                        .doc_len = total_doc_len});
        doc_lens_.push_back(total_doc_len);  // 段级 total（不是 per-field）
        live_.push_back(std::uint8_t{1});
        count_pub_.store(docid + 1, std::memory_order_release);
        // 各字段写倒排（默认 → inv_，其余 → fields_ map）。
        for (const auto& f : fields) {
            if (f.field_name == kDefaultField) {
                inv_.add_doc(docid, *f.terms);
            } else {
                // 双检写：常态（字段已存在）只读锁；首次出现升级独占 emplace。
                // fields_ 的 unique_ptr<InvertedIndex> 地址稳定 → 出锁后用安全。
                bm25::InvertedIndex* inv = nullptr;
                {
                    std::shared_lock lk(fields_mu_);
                    auto it = fields_.find(f.field_name);
                    if (it != fields_.end()) inv = it->second.get();
                }
                if (!inv) {
                    std::unique_lock lk(fields_mu_);
                    auto it = fields_.find(f.field_name);
                    if (it == fields_.end()) {
                        // 默认 BM25 参数 + 存 positions（与 TextPlugin 默认同款；
                        // 真正可调的 Bm25Params 来源待 Slice B 与配置层对位时引入，
                        // 这里与 S27-2 SealedSegment::inv_ 走一致的隐式默认）。
                        it = fields_.emplace(std::string(f.field_name),
                                             std::make_unique<bm25::InvertedIndex>())
                                 .first;
                    }
                    inv = it->second.get();
                }
                inv->add_doc(docid, *f.terms);
            }
        }
        return docid;
    }

    // ---- LiveChecker（按段内 docid） ----
    [[nodiscard]] bool is_live(std::uint64_t docid) const override {
        return docid < count_pub_.load(std::memory_order_acquire) &&
               live_[docid].load(std::memory_order_relaxed) != 0;
    }
    [[nodiscard]] std::uint32_t doc_len(std::uint64_t docid) const override {
        return docid < count_pub_.load(std::memory_order_acquire)
                   ? doc_lens_[docid]
                   : 0;
    }

    // ---- 段级删除（设计文档 §3.4：封口段仅 live_docs 可变）----
    // 翻位段级 live_[docid]=0（与 Lucene 段模型一致：删除只翻位，posting 不动；
    // 物理回收靠 merge）。docid 越界返回 false。
    //
    // **不变性 vs 不可变性**：段一旦 flush 落盘，`keys_/lsns_/slots_/doc_lens_/
    // inv_/fields_` 即冻结（设计 §3.4）；**唯 `live_docs` 在封口后可变**，因
    // 删除是「使 docid 不再被访问」而不修改其内容。mark_dead 是这一可变态的
    // 唯一公开接口。
    //
    // **设计文档 §4 接受 df 含已删文档的近似**（idf 高估，merge 自愈）。本方法
    // 不调 InvertedIndex 统计——段级 df/N/sum_dl 在 merge 时物理删死 doc 才归正。
    //
    // const 语义边界：mark_dead 必须能改 live_，故 live_ 加 mutable（与
    // TextPlugin::cache_ mutable 同款模式）。接口本身**非常量**——caller 必须
    // 拿到非 const SealedSegment*（SegmentSet::segment() 显式返回非 const
    // 访问，或经 const_cast 显式 cast，二者都表明「删除是 mutation」）。
    [[nodiscard]] bool mark_dead(DocId docid) {
        if (docid >= count_pub_.load(std::memory_order_acquire)) return false;
        live_[docid].store(0, std::memory_order_relaxed);
        dead_dirty_.store(true, std::memory_order_relaxed);  // S27-3 B2b 步骤 4:待重存
        return true;
    }

    // S27-3 步骤 5:原地 posting 压实——postings 删死 docid(doc_store 行
    // 不动,docid 稠密不变量保持;df/N/sum_dl 随 compact 归正)。字节变了 →
    // 置 dead_dirty_ 令下次 checkpoint 重存。调用契约:reducer 静止点
    // (compact 遍历 tbb map 与并发 add_doc 不兼容,同旧 fields_ 约束;
    // 并发**查询**安全——CoW posting)。返回压实掉的 posting 数。
    std::size_t compact_postings(double dead_ratio_threshold) {
        std::size_t n = inv_.compact(*this, dead_ratio_threshold);
        {
            std::shared_lock lk(fields_mu_);
            for (auto& [name, inv] : fields_) {
                n += inv->compact(*this, dead_ratio_threshold);
            }
        }
        if (n > 0) dead_dirty_.store(true, std::memory_order_relaxed);
        return n;
    }

    // S27-3 B2b 步骤 4:自上次 save 后是否有新的 mark_dead。live_ 位随
    // kSegDocStore 持久化,但封口段不会自动重存——不在 checkpoint 时重存
    // 脏段,ckpt **之前**的删除会在 recovery 后复活为幽灵(fold 只补 ckpt
    // 之后的窗口)。SegmentSet::resave_dead_dirty 消费本标记。
    [[nodiscard]] bool dead_dirty() const {
        return dead_dirty_.load(std::memory_order_relaxed);
    }
    void clear_dead_dirty() { dead_dirty_.store(false, std::memory_order_relaxed); }
    // 活文档计数（live_==1 的数量）——测试 / 内省用。
    [[nodiscard]] std::size_t live_doc_count() const {
        const auto cnt = count_pub_.load(std::memory_order_acquire);
        std::size_t n = 0;
        for (std::uint64_t i = 0; i < cnt; ++i) {
            if (live_[i].load(std::memory_order_relaxed) != 0) ++n;
        }
        return n;
    }

    [[nodiscard]] std::size_t doc_count() const {
        return count_pub_.load(std::memory_order_acquire);
    }
    [[nodiscard]] const bm25::InvertedIndex& inverted() const { return inv_; }
    [[nodiscard]] const std::string& key_at(DocId d) const { return keys_[d]; }
    [[nodiscard]] Lsn lsn_at(DocId d) const { return lsns_[d]; }

    // 单字段默认视图（multi_segment_search 用；向后兼容）。
    [[nodiscard]] SegmentView view() const {
        return SegmentView{
            &inv_, this,
            [this](DocId d) { return keys_[d]; },
            [this](DocId d) { return lsns_[d]; }};
    }

    // 多字段视图（multi_field_segment_search 用）：把本段所有字段（含默认）汇总。
    // 默认字段若本段有写入过（inv_ 非空）则列入，否则省略——`inv_` 永久驻留故以
    // 段是否接受过任何 add 判定太重，简化为：默认字段**总**列出（multi_field_…search
    // 调用方会按 field_terms 过滤，未被查询命中的字段不被 search，零开销）。
    // fields_mu_ 只读锁出 view 期间不释放 → 字段倒排地址稳定（InvertedIndex 本体
    // 不会被移动，唯一指针在 map 内不动），view 持有方持本段活过查询即可。
    [[nodiscard]] MultiFieldSegmentView multi_view() const {
        std::vector<FieldSegmentView> fvs;
        std::shared_lock lk(fields_mu_);  // S27-4 P2:size() 读也须在锁内
        fvs.reserve(1 + fields_.size());
        fvs.push_back(FieldSegmentView{kDefaultField, &inv_});
        for (const auto& [name, inv] : fields_) {
            fvs.push_back(FieldSegmentView{name, inv.get()});
        }
        return MultiFieldSegmentView{this, std::move(fvs)};
    }

    // 命名字段访问（测试 / 内省用）。返回的指针活到下次 map 结构变更（emplace/
    // 删字段）；持有方应持本段所有权。
    [[nodiscard]] const bm25::InvertedIndex*
    field_index(std::string_view field) const {
        std::shared_lock lk(fields_mu_);
        auto it = fields_.find(field);
        return it == fields_.end() ? nullptr : it->second.get();
    }

    // ---- 落盘 / 载入（复用 SearchCheckpoint 段级 CRC 容器） ----
    [[nodiscard]] bool save(const std::string& path,
                            std::uint64_t watermark) const {
        SectionWriter sw;
        {
            std::vector<std::byte> b;
            inv_.serialize(b);
            sw.add(CkptSectionType::kBm25Default, std::move(b));
        }
        // 命名字段（多字段）：仅当 fields_ 非空才写 kSegFields。
        std::vector<std::byte> fields_bytes;
        if (!fields_.empty()) {
            fields_bytes = encode_fields();
            sw.add(CkptSectionType::kSegFields, std::move(fields_bytes));
        }
        sw.add(CkptSectionType::kSegDocStore, encode_doc_store());
        return SearchCheckpoint::write(path, watermark, sw.sections());
    }

    // CRC 或结构校验失败 → nullptr（整段拒收，退全量重建）。
    [[nodiscard]] static std::unique_ptr<SealedSegment> load(
        const std::string& path) {
        auto lc = SearchCheckpoint::read(path);
        if (!lc) return nullptr;
        auto seg = std::make_unique<SealedSegment>();
        bool have_inv = false, have_ds = false, have_fields = false;
        for (const auto& s : lc->sections) {
            if (!s.crc_ok) return nullptr;
            const auto st = static_cast<CkptSectionType>(s.type);
            std::span<const std::byte> pl(s.payload.data(), s.payload.size());
            if (st == CkptSectionType::kBm25Default) {
                if (!seg->inv_.deserialize(pl)) return nullptr;
                have_inv = true;
            } else if (st == CkptSectionType::kSegDocStore) {
                if (!seg->decode_doc_store(pl)) return nullptr;
                have_ds = true;
            } else if (st == CkptSectionType::kSegFields) {
                if (!seg->decode_fields(pl)) return nullptr;
                have_fields = true;
            }
            // 未知段型：保守拒收（避免静默丢字段导致查询语义变化）。
            // 本头只读段文件（无下游旧兼容负担），强校验优于宽松忽略。
        }
        if (!have_inv || !have_ds) return nullptr;
        (void)have_fields;  // 可选；不强制要求
        return seg;
    }

private:
    static constexpr std::uint32_t kDocStoreMagic = 0x54534453;  // 'SDST'
    static constexpr std::uint32_t kDocStoreVersion = 1;
    // 多字段段型 magic = 'MFLS'（Multi-FieLd Segment），与 SearchCheckpoint 段
    // type 字段独立（后者只是枚举序号）；此处 magic 仅用于本段内容自描述与
    // 截断/混淆检测。
    static constexpr std::uint32_t kSegFieldsMagic = 0x534C464D;
    static constexpr std::uint32_t kSegFieldsVersion = 1;

    // ---- 多字段段编码 ----
    // 格式（与 spec §Step 2 kSegFields 一致；InvertedIndex::serialize 是流式
    // 字节，无自身长度前缀 → 本段在每个字段倒排前显式写 u32 inv_len 以便切片）：
    //   [u32 magic = 0x534C464D]
    //   [u32 version = 1]
    //   [u32 field_count]
    //   per field:
    //     [u32 name_len]
    //     [name bytes]
    //     [u32 inv_len]                       // InvertedIndex::serialize 字节数
    //     [InvertedIndex::serialize bytes]    // 自带 INV 框架 magic + version
    [[nodiscard]] std::vector<std::byte> encode_fields() const {
        using namespace detail;
        std::vector<std::byte> b;
        put_u32(b, kSegFieldsMagic);
        put_u32(b, kSegFieldsVersion);
        put_u32(b, static_cast<std::uint32_t>(fields_.size()));
        std::shared_lock lk(fields_mu_);
        for (const auto& [name, inv] : fields_) {
            put_u32(b, static_cast<std::uint32_t>(name.size()));
            b.insert(b.end(),
                     reinterpret_cast<const std::byte*>(name.data()),
                     reinterpret_cast<const std::byte*>(name.data()) + name.size());
            // 先序列化到本地缓冲取长度，再写入（避免对 b 的边读边写歧义）。
            std::vector<std::byte> inv_bytes;
            inv->serialize(inv_bytes);
            put_u32(b, static_cast<std::uint32_t>(inv_bytes.size()));
            b.insert(b.end(), inv_bytes.begin(), inv_bytes.end());
        }
        return b;
    }

    [[nodiscard]] bool decode_fields(std::span<const std::byte> in) {
        const std::byte* p = in.data();
        const std::byte* end = p + in.size();
        auto rd_u32 = [&](std::uint32_t& v) {
            if (end - p < 4) return false;
            std::memcpy(&v, p, 4);
            p += 4;
            return true;
        };
        std::uint32_t magic = 0, ver = 0, fc = 0;
        if (!rd_u32(magic) || magic != kSegFieldsMagic) return false;
        if (!rd_u32(ver) || ver != kSegFieldsVersion) return false;
        if (!rd_u32(fc)) return false;
        std::unique_lock lk(fields_mu_);
        fields_.clear();
        fields_.reserve(fc);
        for (std::uint32_t i = 0; i < fc; ++i) {
            std::uint32_t nlen = 0, ilen = 0;
            if (!rd_u32(nlen)) return false;
            if (static_cast<std::size_t>(end - p) < nlen) return false;
            std::string name(reinterpret_cast<const char*>(p), nlen);
            p += nlen;
            if (!rd_u32(ilen)) return false;
            if (static_cast<std::size_t>(end - p) < ilen) return false;
            auto inv = std::make_unique<bm25::InvertedIndex>();
            std::span<const std::byte> pl(p, ilen);
            if (!inv->deserialize(pl)) return false;
            p += ilen;
            fields_.emplace(std::move(name), std::move(inv));
        }
        return true;
    }

    // ---- doc_store 段编码（段级 total doc_len，与 TextPlugin 语义一致） ----
    [[nodiscard]] std::vector<std::byte> encode_doc_store() const {
        using namespace detail;
        std::vector<std::byte> b;
        put_u32(b, kDocStoreMagic);
        put_u32(b, kDocStoreVersion);
        const auto n_docs = count_pub_.load(std::memory_order_acquire);
        put_u32(b, static_cast<std::uint32_t>(n_docs));
        for (std::size_t i = 0; i < n_docs; ++i) {
            put_u32(b, static_cast<std::uint32_t>(keys_[i].size()));
            b.insert(b.end(),
                     reinterpret_cast<const std::byte*>(keys_[i].data()),
                     reinterpret_cast<const std::byte*>(keys_[i].data()) +
                         keys_[i].size());
            put_u64(b, lsns_[i]);
            put_u64(b, slots_[i].loc.offset);
            put_u32(b, slots_[i].loc.file_id);
            put_u32(b, slots_[i].loc.total_sz);
            put_u32(b, slots_[i].tstamp);
            put_u32(b, slots_[i].doc_len);
            b.push_back(static_cast<std::byte>(
                live_[i].load(std::memory_order_relaxed)));
        }
        return b;
    }

    [[nodiscard]] bool decode_doc_store(std::span<const std::byte> in) {
        const std::byte* p = in.data();
        const std::byte* end = p + in.size();
        auto rd_u32 = [&](std::uint32_t& v) {
            if (end - p < 4) return false;
            std::memcpy(&v, p, 4);
            p += 4;
            return true;
        };
        auto rd_u64 = [&](std::uint64_t& v) {
            if (end - p < 8) return false;
            std::memcpy(&v, p, 8);
            p += 8;
            return true;
        };
        std::uint32_t magic = 0, ver = 0, n = 0;
        if (!rd_u32(magic) || magic != kDocStoreMagic) return false;
        if (!rd_u32(ver) || ver != kDocStoreVersion) return false;
        if (!rd_u32(n)) return false;
        keys_.clear(); lsns_.clear(); slots_.clear();
        doc_lens_.clear(); live_.clear();
        for (std::uint32_t i = 0; i < n; ++i) {
            std::uint32_t klen = 0;
            if (!rd_u32(klen)) return false;
            if (static_cast<std::size_t>(end - p) < klen) return false;
            keys_.push_back(std::string(reinterpret_cast<const char*>(p), klen));
            p += klen;
            std::uint64_t lsn = 0, off = 0;
            std::uint32_t fid = 0, tsz = 0, ts = 0, dl = 0;
            if (!rd_u64(lsn) || !rd_u64(off) || !rd_u32(fid) || !rd_u32(tsz) ||
                !rd_u32(ts) || !rd_u32(dl)) {
                return false;
            }
            if (end - p < 1) return false;
            const std::uint8_t lv = static_cast<std::uint8_t>(*p);
            ++p;
            lsns_.push_back(lsn);
            slots_.push_back(index::DocSlot{
                .loc = index::DocLoc{.offset = off, .file_id = fid, .total_sz = tsz},
                .tstamp = ts, .doc_len = dl});
            doc_lens_.push_back(dl);
            live_.push_back(lv);
        }
        count_pub_.store(n, std::memory_order_release);  // load 单线程,尾部发布
        return true;
    }

    // ---- 段内存储 ----
    bm25::InvertedIndex        inv_;       // 默认字段（kDefaultField）
    // mutable live_：设计文档 §3.4「封口段仅 live_docs 可变」。mark_dead 翻位
    // 删除是封口后唯一允许的 mutation；接口本身已显式标注非常量（caller 须
    // 取到非 const SealedSegment*，见 mark_dead 注释）。
    // S27-3 步骤 3 + S27-4 P2:Building 段查询并发化——doc_store 底座
    // RowChunks(chunk 永不搬移 + spine 原子发布,deque 的节点指针表扩容
    // 对并发读者不安全,见 row_chunks.hpp 头注),已发布行数经 count_pub_
    // (release 发布/acquire 读)界定读者可达范围;live_ 元素原子(mark_dead
    // 翻位 vs 读者 is_live,TSan 干净)。写序:先追加行,再发布计数,最后
    // add_doc 进倒排——读者经 posting 拿到的 docid 必有已发布的行。
    RowChunks<std::string>     keys_;
    RowChunks<Lsn>             lsns_;
    RowChunks<index::DocSlot>  slots_;
    RowChunks<std::uint32_t>   doc_lens_;   // 段级 total doc_len（Σ 各字段 dl）
    mutable RowChunks<std::atomic<std::uint8_t>> live_;
    std::atomic<std::uint64_t> count_pub_{0};
    // S27-4 P2:原子——builder(覆盖 mark_dead)与 reducer(on_delete)并发翻位。
    std::atomic<bool> dead_dirty_{false};  // S27-3 B2b 步骤 4:save 后有新 mark_dead

    // 命名字段（除默认）：字段名 → InvertedIndex。
    // fields_mu_ 只护 map 结构（与 TextPlugin::fields_ 同款约定：本体地址稳定，
    // 写线程首次 emplace vs 查询 find 互斥；查询持 shared_lock 出 view 后用裸指针）。
    mutable std::shared_mutex fields_mu_;
    std::unordered_map<std::string, std::unique_ptr<bm25::InvertedIndex>,
                       StringHash, std::equal_to<>> fields_;
};

// ===========================================================================
// 多段多字段查询归并（S27-3 Slice A）。见
// doc/segment-index-design-zh.md §3.5（多段查询）+ §4（BM25 跨段统计
// G-on-the-fly，对标 TextPlugin::search_fields 语义）。
//
// 本函数与 multi_segment_search 同构（串行逐段 + G-on-the-fly 共享 idf +
// 全局 top-k），但额外按字段独立累加（doc 可同时命中多字段，分数按字段相加）。
// **每字段用该字段的 ExtStats**（独立 N/sum_dl/df）——字段 = 不同语料（标题 vs
// 正文 vs 标签），独立 idf 与 TextPlugin::search_fields 严格对齐（该函数对每
// 字段各调 inv->search 一次，本地统计——TextPlugin 的 fields_ 各自一份
// InvertedIndex，每份的 N/sum_dl/df 已天然字段隔离；本函数跨段场景下同样为
// 每字段聚合各自的全局统计）。
//
// 实现位置说明：因 MultiFieldSegmentView 持 SealedSegment*（段 IS-A
// LiveChecker），且 segment.hpp 已 include segment_query.hpp（段内 view()
// 返回 SegmentView），反向 include 会成环 → 本函数体放本头（与 SealedSegment
// 同一头），保持头文件自包含 + 零环依赖。
// ===========================================================================

// §3.5 多段查询的两层归并（multi-field 版）：① 跨段跨字段聚合每字段的全局
// N/sum_dl/df（G-on-the-fly）② 串行逐段每字段 search + 段内跨字段累加到 docid
// ③ 全局 top-k。返回按分数降序（并列以 key 升序稳定）的 top-k（与
// multi_segment_search / TextPlugin::search_fields 排序一致）。
//
// 关键不变量：
//   - 一个 docid 只在一个段（并集，不求和）；同段内的多字段命中按字段累加（与
//     TextPlugin::search_fields 一致：acc[ord]+=score*boost）。
//   - 每字段一份独立 ExtStats（独立 N/sum_dl/df）；不跨字段混用 idf。
//   - doc_len 取自段级 LiveChecker（total doc_len，与 TextPlugin::docs_ 一致）。
//   - 字段在某个段缺失（fields_ 空）→ 跳过该段该字段，无空查询展开。
[[nodiscard]] inline std::vector<SearchHit> multi_field_segment_search(
    std::span<const MultiFieldSegmentView> segs,
    const std::unordered_map<std::string,
                             std::vector<std::string>>& field_terms,
    std::size_t k,
    const bm25::Bm25Params* params = nullptr) {
    if (field_terms.empty() || k == 0) return {};

    // 阶段 1：跨段跨字段聚合每字段的 G-on-the-fly 全局统计。
    // per-field：每字段独立一份 N / sum_dl / per-term df。
    // S29-5：per-term df 从 unordered_map 改扁平 pair 列表（ExtStats::df
    // 类型变更，词数个位数场景线性扫描 + 免节点分配）。
    std::unordered_map<std::string, bm25::ExtStats,
                       StringHash, std::equal_to<>> per_field_ext;
    std::unordered_map<std::string,
                       std::vector<std::pair<std::string, std::uint64_t>>,
                       StringHash, std::equal_to<>> per_field_df;
    for (const auto& [fname, terms] : field_terms) {
        if (terms.empty()) continue;
        auto& ext = per_field_ext[fname];
        auto& dfm = per_field_df[fname];
        ext.df = nullptr;  // 阶段 2 才回填
        dfm.reserve(terms.size());
        for (const auto& t : terms) dfm.emplace_back(t, 0);
    }

    for (const auto& s : segs) {
        for (const auto& fv : s.fields) {
            auto ext_it = per_field_ext.find(fv.field_name);
            if (ext_it == per_field_ext.end()) continue;
            // 该段该字段贡献：N/sum_dl 与各 term 的 df。
            ext_it->second.N      += fv.inv->live_doc_count();
            ext_it->second.sum_dl += fv.inv->sum_doc_len();
            auto df_it = per_field_df.find(fv.field_name);
            if (df_it != per_field_df.end()) {
                for (auto& [term, df] : df_it->second) {
                    df += fv.inv->doc_freq(term);
                }
            }
        }
    }
    // 把每个字段的 df map 指针挂回 ext（InvertedIndex::search 走 ext.df 取全局 df）。
    for (auto& [fname, ext] : per_field_ext) {
        auto df_it = per_field_df.find(fname);
        ext.df = (df_it != per_field_df.end()) ? &df_it->second : nullptr;
    }

    // 阶段 2：串行逐段，每字段 search + 跨字段累加到段内 docid。
    // 用全局 merged（SearchHit 序列）维护统一 top-k 候选（分数已含跨字段累加）。
    std::vector<SearchHit> merged;
    for (const auto& s : segs) {
        const SealedSegment* seg = s.seg;
        std::unordered_map<DocId, double> acc_local;
        for (const auto& fv : s.fields) {
            // fv.field_name 是 string_view（借段 fields_ map 的 key），而
            // field_terms 用 owning string 作 key——为省去 string→string_view
            // 异构查找要求（field_terms 未用 StringHash），构造一次 string
            // 用于查找；段数×字段数 量级，开销可忽略。
            auto tf_it = field_terms.find(std::string(fv.field_name));
            if (tf_it == field_terms.end()) continue;
            const auto& terms = tf_it->second;
            if (terms.empty()) continue;
            auto ext_it = per_field_ext.find(fv.field_name);
            if (ext_it == per_field_ext.end()) continue;
            // ext.df 已挂回（阶段 1 末尾）；nullptr 走 InvertedIndex 本地统计（不应发生）。
            const auto hits =
                fv.inv->search(terms, k, *seg, params, &ext_it->second);
            for (const auto& h : hits) {
                const auto docid = static_cast<DocId>(h.ord);
                acc_local[docid] += static_cast<double>(h.score);
            }
        }
        // 段内累加完 → 物化 SearchHit（key/lsn 由段 doc_store 提供）。
        merged.reserve(merged.size() + acc_local.size());
        for (const auto& [docid, score] : acc_local) {
            merged.push_back(SearchHit{
                seg->key_at(docid),
                seg->lsn_at(docid),
                score});
        }
    }

    // 阶段 3：全局 top-k（分数降序，并列 key 升序稳定——与 multi_segment_search
    // 一致，便于跨调用比较）。
    const auto cmp = [](const SearchHit& a, const SearchHit& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.key < b.key;
    };
    if (merged.size() > k) {
        std::partial_sort(merged.begin(),
                          merged.begin() + static_cast<std::ptrdiff_t>(k),
                          merged.end(), cmp);
        merged.resize(k);
    } else {
        std::sort(merged.begin(), merged.end(), cmp);
    }
    return merged;
}

}  // namespace bitcask::search