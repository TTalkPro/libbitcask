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
#include "bitcask/segment_query.hpp"  // Slice 4:SegmentView / multi_segment_search
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

// ===========================================================================
// S30-P1 Slice 3：其余查询面等价性(phrase/near/explain/wildcard/fuzzy/bool)
// ===========================================================================

namespace {

// 带 positions 的短语语料:"alpha beta"(相邻)出现于 d%3==0 的文档,
// "alpha ... beta"(隔 2)出现于 d%3==1,只有 alpha 的 d%3==2。
std::unique_ptr<Corpus> build_phrase_corpus(std::uint32_t docs,
                                            const std::string& tag) {
    auto cp = std::make_unique<Corpus>();
    Corpus& c = *cp;
    c.doc_count = docs;
    for (std::uint32_t d = 0; d < docs; ++d) {
        TermPositions tp;
        switch (d % 3) {
            case 0:
                tp["alpha"] = {1, {0}};
                tp["beta"] = {1, {1}};
                break;
            case 1:
                tp["alpha"] = {1, {0}};
                tp["beta"] = {1, {3}};
                break;
            default:
                tp["alpha"] = {2, {0, 5}};
                break;
        }
        tp["fill" + std::to_string(d % 20)] = {1, {9}};
        c.idx.add_doc(d, tp);
        c.live.doc_lens[d] = 4;
    }
    c.path = (std::filesystem::temp_directory_path() /
              ("segv2_" + tag + ".seg"))
                 .string();
    std::filesystem::remove(c.path);
    return cp;
}

// 无序容差比较(wildcard/fuzzy:采集顺序差 → 浮点累加序差 → 末位 ulp 差;
// 近平分还可能重排)。按 ord 归并后逐分数近似比较。
void expect_same_results_approx(const std::vector<SearchResult>& a,
                                const std::vector<SearchResult>& b,
                                const std::string& what) {
    ASSERT_EQ(a.size(), b.size()) << what;
    auto key_sorted = [](std::vector<SearchResult> v) {
        std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) {
            return x.ord < y.ord;
        });
        return v;
    };
    const auto sa = key_sorted(a);
    const auto sb = key_sorted(b);
    for (std::size_t i = 0; i < sa.size(); ++i) {
        EXPECT_EQ(sa[i].ord, sb[i].ord) << what << " @" << i;
        EXPECT_NEAR(sa[i].score, sb[i].score,
                    std::abs(sa[i].score) * 1e-5F + 1e-7F)
            << what << " @" << i;
    }
}

}  // namespace

TEST(SegmentV2Slice3, PhraseAndNearEquivalence) {
    auto cp = build_phrase_corpus(120, "phrase");
    auto& c = *cp;
    auto seg = write_and_open(c);
    ASSERT_NE(seg, nullptr);

    const std::vector<std::string> pq = {"alpha", "beta"};
    expect_same_results(seg->search_phrase(kField, pq, 10, c.live),
                        c.idx.search_phrase(pq, 10, c.live), "phrase");
    for (std::uint32_t slop : {0u, 1u, 2u, 5u}) {
        expect_same_results(
            seg->search_near(kField, pq, 10, slop, c.live),
            c.idx.search_near(pq, 10, slop, c.live),
            "near slop=" + std::to_string(slop));
    }
    // 缺词 → 双侧空。
    EXPECT_TRUE(seg->search_phrase(kField, {"alpha", "ghost"}, 10, c.live)
                    .empty());
    EXPECT_TRUE(c.idx.search_phrase({"alpha", "ghost"}, 10, c.live).empty());
}

