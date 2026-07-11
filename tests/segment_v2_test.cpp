// segment_v2_test.cpp — v2 段盘格式 round-trip 等价性(S30-P1)。
// 核心不变量:MmapSegment 查询与源 InvertedIndex **逐位一致**(共享
// bm25_search_impl 实现 + 同源统计;分数用 float 精确相等断言)。

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "bitcask/inverted.hpp"
#include "bitcask/segment_v2.hpp"
#include "test_support.hpp"

using namespace bitcask::bm25;
using bitcask::search::MmapSegment;
using bitcask::search::SegV2DocRow;
using bitcask::search::write_segment_v2;

namespace {

constexpr std::string_view kField = "body";

struct Corpus {
    InvertedIndex idx;
    FakeLiveChecker live;
    std::vector<std::string> vocab;
    std::uint32_t doc_count = 0;
    std::string path;

    // doc_store 合成行(纯函数:writer 会调两遍)。
    [[nodiscard]] SegV2DocRow row(std::uint32_t docid) const {
        static thread_local std::string key;
        key = "key" + std::to_string(docid);
        SegV2DocRow r;
        r.key = key;
        r.lsn = 1000 + docid;
        r.slot.loc.offset = docid * 64;
        r.slot.loc.file_id = 7;
        r.slot.loc.total_sz = 64;
        r.slot.tstamp = 42;
        r.slot.doc_len = 5 + docid % 7;
        return r;
    }
};

// 确定性语料:hot 全量命中(大 df,推 WAND 档),mid/rare 分层,tf 变化,
// 带 positions。
std::unique_ptr<Corpus> build_corpus(std::uint32_t docs,
                                     const std::string& file_tag) {
    auto cp = std::make_unique<Corpus>();
    Corpus& c = *cp;
    c.doc_count = docs;
    for (std::uint32_t d = 0; d < docs; ++d) {
        TermPositions tp;
        tp["hot"] = {1 + d % 4, {0, 3}};
        tp["mid" + std::to_string(d % 10)] = {1 + d % 2, {1}};
        tp["rare" + std::to_string(d % 100)] = {1, {2}};
        c.idx.add_doc(d, tp);
        c.live.doc_lens[d] = 5 + d % 7;
    }
    c.vocab.emplace_back("hot");
    for (int i = 0; i < 10; ++i) c.vocab.push_back("mid" + std::to_string(i));
    for (int i = 0; i < 100; ++i) c.vocab.push_back("rare" + std::to_string(i));
    c.path = (std::filesystem::temp_directory_path() /
              ("segv2_" + file_tag + ".seg"))
                 .string();
    std::filesystem::remove(c.path);
    return cp;
}

std::unique_ptr<MmapSegment> write_and_open(const Corpus& c,
                                            bool verify_crc = true) {
    std::vector<std::pair<std::string_view, const InvertedIndex*>> fields;
    fields.emplace_back(kField, &c.idx);
    if (!write_segment_v2(c.path, /*seg_id=*/9, fields, c.doc_count,
                          [&c](std::uint32_t d) { return c.row(d); },
                          c.idx.sum_doc_len())) {
        return nullptr;
    }
    return MmapSegment::open(c.path, Bm25Params{}, verify_crc);
}

void expect_same_results(const std::vector<SearchResult>& a,
                         const std::vector<SearchResult>& b,
                         const std::string& what) {
    ASSERT_EQ(a.size(), b.size()) << what;
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].ord, b[i].ord) << what << " @" << i;
        EXPECT_EQ(a[i].score, b[i].score) << what << " @" << i;  // 位级
    }
}

}  // namespace

