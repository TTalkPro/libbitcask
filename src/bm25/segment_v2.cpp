// segment_v2.cpp — v2 段盘格式流式 writer + MmapSegment 只读 reader(S30-P1)。
// 版式/契约见 include/bitcask/segment_v2.hpp 与 docs/design/s30-mmap-segments.md。

#include "bitcask/segment_v2.hpp"

#include "bitcask/hw_crc32.hpp"
#include "bitcask/vbyte.hpp"
#include "bm25_search_impl.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace bitcask::search {

using bm25::FlatPostings;
using bm25::PostingList;

namespace {

// ---- 位打包(LSB-first;块内 ≤128 值,writer 侧按块小缓冲) ----

struct BitWriter {
    std::vector<std::uint8_t>& out;
    std::uint64_t acc = 0;
    unsigned nbits = 0;
    void put(std::uint32_t v, unsigned bits) {
        if (bits == 0) return;  // 全部等于 base(docid)/0(tf,dl 不可能:见 caller)
        acc |= static_cast<std::uint64_t>(v) << nbits;
        nbits += bits;
        while (nbits >= 8) {
            out.push_back(static_cast<std::uint8_t>(acc & 0xFF));
            acc >>= 8;
            nbits -= 8;
        }
    }
    void flush() {
        if (nbits > 0) {
            out.push_back(static_cast<std::uint8_t>(acc & 0xFF));
            acc = 0;
            nbits = 0;
        }
    }
};

struct BitReader {
    const std::uint8_t* p;
    std::uint64_t acc = 0;
    unsigned nbits = 0;
    std::uint32_t get(unsigned bits) {
        if (bits == 0) return 0;
        // 逐字节装载(解码按块进行,越界由 caller 的字节数预校验兜住)。
        while (nbits < bits) {
            acc |= static_cast<std::uint64_t>(*p++) << nbits;
            nbits += 8;
        }
        auto v = static_cast<std::uint32_t>(acc & ((1ull << bits) - 1));
        acc >>= bits;
        nbits -= bits;
        return v;
    }
};

[[nodiscard]] constexpr std::size_t packed_bytes(std::size_t count,
                                                 unsigned bits) noexcept {
    return (count * bits + 7) / 8;
}

// ---- 流式文件 writer(offset 追踪 + 节级增量 CRC) ----

struct SectionRec {
    std::uint32_t kind;       // 0 = header 伪节(只记 CRC)
    std::uint32_t field_idx;  // segv2::kGlobalField = 全局
    std::uint64_t off;
    std::uint64_t len;
    std::uint32_t crc;
};

class StreamWriter {
public:
    explicit StreamWriter(std::FILE* f) : f_(f) {}

    void begin(std::uint32_t kind, std::uint32_t field_idx) {
        cur_ = SectionRec{kind, field_idx, off_, 0, 0};
    }
    void write(const void* p, std::size_t n) {
        if (!ok_ || n == 0) return;
        if (std::fwrite(p, 1, n, f_) != n) {
            ok_ = false;
            return;
        }
        cur_.crc = hw::crc32_update(
            cur_.crc,
            std::span<const std::byte>(static_cast<const std::byte*>(p), n));
        cur_.len += n;
        off_ += n;
    }
    template <class T>
    void write_pod(const T& v) {
        write(&v, sizeof(T));
    }
    void end() { recs_.push_back(cur_); }

    [[nodiscard]] std::uint64_t offset() const noexcept { return off_; }
    [[nodiscard]] std::uint64_t section_len() const noexcept { return cur_.len; }
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] const std::vector<SectionRec>& recs() const { return recs_; }

private:
    std::FILE* f_;
    std::uint64_t off_ = 0;
    SectionRec cur_{};
    std::vector<SectionRec> recs_;
    bool ok_ = true;
};

struct FileCloser {
    void operator()(std::FILE* fp) const noexcept {
        if (fp) std::fclose(fp);
    }
};

// 每 posting 块头(打包宽度;pad 保留)。
struct BlockHeader {
    std::uint8_t docid_bits;
    std::uint8_t tf_bits;
    std::uint8_t dl_bits;
    std::uint8_t pad = 0;
};
static_assert(sizeof(BlockHeader) == 4);