TEST(SegmentV2Slice3, ExplainEquivalence) {
    auto cp = build_corpus(90, "explain");
    auto& c = *cp;
    auto seg = write_and_open(c);
    ASSERT_NE(seg, nullptr);

    const std::vector<std::string> q = {"hot", "mid3", "ghost"};
    for (std::uint64_t docid : {0ull, 13ull, 89ull}) {
        const auto a = seg->explain(kField, q, docid, c.live);
        const auto b = c.idx.explain(q, docid, c.live);
        ASSERT_EQ(a.terms.size(), b.terms.size());
        EXPECT_EQ(a.total, b.total) << docid;
        for (std::size_t i = 0; i < a.terms.size(); ++i) {
            EXPECT_EQ(a.terms[i].term, b.terms[i].term);
            EXPECT_EQ(a.terms[i].df, b.terms[i].df);
            EXPECT_EQ(a.terms[i].idf, b.terms[i].idf);
            EXPECT_EQ(a.terms[i].tf, b.terms[i].tf);
            EXPECT_EQ(a.terms[i].tf_norm, b.terms[i].tf_norm);
            EXPECT_EQ(a.terms[i].contribution, b.terms[i].contribution);
        }
    }
}

TEST(SegmentV2Slice3, WildcardEquivalence) {
    auto cp = build_corpus(150, "wildcard");
    auto& c = *cp;
    auto seg = write_and_open(c);
    ASSERT_NE(seg, nullptr);

    // 前缀 / 中缀 / 全扫 / 问号 各形态。
    for (const char* pat : {"mid*", "rare1*", "*id3", "m?d4", "*are99*"}) {
        expect_same_results_approx(
            seg->search_wildcard(kField, pat, 20, c.live),
            c.idx.search_wildcard(pat, 20, c.live),
            std::string("pat=") + pat);
    }
    EXPECT_TRUE(seg->search_wildcard(kField, "zzz*", 10, c.live).empty());
}

TEST(SegmentV2Slice3, FuzzyEquivalence) {
    auto cp = build_corpus(150, "fuzzy");
    auto& c = *cp;
    auto seg = write_and_open(c);
    ASSERT_NE(seg, nullptr);

    for (std::uint32_t maxd : {1u, 2u}) {
        expect_same_results_approx(
            seg->search_fuzzy(kField, {"mid3"}, 20, maxd, c.live),
            c.idx.search_fuzzy({"mid3"}, 20, maxd, c.live),
            "fuzzy d=" + std::to_string(maxd));
    }
    // 多查询词:同一 vocab term 至多入选一次的语义。
    expect_same_results_approx(
        seg->search_fuzzy(kField, {"mid1", "mid2"}, 20, 1, c.live),
        c.idx.search_fuzzy({"mid1", "mid2"}, 20, 1, c.live), "fuzzy multi");
}

TEST(SegmentV2Slice3, BoolEquivalence) {
    auto cp = build_corpus(400, "bool");
    auto& c = *cp;
    auto seg = write_and_open(c);
    ASSERT_NE(seg, nullptr);

    using bitcask::bm25::QueryNode;
    // 扁平:MUST 交集 / SHOULD 加分 / MUST_NOT 排除 各组合。
    std::vector<QueryNode> cases;
    cases.push_back(QueryNode::must_all(
        {QueryNode::must_term("hot"), QueryNode::must_term("mid3")}));
    cases.push_back(QueryNode::must_all({QueryNode::must_term("hot"),
                                         QueryNode::must_term("mid3"),
                                         QueryNode::must_not_term("rare13")}));
    cases.push_back(QueryNode::should_any({QueryNode::should_term("mid1"),
                                           QueryNode::should_term("rare42")}));
    cases.push_back(QueryNode::must_all({QueryNode::must_term("mid5"),
                                         QueryNode::should_term("rare55")}));
    for (std::size_t i = 0; i < cases.size(); ++i) {
        expect_same_results(seg->bool_search(kField, cases[i], 15, c.live),
                            c.idx.bool_search(cases[i], 15, c.live),
                            "bool" + std::to_string(i));
    }

    // BMW must-only 大列表分支(hot df=400 且纯 MUST → 走块跳跃路径)。
    auto bmw = QueryNode::must_all(
        {QueryNode::must_term("hot"), QueryNode::must_term("mid7")});
    expect_same_results(seg->bool_search(kField, bmw, 5, c.live),
                        c.idx.bool_search(bmw, 5, c.live), "bmw");
}