// 小语料(BOW 档):全 vocab 单词 + 组合查询逐位一致;doc_freq 一致;
// 缺席词一致。
TEST(SegmentV2, RoundTripBitIdenticalBow) {
    auto cp = build_corpus(60, "bow");
    auto& c = *cp;
    auto seg = write_and_open(c);
    ASSERT_NE(seg, nullptr);

    for (const auto& t : c.vocab) {
        EXPECT_EQ(seg->doc_freq(kField, t), c.idx.doc_freq(t)) << t;
        expect_same_results(seg->search(kField, {t}, 10, c.live),
                            c.idx.search({t}, 10, c.live), "single:" + t);
    }
    EXPECT_EQ(seg->doc_freq(kField, "ghost"), 0u);
    EXPECT_TRUE(seg->search(kField, {"ghost"}, 10, c.live).empty());

    const std::vector<std::string> q = {"mid1", "rare42", "mid7", "ghost"};
    expect_same_results(seg->search(kField, q, 10, c.live),
                        c.idx.search(q, 10, c.live), "combo");
    // 重复词(两份快照语义)。
    const std::vector<std::string> dq = {"mid1", "mid1"};
    expect_same_results(seg->search(kField, dq, 10, c.live),
                        c.idx.search(dq, 10, c.live), "dup");
}

// 大语料(WAND 档,hot df=5000 ≥ 1024):随机查询矩阵逐位一致。
TEST(SegmentV2, RoundTripBitIdenticalWandRandomized) {
    auto cp = build_corpus(5000, "wand");
    auto& c = *cp;
    auto seg = write_and_open(c);
    ASSERT_NE(seg, nullptr);

    std::mt19937 rng(30011);
    std::uniform_int_distribution<std::size_t> vpick(0, c.vocab.size() - 1);
    std::uniform_int_distribution<int> nterms(1, 6);
    std::uniform_int_distribution<int> kpick(1, 3);
    for (int trial = 0; trial < 200; ++trial) {
        std::vector<std::string> q;
        const int n = nterms(rng);
        for (int i = 0; i < n; ++i) q.push_back(c.vocab[vpick(rng)]);
        const std::size_t k = static_cast<std::size_t>(kpick(rng)) * 5;
        expect_same_results(seg->search(kField, q, k, c.live),
                            c.idx.search(q, k, c.live),
                            "trial" + std::to_string(trial));
        if (::testing::Test::HasFailure()) break;
    }
}

// G-on-the-fly:外部统计注入(跨段全局 df/N/sum_dl 语义)逐位一致。
TEST(SegmentV2, ExtStatsEquivalence) {
    auto cp = build_corpus(2000, "ext");
    auto& c = *cp;
    auto seg = write_and_open(c);
    ASSERT_NE(seg, nullptr);

    std::vector<std::pair<std::string, std::uint64_t>> df = {
        {"hot", 12345}, {"mid3", 777}};
    ExtStats ext;
    ext.N = 50000;
    ext.sum_dl = 300000;
    ext.df = &df;
    const std::vector<std::string> q = {"hot", "mid3", "rare9"};
    expect_same_results(seg->search(kField, q, 10, c.live, nullptr, &ext),
                        c.idx.search(q, 10, c.live, nullptr, &ext), "ext");
}

// live 过滤(死文档跳过 + live_df→idf 变化)逐位一致。
TEST(SegmentV2, LiveFilterEquivalence) {
    auto cp = build_corpus(300, "live");
    auto& c = *cp;
    // 删掉 1/3 文档。
    for (std::uint32_t d = 0; d < 300; d += 3) c.live.doc_lens.erase(d);
    auto seg = write_and_open(c);
    ASSERT_NE(seg, nullptr);
    const std::vector<std::string> q = {"hot", "mid2"};
    expect_same_results(seg->search(kField, q, 20, c.live),
                        c.idx.search(q, 20, c.live), "live");
}