[[nodiscard]] unsigned width_of(std::uint32_t v) noexcept {
    return static_cast<unsigned>(std::bit_width(v));
}

}  // namespace

// ===========================================================================
// writer
// ===========================================================================

bool write_segment_v2(
    const std::string& path,
    std::uint64_t seg_id,
    std::span<const std::pair<std::string_view, const bm25::InvertedIndex*>>
        fields,
    std::uint32_t doc_count,
    const std::function<SegV2DocRow(std::uint32_t docid)>& doc_row,
    std::uint64_t total_doc_len) {
    const std::string tmp = path + ".tmp";
    std::unique_ptr<std::FILE, FileCloser> f(std::fopen(tmp.c_str(), "wb"));
    if (!f) return false;
    StreamWriter w(f.get());

    // ---- Header(64B,伪节 kind=0 只记 CRC) ----
    w.begin(0, segv2::kGlobalField);
    w.write_pod(segv2::kMagic);
    w.write_pod(segv2::kVersion);
    w.write_pod(seg_id);
    w.write_pod(static_cast<std::uint64_t>(doc_count));
    w.write_pod(total_doc_len);
    w.write_pod(static_cast<std::uint32_t>(fields.size()));
    const std::uint32_t flags = 0;
    w.write_pod(flags);
    const std::uint8_t hdr_pad[24] = {};
    w.write(hdr_pad, sizeof(hdr_pad));
    w.end();

    // ---- 每字段:kPostings(流式)→ kBlocks → kTermDict → kTermBlob →
    //      kFieldStats → kFieldName ----
    bool field_ok = true;
    for (std::uint32_t fi = 0; fi < fields.size(); ++fi) {
        const auto& [fname, inv] = fields[fi];
        // 瞬态缓冲:O(词典 + 块表)(posting/positions 全流式)。
        std::vector<segv2::TermRec> dict;
        std::vector<segv2::BlockMeta> metas;
        std::string blob;
        bool any_positions = false;
        std::vector<std::uint8_t> pack_buf;   // 单块打包缓冲(≤ ~1.6KB)
        std::vector<std::uint8_t> pos_buf;    // 单 term positions 编码缓冲
        std::vector<std::uint32_t> pos_offs;  // 单 term 行偏移(df+1)

        w.begin(static_cast<std::uint32_t>(segv2::Section::kPostings), fi);
        inv->visit_postings_sorted([&](std::string_view term,
                                       const PostingList& pl) {
            if (!field_ok || pl.empty()) return;  // 空列表不入 v2 词典
            const std::size_t n = pl.size();
            if (pl.ords.back() > 0xFFFFFFFFull) {
                field_ok = false;  // 段内 docid 必须 u32(格式硬约束)
                return;
            }
            segv2::TermRec rec{};
            rec.term_off = blob.size();
            rec.term_len = static_cast<std::uint32_t>(term.size());
            rec.df = static_cast<std::uint32_t>(n);
            rec.max_tf = pl.max_tf;
            rec.block_count = static_cast<std::uint32_t>(
                (n + segv2::kBlockSize - 1) / segv2::kBlockSize);
            rec.postings_off = w.section_len();
            rec.blocks_off = metas.size() * sizeof(segv2::BlockMeta);
            rec.pos_off = static_cast<std::uint64_t>(-1);
            blob.append(term);

            // posting 块(与 finalize() 同源的块元数据语义:min_dl 跳 0,
            // 全 0 回退 1)。
            for (std::size_t start = 0; start < n;
                 start += segv2::kBlockSize) {
                const std::size_t cnt =
                    std::min(segv2::kBlockSize, n - start);
                const auto first =
                    static_cast<std::uint32_t>(pl.ords[start]);
                const auto last =
                    static_cast<std::uint32_t>(pl.ords[start + cnt - 1]);
                std::uint32_t blk_max_tf = 0;
                std::uint32_t min_dl = 0xFFFFFFFFu;
                std::uint32_t max_dl = 0;
                for (std::size_t i = start; i < start + cnt; ++i) {
                    blk_max_tf = std::max(blk_max_tf, pl.tfs[i]);
                    max_dl = std::max(max_dl, pl.dls[i]);
                    if (pl.dls[i] > 0) min_dl = std::min(min_dl, pl.dls[i]);
                }
                if (min_dl == 0xFFFFFFFFu) min_dl = 1;

                segv2::BlockMeta bm{};
                bm.first_docid = first;
                bm.last_docid = last;
                bm.max_tf = blk_max_tf;
                bm.min_dl = min_dl;
                bm.data_off = static_cast<std::uint32_t>(w.section_len() -
                                                         rec.postings_off);
                metas.push_back(bm);

                BlockHeader bh{};
                bh.docid_bits =
                    static_cast<std::uint8_t>(width_of(last - first));
                bh.tf_bits = static_cast<std::uint8_t>(width_of(blk_max_tf));
                bh.dl_bits = static_cast<std::uint8_t>(width_of(max_dl));
                pack_buf.clear();
                {
                    BitWriter bw{pack_buf};
                    for (std::size_t i = start; i < start + cnt; ++i) {
                        bw.put(static_cast<std::uint32_t>(pl.ords[i]) - first,
                               bh.docid_bits);
                    }
                    bw.flush();
                    for (std::size_t i = start; i < start + cnt; ++i) {
                        bw.put(pl.tfs[i], bh.tf_bits);
                    }
                    bw.flush();
                    for (std::size_t i = start; i < start + cnt; ++i) {
                        bw.put(pl.dls[i], bh.dl_bits);
                    }
                    bw.flush();
                }
                w.write_pod(bh);
                w.write(pack_buf.data(), pack_buf.size());
            }

            // positions(可选,内联在 kPostings 节:off 数组 + gap-varint)。
            if (!pl.pos_off.empty()) {
                any_positions = true;
                rec.pos_off = w.section_len();
                pos_offs.clear();
                pos_buf.clear();
                pos_offs.reserve(n + 1);
                for (std::size_t i = 0; i < n; ++i) {
                    pos_offs.push_back(
                        static_cast<std::uint32_t>(pos_buf.size()));
                    std::uint32_t prev = 0;
                    for (std::uint32_t pv : pl.positions(i)) {
                        codec::vbyte_encode(pv - prev, pos_buf);
                        prev = pv;
                    }
                }
                pos_offs.push_back(static_cast<std::uint32_t>(pos_buf.size()));
                w.write(pos_offs.data(), pos_offs.size() * 4);
                w.write(pos_buf.data(), pos_buf.size());
            }

            dict.push_back(rec);
        });
        w.end();  // kPostings
        if (!field_ok || !w.ok()) break;

        w.begin(static_cast<std::uint32_t>(segv2::Section::kBlocks), fi);
        w.write(metas.data(), metas.size() * sizeof(segv2::BlockMeta));
        w.end();

        w.begin(static_cast<std::uint32_t>(segv2::Section::kTermDict), fi);
        w.write(dict.data(), dict.size() * sizeof(segv2::TermRec));
        w.end();

        w.begin(static_cast<std::uint32_t>(segv2::Section::kTermBlob), fi);
        w.write(blob.data(), blob.size());
        w.end();

        segv2::FieldStats st{};
        st.live_doc_count = inv->live_doc_count();
        st.sum_doc_len = inv->sum_doc_len();
        st.term_count = dict.size();
        st.has_positions = any_positions ? 1 : 0;
        w.begin(static_cast<std::uint32_t>(segv2::Section::kFieldStats), fi);
        w.write_pod(st);
        w.end();

        w.begin(static_cast<std::uint32_t>(segv2::Section::kFieldName), fi);
        w.write(fname.data(), fname.size());
        w.end();
    }

    // ---- kDocStore:[u64 doc_count][rows][key blob](两趟流式,免 key
    //      缓冲;key_blob_len = 节长 - 8 - 48·doc_count 可推导) ----
    if (field_ok && w.ok()) {
        w.begin(static_cast<std::uint32_t>(segv2::Section::kDocStore),
                segv2::kGlobalField);
        w.write_pod(static_cast<std::uint64_t>(doc_count));
        std::uint64_t key_cum = 0;
        for (std::uint32_t d = 0; d < doc_count; ++d) {
            const SegV2DocRow in = doc_row(d);
            segv2::DocRow row{};
            row.lsn = in.lsn;
            row.key_off = key_cum;
            row.loc_offset = in.slot.loc.offset;
            row.loc_file_id = in.slot.loc.file_id;
            row.loc_total_sz = in.slot.loc.total_sz;
            row.tstamp = in.slot.tstamp;
            row.doc_len = in.slot.doc_len;
            row.key_len = static_cast<std::uint32_t>(in.key.size());
            key_cum += in.key.size();
            w.write_pod(row);
        }
        for (std::uint32_t d = 0; d < doc_count; ++d) {
            const SegV2DocRow in = doc_row(d);
            w.write(in.key.data(), in.key.size());
        }
        w.end();
    }

    // ---- Footer + Tail ----
    bool ok = field_ok && w.ok();
    if (ok) {
        const std::uint64_t footer_off = w.offset();
        // footer 自身也走 CRC(伪节不入表,footer 尾附 crc 字段)。
        std::vector<std::byte> fb;
        auto put = [&fb](const void* p, std::size_t n) {
            const auto* b = static_cast<const std::byte*>(p);
            fb.insert(fb.end(), b, b + n);
        };
        put(&segv2::kFooterMagic, 4);
        const auto scount = static_cast<std::uint32_t>(w.recs().size());
        put(&scount, 4);
        for (const auto& r : w.recs()) {
            put(&r.kind, 4);
            put(&r.field_idx, 4);
            put(&r.off, 8);
            put(&r.len, 8);
            put(&r.crc, 4);
        }
        const std::uint32_t fcrc = hw::crc32_update(0, fb);
        put(&fcrc, 4);
        const auto flen = static_cast<std::uint32_t>(fb.size());
        ok = std::fwrite(fb.data(), 1, fb.size(), f.get()) == fb.size();
        if (ok) {
            ok = std::fwrite(&footer_off, 1, 8, f.get()) == 8 &&
                 std::fwrite(&flen, 1, 4, f.get()) == 4 &&
                 std::fwrite(&segv2::kTailMagic, 1, 4, f.get()) == 4;
        }
    }

    if (ok) {
        ok = std::fflush(f.get()) == 0 && ::fdatasync(fileno(f.get())) == 0;
    }
    f.reset();
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

// ===========================================================================
// MmapSegment
// ===========================================================================

MmapSegment::~MmapSegment() {
    if (base_ != nullptr) {
        ::munmap(const_cast<std::byte*>(base_), len_);
    }
}

namespace {
template <class T>
T load_pod(const std::byte* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}
}  // namespace

std::unique_ptr<MmapSegment> MmapSegment::open(const std::string& path,
                                               bm25::Bm25Params params,
                                               bool verify_crc) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < 64 + 16) {
        ::close(fd);
        return nullptr;
    }
    const auto fsize = static_cast<std::size_t>(st.st_size);
    void* map = ::mmap(nullptr, fsize, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);  // mmap 后 fd 可关(纯映射读,无 pread 路径)
    if (map == MAP_FAILED) return nullptr;

    auto seg = std::unique_ptr<MmapSegment>(new MmapSegment());
    seg->base_ = static_cast<const std::byte*>(map);
    seg->len_ = fsize;
    const std::byte* b = seg->base_;

    // ---- Tail → Footer ----
    const std::byte* tail = b + fsize - 16;
    const auto footer_off = load_pod<std::uint64_t>(tail);
    const auto footer_len = load_pod<std::uint32_t>(tail + 8);
    if (load_pod<std::uint32_t>(tail + 12) != segv2::kTailMagic) return nullptr;
    if (footer_off + footer_len + 16 != fsize) return nullptr;
    const std::byte* fb = b + footer_off;
    if (footer_len < 12) return nullptr;
    if (load_pod<std::uint32_t>(fb) != segv2::kFooterMagic) return nullptr;
    const auto scount = load_pod<std::uint32_t>(fb + 4);
    const std::size_t entry_sz = 4 + 4 + 8 + 8 + 4;
    if (footer_len != 8 + scount * entry_sz + 4) return nullptr;
    const auto fcrc = load_pod<std::uint32_t>(fb + footer_len - 4);
    if (hw::crc32_update(
            0, std::span<const std::byte>(fb, footer_len - 4)) != fcrc) {
        return nullptr;
    }

    // ---- Header ----
    if (load_pod<std::uint32_t>(b) != segv2::kMagic) return nullptr;
    if (load_pod<std::uint32_t>(b + 4) != segv2::kVersion) return nullptr;
    seg->seg_id_ = load_pod<std::uint64_t>(b + 8);
    const auto dc64 = load_pod<std::uint64_t>(b + 16);
    if (dc64 > 0xFFFFFFFFull) return nullptr;
    seg->doc_count_ = static_cast<std::uint32_t>(dc64);
    seg->total_doc_len_ = load_pod<std::uint64_t>(b + 24);
    const auto field_count = load_pod<std::uint32_t>(b + 32);
    seg->fields_.resize(field_count);
    seg->params_ = params;

    // ---- 节表:界检查 + (默认)CRC + 归位 ----
    bool have_docstore = false;
    for (std::uint32_t i = 0; i < scount; ++i) {
        const std::byte* e = fb + 8 + i * entry_sz;
        const auto kind = load_pod<std::uint32_t>(e);
        const auto field_idx = load_pod<std::uint32_t>(e + 4);
        const auto off = load_pod<std::uint64_t>(e + 8);
        const auto len = load_pod<std::uint64_t>(e + 16);
        const auto crc = load_pod<std::uint32_t>(e + 24);
        if (off + len > footer_off) return nullptr;  // 节必在 footer 之前
        if (verify_crc &&
            hw::crc32_update(0, std::span<const std::byte>(b + off, len)) !=
                crc) {
            return nullptr;
        }
        if (kind == 0) continue;  // header 伪节(CRC 已验)
        if (field_idx != segv2::kGlobalField) {
            if (field_idx >= field_count) return nullptr;
            Field& f = seg->fields_[field_idx];
            switch (static_cast<segv2::Section>(kind)) {
                case segv2::Section::kPostings:
                    f.postings = b + off;
                    f.postings_len = len;
                    break;
                case segv2::Section::kBlocks:
                    f.blocks = b + off;
                    f.blocks_len = len;
                    break;
                case segv2::Section::kTermDict:
                    if (len % sizeof(segv2::TermRec) != 0) return nullptr;
                    f.dict = b + off;
                    f.dict_count = len / sizeof(segv2::TermRec);
                    break;
                case segv2::Section::kTermBlob:
                    f.blob = b + off;
                    f.blob_len = len;
                    break;
                case segv2::Section::kFieldStats:
                    if (len != sizeof(segv2::FieldStats)) return nullptr;
                    f.stats = load_pod<segv2::FieldStats>(b + off);
                    break;
                case segv2::Section::kFieldName:
                    f.name.assign(reinterpret_cast<const char*>(b + off), len);
                    break;
                default:
                    return nullptr;  // 未知节型:新写端 → 整体拒收
            }
        } else if (static_cast<segv2::Section>(kind) ==
                   segv2::Section::kDocStore) {
            if (len < 8) return nullptr;
            const auto sdc = load_pod<std::uint64_t>(b + off);
            if (sdc != dc64) return nullptr;
            const std::uint64_t rows_bytes = sdc * sizeof(segv2::DocRow);
            if (8 + rows_bytes > len) return nullptr;
            seg->rows_ = b + off + 8;
            seg->key_blob_ = seg->rows_ + rows_bytes;
            seg->key_blob_len_ = len - 8 - rows_bytes;
            have_docstore = true;
        } else {
            return nullptr;
        }
    }
    if (!have_docstore) return nullptr;
    for (const auto& f : seg->fields_) {
        // 每字段节必须齐全(dict_count==0 的空字段也要有节存在)。
        if (f.dict == nullptr && f.dict_count != 0) return nullptr;
        if (f.stats.term_count != f.dict_count) return nullptr;
    }

    // ---- live 位图(初始全活;sidecar 由 caller 叠加) ----
    seg->live_ =
        std::make_unique<std::atomic<std::uint8_t>[]>(seg->doc_count_);
    for (std::uint32_t i = 0; i < seg->doc_count_; ++i) {
        seg->live_[i].store(1, std::memory_order_relaxed);
    }
    return seg;
}