TEST(SegmentV2Slice3, BoolTreeWithPhraseEquivalence) {
    auto cp = build_phrase_corpus(150, "booltree");
    auto& c = *cp;
    auto seg = write_and_open(c);
    ASSERT_NE(seg, nullptr);

    using bitcask::bm25::QueryNode;
    using bitcask::bm25::QueryOp;
    // 树:(必须短语 "alpha beta") + (SHOULD fill3) - (MUST_NOT fill7)。
    QueryNode phrase;
    phrase.op = QueryOp::MUST;
    phrase.is_phrase = true;
    phrase.phrase_terms = {"alpha", "beta"};
    QueryNode root;
    root.op = QueryOp::SHOULD;
    root.children.push_back(phrase);
    root.children.push_back(QueryNode::should_term("fill3"));
    root.children.push_back(QueryNode::must_not_term("fill7"));

    expect_same_results(seg->bool_search_tree(kField, root, 20, c.live),
                        c.idx.bool_search_tree(root, 20, c.live), "tree");

    // 嵌套组:( +alpha ( fill1 fill2 ) )。
    QueryNode grp;
    grp.op = QueryOp::SHOULD;
    grp.children.push_back(QueryNode::should_term("fill1"));
    grp.children.push_back(QueryNode::should_term("fill2"));
    QueryNode root2;
    root2.op = QueryOp::SHOULD;
    root2.children.push_back(QueryNode::must_term("alpha"));
    root2.children.push_back(grp);
    expect_same_results(seg->bool_search_tree(kField, root2, 20, c.live),
                        c.idx.bool_search_tree(root2, 20, c.live), "tree2");
}

// ===========================================================================
// S30-P1 Slice 4：TermIndex 接口接线——SegmentView 经同一指针指内存段与
// mmap 段,multi_segment_search 消费方无感;混合段集逐位等价。
// ===========================================================================

namespace {

using bitcask::search::MmapFieldIndex;
using bitcask::search::SegmentView;
using bitcask::search::multi_segment_search;

// 内存侧视图(合成 doc_store 与 writer 输入同源:key<d> / lsn 1000+d)。
SegmentView mem_view(const Corpus& c) {
    return SegmentView{&c.idx, &c.live,
                       [](bitcask::DocId d) {
                           return "key" + std::to_string(d);
                       },
                       [](bitcask::DocId d) -> bitcask::Lsn {
                           return 1000 + d;
                       },
                       nullptr};
}

// mmap 侧视图(key/lsn 走段 doc_store;live 用同一 FakeLiveChecker 保证
// 两侧删除语义一致)。
SegmentView mmap_view(const MmapFieldIndex& fi, const Corpus& c) {
    const auto* seg = fi.segment();
    return SegmentView{&fi, &c.live,
                       [seg](bitcask::DocId d) {
                           return std::string(seg->key_of(
                               static_cast<std::uint32_t>(d)));
                       },
                       [seg](bitcask::DocId d) -> bitcask::Lsn {
                           return seg->lsn_of(static_cast<std::uint32_t>(d));
                       },
                       nullptr};
}

void expect_same_hits(const std::vector<bitcask::search::SearchHit>& a,
                      const std::vector<bitcask::search::SearchHit>& b,
                      const std::string& what) {
    ASSERT_EQ(a.size(), b.size()) << what;
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].key, b[i].key) << what << " @" << i;
        EXPECT_EQ(a[i].ord, b[i].ord) << what << " @" << i;
        EXPECT_EQ(a[i].score, b[i].score) << what << " @" << i;
    }
}

}  // namespace