// 解码快照与内存 snapshot_flat 逐位一致(先 finalize 对齐块元数据规范集)。
TEST(SegmentV2, DecodeMatchesSnapshotFlat) {
    auto cp = build_corpus(700, "decode");
    auto& c = *cp;  // hot df=700 ≥ 128 → 有块
    c.idx.finalize_all_postings();
    auto seg = write_and_open(c);
    ASSERT_NE(seg, nullptr);

    for (const auto& t : {std::string("hot"), std::string("mid0"),
                          std::string("rare99")}) {
        FlatPostings got;
        ASSERT_TRUE(seg->decode_postings(kField, t, got)) << t;
        // 参照:内存索引直接拷快照。
        FlatPostings want;
        {
            auto r = c.idx.search({t}, 1, c.live);  // 触发无关;直接经解码器难取
            (void)r;
        }
        // 经 doc_freq/df 无法拿快照——用 shard 内部接口最直接:
        auto& shard = c.idx.shard_for(t);
        InvertedIndex::PostingMap::const_accessor acc;
        ASSERT_TRUE(shard.inverted.find(acc, std::string(t)));
        acc->second->snapshot_flat(want);

        ASSERT_EQ(got.ords, want.ords) << t;
        ASSERT_EQ(got.tfs, want.tfs) << t;
        EXPECT_EQ(got.max_tf, want.max_tf) << t;
        ASSERT_EQ(got.blocks.size(), want.blocks.size()) << t;
        for (std::size_t i = 0; i < got.blocks.size(); ++i) {
            EXPECT_EQ(got.blocks[i].base_ord, want.blocks[i].base_ord);
            EXPECT_EQ(got.blocks[i].end_ord, want.blocks[i].end_ord);
            EXPECT_EQ(got.blocks[i].max_tf, want.blocks[i].max_tf);
            EXPECT_EQ(got.blocks[i].min_dl, want.blocks[i].min_dl);
            EXPECT_EQ(got.blocks[i].start_idx, want.blocks[i].start_idx);
            EXPECT_EQ(got.blocks[i].count, want.blocks[i].count);
        }
    }
}

// doc_store 行/统计/元信息 round-trip。
TEST(SegmentV2, DocStoreRoundTrip) {
    auto cp = build_corpus(50, "docstore");
    auto& c = *cp;
    auto seg = write_and_open(c);
    ASSERT_NE(seg, nullptr);

    EXPECT_EQ(seg->seg_id(), 9u);
    EXPECT_EQ(seg->doc_count(), 50u);
    EXPECT_EQ(seg->live_doc_count(kField), c.idx.live_doc_count());
    EXPECT_EQ(seg->sum_doc_len(kField), c.idx.sum_doc_len());
    ASSERT_EQ(seg->field_names().size(), 1u);
    EXPECT_EQ(seg->field_names()[0], kField);

    for (std::uint32_t d = 0; d < 50; ++d) {
        EXPECT_EQ(seg->key_of(d), "key" + std::to_string(d));
        EXPECT_EQ(seg->lsn_of(d), 1000u + d);
        const auto slot = seg->slot_of(d);
        EXPECT_EQ(slot.loc.offset, d * 64u);
        EXPECT_EQ(slot.loc.file_id, 7u);
        EXPECT_EQ(slot.loc.total_sz, 64u);
        EXPECT_EQ(slot.tstamp, 42u);
        EXPECT_EQ(slot.doc_len, 5 + d % 7);
        EXPECT_EQ(seg->doc_len(d), 5 + d % 7);  // LiveChecker 面
        EXPECT_TRUE(seg->is_live(d));
    }
    EXPECT_TRUE(seg->key_of(50).empty());  // 越界
    EXPECT_FALSE(seg->is_live(50));
}

// live sidecar:mark_dead → save → 重开 + load → 位图还原;篡改拒载。
TEST(SegmentV2, LiveSidecarRoundTrip) {
    auto cp = build_corpus(40, "sidecar");
    auto& c = *cp;
    auto seg = write_and_open(c);
    ASSERT_NE(seg, nullptr);
    for (std::uint32_t d = 0; d < 40; d += 4) seg->mark_dead(d);
    EXPECT_EQ(seg->live_count(), 30u);
    const std::string side = c.path + ".live";
    ASSERT_TRUE(seg->save_live_sidecar(side));

    auto seg2 = MmapSegment::open(c.path);
    ASSERT_NE(seg2, nullptr);
    EXPECT_TRUE(seg2->is_live(0));  // 未叠加 sidecar 前全活
    ASSERT_TRUE(seg2->load_live_sidecar(side));
    EXPECT_EQ(seg2->live_count(), 30u);
    for (std::uint32_t d = 0; d < 40; ++d) {
        EXPECT_EQ(seg2->is_live(d), d % 4 != 0) << d;
    }

    // 篡改 sidecar → CRC 拒载(位图不变)。
    {
        std::fstream fs(side,
                        std::ios::in | std::ios::out | std::ios::binary);
        fs.seekp(17);
        char x = 0x5A;
        fs.write(&x, 1);
    }
    auto seg3 = MmapSegment::open(c.path);
    ASSERT_NE(seg3, nullptr);
    EXPECT_FALSE(seg3->load_live_sidecar(side));
    EXPECT_EQ(seg3->live_count(), 40u);
}