std::vector<std::string_view> MmapSegment::field_names() const {
    std::vector<std::string_view> out;
    out.reserve(fields_.size());
    for (const auto& f : fields_) out.push_back(f.name);
    return out;
}

const MmapSegment::Field* MmapSegment::field_of(std::string_view name) const {
    for (const auto& f : fields_) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

bool MmapSegment::find_term(const Field& f, std::string_view term,
                            segv2::TermRec& rec) const {
    // 定长 TermRec 数组按 term 字节序升序 → mmap 上直接二分。
    std::size_t lo = 0;
    std::size_t hi = f.dict_count;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const auto r = load_pod<segv2::TermRec>(
            f.dict + mid * sizeof(segv2::TermRec));
        if (r.term_off + r.term_len > f.blob_len) return false;  // 防御
        const std::string_view t(
            reinterpret_cast<const char*>(f.blob + r.term_off), r.term_len);
        const int c = t.compare(term);
        if (c == 0) {
            rec = r;
            return true;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

bool MmapSegment::decode_rec(const Field& f, const segv2::TermRec& rec,
                             FlatPostings& out) const {
    out.ords.clear();
    out.tfs.clear();
    out.blocks.clear();
    out.max_tf = rec.max_tf;
    out.ords.reserve(rec.df);
    out.tfs.reserve(rec.df);

    const std::uint64_t metas_end =
        rec.blocks_off +
        static_cast<std::uint64_t>(rec.block_count) * sizeof(segv2::BlockMeta);
    if (metas_end > f.blocks_len) return false;

    for (std::uint32_t bi = 0; bi < rec.block_count; ++bi) {
        const auto bm = load_pod<segv2::BlockMeta>(
            f.blocks + rec.blocks_off + bi * sizeof(segv2::BlockMeta));
        const std::size_t cnt =
            (bi + 1 < rec.block_count)
                ? segv2::kBlockSize
                : rec.df - static_cast<std::size_t>(rec.block_count - 1) *
                               segv2::kBlockSize;
        const std::uint64_t data = rec.postings_off + bm.data_off;
        if (data + sizeof(BlockHeader) > f.postings_len) return false;
        const auto bh = load_pod<BlockHeader>(f.postings + data);
        const std::size_t need = sizeof(BlockHeader) +
                                 packed_bytes(cnt, bh.docid_bits) +
                                 packed_bytes(cnt, bh.tf_bits) +
                                 packed_bytes(cnt, bh.dl_bits);
        if (data + need > f.postings_len) return false;

        const auto* p = reinterpret_cast<const std::uint8_t*>(
            f.postings + data + sizeof(BlockHeader));
        {
            BitReader br{p};
            for (std::size_t i = 0; i < cnt; ++i) {
                out.ords.push_back(bm.first_docid + br.get(bh.docid_bits));
            }
        }
        p += packed_bytes(cnt, bh.docid_bits);
        {
            BitReader br{p};
            for (std::size_t i = 0; i < cnt; ++i) {
                out.tfs.push_back(br.get(bh.tf_bits));
            }
        }
        // dl 列本解码不消费(FlatPostings 无 dls;评分 dl 走 LiveChecker)。
    }

    // 块元数据重建:与 PostingList::finalize() 同语义——仅 df ≥ kBlockSize
    // 时物化(含部分尾块)。块布局差异不影响结果(块跳跃是 admissible 剪枝,
    // 见 search_wand_impl 注),此处对齐 finalize 便于对拍。
    if (rec.df >= segv2::kBlockSize) {
        out.blocks.reserve(rec.block_count);
        for (std::uint32_t bi = 0; bi < rec.block_count; ++bi) {
            const auto bm = load_pod<segv2::BlockMeta>(
                f.blocks + rec.blocks_off + bi * sizeof(segv2::BlockMeta));
            const std::size_t start = bi * segv2::kBlockSize;
            const std::size_t cnt =
                (bi + 1 < rec.block_count)
                    ? segv2::kBlockSize
                    : rec.df - static_cast<std::size_t>(rec.block_count - 1) *
                                   segv2::kBlockSize;
            out.blocks.push_back({bm.first_docid, bm.last_docid, bm.max_tf,
                                  bm.min_dl, start, cnt});
        }
    }
    return true;
}

bool MmapSegment::decode_postings(std::string_view field,
                                  std::string_view term,
                                  FlatPostings& out) const {
    const Field* f = field_of(field);
    if (f == nullptr) return false;
    segv2::TermRec rec{};
    if (!find_term(*f, term, rec)) return false;
    return decode_rec(*f, rec, out);
}

std::uint64_t MmapSegment::doc_freq(std::string_view field,
                                    std::string_view term) const {
    const Field* f = field_of(field);
    if (f == nullptr) return 0;
    segv2::TermRec rec{};
    return find_term(*f, term, rec) ? rec.df : 0;
}

std::uint64_t MmapSegment::live_doc_count(std::string_view field) const {
    const Field* f = field_of(field);
    return f != nullptr ? f->stats.live_doc_count : 0;
}

std::uint64_t MmapSegment::sum_doc_len(std::string_view field) const {
    const Field* f = field_of(field);
    return f != nullptr ? f->stats.sum_doc_len : 0;
}

std::vector<bm25::SearchResult> MmapSegment::search(
    std::string_view field,
    const std::vector<std::string>& query_terms,
    std::size_t k,
    const bm25::LiveChecker& live_checker,
    const bm25::Bm25Params* params_override,
    const bm25::ExtStats* ext) const {
    const Field* f = field_of(field);
    if (f == nullptr) return {};
    const bm25::Bm25Params& params =
        params_override ? *params_override : params_;
    const std::uint64_t N = ext ? ext->N : f->stats.live_doc_count;
    const std::uint64_t sum_dl = ext ? ext->sum_dl : f->stats.sum_doc_len;

    // 逐词按需解码进 thread_local 池(mmap → FlatPostings;池指针在两趟间
    // 稳定:先全部填充,再收集指针——emplace 增长会搬移元素)。
    static thread_local std::vector<FlatPostings> fp_pool;
    static thread_local std::vector<std::size_t> hit_idx;  // 命中词下标
    hit_idx.clear();
    std::size_t n = 0;
    std::size_t total = 0;
    for (std::size_t i = 0; i < query_terms.size(); ++i) {
        segv2::TermRec rec{};
        if (!find_term(*f, query_terms[i], rec)) continue;
        if (n == fp_pool.size()) fp_pool.emplace_back();
        if (!decode_rec(*f, rec, fp_pool[n])) continue;  // 防御:视作缺席
        hit_idx.push_back(i);
        total += rec.df;
        ++n;
    }
    if (total == 0) return {};

    if (total >= bm25::detail::kWandRouteThreshold) {
        static thread_local std::vector<const FlatPostings*> fp_ptrs;
        static thread_local std::vector<std::string_view> term_views;
        fp_ptrs.clear();
        term_views.clear();
        for (std::size_t j = 0; j < n; ++j) {
            fp_ptrs.push_back(&fp_pool[j]);
            term_views.push_back(query_terms[hit_idx[j]]);
        }
        return bm25::detail::search_wand_impl(
            term_views, fp_ptrs, k, live_checker, params, N, sum_dl,
            ext ? ext->df : nullptr, query_terms.size());
    }

    static thread_local std::vector<bm25::detail::ScoredTermView> views;
    views.clear();
    for (std::size_t j = 0; j < n; ++j) {
        views.push_back({&query_terms[hit_idx[j]], &fp_pool[j]});
    }
    return bm25::detail::score_bow_topk(views, k, N, sum_dl, params,
                                        live_checker,
                                        ext ? ext->df : nullptr);
}

// ---- doc_store ----

std::string_view MmapSegment::key_of(std::uint32_t docid) const {
    if (docid >= doc_count_) return {};
    const auto row =
        load_pod<segv2::DocRow>(rows_ + docid * sizeof(segv2::DocRow));
    if (row.key_off + row.key_len > key_blob_len_) return {};
    return {reinterpret_cast<const char*>(key_blob_ + row.key_off),
            row.key_len};
}

std::uint64_t MmapSegment::lsn_of(std::uint32_t docid) const {
    if (docid >= doc_count_) return 0;
    return load_pod<segv2::DocRow>(rows_ + docid * sizeof(segv2::DocRow)).lsn;
}

index::DocSlot MmapSegment::slot_of(std::uint32_t docid) const {
    index::DocSlot out{};
    if (docid >= doc_count_) return out;
    const auto row =
        load_pod<segv2::DocRow>(rows_ + docid * sizeof(segv2::DocRow));
    out.loc.offset = row.loc_offset;
    out.loc.file_id = row.loc_file_id;
    out.loc.total_sz = row.loc_total_sz;
    out.tstamp = row.tstamp;
    out.doc_len = row.doc_len;
    return out;
}

// ---- LiveChecker / live 位图 ----

bool MmapSegment::is_live(std::uint64_t docid) const {
    return docid < doc_count_ &&
           live_[docid].load(std::memory_order_relaxed) != 0;
}

std::uint32_t MmapSegment::doc_len(std::uint64_t docid) const {
    if (docid >= doc_count_) return 0;
    return load_pod<segv2::DocRow>(rows_ + docid * sizeof(segv2::DocRow))
        .doc_len;
}

void MmapSegment::mark_dead(std::uint32_t docid) noexcept {
    if (docid >= doc_count_) return;
    if (live_[docid].exchange(0, std::memory_order_relaxed) != 0) {
        dead_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

std::uint64_t MmapSegment::live_count() const noexcept {
    return doc_count_ - dead_count_.load(std::memory_order_relaxed);
}

// ---- live sidecar ----

bool MmapSegment::save_live_sidecar(const std::string& path) const {
    const std::string tmp = path + ".tmp";
    std::unique_ptr<std::FILE, FileCloser> f(std::fopen(tmp.c_str(), "wb"));
    if (!f) return false;
    std::vector<std::byte> buf;
    auto put = [&buf](const void* p, std::size_t nn) {
        const auto* bb = static_cast<const std::byte*>(p);
        buf.insert(buf.end(), bb, bb + nn);
    };
    put(&segv2::kLiveMagic, 4);
    put(&segv2::kVersion, 4);
    const auto dc = static_cast<std::uint64_t>(doc_count_);
    put(&dc, 8);
    std::vector<std::uint8_t> bits((doc_count_ + 7) / 8, 0);
    for (std::uint32_t i = 0; i < doc_count_; ++i) {
        if (live_[i].load(std::memory_order_relaxed) != 0) {
            bits[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
        }
    }
    put(bits.data(), bits.size());
    const std::uint32_t crc = hw::crc32_update(0, buf);
    put(&crc, 4);
    const bool ok =
        std::fwrite(buf.data(), 1, buf.size(), f.get()) == buf.size() &&
        std::fflush(f.get()) == 0 && ::fdatasync(fileno(f.get())) == 0;
    f.reset();
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool MmapSegment::load_live_sidecar(const std::string& path) {
    std::unique_ptr<std::FILE, FileCloser> f(std::fopen(path.c_str(), "rb"));
    if (!f) return false;
    std::fseek(f.get(), 0, SEEK_END);
    const long sz = std::ftell(f.get());
    std::fseek(f.get(), 0, SEEK_SET);
    const std::size_t bits_len = (doc_count_ + 7) / 8;
    if (sz < 0 || static_cast<std::size_t>(sz) != 16 + bits_len + 4) {
        return false;
    }
    std::vector<std::byte> buf(static_cast<std::size_t>(sz));
    if (std::fread(buf.data(), 1, buf.size(), f.get()) != buf.size()) {
        return false;
    }
    if (load_pod<std::uint32_t>(buf.data()) != segv2::kLiveMagic) return false;
    if (load_pod<std::uint32_t>(buf.data() + 4) != segv2::kVersion) return false;
    if (load_pod<std::uint64_t>(buf.data() + 8) != doc_count_) return false;
    const auto crc = load_pod<std::uint32_t>(buf.data() + buf.size() - 4);
    if (hw::crc32_update(0, std::span<const std::byte>(buf.data(),
                                                       buf.size() - 4)) !=
        crc) {
        return false;
    }
    const auto* bits =
        reinterpret_cast<const std::uint8_t*>(buf.data() + 16);
    std::uint64_t dead = 0;
    for (std::uint32_t i = 0; i < doc_count_; ++i) {
        const bool alive = (bits[i / 8] >> (i % 8)) & 1u;
        live_[i].store(alive ? 1 : 0, std::memory_order_relaxed);
        dead += alive ? 0 : 1;
    }
    dead_count_.store(dead, std::memory_order_relaxed);
    return true;
}

}  // namespace bitcask::search