// 双段 multi_segment_search:全内存 vs 全 mmap vs 混合——三者逐位一致
// (G-on-the-fly 聚合统计 + 逐段打分 + 并集归并全经 TermIndex 接口)。
TEST(SegmentV2Slice4, MultiSegmentSearchMemMmapMixedEquivalence) {
    auto cpa = build_corpus(60, "s4a");
    auto& ca = *cpa;
    auto cpb = build_corpus(3000, "s4b");  // B 段 hot df=3000 → WAND 档
    auto& cb = *cpb;
    auto sega = write_and_open(ca);
    auto segb = write_and_open(cb);
    ASSERT_NE(sega, nullptr);
    ASSERT_NE(segb, nullptr);
    MmapFieldIndex fia(sega.get(), std::string(kField));
    MmapFieldIndex fib(segb.get(), std::string(kField));

    for (const auto& q : std::vector<std::vector<std::string>>{
             {"hot"},
             {"mid1", "rare42"},
             {"hot", "mid3", "ghost"},
         }) {
        std::vector<SegmentView> mem;
        mem.push_back(mem_view(ca));
        mem.push_back(mem_view(cb));
        std::vector<SegmentView> mm;
        mm.push_back(mmap_view(fia, ca));
        mm.push_back(mmap_view(fib, cb));
        std::vector<SegmentView> mixed;
        mixed.push_back(mem_view(ca));
        mixed.push_back(mmap_view(fib, cb));

        const auto h_mem = multi_segment_search(mem, q, 10);
        const auto h_mm = multi_segment_search(mm, q, 10);
        const auto h_mixed = multi_segment_search(mixed, q, 10);
        expect_same_hits(h_mem, h_mm, "mm:" + q[0]);
        expect_same_hits(h_mem, h_mixed, "mixed:" + q[0]);
        ASSERT_FALSE(h_mem.empty());
    }
}

// 经 TermIndex 基类指针调用全部查询面(虚派发路径)——与具体类型直调一致。
TEST(SegmentV2Slice4, PolymorphicSurfaceEquivalence) {
    auto cp = build_phrase_corpus(120, "s4poly");
    auto& c = *cp;
    auto seg = write_and_open(c);
    ASSERT_NE(seg, nullptr);
    MmapFieldIndex fi(seg.get(), std::string(kField));

    const bitcask::bm25::TermIndex* tmem = &c.idx;
    const bitcask::bm25::TermIndex* tmm = &fi;

    EXPECT_EQ(tmem->live_doc_count(), tmm->live_doc_count());
    EXPECT_EQ(tmem->sum_doc_len(), tmm->sum_doc_len());
    EXPECT_EQ(tmem->doc_freq("alpha"), tmm->doc_freq("alpha"));

    const std::vector<std::string> q = {"alpha", "beta"};
    expect_same_results(tmm->search(q, 10, c.live), tmem->search(q, 10, c.live),
                        "search");
    expect_same_results(tmm->search_phrase(q, 10, c.live),
                        tmem->search_phrase(q, 10, c.live), "phrase");
    expect_same_results(tmm->search_near(q, 10, 2, c.live),
                        tmem->search_near(q, 10, 2, c.live), "near");
    expect_same_results_approx(tmm->search_wildcard("fill1*", 10, c.live),
                               tmem->search_wildcard("fill1*", 10, c.live),
                               "wildcard");
    expect_same_results_approx(tmm->search_fuzzy({"alpha"}, 10, 1, c.live),
                               tmem->search_fuzzy({"alpha"}, 10, 1, c.live),
                               "fuzzy");
    using bitcask::bm25::QueryNode;
    auto bq = QueryNode::must_all(
        {QueryNode::must_term("alpha"), QueryNode::must_not_term("fill7")});
    expect_same_results(tmm->bool_search(bq, 10, c.live),
                        tmem->bool_search(bq, 10, c.live), "bool");
    const auto ea = tmm->explain(q, 3, c.live);
    const auto eb = tmem->explain(q, 3, c.live);
    EXPECT_EQ(ea.total, eb.total);
}