// 段级 CRC:文件任意字节篡改 → open 拒载;verify_crc=false 跳过校验仍可开
// (S21-A6 可信读 opt-in)。
TEST(SegmentV2, CrcTamperRejected) {
    auto cp = build_corpus(200, "crc");
    auto& c = *cp;
    auto seg = write_and_open(c);
    ASSERT_NE(seg, nullptr);
    seg.reset();

    const auto fsize = std::filesystem::file_size(c.path);
    // 在文件前 3/4 均匀取几个位置翻转(命中 header/postings/dict/docstore)。
    for (std::uintmax_t pos :
         {std::uintmax_t{8}, fsize / 4, fsize / 2, fsize * 3 / 4}) {
        std::vector<char> orig(1);
        {
            std::fstream fs(c.path,
                            std::ios::in | std::ios::out | std::ios::binary);
            fs.seekg(static_cast<std::streamoff>(pos));
            fs.read(orig.data(), 1);
            fs.seekp(static_cast<std::streamoff>(pos));
            const char flipped = static_cast<char>(orig[0] ^ 0x40);
            fs.write(&flipped, 1);
        }
        EXPECT_EQ(MmapSegment::open(c.path), nullptr) << "pos=" << pos;
        {
            std::fstream fs(c.path,
                            std::ios::in | std::ios::out | std::ios::binary);
            fs.seekp(static_cast<std::streamoff>(pos));
            fs.write(orig.data(), 1);
        }
    }
    // 复原后可开;verify_crc=false 也可开。
    EXPECT_NE(MmapSegment::open(c.path), nullptr);
    EXPECT_NE(MmapSegment::open(c.path, Bm25Params{}, false), nullptr);
    // 尾部截断 → 拒载。
    std::filesystem::resize_file(c.path, fsize - 8);
    EXPECT_EQ(MmapSegment::open(c.path), nullptr);
}

// 多字段:两字段各自词典/统计隔离,查询等价。
TEST(SegmentV2, MultiFieldRoundTrip) {
    InvertedIndex title;
    InvertedIndex body;
    FakeLiveChecker live;
    for (std::uint32_t d = 0; d < 80; ++d) {
        title.add_doc(d, {{"t" + std::to_string(d % 5), {1, {0}}}});
        body.add_doc(d, {{"b" + std::to_string(d % 7), {2, {1}}},
                         {"shared", {1, {2}}}});
        live.doc_lens[d] = 3;
    }
    title.finalize_all_postings();
    const auto path = (std::filesystem::temp_directory_path() /
                       "segv2_multifield.seg")
                          .string();
    std::filesystem::remove(path);
    std::vector<std::pair<std::string_view, const InvertedIndex*>> fields;
    fields.emplace_back("title", &title);
    fields.emplace_back("body", &body);
    static thread_local std::string key;
    ASSERT_TRUE(write_segment_v2(
        path, 1, fields, 80,
        [](std::uint32_t d) {
            key = "k" + std::to_string(d);
            SegV2DocRow r;
            r.key = key;
            r.lsn = d;
            return r;
        },
        body.sum_doc_len()));
    auto seg = MmapSegment::open(path);
    ASSERT_NE(seg, nullptr);
    ASSERT_EQ(seg->field_names().size(), 2u);

    EXPECT_EQ(seg->doc_freq("title", "t1"), title.doc_freq("t1"));
    EXPECT_EQ(seg->doc_freq("body", "shared"), body.doc_freq("shared"));
    EXPECT_EQ(seg->doc_freq("title", "shared"), 0u);  // 字段隔离
    expect_same_results(seg->search("title", {"t2"}, 10, live),
                        title.search({"t2"}, 10, live), "title");
    expect_same_results(seg->search("body", {"shared", "b3"}, 10, live),
                        body.search({"shared", "b3"}, 10, live), "body");
    EXPECT_TRUE(seg->search("nofield", {"x"}, 10, live).empty());
}
