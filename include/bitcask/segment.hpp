// 封口段（SealedSegment，S27-2 Slice 3）。见 doc/segment-index-design-zh.md §2.2/§3.4/§8。
//
// 一个不可变、自包含的段：段内倒排（本地 docid）+ 平坦 doc_store（docid→{key, lsn,
// DocSlot, live}）+ 段统计。落盘复用 SearchCheckpoint 段级 CRC 容器（§8）：
//   section kBm25Default = InvertedIndex::serialize 字节
//   section kSegDocStore = 本头 encode 的平坦 doc_store
// 此阶段只覆盖**默认字段**倒排（多字段/向量段化后续）；封口段 docid dense 0..N →
// 平坦定长数组（chunk 退役，§3.4）。SealedSegment IS-A LiveChecker（段内 docid）。

#pragma once

#include "bitcask/index_ids.hpp"
#include "bitcask/index.hpp"           // DocSlot / DocLoc
#include "bitcask/inverted.hpp"        // InvertedIndex / TermPositions / LiveChecker
#include "bitcask/search_checkpoint.hpp"
#include "bitcask/segment_query.hpp"   // SegmentView

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bitcask::search {

class SealedSegment : public bm25::LiveChecker {
public:
    SealedSegment() = default;
    SealedSegment(const SealedSegment&) = delete;
    SealedSegment& operator=(const SealedSegment&) = delete;

    // 构建（内存段）：追加一篇文档，返回段内本地 docid（自增）。doc_len 取 Σtf，
    // 与 inv_ 内部统计一致（BM25 打分从 LiveChecker 读 doc_len）。
    DocId add(std::string key, Lsn lsn, const bm25::TermPositions& terms,
              index::DocLoc loc = {}, std::uint32_t tstamp = 0) {
        const DocId docid = static_cast<DocId>(keys_.size());
        std::uint32_t dl = 0;
        for (const auto& [t, d] : terms) dl += d.first;
        inv_.add_doc(docid, terms);
        keys_.push_back(std::move(key));
        lsns_.push_back(lsn);
        slots_.push_back(index::DocSlot{.loc = loc, .tstamp = tstamp, .doc_len = dl});
        doc_lens_.push_back(dl);
        live_.push_back(1);
        return docid;
    }

    // ---- LiveChecker（按段内 docid） ----
    [[nodiscard]] bool is_live(std::uint64_t docid) const override {
        return docid < live_.size() && live_[docid] != 0;
    }
    [[nodiscard]] std::uint32_t doc_len(std::uint64_t docid) const override {
        return docid < doc_lens_.size() ? doc_lens_[docid] : 0;
    }

    [[nodiscard]] std::size_t doc_count() const { return keys_.size(); }
    [[nodiscard]] const bm25::InvertedIndex& inverted() const { return inv_; }
    [[nodiscard]] const std::string& key_at(DocId d) const { return keys_[d]; }
    [[nodiscard]] Lsn lsn_at(DocId d) const { return lsns_[d]; }

    // 查询视图（供 multi_segment_search；引用本段，段须活过 view 使用）。
    [[nodiscard]] SegmentView view() const {
        return SegmentView{
            &inv_, this,
            [this](DocId d) { return keys_[d]; },
            [this](DocId d) { return lsns_[d]; }};
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
        sw.add(CkptSectionType::kSegDocStore, encode_doc_store());
        return SearchCheckpoint::write(path, watermark, sw.sections());
    }

    // CRC 或结构校验失败 → nullptr（整段拒收，退全量重建）。
    [[nodiscard]] static std::unique_ptr<SealedSegment> load(
        const std::string& path) {
        auto lc = SearchCheckpoint::read(path);
        if (!lc) return nullptr;
        auto seg = std::make_unique<SealedSegment>();
        bool have_inv = false, have_ds = false;
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
            }
        }
        if (!have_inv || !have_ds) return nullptr;
        return seg;
    }

private:
    static constexpr std::uint32_t kDocStoreMagic = 0x54534453;  // 'SDST'
    static constexpr std::uint32_t kDocStoreVersion = 1;

    [[nodiscard]] std::vector<std::byte> encode_doc_store() const {
        using namespace detail;
        std::vector<std::byte> b;
        put_u32(b, kDocStoreMagic);
        put_u32(b, kDocStoreVersion);
        put_u32(b, static_cast<std::uint32_t>(keys_.size()));
        for (std::size_t i = 0; i < keys_.size(); ++i) {
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
            b.push_back(static_cast<std::byte>(live_[i]));
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
        keys_.reserve(n); lsns_.reserve(n); slots_.reserve(n);
        doc_lens_.reserve(n); live_.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            std::uint32_t klen = 0;
            if (!rd_u32(klen)) return false;
            if (static_cast<std::size_t>(end - p) < klen) return false;
            keys_.emplace_back(reinterpret_cast<const char*>(p), klen);
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
        return true;
    }

    bm25::InvertedIndex        inv_;
    std::vector<std::string>   keys_;
    std::vector<Lsn>           lsns_;
    std::vector<index::DocSlot> slots_;
    std::vector<std::uint32_t> doc_lens_;   // SoA（LiveChecker::fill_doc_lens）
    std::vector<std::uint8_t>  live_;       // R3：可变位图（本 slice 只置 1）
};

}  // namespace bitcask::search
