#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <queue>
#include <random>
#include <set>
#include <filesystem>
#include <fstream>
#include <thread>

#include "bitcask/analyzer.hpp"
#include "bitcask/inverted.hpp"
#include "bitcask/segment_query.hpp"  // S27-2 Slice 2：多段查询归并
#include "bitcask/segment.hpp"        // S27-2 Slice 3：SealedSegment 落盘
#include "bitcask/segment_set.hpp"    // S27-2 Slice 4：段管理器 / 清单
#include "bitcask/term_snapshot_cache.hpp"  // S29-6B：查询快照缓存
#include "test_support.hpp"

using namespace bitcask::bm25;
using namespace bitcask::text;

TEST(InvertedIndex, AddAndSearch) {
    InvertedIndex idx;
    auto analyzer = AnalyzerFactory::create(AnalyzerConfig{});
    ASSERT_NE(analyzer, nullptr);

    auto tfs0 = analyzer->analyze_with_positions("北京市朝阳区");
    idx.add_doc(0, tfs0);

    auto tfs1 = analyzer->analyze_with_positions("上海浦东");
    idx.add_doc(1, tfs1);

    EXPECT_EQ(idx.live_doc_count(), 2u);

    FakeLiveChecker checker;
    checker.doc_lens[0] = 10;
    checker.doc_lens[1] = 6;

    auto q_tfs = analyzer->analyze("北京");
    std::vector<std::string> terms;
    for (auto& [t, _] : q_tfs) terms.push_back(t);

    auto results = idx.search(terms, 10, checker);
    ASSERT_GE(results.size(), 1u);
    EXPECT_EQ(results[0].ord, 0u);
    EXPECT_GT(results[0].score, 0.0f);
}

TEST(InvertedIndex, RemoveDocUpdatesStats) {
    InvertedIndex idx;
    idx.add_doc(0, {{"hello", tp(1, {0})}, {"world", tp(1, {1})}});
    idx.add_doc(1, {{"hello", tp(2, {0, 1})}});

    EXPECT_EQ(idx.live_doc_count(), 2u);
    EXPECT_EQ(idx.sum_doc_len(), 4u);

    idx.remove_doc(2, {{"hello", 1}, {"world", 1}});
    EXPECT_EQ(idx.live_doc_count(), 1u);
    EXPECT_EQ(idx.sum_doc_len(), 2u);
}

TEST(InvertedIndex, SearchSkipsDeleted) {
    InvertedIndex idx;
    idx.add_doc(0, {{"hello", tp(2, {0, 1})}});
    idx.add_doc(1, {{"hello", tp(1, {0})}});

    FakeLiveChecker checker;
    checker.doc_lens[1] = 5;

    auto results = idx.search({"hello"}, 10, checker);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].ord, 1u);
}

TEST(InvertedIndex, SearchTopK) {
    InvertedIndex idx;
    idx.add_doc(0, {{"rare", tp(1, {0})}, {"common", tp(1, {1})}});
    idx.add_doc(1, {{"common", tp(5, {0, 1, 2, 3, 4})}, {"unique1", tp(1, {5})}});
    idx.add_doc(2, {{"common", tp(3, {0, 1, 2})}});

    FakeLiveChecker checker;
    checker.doc_lens[0] = 2;
    checker.doc_lens[1] = 6;
    checker.doc_lens[2] = 3;

    auto results = idx.search({"rare"}, 10, checker);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].ord, 0u);

    auto results2 = idx.search({"unique1"}, 10, checker);
    ASSERT_EQ(results2.size(), 1u);
    EXPECT_EQ(results2[0].ord, 1u);
}

TEST(InvertedIndex, EmptyQueryReturnsEmpty) {
    InvertedIndex idx;
    FakeLiveChecker checker;
    auto results = idx.search({}, 10, checker);
    EXPECT_TRUE(results.empty());
}

TEST(InvertedIndex, NoMatchReturnsEmpty) {
    InvertedIndex idx;
    idx.add_doc(0, {{"hello", tp(1, {0})}});

    FakeLiveChecker checker;
    checker.doc_lens[0] = 10;

    auto results = idx.search({"nonexistent"}, 10, checker);
    EXPECT_TRUE(results.empty());
}

TEST(InvertedIndex, AvgDocLen) {
    InvertedIndex idx;
    idx.add_doc(0, {{"a", tp(2, {0, 1})}, {"b", tp(3, {2, 3, 4})}});
    idx.add_doc(1, {{"c", tp(5, {0, 1, 2, 3, 4})}});

    EXPECT_DOUBLE_EQ(idx.avg_doc_len(), 5.0);
}

TEST(InvertedIndex, DfReturnsPostingCount) {
    InvertedIndex idx;
    idx.add_doc(0, {{"hello", tp(1, {0})}});
    idx.add_doc(1, {{"hello", tp(2, {0, 1})}});
    idx.add_doc(2, {{"world", tp(1, {0})}});

    EXPECT_EQ(idx.df("hello"), 2u);
    EXPECT_EQ(idx.df("world"), 1u);
    EXPECT_EQ(idx.df("missing"), 0u);
}

TEST(InvertedIndex, PhraseSearchLatin) {
    InvertedIndex idx;
    idx.add_doc(0, {{"quick", tp(1, {0})}, {"brown", tp(1, {1})}, {"fox", tp(1, {2})}});
    idx.add_doc(1, {{"brown", tp(1, {0})}, {"fox", tp(1, {1})}, {"jumps", tp(1, {2})}});
    idx.add_doc(2, {{"the", tp(1, {0})}, {"fox", tp(1, {1})}, {"quick", tp(1, {2})}});

    FakeLiveChecker checker;
    checker.doc_lens[0] = 3;
    checker.doc_lens[1] = 3;
    checker.doc_lens[2] = 3;

    auto results = idx.search_phrase({"quick", "brown", "fox"}, 10, checker);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].ord, 0u);
}

// S7-5：短语候选评分并行路径（候选数 ≥ kPhraseParallelThreshold=2048 触发
// tbb::parallel_for）的正确性 + 确定性护栏。
// 构造：3000 doc 全含首词 "alpha"（候选数 3000 > 2048 → 走并行）。偶数 doc 把
// "alpha"/"beta" 交错排（alpha@0,2,..  beta@1,3,..）→ 形成 reps=(ord%5)+1 次
// 紧邻短语；奇数 doc 把 beta 排到远处（beta@5）→ slop=0 不成短语。
// 断言：① 仅偶数 ord 入选（奇数 phrase_tf=0）② 得分随 reps 单调（ranking 正确）
// ③ 连跑两次 (ord,score) 序列逐字节一致（并行不破坏确定性）。
TEST(InvertedIndex, PhraseSearchParallelPathDeterministic) {
    InvertedIndex idx;
    FakeLiveChecker checker;
    constexpr std::uint64_t kDocs = 3000;  // > kPhraseParallelThreshold(2048)
    for (std::uint64_t ord = 0; ord < kDocs; ++ord) {
        if (ord % 2 == 0) {
            const std::uint32_t reps = static_cast<std::uint32_t>(ord % 5) + 1;
            std::vector<std::uint32_t> apos, bpos;
            for (std::uint32_t r = 0; r < reps; ++r) {
                apos.push_back(2 * r);
                bpos.push_back(2 * r + 1);
            }
            idx.add_doc(ord, {{"alpha", tp(reps, apos)}, {"beta", tp(reps, bpos)}});
            checker.doc_lens[ord] = 2 * reps;
        } else {
            // alpha@0, beta@5 —— 间隔过大，slop=0 不成短语，但 alpha 仍是候选。
            idx.add_doc(ord, {{"alpha", tp(1, {0})}, {"beta", tp(1, {5})}});
            checker.doc_lens[ord] = 6;
        }
    }

    const std::size_t k = 50;
    auto r1 = idx.search_phrase({"alpha", "beta"}, k, checker);
    ASSERT_EQ(r1.size(), k);
    for (const auto& hit : r1) {
        EXPECT_EQ(hit.ord % 2, 0u) << "奇数 doc 不应成短语: ord=" << hit.ord;
        EXPECT_GT(hit.score, 0.0f);
    }
    // 得分按 reps 桶降序（reps=5 的 doc 必排在 reps=1 之前）。
    for (std::size_t i = 1; i < r1.size(); ++i) {
        EXPECT_GE(r1[i - 1].score, r1[i].score) << "结果未按分数降序";
    }
    // 确定性：再跑一次，(ord, score) 逐项一致。
    auto r2 = idx.search_phrase({"alpha", "beta"}, k, checker);
    ASSERT_EQ(r2.size(), r1.size());
    for (std::size_t i = 0; i < r1.size(); ++i) {
        EXPECT_EQ(r1[i].ord, r2[i].ord);
        EXPECT_FLOAT_EQ(r1[i].score, r2[i].score);
    }
}

// S8.7：近邻搜索。doc "quick brown fox"，quick@0 fox@2（间隔 brown）。
TEST(InvertedIndex, NearSearchSlop) {
    InvertedIndex idx;
    idx.add_doc(0, {{"quick", tp(1, {0})}, {"brown", tp(1, {1})}, {"fox", tp(1, {2})}});
    FakeLiveChecker checker;
    checker.doc_lens[0] = 3;

    // slop=0（=严格短语）：quick 后紧跟 fox？不，中间隔了 brown → 不匹配。
    EXPECT_TRUE(idx.search_near({"quick", "fox"}, 10, 0, checker).empty());
    // slop=1：允许间隙 1 → quick(0) → fox 在 (0,2] 内 → 匹配。
    auto r = idx.search_near({"quick", "fox"}, 10, 1, checker);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].ord, 0u);
}

// S8.7：近邻保持顺序——逆序不匹配。
TEST(InvertedIndex, NearSearchOrdered) {
    InvertedIndex idx;
    idx.add_doc(0, {{"fox", tp(1, {0})}, {"quick", tp(1, {2})}});  // fox 在前
    FakeLiveChecker checker;
    checker.doc_lens[0] = 3;
    // 查 "quick fox"（要求 quick 在前），即使 slop 大也不匹配（顺序错）。
    EXPECT_TRUE(idx.search_near({"quick", "fox"}, 10, 5, checker).empty());
    // 查 "fox quick"（正确顺序）→ slop=1 匹配。
    auto r = idx.search_near({"fox", "quick"}, 10, 1, checker);
    ASSERT_EQ(r.size(), 1u);
}

TEST(InvertedIndex, PhraseSearchNoFalsePositive) {
    InvertedIndex idx;
    idx.add_doc(0, {{"the", tp(1, {0})}, {"fox", tp(1, {1})}, {"quick", tp(1, {2})}});

    FakeLiveChecker checker;
    checker.doc_lens[0] = 3;

    auto results = idx.search_phrase({"quick", "fox"}, 10, checker);
    EXPECT_TRUE(results.empty());
}

// 同一文档内短语多次出现 → phrase_tf>1，应比只出现一次的文档分数高。
// 回归 S9.7：把 other_pl.find 提到 start_pos 循环外后，内层多次 binary_search
// 的计数路径仍需正确。
TEST(InvertedIndex, PhraseSearchRepeatedInDoc) {
    InvertedIndex idx;
    idx.add_doc(0, {{"a", tp(2, {0, 2})}, {"b", tp(2, {1, 3})}});  // "a b a b" → 2 次
    idx.add_doc(1, {{"a", tp(1, {0})}, {"b", tp(1, {1})}});         // "a b" → 1 次

    FakeLiveChecker checker;
    checker.doc_lens[0] = 4;
    checker.doc_lens[1] = 2;

    auto results = idx.search_phrase({"a", "b"}, 10, checker);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].ord, 0u);  // phrase_tf=2 排在前
    EXPECT_GT(results[0].score, results[1].score);
}

TEST(InvertedIndex, PhraseSearchSkipsDeleted) {
    InvertedIndex idx;
    idx.add_doc(0, {{"hello", tp(1, {0})}, {"world", tp(1, {1})}});
    idx.add_doc(1, {{"hello", tp(1, {0})}, {"world", tp(1, {1})}});

    FakeLiveChecker checker;
    checker.doc_lens[1] = 2;

    auto results = idx.search_phrase({"hello", "world"}, 10, checker);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].ord, 1u);
}

TEST(InvertedIndex, PhraseSearchEmptyQuery) {
    InvertedIndex idx;
    FakeLiveChecker checker;
    auto results = idx.search_phrase({}, 10, checker);
    EXPECT_TRUE(results.empty());
}

TEST(InvertedIndex, PhraseSearchMissingTerm) {
    InvertedIndex idx;
    idx.add_doc(0, {{"hello", tp(1, {0})}});

    FakeLiveChecker checker;
    checker.doc_lens[0] = 1;

    auto results = idx.search_phrase({"hello", "missing"}, 10, checker);
    EXPECT_TRUE(results.empty());
}

TEST(InvertedIndex, SaveLoadRoundtrip) {
    auto tmp = std::filesystem::temp_directory_path() / "inv_test_roundtrip.inv";
    auto cleanup = [&]() { std::filesystem::remove(tmp); };
    cleanup();

    InvertedIndex idx;
    idx.add_doc(0, {{"hello", tp(1, {0})}, {"world", tp(2, {1, 2})}});
    idx.add_doc(1, {{"hello", tp(3, {0, 1, 2})}, {"foo", tp(1, {3})}});

    EXPECT_TRUE(idx.save(tmp.string()));

    InvertedIndex idx2;
    EXPECT_TRUE(idx2.load(tmp.string()));

    EXPECT_EQ(idx2.live_doc_count(), 2u);
    EXPECT_EQ(idx2.sum_doc_len(), 7u);
    EXPECT_EQ(idx2.df("hello"), 2u);
    EXPECT_EQ(idx2.df("world"), 1u);
    EXPECT_EQ(idx2.df("foo"), 1u);

    FakeLiveChecker checker;
    checker.doc_lens[0] = 3;
    checker.doc_lens[1] = 4;

    auto results = idx2.search({"hello"}, 10, checker);
    ASSERT_EQ(results.size(), 2u);

    auto phrase_results = idx2.search_phrase({"hello", "world"}, 10, checker);
    ASSERT_EQ(phrase_results.size(), 1u);
    EXPECT_EQ(phrase_results[0].ord, 0u);

    cleanup();
}

// P14e/S2:serialize/deserialize 到字节缓冲(供 search.ckpt 段),与 save/load
// 等价。直接测缓冲 round-trip 守护新 API。
TEST(InvertedIndex, SerializeDeserializeRoundtrip) {
    InvertedIndex idx;
    idx.add_doc(0, {{"hello", tp(1, {0})}, {"world", tp(2, {1, 2})}});
    idx.add_doc(1, {{"hello", tp(3, {0, 1, 2})}, {"foo", tp(1, {3})}});

    std::vector<std::byte> buf;
    idx.serialize(buf);
    EXPECT_FALSE(buf.empty());

    InvertedIndex idx2;
    ASSERT_TRUE(idx2.deserialize(buf));
    EXPECT_EQ(idx2.live_doc_count(), 2u);
    EXPECT_EQ(idx2.sum_doc_len(), 7u);
    EXPECT_EQ(idx2.df("hello"), 2u);
    EXPECT_EQ(idx2.df("foo"), 1u);

    FakeLiveChecker checker;
    checker.doc_lens[0] = 3;
    checker.doc_lens[1] = 4;
    auto results = idx2.search({"hello"}, 10, checker);
    EXPECT_EQ(results.size(), 2u);
    auto phrase = idx2.search_phrase({"hello", "world"}, 10, checker);
    ASSERT_EQ(phrase.size(), 1u);
    EXPECT_EQ(phrase[0].ord, 0u);

    // 截断缓冲 → 干净拒绝。
    InvertedIndex idx3;
    EXPECT_FALSE(idx3.deserialize(
        std::span<const std::byte>(buf.data(), buf.size() / 2)));
}

TEST(InvertedIndex, LoadMissingFileReturnsFalse) {
    InvertedIndex idx;
    EXPECT_FALSE(idx.load("/nonexistent/path/to/file.inv"));
}

TEST(InvertedIndex, SaveLoadEmpty) {
    auto tmp = std::filesystem::temp_directory_path() / "inv_test_empty.inv";
    auto cleanup = [&]() { std::filesystem::remove(tmp); };
    cleanup();

    InvertedIndex idx;
    EXPECT_TRUE(idx.save(tmp.string()));

    InvertedIndex idx2;
    EXPECT_TRUE(idx2.load(tmp.string()));
    EXPECT_EQ(idx2.live_doc_count(), 0u);
    EXPECT_EQ(idx2.sum_doc_len(), 0u);

    cleanup();
}

TEST(InvertedIndex, DfLiveCountsOnlyLive) {
    InvertedIndex idx;
    idx.add_doc(0, {{"hello", tp(1, {0})}});
    idx.add_doc(1, {{"hello", tp(2, {0, 1})}});
    idx.add_doc(2, {{"hello", tp(1, {0})}});

    EXPECT_EQ(idx.df("hello"), 3u);

    FakeLiveChecker full_checker;
    full_checker.doc_lens[0] = 1;
    full_checker.doc_lens[1] = 2;
    full_checker.doc_lens[2] = 1;
    EXPECT_EQ(idx.df_live("hello", full_checker), 3u);

    FakeLiveChecker partial_checker;
    partial_checker.doc_lens[0] = 1;
    partial_checker.doc_lens[2] = 1;
    EXPECT_EQ(idx.df_live("hello", partial_checker), 2u);

    FakeLiveChecker empty_checker;
    EXPECT_EQ(idx.df_live("hello", empty_checker), 0u);
}

TEST(InvertedIndex, SearchUsesLiveDf) {
    InvertedIndex idx;
    idx.add_doc(0, {{"term", tp(1, {0})}});
    idx.add_doc(1, {{"term", tp(1, {0})}});
    idx.add_doc(2, {{"other", tp(1, {0})}});

    FakeLiveChecker all_live;
    all_live.doc_lens[0] = 1;
    all_live.doc_lens[1] = 1;
    all_live.doc_lens[2] = 1;

    auto r1 = idx.search({"term"}, 10, all_live);
    ASSERT_EQ(r1.size(), 2u);

    FakeLiveChecker doc1_dead;
    doc1_dead.doc_lens[0] = 1;
    doc1_dead.doc_lens[2] = 1;

    auto r2 = idx.search({"term"}, 10, doc1_dead);
    ASSERT_EQ(r2.size(), 1u);
    ASSERT_EQ(r2[0].ord, 0u);
    EXPECT_GT(r2[0].score, r1[0].score);
}

TEST(QueryParser, SimpleTerm) {
    auto node = parse_query("hello");
    EXPECT_EQ(node.op, QueryOp::SHOULD);
    EXPECT_EQ(node.term, "hello");
}

TEST(QueryParser, PlusPrefix) {
    auto node = parse_query("+hello");
    EXPECT_EQ(node.op, QueryOp::SHOULD);
    EXPECT_FALSE(node.children.empty());
    EXPECT_EQ(node.children[0].op, QueryOp::MUST);
    EXPECT_EQ(node.children[0].term, "hello");
}

TEST(QueryParser, MinusPrefix) {
    auto node = parse_query("-hello");
    EXPECT_EQ(node.op, QueryOp::SHOULD);
    EXPECT_FALSE(node.children.empty());
    EXPECT_EQ(node.children[0].op, QueryOp::MUST_NOT);
    EXPECT_EQ(node.children[0].term, "hello");
}

TEST(QueryParser, MixedTerms) {
    auto node = parse_query("+hello -world foo");
    EXPECT_EQ(node.op, QueryOp::SHOULD);
    ASSERT_EQ(node.children.size(), 3u);
    EXPECT_EQ(node.children[0].op, QueryOp::MUST);
    EXPECT_EQ(node.children[0].term, "hello");
    EXPECT_EQ(node.children[1].op, QueryOp::MUST_NOT);
    EXPECT_EQ(node.children[1].term, "world");
    EXPECT_EQ(node.children[2].op, QueryOp::SHOULD);
    EXPECT_EQ(node.children[2].term, "foo");
}

TEST(QueryParser, EmptyString) {
    auto node = parse_query("");
    EXPECT_EQ(node.op, QueryOp::SHOULD);
    EXPECT_TRUE(node.term.empty());
}

TEST(QueryParser, AllShouldTermsSingleChild) {
    auto node = parse_query("hello world");
    EXPECT_EQ(node.op, QueryOp::SHOULD);
    EXPECT_FALSE(node.children.empty());
    ASSERT_EQ(node.children.size(), 2u);
    EXPECT_EQ(node.children[0].op, QueryOp::SHOULD);
    EXPECT_EQ(node.children[0].term, "hello");
    EXPECT_EQ(node.children[1].op, QueryOp::SHOULD);
    EXPECT_EQ(node.children[1].term, "world");
}

TEST(QueryParser, WhitespaceOnly) {
    auto node = parse_query("   \t\n  ");
    EXPECT_EQ(node.op, QueryOp::SHOULD);
    EXPECT_TRUE(node.term.empty());
}

// S8.6：字段限定 field:term。
TEST(QueryParser, FieldQualified) {
    auto node = parse_query("title:hello");
    EXPECT_EQ(node.field, "title");
    EXPECT_EQ(node.term, "hello");
    EXPECT_FLOAT_EQ(node.boost, 1.0F);
}

// S8.6：boost ^N。
TEST(QueryParser, BoostSuffix) {
    auto node = parse_query("hello^3");
    EXPECT_EQ(node.term, "hello");
    EXPECT_TRUE(node.field.empty());
    EXPECT_FLOAT_EQ(node.boost, 3.0F);
}

// S8.6：field:term^boost 组合。
TEST(QueryParser, FieldAndBoost) {
    auto node = parse_query("title:hello^2.5");
    EXPECT_EQ(node.field, "title");
    EXPECT_EQ(node.term, "hello");
    EXPECT_FLOAT_EQ(node.boost, 2.5F);
}

// S8.6 R5：http://x、12:30 不应被误判为字段限定。
TEST(QueryParser, ColonNotFieldWhenInvalidName) {
    auto n1 = parse_query("http://example.com");
    EXPECT_TRUE(n1.field.empty());  // "http" 后是 "//"，但整体仍按 term（冒号右是 //）
    auto n2 = parse_query(":leading");
    EXPECT_TRUE(n2.field.empty());  // 冒号在首位，colon>0 不满足
}

// S8.6：多 token 混合字段/boost/前缀。
TEST(QueryParser, MixedFieldBoost) {
    auto node = parse_query("+title:foo^2 body:bar");
    ASSERT_EQ(node.children.size(), 2u);
    EXPECT_EQ(node.children[0].op, QueryOp::MUST);
    EXPECT_EQ(node.children[0].field, "title");
    EXPECT_EQ(node.children[0].term, "foo");
    EXPECT_FLOAT_EQ(node.children[0].boost, 2.0F);
    EXPECT_EQ(node.children[1].op, QueryOp::SHOULD);
    EXPECT_EQ(node.children[1].field, "body");
    EXPECT_EQ(node.children[1].term, "bar");
}

TEST(InvertedIndex, BoolSearchShould) {
    InvertedIndex idx;
    idx.add_doc(0, {{"hello", tp(1, {0})}, {"world", tp(1, {1})}});
    idx.add_doc(1, {{"hello", tp(1, {0})}});
    idx.add_doc(2, {{"world", tp(1, {0})}});

    FakeLiveChecker checker;
    checker.doc_lens[0] = 2;
    checker.doc_lens[1] = 1;
    checker.doc_lens[2] = 1;

    auto node = parse_query("hello world");
    auto results = idx.bool_search(node, 10, checker);
    ASSERT_EQ(results.size(), 3u);
}

TEST(InvertedIndex, BoolSearchMust) {
    InvertedIndex idx;
    idx.add_doc(0, {{"hello", tp(1, {0})}, {"world", tp(1, {1})}});
    idx.add_doc(1, {{"hello", tp(1, {0})}});
    idx.add_doc(2, {{"world", tp(1, {0})}});

    FakeLiveChecker checker;
    checker.doc_lens[0] = 2;
    checker.doc_lens[1] = 1;
    checker.doc_lens[2] = 1;

    auto node = parse_query("+hello +world");
    auto results = idx.bool_search(node, 10, checker);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].ord, 0u);
}

// ord > 2^32 的 bool_search MUST 交集（intersect_u64 Inoue 原生 u64 SIMD 路径）。
TEST(InvertedIndex, BoolSearchMustU64Fallback) {
    InvertedIndex idx;
    const std::uint64_t base = 5'000'000'000ULL;
    idx.add_doc(base + 0, {{"hello", tp(1, {0})}, {"world", tp(1, {1})}});
    idx.add_doc(base + 1, {{"hello", tp(1, {0})}});
    idx.add_doc(base + 2, {{"world", tp(1, {0})}});
    idx.add_doc(base + 3, {{"hello", tp(1, {0})}, {"world", tp(1, {1})}});

    FakeLiveChecker checker;
    checker.doc_lens[base + 0] = 2;
    checker.doc_lens[base + 1] = 1;
    checker.doc_lens[base + 2] = 1;
    checker.doc_lens[base + 3] = 2;

    auto node = parse_query("+hello +world");
    auto results = idx.bool_search(node, 10, checker);
    ASSERT_EQ(results.size(), 2u);  // hello AND world → base+0, base+3
    std::set<std::uint64_t> ords;
    for (auto& r : results) ords.insert(r.ord);
    EXPECT_TRUE(ords.count(base + 0));
    EXPECT_TRUE(ords.count(base + 3));
    EXPECT_FALSE(ords.count(base + 1));  // 只含 hello
    EXPECT_FALSE(ords.count(base + 2));  // 只含 world
}

TEST(InvertedIndex, BoolSearchMustNot) {
    InvertedIndex idx;
    idx.add_doc(0, {{"hello", tp(1, {0})}});
    idx.add_doc(1, {{"hello", tp(1, {0})}, {"world", tp(1, {1})}});
    idx.add_doc(2, {{"world", tp(1, {0})}});

    FakeLiveChecker checker;
    checker.doc_lens[0] = 1;
    checker.doc_lens[1] = 2;
    checker.doc_lens[2] = 1;

    auto node = parse_query("-nonexistent");
    auto results = idx.bool_search(node, 10, checker);
    EXPECT_TRUE(results.empty());
}

TEST(InvertedIndex, BoolSearchNoMatch) {
    InvertedIndex idx;
    idx.add_doc(0, {{"hello", tp(1, {0})}});

    FakeLiveChecker checker;
    checker.doc_lens[0] = 1;

    auto node = parse_query("+nonexistent");
    auto results = idx.bool_search(node, 10, checker);
    EXPECT_TRUE(results.empty());
}

// MUST + SHOULD 混合：MUST 决定候选集，SHOULD 只参与打分、不扩大候选。
// 回归 S9.6 修复——此前 "只含 should、不含 must" 的文档会被错误纳入结果。
TEST(InvertedIndex, BoolSearchMustWithShouldBoost) {
    InvertedIndex idx;
    idx.add_doc(0, {{"hello", tp(1, {0})}, {"world", tp(1, {1})}});  // must + should
    idx.add_doc(1, {{"hello", tp(1, {0})}});                          // must only
    idx.add_doc(2, {{"world", tp(1, {0})}});                          // should only（无 must）

    FakeLiveChecker checker;
    checker.doc_lens[0] = 2;
    checker.doc_lens[1] = 1;
    checker.doc_lens[2] = 1;

    auto node = parse_query("+hello world");
    auto results = idx.bool_search(node, 10, checker);

    // doc2 不含 must 词 hello，必须被排除。
    ASSERT_EQ(results.size(), 2u);
    for (auto& r : results) EXPECT_NE(r.ord, 2u);

    // 结果按分数降序：doc0（含 should 词 world 加分）应排在 doc1 之前。
    EXPECT_EQ(results[0].ord, 0u);
    EXPECT_EQ(results[1].ord, 1u);
    EXPECT_GT(results[0].score, results[1].score);
}

TEST(InvertedIndex, BoolSearchTopK) {
    InvertedIndex idx;
    idx.add_doc(0, {{"common", tp(1, {0})}});
    idx.add_doc(1, {{"common", tp(5, {0, 1, 2, 3, 4})}});
    idx.add_doc(2, {{"common", tp(3, {0, 1, 2})}, {"rare", tp(1, {3})}});

    FakeLiveChecker checker;
    checker.doc_lens[0] = 1;
    checker.doc_lens[1] = 5;
    checker.doc_lens[2] = 4;

    auto node = parse_query("+common");
    auto results = idx.bool_search(node, 2, checker);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].ord, 1u);
    EXPECT_EQ(results[1].ord, 2u);
}

TEST(InvertedIndex, VByteCodecRoundtrip) {
    auto compressed = bitcask::codec::gap_encode({3, 7, 15, 20});
    auto decoded = bitcask::codec::gap_decode(compressed);
    EXPECT_EQ(decoded.size(), 4u);
    EXPECT_EQ(decoded[0], 3u);
    EXPECT_EQ(decoded[1], 7u);
    EXPECT_EQ(decoded[2], 15u);
    EXPECT_EQ(decoded[3], 20u);
}

TEST(InvertedIndex, VByteEncodeDecode) {
    std::vector<std::uint8_t> buf;
    bitcask::codec::vbyte_encode(127, buf);
    EXPECT_EQ(buf.size(), 1u);
    EXPECT_EQ(buf[0], 0x7F | 0x80);

    buf.clear();
    bitcask::codec::vbyte_encode(128, buf);
    EXPECT_EQ(buf.size(), 2u);

    buf.clear();
    bitcask::codec::vbyte_encode(300, buf);
    EXPECT_EQ(buf.size(), 2u);

    auto [val128, pos128] = bitcask::codec::vbyte_decode(buf.data(), 0);
    EXPECT_EQ(pos128, 2u);
    EXPECT_EQ(val128, 300u);
}

// finalize_all_postings 不改变 search 结果（幂等、不损坏 items）。
TEST(InvertedIndex, FinalizePreservesSearchResults) {
    InvertedIndex idx;
    idx.add_doc(0, {{"hello", tp(1, {0})}});
    idx.add_doc(100, {{"hello", tp(2, {0, 1})}});
    idx.add_doc(1000, {{"hello", tp(3, {0, 1, 2})}});

    auto shard = idx.df("hello");
    (void)shard;

    FakeLiveChecker checker;
    checker.doc_lens[0] = 1;
    checker.doc_lens[100] = 2;
    checker.doc_lens[1000] = 3;

    auto before = idx.search({"hello"}, 10, checker);
    ASSERT_EQ(before.size(), 3u);

    idx.finalize_all_postings();

    auto after = idx.search({"hello"}, 10, checker);
    ASSERT_EQ(after.size(), 3u);

    std::vector<std::uint64_t> before_ords;
    for (auto& r : before) before_ords.push_back(r.ord);
    std::vector<std::uint64_t> after_ords;
    for (auto& r : after) after_ords.push_back(r.ord);
    std::sort(before_ords.begin(), before_ords.end());
    std::sort(after_ords.begin(), after_ords.end());
    EXPECT_EQ(before_ords, after_ords);
}

// finalize_all_postings 后 df 不变（不丢/不增 posting）。
TEST(InvertedIndex, FinalizeKeepsDfStable) {
    InvertedIndex idx;
    for (std::uint64_t i = 0; i < 100; ++i) {
        idx.add_doc(i * 1000, {{"term", tp(1, {0})}});
    }

    idx.finalize_all_postings();

    auto shard = idx.df("term");
    ASSERT_EQ(shard, 100u);
}

TEST(InvertedIndex, SaveLoadWithFinalizedPostings) {
    auto tmp = std::filesystem::temp_directory_path() / "inv_finalized_test.inv";
    auto cleanup = [&]() { std::filesystem::remove(tmp); };
    cleanup();

    InvertedIndex idx;
    idx.add_doc(0, {{"hello", tp(1, {0})}, {"world", tp(2, {1, 2})}});
    idx.add_doc(1, {{"hello", tp(3, {0, 1, 2})}, {"foo", tp(1, {3})}});

    idx.finalize_all_postings();

    EXPECT_TRUE(idx.save(tmp.string()));

    InvertedIndex idx2;
    EXPECT_TRUE(idx2.load(tmp.string()));

    FakeLiveChecker checker;
    checker.doc_lens[0] = 3;
    checker.doc_lens[1] = 4;

    auto results = idx2.search({"hello"}, 10, checker);
    ASSERT_EQ(results.size(), 2u);

    auto phrase_results = idx2.search_phrase({"hello", "world"}, 10, checker);
    ASSERT_EQ(phrase_results.size(), 1u);
    EXPECT_EQ(phrase_results[0].ord, 0u);

    cleanup();
}

// 回归（O3 时发现的 load 回填缺失）：load 的 comp==1 分支此前只读入
// compressed_ords，不回填 items[].ord（resize 后全 0）。而 find()/
// note_appended()/compact()/df 等内存路径都以 items[].ord 为事实来源——
// 快照重载后对既有 term 增量 add_doc 会触发 note_appended 使压缩失效，
// 旧 posting 的 ord 全部读成 0，搜索结果损坏。覆盖「load → explain →
// 增量写 → 搜索」链路。
TEST(InvertedIndex, LoadFinalizedThenAddDocKeepsOldOrds) {
    auto tmp = std::filesystem::temp_directory_path() / "inv_finalized_add.inv";
    auto cleanup = [&]() { std::filesystem::remove(tmp); };
    cleanup();

    InvertedIndex idx;
    idx.add_doc(10, {{"hello", tp(1, {0})}});
    idx.add_doc(20, {{"hello", tp(2, {0, 1})}});
    idx.finalize_all_postings();
    ASSERT_TRUE(idx.save(tmp.string()));

    InvertedIndex idx2;
    ASSERT_TRUE(idx2.load(tmp.string()));

    // explain 走 pl.find(ord)（二分 items[].ord）——load 后必须能命中。
    FakeLiveChecker checker;
    checker.doc_lens[10] = 1;
    checker.doc_lens[20] = 2;
    auto ex = idx2.explain({"hello"}, 20, checker);
    ASSERT_EQ(ex.terms.size(), 1u);
    EXPECT_EQ(ex.terms[0].tf, 2u);

    // 对既有 term 增量写一条新 posting（note_appended 使压缩失效，
    // 此后 ord 完全依赖 items[].ord）。
    idx2.add_doc(30, {{"hello", tp(1, {0})}});
    checker.doc_lens[30] = 1;

    auto results = idx2.search({"hello"}, 10, checker);
    std::vector<std::uint64_t> ords;
    for (auto& r : results) ords.push_back(r.ord);
    std::sort(ords.begin(), ords.end());
    EXPECT_EQ(ords, (std::vector<std::uint64_t>{10, 20, 30}));

    cleanup();
}

// 回归 S9.4 验证缺口：已删除——v6 不再兼容 v1..v5 旧快照（项目规则
// 「不考虑向后兼容性」），由外部迁移工具或直接重新索引处理。
//
// 此前（v5）的 LoadV3SnapshotBackwardCompat 测试手工构造 v3 格式
// 快照（positions 为原始 u32 数组，非 v4 的 gap 压缩），确认 v4/v5
// 代码能向后兼容读入。v6 load 严格校验 version==6，v3 会被拒；
// 此处不再保留该测试。

TEST(PostingList, BlockMetadata) {
    InvertedIndex idx;
    for (std::uint64_t i = 0; i < 300; ++i) {
        std::uint32_t tf = static_cast<std::uint32_t>((i % 10) + 1);
        idx.add_doc(i, {{"term", tp(tf, {0})}});
    }

    idx.finalize_all_postings();

    auto& shard = idx.shard_for("term");
    InvertedIndex::PostingMap::const_accessor acc;
    ASSERT_TRUE(shard.inverted.find(acc, "term"));
    auto& pl = *acc->second;

    EXPECT_GE(pl.blocks.size(), 2u);
    std::size_t expected_blocks = (300 + PostingList::kBlockSize - 1) / PostingList::kBlockSize;
    EXPECT_EQ(pl.blocks.size(), expected_blocks);

    for (std::size_t b = 0; b < pl.blocks.size(); ++b) {
        auto& blk = pl.blocks[b];
        EXPECT_LE(blk.count, PostingList::kBlockSize);
        EXPECT_EQ(blk.base_ord, blk.start_idx);
        EXPECT_TRUE(blk.max_tf > 0);
    }
}

TEST(InvertedIndex, BlockMaxWandBasic) {
    InvertedIndex idx;
    for (std::uint64_t i = 0; i < 500; ++i) {
        std::uint32_t tf = static_cast<std::uint32_t>((i % 20) + 1);
        idx.add_doc(i, {{"common", tp(tf, {0})}, {"term", tp(1, {1})}});
    }

    FakeLiveChecker checker;
    for (std::uint64_t i = 0; i < 500; ++i) {
        checker.doc_lens[i] = 10;
    }

    auto results_wand = idx.search({"common", "term"}, 10, checker);
    ASSERT_FALSE(results_wand.empty());

    InvertedIndex idx2;
    for (std::uint64_t i = 0; i < 500; ++i) {
        std::uint32_t tf = static_cast<std::uint32_t>((i % 20) + 1);
        idx2.add_doc(i, {{"common", tp(tf, {0})}, {"term", tp(1, {1})}});
    }
    auto results_daat = idx2.search({"common", "term"}, 10, checker);
    ASSERT_EQ(results_wand.size(), results_daat.size());
    for (std::size_t i = 0; i < results_wand.size(); ++i) {
        EXPECT_EQ(results_wand[i].ord, results_daat[i].ord);
    }
}

TEST(InvertedIndex, BlockMaxWandLargeDataset) {
    InvertedIndex idx;
    for (std::uint64_t i = 0; i < 3000; ++i) {
        std::uint32_t tf = static_cast<std::uint32_t>((i % 50) + 1);
        idx.add_doc(i, {{"common", tp(tf, {0})}, {"rare", tp(1, {1})}});
    }

    FakeLiveChecker checker;
    for (std::uint64_t i = 0; i < 3000; ++i) {
        checker.doc_lens[i] = static_cast<std::uint32_t>((i % 100) + 10);
    }

    auto results_wand = idx.search({"common", "rare"}, 5, checker);

    InvertedIndex idx2;
    for (std::uint64_t i = 0; i < 3000; ++i) {
        std::uint32_t tf = static_cast<std::uint32_t>((i % 50) + 1);
        idx2.add_doc(i, {{"common", tp(tf, {0})}, {"rare", tp(1, {1})}});
    }
    auto results_daat = idx2.search({"common", "rare"}, 5, checker);

    ASSERT_EQ(results_wand.size(), results_daat.size());
    for (std::size_t i = 0; i < results_wand.size(); ++i) {
        EXPECT_EQ(results_wand[i].ord, results_daat[i].ord);
        EXPECT_FLOAT_EQ(results_wand[i].score, results_daat[i].score);
    }
}

TEST(InvertedIndex, BlockMaxWandSingleTerm) {
    InvertedIndex idx;
    for (std::uint64_t i = 0; i < 2000; ++i) {
        std::uint32_t tf = static_cast<std::uint32_t>((i % 30) + 1);
        idx.add_doc(i, {{"term", tp(tf, {0})}});
    }

    FakeLiveChecker checker;
    for (std::uint64_t i = 0; i < 2000; ++i) {
        checker.doc_lens[i] = 5;
    }

    auto results_wand = idx.search({"term"}, 10, checker);

    InvertedIndex idx2;
    for (std::uint64_t i = 0; i < 2000; ++i) {
        std::uint32_t tf = static_cast<std::uint32_t>((i % 30) + 1);
        idx2.add_doc(i, {{"term", tp(tf, {0})}});
    }
    auto results_daat = idx2.search({"term"}, 10, checker);

    ASSERT_EQ(results_wand.size(), results_daat.size());
    for (std::size_t i = 0; i < results_wand.size(); ++i) {
        EXPECT_EQ(results_wand[i].ord, results_daat[i].ord);
        EXPECT_FLOAT_EQ(results_wand[i].score, results_daat[i].score);
    }
}

// S10.6 回归：在线（未 finalize）索引也应增量封块，使 WAND 走块跳跃。
// ① add_doc 满 kBlockSize 后 blocks 非空（此前要等 finalize_all_postings）。
// ② 增量块下 WAND 结果与 finalize 后完全一致（块跳跃是精确剪枝，不改 top-k）。
// ③ finalize 后块数为含部分尾块的规范数（验证 note_appended 与 finalize 不重复/不冲突）。
TEST(InvertedIndex, IncrementalBlocksOnLiveIndex) {
    constexpr std::uint64_t kDocs = 600;  // 2 term × 600 = 1200 posting ≥ kWandThreshold(1024)
    InvertedIndex idx;
    for (std::uint64_t i = 0; i < kDocs; ++i) {
        std::uint32_t tf = static_cast<std::uint32_t>((i % 17) + 1);
        idx.add_doc(i, {{"common", tp(tf, {0})}, {"rare", tp(1, {1})}});
    }

    FakeLiveChecker checker;
    for (std::uint64_t i = 0; i < kDocs; ++i) checker.doc_lens[i] = 10;

    // ① 在线索引（未 finalize）：blocks 已增量封满块，仅含整块。
    {
        auto& shard = idx.shard_for("common");
        InvertedIndex::PostingMap::const_accessor acc;
        ASSERT_TRUE(shard.inverted.find(acc, "common"));
        ASSERT_EQ(acc->second->blocks.size(), kDocs / PostingList::kBlockSize);  // 600/128=4 满块
        for (auto& blk : acc->second->blocks) {
            EXPECT_EQ(blk.count, PostingList::kBlockSize);
        }
    }

    auto live = idx.search({"common", "rare"}, 10, checker);

    // ② finalize 后再搜，结果必须与在线一致。
    idx.finalize_all_postings();
    auto after = idx.search({"common", "rare"}, 10, checker);

    ASSERT_EQ(live.size(), after.size());
    for (std::size_t i = 0; i < live.size(); ++i) {
        EXPECT_EQ(live[i].ord, after[i].ord);
        EXPECT_FLOAT_EQ(live[i].score, after[i].score);
    }

    // ③ finalize 后块数为含部分尾块的规范数 ceil(600/128)=5。
    {
        auto& shard = idx.shard_for("common");
        InvertedIndex::PostingMap::const_accessor acc;
        ASSERT_TRUE(shard.inverted.find(acc, "common"));
        EXPECT_EQ(acc->second->blocks.size(),
                  (kDocs + PostingList::kBlockSize - 1) / PostingList::kBlockSize);
    }
}

// S10.10：index_positions=false 时不存 positions（省内存）。
// 普通 BM25 搜索照常工作；短语搜索因无位置可匹配返回空。
TEST(InvertedIndex, IndexPositionsDisabled) {
    InvertedIndex idx(Bm25Params{}, /*index_positions=*/false);
    EXPECT_FALSE(idx.index_positions());

    idx.add_doc(0, {{"quick", tp(1, {0})}, {"brown", tp(1, {1})}});
    idx.add_doc(1, {{"quick", tp(1, {0})}, {"fox", tp(1, {1})}});

    FakeLiveChecker checker;
    checker.doc_lens[0] = 2;
    checker.doc_lens[1] = 2;

    // 普通搜索正常。
    auto res = idx.search({"quick"}, 10, checker);
    EXPECT_EQ(res.size(), 2u);

    // positions 未存：posting 的 positions 为空。
    {
        auto& shard = idx.shard_for("quick");
        InvertedIndex::PostingMap::const_accessor acc;
        ASSERT_TRUE(shard.inverted.find(acc, "quick"));
        // S22-M6：SoA 后无位置 = pos_data/pos_off 恒空（惰性未物化）。
        EXPECT_TRUE(acc->second->pos_data.empty());
        EXPECT_TRUE(acc->second->pos_off.empty());
        for (std::size_t i = 0; i < acc->second->size(); ++i) {
            EXPECT_TRUE(acc->second->positions(i).empty());
        }
    }

    // 短语搜索无位置可匹配 → 空。
    auto phrase = idx.search_phrase({"quick", "brown"}, 10, checker);
    EXPECT_TRUE(phrase.empty());
}

// 对照：默认 index_positions=true 时 positions 正常存、短语可匹配。
TEST(InvertedIndex, IndexPositionsEnabledByDefault) {
    InvertedIndex idx;  // 默认构造
    EXPECT_TRUE(idx.index_positions());

    idx.add_doc(0, {{"quick", tp(1, {0})}, {"brown", tp(1, {1})}});

    FakeLiveChecker checker;
    checker.doc_lens[0] = 2;

    auto phrase = idx.search_phrase({"quick", "brown"}, 10, checker);
    ASSERT_EQ(phrase.size(), 1u);
    EXPECT_EQ(phrase[0].ord, 0u);
}

// S10.11：死点占比 ≥ 阈值时压实，删掉死 posting；结果集与分数不变（透明优化）。
TEST(InvertedIndex, CompactRemovesDeadPostingsPreservesScores) {
    InvertedIndex idx;
    for (std::uint64_t i = 0; i < 200; ++i) {
        idx.add_doc(i, {{"term", tp(static_cast<std::uint32_t>((i % 5) + 1), {0})}});
    }
    EXPECT_EQ(idx.df("term"), 200u);  // posting 行含死点

    FakeLiveChecker checker;
    for (std::uint64_t i = 0; i < 200; ++i) checker.doc_lens[i] = 10;
    // 标记奇数 ord 为死（is_live=false）。
    for (std::uint64_t i = 1; i < 200; i += 2) checker.doc_lens.erase(i);

    auto before = idx.search({"term"}, 100, checker);

    auto n = idx.compact(checker, 0.4);  // 50% 死 ≥ 0.4 → 压实
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(idx.df("term"), 100u);  // 死 posting 已删，只剩 live

    auto after = idx.search({"term"}, 100, checker);
    ASSERT_EQ(before.size(), after.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(before[i].ord, after[i].ord);
        EXPECT_FLOAT_EQ(before[i].score, after[i].score);
    }
}

// S10.11：死点占比低于阈值时不压实（避免无谓重建）。
TEST(InvertedIndex, CompactSkipsBelowThreshold) {
    InvertedIndex idx;
    for (std::uint64_t i = 0; i < 100; ++i) idx.add_doc(i, {{"term", tp(1, {0})}});

    FakeLiveChecker checker;
    for (std::uint64_t i = 0; i < 100; ++i) checker.doc_lens[i] = 10;
    checker.doc_lens.erase(0);  // 仅 1% 死

    auto n = idx.compact(checker, 0.5);
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(idx.df("term"), 100u);  // 未压实
}

// P1 回归：多读者 search × 单写者对同一 term 持续 add_doc 并发。
// 对齐生产线程模型（IndexPool 单写 worker + NIF dirty 线程多读）。
// snapshot_flat 在桶读锁（const_accessor）下拷贝、写者持写 accessor 追加，
// 互斥成立——TSan 构建下本测试验证无 data race。写者只追加已存在的 term
//（不插新 key），与 add_doc 既有桶级锁语义一致。
TEST(InvertedIndex, SearchConcurrentWithSingleWriter) {
    class AllLive : public LiveChecker {
    public:
        [[nodiscard]] bool is_live(std::uint64_t) const override { return true; }
        [[nodiscard]] std::uint32_t doc_len(std::uint64_t) const override { return 4; }
    };

    InvertedIndex idx;
    // 预热超过 kWandThreshold，让读者既走 WAND 也走标量路径（k 小走 WAND）。
    for (std::uint64_t i = 0; i < 2000; ++i) {
        idx.add_doc(i, {{"hot", tp(2, {0, 1})}, {"warm", tp(2, {2, 3})}});
    }

    AllLive checker;
    std::atomic<bool> stop{false};
    std::atomic<bool> bad{false};

    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto res = idx.search({"hot", "warm"}, 10, checker);
                if (res.size() > 10) { bad.store(true); return; }
                for (auto& h : res) {
                    // 写者只发布 ord < 4000；任何越界 ord 都是撕裂读。
                    if (h.ord >= 4000) { bad.store(true); return; }
                }
            }
        });
    }

    // 单写者（当前线程）：对既有 term 持续追加。
    for (std::uint64_t i = 2000; i < 4000; ++i) {
        idx.add_doc(i, {{"hot", tp(1, {0})}});
    }
    stop.store(true);
    for (auto& t : readers) t.join();

    EXPECT_FALSE(bad.load());
    auto final_res = idx.search({"hot"}, 10, checker);
    EXPECT_EQ(final_res.size(), 10u);
}

// P2-min：CoW 协议确定性验证。读者持 shared_ptr 引用期间（use_count>1），
// 写者对同 term 的 add_doc 必须克隆替换而非原地改——读者持有的对象冻结，
// 索引侧换上含新 posting 的新对象。无读者持引用时（use_count==1）原地改。
TEST(InvertedIndex, CowClonesWhenReaderHoldsReference) {
    InvertedIndex idx;
    idx.add_doc(0, {{"hot", tp(1, {0})}});

    // 模拟 phrase 读者：取引用后释放桶锁。
    std::shared_ptr<const PostingList> held;
    {
        InvertedIndex::PostingMap::const_accessor acc;
        ASSERT_TRUE(idx.shard_for("hot").inverted.find(acc, "hot"));
        held = acc->second;
    }
    ASSERT_EQ(held->size(), 1u);

    // 写者追加同 term → CoW：held 冻结，索引换新对象。
    idx.add_doc(1, {{"hot", tp(1, {0})}});
    EXPECT_EQ(held->size(), 1u);        // 读者视图不变
    {
        InvertedIndex::PostingMap::const_accessor acc;
        ASSERT_TRUE(idx.shard_for("hot").inverted.find(acc, "hot"));
        EXPECT_EQ(acc->second->size(), 2u);         // 索引侧已更新
        EXPECT_NE(acc->second.get(), held.get());   // 确实是克隆出的新对象
    }

    // 读者释放后（use_count 回到 1）→ 原地改，不再克隆。
    held.reset();
    const PostingList* before;
    {
        InvertedIndex::PostingMap::const_accessor acc;
        ASSERT_TRUE(idx.shard_for("hot").inverted.find(acc, "hot"));
        before = acc->second.get();
    }
    idx.add_doc(2, {{"hot", tp(1, {0})}});
    {
        InvertedIndex::PostingMap::const_accessor acc;
        ASSERT_TRUE(idx.shard_for("hot").inverted.find(acc, "hot"));
        EXPECT_EQ(acc->second.get(), before);       // 同一对象（原地追加）
        EXPECT_EQ(acc->second->size(), 3u);
    }
}

// P2-min 回归：phrase 读者（零拷贝持引用）× 单写者同 term 追加并发。
// 读者持引用期间写者每次 add_doc 都触发 CoW；读者的快照自洽（短语命中数
// 不超过其取引用时刻的发布量）。TSan 构建下验证无 data race。
TEST(InvertedIndex, PhraseSearchConcurrentWithSingleWriter) {
    class AllLive : public LiveChecker {
    public:
        [[nodiscard]] bool is_live(std::uint64_t) const override { return true; }
        [[nodiscard]] std::uint32_t doc_len(std::uint64_t) const override { return 2; }
    };

    InvertedIndex idx;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        idx.add_doc(i, {{"p0", tp(1, {0})}, {"p1", tp(1, {1})}});
    }

    AllLive checker;
    std::atomic<bool> stop{false};
    std::atomic<bool> bad{false};

    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto res = idx.search_phrase({"p0", "p1"}, 5, checker);
                if (res.size() > 5) { bad.store(true); return; }
                for (auto& h : res) {
                    if (h.ord >= 3000) { bad.store(true); return; }
                }
            }
        });
    }

    for (std::uint64_t i = 1000; i < 3000; ++i) {
        idx.add_doc(i, {{"p0", tp(1, {0})}, {"p1", tp(1, {1})}});
    }
    stop.store(true);
    for (auto& t : readers) t.join();

    EXPECT_FALSE(bad.load());
    auto final_res = idx.search_phrase({"p0", "p1"}, 5, checker);
    EXPECT_EQ(final_res.size(), 5u);
}

// =========================================================================
// intersect_u64（Inoue 块过滤 + u64 AVX2 / galloping / 标量）黑盒对拍
// =========================================================================

#include "bitcask/intersect.hpp"

namespace {
std::vector<std::uint64_t> ref_intersect(const std::vector<std::uint64_t>& a,
                                         const std::vector<std::uint64_t>& b) {
    std::vector<std::uint64_t> r;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                          std::back_inserter(r));
    return r;
}
std::vector<std::uint64_t> make_sorted_unique(std::uint64_t& seed, std::size_t n,
                                              std::uint64_t value_range,
                                              std::uint64_t base = 0) {
    auto next = [&seed] {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return seed >> 33;
    };
    std::vector<std::uint64_t> v(n);
    for (auto& x : v) x = base + static_cast<std::uint64_t>(next() % value_range);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}
}  // namespace

TEST(IntersectU64, AgreesWithSetIntersectionRandomized) {
    std::uint64_t seed = 7;
    std::vector<std::uint64_t> out;
    const std::size_t sizes[] = {0, 1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 63, 64, 200, 777};
    for (auto na : sizes) {
        for (auto nb : sizes) {
            for (std::uint64_t range : {50ULL, 1000ULL, 1000000ULL}) {
                auto a = make_sorted_unique(seed, na, range);
                auto b = make_sorted_unique(seed, nb, range);
                intersect_u64(a, b, out);
                ASSERT_EQ(out, ref_intersect(a, b))
                    << "na=" << na << " nb=" << nb << " range=" << range;
            }
        }
    }
}

TEST(IntersectU64, HighValuesAcross232Boundary) {
    std::uint64_t seed = 42;
    std::vector<std::uint64_t> out;
    const std::uint64_t bases[] = {0ULL, 0xFFFFFF00ULL, 5'000'000'000ULL,
                                   0x7FFFFFFFFFFFFF00ULL};
    const std::size_t sizes[] = {0, 1, 3, 4, 5, 8, 9, 64, 200};
    for (auto base : bases) {
        for (auto na : sizes) {
            for (auto nb : sizes) {
                auto a = make_sorted_unique(seed, na, 1000, base);
                auto b = make_sorted_unique(seed, nb, 1000, base);
                intersect_u64(a, b, out);
                ASSERT_EQ(out, ref_intersect(a, b))
                    << "base=" << base << " na=" << na << " nb=" << nb;
            }
        }
    }
}

TEST(IntersectU64, Low32BitCollision) {
    std::vector<std::uint64_t> a, b, out;
    for (std::uint64_t k = 0; k < 64; ++k) a.push_back((1ULL << 32) | (k * 7));
    for (std::uint64_t k = 0; k < 64; ++k) b.push_back((2ULL << 32) | (k * 7));
    intersect_u64(a, b, out);
    EXPECT_TRUE(out.empty());
    intersect_u64(a, a, out);
    EXPECT_EQ(out, a);
}

TEST(IntersectU64, GallopingPathSkewed) {
    std::uint64_t seed = 99;
    std::vector<std::uint64_t> out;
    auto small_v = make_sorted_unique(seed, 20, 100000);
    auto large_v = make_sorted_unique(seed, 5000, 100000);
    intersect_u64(small_v, large_v, out);
    EXPECT_EQ(out, ref_intersect(small_v, large_v));
    intersect_u64(large_v, small_v, out);
    EXPECT_EQ(out, ref_intersect(small_v, large_v));
}

TEST(IntersectU64, FullAndNoOverlap) {
    std::vector<std::uint64_t> a, out;
    for (std::uint64_t i = 0; i < 1000; ++i) a.push_back(i * 2);
    intersect_u64(a, a, out);
    EXPECT_EQ(out, a);
    std::vector<std::uint64_t> b;
    for (std::uint64_t i = 0; i < 1000; ++i) b.push_back(i * 2 + 1);
    intersect_u64(a, b, out);
    EXPECT_TRUE(out.empty());
}

// ── K1:k-way 交集专项(run_must_intersect 从 pairwise 改 leapfrog)──────

// 三词 MUST:A=全部,B=偶数,C=3 的倍数 → 交集 = 6 的倍数。
TEST(InvertedIndex, BoolMustKwayThreeTerms) {
    InvertedIndex idx;
    FakeLiveChecker checker;
    for (std::uint64_t d = 0; d < 100; ++d) {
        TermPositions m;
        m.emplace("aaa", tp(1, {0}));
        if (d % 2 == 0) m.emplace("bbb", tp(1, {1}));
        if (d % 3 == 0) m.emplace("ccc", tp(1, {2}));
        idx.add_doc(d, m);
        checker.doc_lens[d] = 3;
    }

    auto node = parse_query("+aaa +bbb +ccc");
    auto results = idx.bool_search(node, 100, checker);
    ASSERT_EQ(results.size(), 17u);  // 0,6,...,96
    for (auto& r : results) {
        EXPECT_EQ(r.ord % 6, 0u) << r.ord;
    }
}

// 三词 MUST + 删除:交集内被删的文档必须被 liveness 全检排除。
TEST(InvertedIndex, BoolMustKwayDeletedExcluded) {
    InvertedIndex idx;
    FakeLiveChecker checker;
    for (std::uint64_t d = 0; d < 60; ++d) {
        TermPositions m;
        m.emplace("aaa", tp(1, {0}));
        m.emplace("bbb", tp(1, {1}));
        if (d % 2 == 0) m.emplace("ccc", tp(1, {2}));
        idx.add_doc(d, m);
        checker.doc_lens[d] = 3;
    }
    // 删掉交集(偶数)里的 0、6、12(FakeLiveChecker:不在 doc_lens 即死)。
    checker.doc_lens.erase(0);
    checker.doc_lens.erase(6);
    checker.doc_lens.erase(12);

    auto node = parse_query("+aaa +bbb +ccc");
    auto results = idx.bool_search(node, 100, checker);
    ASSERT_EQ(results.size(), 27u);  // 30 个偶数 - 3 个被删
    for (auto& r : results) {
        EXPECT_NE(r.ord, 0u);
        EXPECT_NE(r.ord, 6u);
        EXPECT_NE(r.ord, 12u);
    }
}

// 极不对称:冷词(5 docs)∩ 热词(5000 docs)——驱动游标走冷词,
// 热词侧全靠 galloping advance。
TEST(InvertedIndex, BoolMustKwayAsymmetric) {
    InvertedIndex idx;
    FakeLiveChecker checker;
    for (std::uint64_t d = 0; d < 5000; ++d) {
        TermPositions m;
        m.emplace("hot", tp(1, {0}));
        if (d % 1000 == 7) m.emplace("rare", tp(1, {1}));  // 7,1007,...,4007
        idx.add_doc(d, m);
        checker.doc_lens[d] = 2;
    }

    auto node = parse_query("+hot +rare");
    auto results = idx.bool_search(node, 100, checker);
    ASSERT_EQ(results.size(), 5u);
    for (auto& r : results) {
        EXPECT_EQ(r.ord % 1000, 7u);
    }
}

// 随机对拍:4 词 MUST,与暴力参考集逐 ord 比对(固定种子可复现)。
TEST(InvertedIndex, BoolMustKwayRandomizedReference) {
    std::mt19937_64 rng(0x5EEDBA5E);
    InvertedIndex idx;
    FakeLiveChecker checker;
    const char* terms[4] = {"t0", "t1", "t2", "t3"};
    std::set<std::uint64_t> reference;

    for (std::uint64_t d = 0; d < 2000; ++d) {
        TermPositions m;
        bool in_all = true;
        for (auto* t : terms) {
            // 各词 60% 概率包含该文档 → 四词交集 ~13%。
            if (rng() % 100 < 60) {
                m.emplace(t, tp(1, {0}));
            } else {
                in_all = false;
            }
        }
        if (m.empty()) m.emplace("filler", tp(1, {0}));
        idx.add_doc(d, m);
        checker.doc_lens[d] = 4;
        if (in_all) reference.insert(d);
    }
    ASSERT_GT(reference.size(), 50u);  // 种子固定,交集非平凡

    auto node = parse_query("+t0 +t1 +t2 +t3");
    auto results = idx.bool_search(node, 5000, checker);
    std::set<std::uint64_t> got;
    for (auto& r : results) got.insert(r.ord);
    EXPECT_EQ(got, reference);
}

// ── B1:must-only 合取 BMW 专项 ──────────────────────────────────────

// 无删除等价性:BMW(+a +b)与原路径(+a +b zzz——不存在的 should 词
// 强制走原路径且不影响分数)逐 ord/score 对比。tf 取变化值避免并列分。
TEST(InvertedIndex, BmwMatchesFallbackNoDeletions) {
    InvertedIndex idx;
    FakeLiveChecker checker;
    std::mt19937_64 rng(0xB1B1);
    for (std::uint64_t d = 0; d < 3000; ++d) {
        TermPositions m;
        if (rng() % 100 < 70) {
            m.emplace("aaa", tp(1 + static_cast<std::uint32_t>(rng() % 7), {0}));
        }
        if (rng() % 100 < 50) {
            m.emplace("bbb", tp(1 + static_cast<std::uint32_t>(rng() % 5), {1}));
        }
        if (m.empty()) m.emplace("filler", tp(1, {0}));
        idx.add_doc(d, m);
        // v5 不变量:checker 的 doc_len 必须等于 add_doc 的 Σtf
        // (生产里 SearchLayer 同源保证;见 LiveChecker 文档)。
        std::uint32_t dl = 0;
        for (auto& [t, pr] : m) dl += pr.first;
        checker.doc_lens[d] = dl;
    }

    for (std::size_t k : {5UL, 37UL, 2000UL}) {
        auto bmw = idx.bool_search(parse_query("+aaa +bbb"), k, checker);
        auto ref = idx.bool_search(parse_query("+aaa +bbb zzz_nonexistent"),
                                   k, checker);
        ASSERT_EQ(bmw.size(), ref.size()) << "k=" << k;
        for (std::size_t i = 0; i < bmw.size(); ++i) {
            EXPECT_EQ(bmw[i].ord, ref[i].ord) << "k=" << k << " i=" << i;
            EXPECT_FLOAT_EQ(bmw[i].score, ref[i].score)
                << "k=" << k << " i=" << i;
        }
    }
}

// 有删除:死文档必须被排除(懒 live 检查),活文档成员集与参考一致
// (k 取满,只断言成员不断言排序——BMW 的 idf 基于 df,删除下与原路径
// 的 live_df idf 有意不同,见设计 §6)。
TEST(InvertedIndex, BmwDeletedDocsExcluded) {
    InvertedIndex idx;
    FakeLiveChecker checker;
    std::set<std::uint64_t> expect;
    for (std::uint64_t d = 0; d < 1000; ++d) {
        TermPositions m;
        m.emplace("aaa", tp(1, {0}));
        if (d % 3 == 0) m.emplace("bbb", tp(2, {1}));
        idx.add_doc(d, m);
        checker.doc_lens[d] = (d % 3 == 0) ? 3 : 1;  // = Σtf(v5 不变量)
        if (d % 3 == 0) expect.insert(d);
    }
    // 删掉交集里的每第 5 个(0,15,30,...)。
    for (std::uint64_t d = 0; d < 1000; d += 15) {
        checker.doc_lens.erase(d);
        expect.erase(d);
    }

    auto results = idx.bool_search(parse_query("+aaa +bbb"), 1000, checker);
    std::set<std::uint64_t> got;
    for (auto& r : results) got.insert(r.ord);
    EXPECT_EQ(got, expect);
}

// 剪枝正确性:高分文档藏在列表深处(后段块的高 tf),小 k 下 BMW 的块
// 跳跃不允许漏掉它们——与原路径 top-k 集逐一对比。
TEST(InvertedIndex, BmwHighScoreInLateBlocksNotPruned) {
    InvertedIndex idx;
    FakeLiveChecker checker;
    for (std::uint64_t d = 0; d < 5000; ++d) {
        // 大部分 tf=1;每 997 个一个 tf=50 的"高分钉子",分散在各块。
        const std::uint32_t tf_a = (d % 997 == 0) ? 50 : 1;
        const std::uint32_t tf_b = (d % 991 == 0) ? 40 : 1;
        TermPositions m;
        m.emplace("aaa", tp(tf_a, {0}));
        m.emplace("bbb", tp(tf_b, {1}));
        idx.add_doc(d, m);
        checker.doc_lens[d] = tf_a + tf_b;  // = Σtf(v5 不变量)
    }

    auto bmw = idx.bool_search(parse_query("+aaa +bbb"), 10, checker);
    auto ref = idx.bool_search(parse_query("+aaa +bbb zzz_nonexistent"),
                               10, checker);
    ASSERT_EQ(bmw.size(), ref.size());
    for (std::size_t i = 0; i < bmw.size(); ++i) {
        EXPECT_EQ(bmw[i].ord, ref[i].ord) << i;
        EXPECT_FLOAT_EQ(bmw[i].score, ref[i].score) << i;
    }
}

// v5 impacts:封块记录块内最小索引时 doc_len;save/load round-trip 保留。
TEST(InvertedIndex, V5BlockMinDlTrackedAndPersisted) {
    InvertedIndex idx;
    // 200 docs → alpha 列表 1 个满块(128)+ 尾巴。doc_len = Σtf 受控:
    // d<128 块内 dl ∈ {5,9}(min=5);其余 dl=7。
    for (std::uint64_t d = 0; d < 200; ++d) {
        TermPositions m;
        if (d < 128) {
            m.emplace("alpha", tp(2, {0}));
            m.emplace("pad", tp((d % 2 == 0) ? 3u : 7u, {1}));  // Σtf = 5 或 9
        } else {
            m.emplace("alpha", tp(3, {0}));
            m.emplace("pad", tp(4, {1}));                       // Σtf = 7
        }
        idx.add_doc(d, m);
    }

    auto tmp = std::filesystem::temp_directory_path() / "inv_v5_mindl.snap";
    std::filesystem::remove(tmp);
    ASSERT_TRUE(idx.save(tmp.string()));

    InvertedIndex idx2;
    ASSERT_TRUE(idx2.load(tmp.string()));

    // 经查询路径间接验证不可行(min_dl 不外露),直接检查快照重载后的
    // 块元数据:save 前 finalize 会重建块(满块+尾块)。
    // 通过再 save 一次比较字节一致性来确认 round-trip 无损。
    auto tmp2 = std::filesystem::temp_directory_path() / "inv_v5_mindl2.snap";
    std::filesystem::remove(tmp2);
    ASSERT_TRUE(idx2.save(tmp2.string()));

    // S37-6：两个 ifstream 必须在 remove 之前析构。POSIX 下 unlink 一个仍
    // 打开的文件合法，故原来把它们留到函数末尾也无妨；**MSVC 的 ifstream
    // 走 CRT 的 _SH_DENYNO，共享位不含 FILE_SHARE_DELETE**，于是下面的
    // remove 抛 ERROR_SHARING_VIOLATION。加一层作用域即可，顺带也是更好的
    // 测试卫生（读完就关）。
    std::vector<char> b1, b2;
    {
        std::ifstream f1(tmp, std::ios::binary), f2(tmp2, std::ios::binary);
        b1.assign((std::istreambuf_iterator<char>(f1)),
                  std::istreambuf_iterator<char>());
        b2.assign((std::istreambuf_iterator<char>(f2)),
                  std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(b1, b2);  // load→save 幂等 ⇒ min_dl 等块字段全数保留

    std::filesystem::remove(tmp);
    std::filesystem::remove(tmp2);
}

// A1:WAND 块跳跃修复(remaining_needed 死代码)后的安全网——
// 小 k(剪枝激进)的 top-k 必须与大 k(堆不满,零剪枝)的前缀一致。
// BM25 分数只取决于 (tf, dl) 离散对,随机取值会大量并列(首版测试
// 因此误报"丢结果"——实为并列层内的合法自由度);这里构造 (tf, dl)
// 与 d 双射使分数几乎唯一,并对仍可能的同分位放宽 ord 断言。
TEST(InvertedIndex, WandSkipTopKEqualsUnprunedPrefix) {
    InvertedIndex idx;
    FakeLiveChecker checker;
    for (std::uint64_t d = 0; d < 4000; ++d) {
        const auto tf = 1 + static_cast<std::uint32_t>(d % 50);
        const auto pad = 1 + static_cast<std::uint32_t>(d / 50);
        TermPositions m;
        m.emplace("hot", tp(tf, {0}));
        m.emplace("pad", tp(pad, {1}));
        idx.add_doc(d, m);
        checker.doc_lens[d] = tf + pad;  // v5 不变量:= Σtf
    }

    // k=4000 → 堆永不满 → threshold 恒 0 → 块跳跃不触发 → 全量评分,
    // 其排序前缀即真值。
    auto full = idx.search({"hot"}, 4000, checker);
    ASSERT_EQ(full.size(), 4000u);

    for (std::size_t k : {1UL, 5UL, 10UL, 100UL}) {
        auto pruned = idx.search({"hot"}, k, checker);
        ASSERT_EQ(pruned.size(), k) << "k=" << k;
        for (std::size_t i = 0; i < k; ++i) {
            EXPECT_FLOAT_EQ(pruned[i].score, full[i].score)
                << "k=" << k << " i=" << i;
            // 同分层内成员可互换,只在该位分数唯一时断言 ord。
            const bool tie_above =
                i > 0 && full[i].score == full[i - 1].score;
            const bool tie_below =
                i + 1 < full.size() && full[i].score == full[i + 1].score;
            if (!tie_above && !tie_below) {
                EXPECT_EQ(pruned[i].ord, full[i].ord)
                    << "k=" << k << " i=" << i;
            }
        }
    }
}

// ===========================================================================
// V6.3.1：排序词典侧表懒重建回归
// ===========================================================================
//
// 验证 lazy rebuild 协议的核心契约：
//   ① ensure_vocab 在 dirty 时重建，清 dirty，发布新 shared_ptr
//   ② add_doc 新增 term 后，相应 shard.vocab_dirty_ 被置 true
//   ③ 旧 term 不触发 dirty（is_new_term=false → 不写）
//   ④ load 后所有 shard 的 vocab_dirty_ 为 true（首搜重建）
//   ⑤ 搜索路径吃 ensure_vocab → 跨 add_doc 边界能找到新 term

TEST(InvertedIndex, SortedVocabSidecarRebuilds) {
    InvertedIndex idx;

    // 起始：所有 shard vocab_dirty_ = true（Shard 默认构造）。
    for (std::size_t s = 0; s < 64; ++s) {
        EXPECT_TRUE(idx.shard_for("dummy_" + std::to_string(s)).vocab_dirty_.load())
            << "shard=" << s << " 起始应为 dirty";
    }

    // 加 3 个 term，hash 后大概率落在 3 个不同 shard（"alpha"/"beta"/"gamma"）。
    idx.add_doc(0, {{"alpha", tp(1, {0})}});
    idx.add_doc(1, {{"beta",  tp(1, {0})}});
    idx.add_doc(2, {{"gamma", tp(1, {0})}});

    // 持有"alpha"/"beta"/"gamma"的 shard 必被标脏；其它 shard 仍为 dirty=true
    // （因为默认构造就是 dirty）。我们断言持有 alpha 的 shard 被标脏。
    EXPECT_TRUE(idx.shard_for("alpha").vocab_dirty_.load())
        << "持有 'alpha' 的 shard 应被标脏";

    FakeLiveChecker checker;
    checker.doc_lens[0] = 1;
    checker.doc_lens[1] = 1;
    checker.doc_lens[2] = 1;
    // 跑 fuzzy 命中 alpha → 持有 alpha 的 shard 走 dirty 路径 rebuild + clean。
    auto warmup = idx.search_fuzzy({"alpha"}, 10, 1, checker);
    ASSERT_EQ(warmup.size(), 1u);
    EXPECT_FALSE(idx.shard_for("alpha").vocab_dirty_.load())
        << "持有 'alpha' 的 shard 在首搜后应 clean";

    // 旧 term 再 add → 不该标脏（is_new_term=false 路径）。此时 alpha 所在
    // shard 已 clean；add_doc 不应再标脏。
    idx.add_doc(3, {{"alpha", tp(1, {0})}});
    EXPECT_FALSE(idx.shard_for("alpha").vocab_dirty_.load())
        << "旧 term 'alpha' add_doc 后所在 shard 不应被重标脏";

    // 新 term add → 持有该 term 的 shard 必 dirty。
    idx.add_doc(4, {{"delta", tp(1, {0})}});
    EXPECT_TRUE(idx.shard_for("delta").vocab_dirty_.load())
        << "新 term 'delta' 所在 shard 必被标脏";

    // 搜索应能找到 delta（走 lazy rebuild 路径）。需要 doc 3、doc 4 在 live 表中。
    checker.doc_lens[3] = 1;
    checker.doc_lens[4] = 1;
    auto fuzzy_with_delta = idx.search_fuzzy({"delta"}, 10, 0, checker);
    EXPECT_EQ(fuzzy_with_delta.size(), 1u)
        << "lazy rebuild 后搜索能找到 add_doc 时新增的 term";

    // rebuild 后 delta_shard 再次 clean。
    EXPECT_FALSE(idx.shard_for("delta").vocab_dirty_.load())
        << "lazy rebuild 后 dirty 应被清";

    // 模糊搜索仍能找到 alpha（rebuild 保留旧 key，doc 0 + doc 3）。
    auto fuzzy_alpha = idx.search_fuzzy({"alpha"}, 10, 0, checker);
    EXPECT_EQ(fuzzy_alpha.size(), 2u) << "alpha 应在 doc 0 和 doc 3 各命中一次";
}

// load → fuzzy 跨边界：load 后所有 shard dirty；fuzzy 遍历 64 个 shard
// 触发 ensure_vocab，全部 clean。
TEST(InvertedIndex, SortedVocabSidecarLoadMarksAllDirty) {
    auto tmp = std::filesystem::temp_directory_path() / "inv_vocab_sidecar.inv";
    std::filesystem::remove(tmp);

    {
        InvertedIndex idx;
        idx.add_doc(0, {{"hello", tp(1, {0})}, {"world", tp(1, {1})}});
        EXPECT_TRUE(idx.save(tmp.string()));
    }
    {
        InvertedIndex idx2;
        EXPECT_TRUE(idx2.load(tmp.string()));

        // load 后所有 shard 都应是 dirty。
        for (std::size_t s = 0; s < 64; ++s) {
            EXPECT_TRUE(idx2.shard_for("dummy_" + std::to_string(s)).vocab_dirty_.load())
                << "load 后 shard " << s << " 应为 dirty";
        }

        FakeLiveChecker checker;
        checker.doc_lens[0] = 2;
        // fuzzy 遍历 64 个 shard 触发 ensure_vocab，全部 clean。
        auto results = idx2.search_fuzzy({"hello"}, 10, 0, checker);
        ASSERT_EQ(results.size(), 1u);

        std::size_t clean_count = 0;
        for (std::size_t s = 0; s < 64; ++s) {
            if (!idx2.shard_for("dummy_" + std::to_string(s)).vocab_dirty_.load()) {
                ++clean_count;
            }
        }
        EXPECT_EQ(clean_count, 64u) << "fuzzy 遍历 64 个 shard 应清掉所有 dirty";
    }
    std::filesystem::remove(tmp);
}

// V6.3.1 通配符 binary search 路径：prefix 模式命中走区间 + lit 预过滤 + wildcard。
TEST(InvertedIndex, SortedVocabSidecarWildcardPrefixRebuild) {
    InvertedIndex idx;
    idx.add_doc(0, {{"hello",  tp(1, {0})}});
    idx.add_doc(1, {{"help",   tp(1, {0})}});
    idx.add_doc(2, {{"helmet", tp(1, {0})}});
    idx.add_doc(3, {{"world",  tp(1, {0})}});

    FakeLiveChecker checker;
    checker.doc_lens[0] = 1;
    checker.doc_lens[1] = 1;
    checker.doc_lens[2] = 1;
    checker.doc_lens[3] = 1;

    // 首次搜索触发 lazy rebuild。
    auto r1 = idx.search_wildcard("hel*", 10, checker);
    EXPECT_EQ(r1.size(), 3u) << "hel* 应匹配 hello/help/helmet 三词";

    // 加新 term 走 dirty 路径。
    idx.add_doc(4, {{"helicopter", tp(1, {0})}});
    checker.doc_lens[4] = 1;
    auto r2 = idx.search_wildcard("hel*", 10, checker);
    EXPECT_EQ(r2.size(), 4u) << "lazy rebuild 后应能找到新 term helicopter";

    // 中缀/后缀模式（pattern[0]=='*'）走全扫。
    auto r3 = idx.search_wildcard("*lo", 10, checker);
    EXPECT_EQ(r3.size(), 1u) << "*lo 应匹配 hello（精确）";
    auto r4 = idx.search_wildcard("*lp", 10, checker);
    EXPECT_EQ(r4.size(), 1u) << "*lp 应匹配 help";
}

// V6.3.4：v6 快照 save/load round-trip + 文件头校验。
// 体积对比在生产规模（1M docs）下验证；此处验证格式正确性与 round-trip。
TEST(InvertedIndex, V6SnapshotRoundtripWithPositions) {
    InvertedIndex idx;
    constexpr std::uint64_t kDocs = 500;
    constexpr int kTermsPerDoc = 5;
    for (std::uint64_t d = 0; d < kDocs; ++d) {
        TermPositions tp;
        for (int t = 0; t < kTermsPerDoc; ++t) {
            std::string term = "term" + std::to_string(t);
            std::uint32_t tf = static_cast<std::uint32_t>((d + t) % 3) + 1;
            tp.emplace(term, std::make_pair(tf, std::vector<std::uint32_t>{0}));
        }
        idx.add_doc(d, tp);
    }
    idx.finalize_all_postings();

    auto tmp = std::filesystem::temp_directory_path() / "inv_v6_rt.inv";
    std::filesystem::remove(tmp);
    ASSERT_TRUE(idx.save(tmp.string()));
    EXPECT_GT(std::filesystem::file_size(tmp), 0u);

    // 校验文件头：magic(4B) + version=6(4B)
    {
        std::FILE* f = std::fopen(tmp.string().c_str(), "rb");
        ASSERT_NE(f, nullptr);
        std::uint32_t magic = 0, ver = 0;
        ASSERT_EQ(std::fread(&magic, 4, 1, f), 1u);
        ASSERT_EQ(std::fread(&ver, 4, 1, f), 1u);
        std::fclose(f);
        EXPECT_EQ(magic, 0x494E5632u);
        EXPECT_EQ(ver, 6u);
    }

    InvertedIndex idx2;
    ASSERT_TRUE(idx2.load(tmp.string()));
    FakeLiveChecker checker;
    for (std::uint64_t d = 0; d < kDocs; ++d) {
        checker.doc_lens[d] = kTermsPerDoc;
    }
    auto results = idx2.search({"term0"}, 10, checker);
    EXPECT_EQ(results.size(), 10u);

    std::filesystem::remove(tmp);
}

// ==========================================================================
// S27-2：ExtStats 外部统计注入（G-on-the-fly enabling primitive）
// ==========================================================================

namespace {
// 直接构造一篇文档的 term→(tf, 无 position) 映射。
TermPositions mk_terms(
    std::initializer_list<std::pair<const char*, std::uint32_t>> ts) {
    TermPositions m;
    for (auto& [t, tf] : ts) m[std::string(t)] = {tf, {}};
    return m;
}
}  // namespace

// 无删除时 doc_freq==live_df、self-stats 与本地统计的 N/sum_dl/df 完全相同 →
// idf/avgdl 相同 → 打分逐位一致。标量路径（posting 少，不走 WAND）。
TEST(InvertedIndex, ExtStatsSelfEquivalenceScalar) {
    InvertedIndex idx;
    FakeLiveChecker checker;
    idx.add_doc(0, mk_terms({{"apple", 2}, {"banana", 1}}));
    idx.add_doc(1, mk_terms({{"banana", 1}, {"cherry", 3}}));
    idx.add_doc(2, mk_terms({{"apple", 1}, {"cherry", 1}, {"date", 1}}));
    idx.add_doc(3, mk_terms({{"apple", 1}}));
    checker.doc_lens[0] = 3; checker.doc_lens[1] = 4;
    checker.doc_lens[2] = 3; checker.doc_lens[3] = 1;

    std::vector<std::string> terms = {"apple", "cherry"};
    auto base = idx.search(terms, 10, checker);
    ASSERT_FALSE(base.empty());

    ExtStats ext;
    ext.N = idx.live_doc_count();
    ext.sum_dl = idx.sum_doc_len();
    std::vector<std::pair<std::string, std::uint64_t>> df;  // S29-5：扁平化
    for (auto& t : terms) df.emplace_back(t, idx.doc_freq(t));
    ext.df = &df;
    auto inj = idx.search(terms, 10, checker, nullptr, &ext);

    ASSERT_EQ(base.size(), inj.size());
    for (std::size_t i = 0; i < base.size(); ++i) {
        EXPECT_EQ(base[i].ord, inj[i].ord);
        EXPECT_FLOAT_EQ(base[i].score, inj[i].score);
    }
}

// 同上但 posting 数跨过 kWandThreshold → 走 search_wand，验证 WAND 路径的
// idf 注入也保持等价（注入 idf 同时用于块上界与打分 → 剪枝仍正确）。
TEST(InvertedIndex, ExtStatsSelfEquivalenceWand) {
    InvertedIndex idx;
    FakeLiveChecker checker;
    constexpr std::uint64_t kN = 1200;  // "common" 有 1200 posting > 1024
    for (std::uint64_t i = 0; i < kN; ++i) {
        TermPositions m;
        m["common"] = {static_cast<std::uint32_t>(1 + (i % 7)), {}};
        m["u" + std::to_string(i)] = {1, {}};
        idx.add_doc(i, m);
        checker.doc_lens[i] = 2 + static_cast<std::uint32_t>(i % 3);
    }
    std::vector<std::string> terms = {"common"};
    auto base = idx.search(terms, 20, checker);
    ASSERT_EQ(base.size(), 20u);

    ExtStats ext;
    ext.N = idx.live_doc_count();
    ext.sum_dl = idx.sum_doc_len();
    std::vector<std::pair<std::string, std::uint64_t>> df;  // S29-5：扁平化
    df.emplace_back("common", idx.doc_freq("common"));
    ext.df = &df;
    auto inj = idx.search(terms, 20, checker, nullptr, &ext);

    ASSERT_EQ(base.size(), inj.size());
    for (std::size_t i = 0; i < base.size(); ++i) {
        EXPECT_EQ(base[i].ord, inj[i].ord);
        EXPECT_FLOAT_EQ(base[i].score, inj[i].score);
    }
}

// 注入不同的全局统计确实改变 idf → 证明注入被真正采纳（非静默忽略）。
TEST(InvertedIndex, ExtStatsInjectionAffectsScore) {
    InvertedIndex idx;
    FakeLiveChecker checker;
    idx.add_doc(0, mk_terms({{"apple", 2}}));
    idx.add_doc(1, mk_terms({{"apple", 1}}));
    checker.doc_lens[0] = 2; checker.doc_lens[1] = 1;

    std::vector<std::string> terms = {"apple"};
    auto base = idx.search(terms, 10, checker);  // 本地：N=2, df=2 → idf 很小
    ASSERT_EQ(base.size(), 2u);

    // 只注入更小的 df，N/sum_dl 保持本地 → avgdl 不变、隔离 idf 效应。
    // base: df=2,N=2 → idf=log(1+0.5/2.5)≈0.182；inj: df=1,N=2 → idf=log(2)≈0.693。
    ExtStats ext;
    ext.N = idx.live_doc_count();
    ext.sum_dl = idx.sum_doc_len();
    std::vector<std::pair<std::string, std::uint64_t>> df;  // S29-5：扁平化
    df.emplace_back("apple", 1);
    ext.df = &df;
    auto inj = idx.search(terms, 10, checker, nullptr, &ext);

    ASSERT_EQ(inj.size(), 2u);
    EXPECT_EQ(base[0].ord, inj[0].ord);          // 同文档集 → 排序不变
    EXPECT_GT(inj[0].score, base[0].score);      // idf 抬升 → 分更高
}

// ==========================================================================
// S27-2 Slice 2：多段查询归并（§3.5）+ G-on-the-fly 单索引等价
// ==========================================================================

// 同一批文档：① 全量放进单索引 whole；② 均分两段（各自本地 docid）。
// multi_segment_search（内部 G-on-the-fly）应与 whole.search 逐位同 key 同分。
// 反证：段本地统计（ext=nullptr）的 idf 与单索引不同 → 分数不一致（证明必要性）。
TEST(InvertedIndex, SegmentMergeEquivalence) {
    using bitcask::search::SegmentView;
    using bitcask::search::multi_segment_search;
    constexpr std::uint64_t kNAll = 8;
    auto keyname = [](std::uint64_t g) { return "doc" + std::to_string(g); };

    // whole：单索引，全局 docid 0..7，"q" 的 tf 各异（→ 分数互异，无并列）。
    InvertedIndex whole;
    FakeLiveChecker wholeChk;
    std::vector<std::string> wholeKeys(kNAll);
    for (std::uint64_t g = 0; g < kNAll; ++g) {
        TermPositions m;
        m["q"] = {static_cast<std::uint32_t>(g + 1), {}};
        whole.add_doc(g, m);
        wholeChk.doc_lens[g] = 10;
        wholeKeys[g] = keyname(g);
    }

    // 两段：A=前 4（本地 docid 0..3），B=后 4（本地 docid 0..3）。
    InvertedIndex segA, segB;
    FakeLiveChecker chkA, chkB;
    std::vector<std::string> keysA(4), keysB(4);
    std::vector<bitcask::Lsn> lsnA(4), lsnB(4);
    for (std::uint64_t g = 0; g < kNAll; ++g) {
        TermPositions m;
        m["q"] = {static_cast<std::uint32_t>(g + 1), {}};
        if (g < 4) {
            segA.add_doc(g, m);
            chkA.doc_lens[g] = 10;
            keysA[g] = keyname(g);
            lsnA[g] = g;
        } else {
            const std::uint64_t l = g - 4;
            segB.add_doc(l, m);
            chkB.doc_lens[l] = 10;
            keysB[l] = keyname(g);
            lsnB[l] = g;
        }
    }

    std::vector<SegmentView> segs = {
        SegmentView{&segA, &chkA,
                    [&](bitcask::DocId d) { return keysA[d]; },
                    [&](bitcask::DocId d) { return lsnA[d]; }},
        SegmentView{&segB, &chkB,
                    [&](bitcask::DocId d) { return keysB[d]; },
                    [&](bitcask::DocId d) { return lsnB[d]; }},
    };

    std::vector<std::string> terms = {"q"};
    const std::size_t k = kNAll;  // 全量 → 无 top-k 边界并列问题

    auto base = whole.search(terms, k, wholeChk);
    ASSERT_EQ(base.size(), kNAll);
    auto merged = multi_segment_search(segs, terms, k);
    ASSERT_EQ(merged.size(), kNAll);

    // 逐位：同 key 序 + 同分（G-on-the-fly → idf/avgdl 与单索引一致）。
    for (std::size_t i = 0; i < base.size(); ++i) {
        EXPECT_EQ(wholeKeys[base[i].ord], merged[i].key) << "i=" << i;
        EXPECT_FLOAT_EQ(static_cast<float>(base[i].score),
                        static_cast<float>(merged[i].score))
            << "i=" << i;
        EXPECT_EQ(merged[i].ord, base[i].ord);  // lsn_of 还原全局 LSN
    }

    // 必要性反证：段 A 用本地统计（ext=nullptr）→ df=4/N=4，idf 与单索引 df=8/N=8
    // 不同 → 同一 doc 分数不一致。这正是需要 G-on-the-fly 的原因。
    auto localA = segA.search(terms, 4, chkA);
    double whole_doc0 = 0.0, localA_doc0 = 0.0;
    for (auto& r : base) if (r.ord == 0) whole_doc0 = r.score;
    for (auto& r : localA) if (r.ord == 0) localA_doc0 = r.score;
    EXPECT_NE(static_cast<float>(whole_doc0), static_cast<float>(localA_doc0));
}

// ==========================================================================
// S27-2 Slice 3：SealedSegment 落盘 round-trip（复用 SearchCheckpoint 段级 CRC）
// ==========================================================================

TEST(InvertedIndex, SealedSegmentRoundTrip) {
    using bitcask::search::SealedSegment;
    using bitcask::search::SegmentView;
    using bitcask::search::multi_segment_search;

    auto mk = [](std::initializer_list<std::pair<const char*, std::uint32_t>> ts) {
        TermPositions m;
        for (auto& [t, tf] : ts) m[std::string(t)] = {tf, {}};
        return m;
    };
    auto mem = std::make_unique<SealedSegment>();
    mem->add("doc0", 100, mk({{"apple", 2}, {"banana", 1}}));
    mem->add("doc1", 101, mk({{"banana", 1}, {"cherry", 3}}));
    mem->add("doc2", 102, mk({{"apple", 1}, {"cherry", 1}}));
    mem->add("doc3", 103, mk({{"apple", 3}}));
    ASSERT_EQ(mem->doc_count(), 4u);

    std::vector<std::string> terms = {"apple", "cherry"};
    std::vector<SegmentView> memSegs = {mem->view()};
    auto base = multi_segment_search(memSegs, terms, 10);
    ASSERT_FALSE(base.empty());

    auto path =
        (std::filesystem::temp_directory_path() / "bitcask_seg_rt.seg").string();
    std::filesystem::remove(path);
    ASSERT_TRUE(mem->save(path, /*watermark=*/104));

    auto loaded = SealedSegment::load(path);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->doc_count(), 4u);
    EXPECT_EQ(loaded->key_at(0), "doc0");
    EXPECT_EQ(loaded->lsn_at(3), 103u);

    std::vector<SegmentView> ldSegs = {loaded->view()};
    auto after = multi_segment_search(ldSegs, terms, 10);

    ASSERT_EQ(base.size(), after.size());
    for (std::size_t i = 0; i < base.size(); ++i) {
        EXPECT_EQ(base[i].key, after[i].key) << "i=" << i;
        EXPECT_EQ(base[i].ord, after[i].ord) << "i=" << i;  // lsn 还原
        EXPECT_FLOAT_EQ(static_cast<float>(base[i].score),
                        static_cast<float>(after[i].score))
            << "i=" << i;
    }
    std::filesystem::remove(path);
}

// 段级 CRC：篡改文件任一字节 → load 整段拒收（nullptr），退全量重建。
TEST(InvertedIndex, SealedSegmentCrcReject) {
    using bitcask::search::SealedSegment;
    auto seg = std::make_unique<SealedSegment>();
    TermPositions m;
    m["apple"] = {2, {}};
    seg->add("doc0", 7, m);
    seg->add("doc1", 8, m);

    auto path =
        (std::filesystem::temp_directory_path() / "bitcask_seg_crc.seg").string();
    std::filesystem::remove(path);
    ASSERT_TRUE(seg->save(path, 9));

    // 翻转中部一个字节。
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.good());
        f.seekg(0, std::ios::end);
        const auto sz = f.tellg();
        ASSERT_GT(sz, 32);
        const auto pos = sz / 2;
        f.seekg(pos);
        char c = 0;
        f.read(&c, 1);
        c = static_cast<char>(~c);
        f.seekp(pos);
        f.write(&c, 1);
    }
    EXPECT_EQ(SealedSegment::load(path), nullptr);
    std::filesystem::remove(path);
}

// ==========================================================================
// S27-2 Slice 4：SegmentSet 段管理器 / 活跃段清单（原子提交 + 重开持久化）
// ==========================================================================

TEST(InvertedIndex, SegmentSetAddQueryReopen) {
    using bitcask::search::SealedSegment;
    using bitcask::search::SegmentSet;

    const auto dir = (std::filesystem::temp_directory_path() /
                      "bitcask_segset_rt").string();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto set = SegmentSet::open(dir);
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->segment_count(), 0u);  // 首次 open：空集

    auto s1 = std::make_unique<SealedSegment>();
    s1->add("a", 1, mk_terms({{"apple", 2}}));
    s1->add("b", 2, mk_terms({{"banana", 1}}));
    ASSERT_TRUE(set->add(std::move(s1), /*hi_lsn=*/3));

    auto s2 = std::make_unique<SealedSegment>();
    s2->add("c", 3, mk_terms({{"apple", 1}, {"cherry", 1}}));
    ASSERT_TRUE(set->add(std::move(s2), /*hi_lsn=*/4));

    EXPECT_EQ(set->segment_count(), 2u);
    EXPECT_EQ(set->total_docs(), 3u);

    auto r = set->search({"apple"}, 10);  // apple 在 a(seg1) + c(seg2)
    ASSERT_EQ(r.size(), 2u);

    // 重开：读 manifest → 载入两段 → 查询逐位一致（持久化 + 跨段 G-on-the-fly）。
    auto set2 = SegmentSet::open(dir);
    ASSERT_NE(set2, nullptr);
    EXPECT_EQ(set2->segment_count(), 2u);
    EXPECT_EQ(set2->total_docs(), 3u);
    auto r2 = set2->search({"apple"}, 10);
    ASSERT_EQ(r2.size(), r.size());
    for (std::size_t i = 0; i < r.size(); ++i) {
        EXPECT_EQ(r[i].key, r2[i].key) << "i=" << i;
        EXPECT_EQ(r[i].ord, r2[i].ord) << "i=" << i;
        EXPECT_FLOAT_EQ(static_cast<float>(r[i].score),
                        static_cast<float>(r2[i].score)) << "i=" << i;
    }
    std::filesystem::remove_all(dir);
}

TEST(InvertedIndex, SegmentSetDrop) {
    using bitcask::search::SealedSegment;
    using bitcask::search::SegmentSet;

    const auto dir = (std::filesystem::temp_directory_path() /
                      "bitcask_segset_drop").string();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto set = SegmentSet::open(dir);
    ASSERT_NE(set, nullptr);
    auto s1 = std::make_unique<SealedSegment>();  // seg_id 0
    s1->add("a", 1, mk_terms({{"apple", 2}}));
    ASSERT_TRUE(set->add(std::move(s1), 2));
    auto s2 = std::make_unique<SealedSegment>();  // seg_id 1
    s2->add("c", 3, mk_terms({{"apple", 1}}));
    ASSERT_TRUE(set->add(std::move(s2), 4));
    ASSERT_EQ(set->search({"apple"}, 10).size(), 2u);

    ASSERT_TRUE(set->drop(0));                     // 删 seg1（含 "a"）
    EXPECT_EQ(set->segment_count(), 1u);
    auto r = set->search({"apple"}, 10);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].key, "c");

    // 重开后 drop 仍生效（manifest 已原子提交）。
    auto set2 = SegmentSet::open(dir);
    ASSERT_NE(set2, nullptr);
    EXPECT_EQ(set2->segment_count(), 1u);
    EXPECT_EQ(set2->search({"apple"}, 10).size(), 1u);
    // next_seg_id 单调（不因 drop 回退）→ 新段不会撞已删 seg_id。
    EXPECT_EQ(set2->next_seg_id(), 2u);
    std::filesystem::remove_all(dir);
}

// ==========================================================================
// S27-3 Slice A：SealedSegment 多字段扩展（round-trip + 字段数/内容一致）
// ==========================================================================

// 多字段段：title + body 两个字段。add() 多字段重载；save→load→multi_view→
// multi_field_segment_search 与内存基线逐位一致。验证 ① 字段数 ② 每字段
// doc_count ③ posting 内容 ④ 篡改任一字段段字节 → CRC 拒收。
TEST(InvertedIndex, SealedSegmentMultiFieldRoundTrip) {
    using bitcask::search::SealedSegment;
    using bitcask::search::MultiFieldSegmentView;
    using bitcask::search::FieldSegmentView;
    using bitcask::search::multi_field_segment_search;
    using bitcask::search::kDefaultField;

    auto mk = [](std::initializer_list<std::pair<const char*, std::uint32_t>> ts) {
        TermPositions m;
        for (auto& [t, tf] : ts) m[std::string(t)] = {tf, {}};
        return m;
    };

    auto mem = std::make_unique<SealedSegment>();
    // 5 文档，每篇：默认字段 + title + body 三处。total_doc_len = Σ 各字段。
    // doc0: 默认=apple, title=rust, body=memory
    // doc1: 默认=apple, title=memory, body=rust
    // doc2: 默认=apple, title=garbage collection, body=systems
    // doc3: 默认=banana, title=memory, body=garbage
    // doc4: 默认=apple, title=systems, body=memory
    struct Doc {
        std::string key;
        std::uint64_t lsn;
        TermPositions def, title, body;
        std::uint32_t total;
    };
    std::vector<Doc> docs = {
        {"d0", 100, mk({{"apple", 1}}), mk({{"rust", 1}}), mk({{"memory", 1}}), 3},
        {"d1", 101, mk({{"apple", 1}}), mk({{"memory", 1}}), mk({{"rust", 1}}), 3},
        {"d2", 102, mk({{"apple", 1}}),
                    mk({{"garbage", 1}, {"collection", 1}}),
                    mk({{"systems", 1}}), 5},
        {"d3", 103, mk({{"banana", 1}}), mk({{"memory", 1}}),
                    mk({{"garbage", 1}}), 3},
        {"d4", 104, mk({{"apple", 1}}), mk({{"systems", 1}}),
                    mk({{"memory", 1}}), 3},
    };
    for (const auto& d : docs) {
        std::array<SealedSegment::FieldInput, 3> fs{{
            {kDefaultField, &d.def},
            {"title", &d.title},
            {"body",  &d.body},
        }};
        mem->add(d.key, d.lsn, std::span<const SealedSegment::FieldInput>(fs),
                 d.total);
    }
    ASSERT_EQ(mem->doc_count(), 5u);
    // 命名字段已建立（与默认字段并列共 3 个：默认 / title / body）。
    EXPECT_NE(mem->field_index("title"), nullptr);
    EXPECT_NE(mem->field_index("body"), nullptr);
    EXPECT_EQ(mem->field_index("title")->live_doc_count(), 5u);
    EXPECT_EQ(mem->field_index("body")->live_doc_count(), 5u);

    // 内存基线查询：title:rust body:memory → d0 (rust+memory) + d1 (memory+rust)
    // + d4 (memory in body) 命中；d0/d1 各双字段命中 → 分数累加。
    std::unordered_map<std::string, std::vector<std::string>> field_terms{
        {"title", {"rust"}},
        {"body",  {"memory"}},
    };
    std::vector<MultiFieldSegmentView> memSegs = {mem->multi_view()};
    auto base = multi_field_segment_search(memSegs, field_terms, 10);
    ASSERT_FALSE(base.empty());

    // 落盘 → 重载 → 内容一致。
    auto path = (std::filesystem::temp_directory_path() /
                 "bitcask_seg_mf_rt.seg").string();
    std::filesystem::remove(path);
    ASSERT_TRUE(mem->save(path, /*watermark=*/105));

    auto loaded = SealedSegment::load(path);
    ASSERT_NE(loaded, nullptr) << "load failed for " << path;
    EXPECT_EQ(loaded->doc_count(), 5u);
    EXPECT_EQ(loaded->key_at(0), "d0");
    EXPECT_EQ(loaded->key_at(4), "d4");
    EXPECT_EQ(loaded->lsn_at(2), 102u);
    // 字段倒排 round-trip：每字段 doc_count 与 posting 一致。
    ASSERT_NE(loaded->field_index("title"), nullptr);
    ASSERT_NE(loaded->field_index("body"), nullptr);
    EXPECT_EQ(loaded->field_index("title")->live_doc_count(), 5u);
    EXPECT_EQ(loaded->field_index("body")->live_doc_count(), 5u);
    // posting 抽样：title 字段 "rust" 仅在 d0（本地 docid=0）；title 字段
    // "memory" 在 d1 / d3 两篇（docid 1, 3）。body 字段 "memory" 在 d0 / d4
    // 两篇（docid 0, 4）。body 字段 "systems" 仅在 d2。
    auto* ti = loaded->field_index("title");
    auto* bi = loaded->field_index("body");
    EXPECT_EQ(ti->doc_freq("rust"), 1u);     // 仅 d0
    EXPECT_EQ(ti->doc_freq("memory"), 2u);   // d1, d3
    EXPECT_EQ(bi->doc_freq("memory"), 2u);   // d0, d4
    EXPECT_EQ(bi->doc_freq("systems"), 1u);  // d2

    // 加载后查询与内存基线逐位一致。
    std::vector<MultiFieldSegmentView> ldSegs = {loaded->multi_view()};
    auto after = multi_field_segment_search(ldSegs, field_terms, 10);
    ASSERT_EQ(base.size(), after.size());
    for (std::size_t i = 0; i < base.size(); ++i) {
        EXPECT_EQ(base[i].key, after[i].key) << "i=" << i;
        EXPECT_EQ(base[i].ord, after[i].ord) << "i=" << i;
        EXPECT_FLOAT_EQ(static_cast<float>(base[i].score),
                        static_cast<float>(after[i].score))
            << "i=" << i;
    }

    // 段级 CRC：篡改文件任一字节 → load 拒收（多字段段在 kSegFields 内的字节
    // 同样受 CRC 保护；切中部任一位置都会让 CRC 失败）。
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.good());
        f.seekg(0, std::ios::end);
        const auto sz = f.tellg();
        ASSERT_GT(sz, 64);
        // 选 file 中部（kSegFields 段内位置）翻转一个字节。
        const auto pos = sz * 3 / 4;
        f.seekg(pos);
        char c = 0;
        f.read(&c, 1);
        c = static_cast<char>(~c);
        f.seekp(pos);
        f.write(&c, 1);
    }
    EXPECT_EQ(SealedSegment::load(path), nullptr);
    std::filesystem::remove(path);
}

// ==========================================================================
// S27-3 Slice A：跨段跨字段 BM25 累加归并，与单索引多字段算法逐位等价
// ==========================================================================
//
// 同 8 篇文档（title + body 两个字段）均分两段构建。
// ① multi_field_segment_search（跨段 G-on-the-fly + 跨字段累加）
// ② 对照基线：把同样 8 篇文档喂「单 InvertedIndex 每字段」的合成索引，用与
//    TextPlugin::search_fields 同构的算法（每字段 search + acc[ord]+=score），
//    doc_len 用 LiveChecker 集中维护 → 这是单索引下 TextPlugin 的行为上界。
// 两路 hit 数、key、score 应逐位一致（证明 G-on-the-fly 跨段聚合与单字段单
// 索引聚合在 BM25 dl/idf 上语义等价）。
// 反证：让段本地统计（ext=nullptr）→ idf 用段本地 df/N → 与基线全局统计不
// 同 → 分数不一致（证 G-on-the-fly 必要性，与 SegmentMergeEquivalence 同款）。
TEST(InvertedIndex, MultiFieldSegmentMergeEquivalence) {
    using bitcask::search::SealedSegment;
    using bitcask::search::MultiFieldSegmentView;
    using bitcask::search::multi_field_segment_search;
    using bitcask::search::kDefaultField;
    using bitcask::search::SearchHit;

    constexpr std::uint64_t kNAll = 8;
    // 8 篇文档：d0..d3 入 segA；d4..d7 入 segB。title/body 各含若干词。
    // 用 rust/memory/garbage/collection/systems 五个 term，df 各异 → 分数互异。
    const char* titles[kNAll] = {
        "rust rust memory",          // d0: title rust×2, memory×1
        "garbage collection rust",   // d1: title garbage, collection, rust
        "memory garbage",            // d2: title memory, garbage
        "memory systems",            // d3: title memory, systems
        "rust garbage",              // d4: title rust, garbage
        "memory memory memory",      // d5: title memory×3
        "systems rust",              // d6: title systems, rust
        "garbage rust memory",       // d7: title garbage, rust, memory
    };
    const char* bodies[kNAll] = {
        "memory systems garbage",    // d0: body memory, systems, garbage
        "rust memory",               // d1: body rust, memory
        "collection memory garbage", // d2: body collection, memory, garbage
        "rust rust memory",          // d3: body rust×2, memory
        "memory",                    // d4: body memory
        "systems garbage memory",    // d5: body systems, garbage, memory
        "rust memory systems",       // d6: body rust, memory, systems
        "garbage collection rust",   // d7: body garbage, collection, rust
    };

    // 两个段：各装 4 篇文档（docid 段内 0..3）。
    auto segA = std::make_unique<SealedSegment>();
    auto segB = std::make_unique<SealedSegment>();
    // 基线：每字段一份单 InvertedIndex（8 篇文档，docid 全局 0..7）+ FakeLiveChecker。
    InvertedIndex whole_title, whole_body;
    FakeLiveChecker whole_chk;
    for (std::uint64_t g = 0; g < kNAll; ++g) {
        // 把 title/body 字符串（空格分词）转 TermPositions。
        auto split_add = [](TermPositions& m, const std::string& s) {
            std::uint32_t tf = 0;
            for (std::size_t i = 0; i < s.size(); ++i) {
                if (s[i] == ' ') {
                    if (tf > 0) m[s.substr(i - tf, tf)] = {tf, {}};
                    tf = 0;
                } else {
                    ++tf;
                }
            }
            if (tf > 0) m[s.substr(s.size() - tf, tf)] = {tf, {}};
        };
        TermPositions tTPM, bTPM;
        split_add(tTPM, titles[g]);
        split_add(bTPM, bodies[g]);
        std::uint32_t ttl_d = 0;
        for (const auto& [k, v] : tTPM) ttl_d += v.first;
        for (const auto& [k, v] : bTPM) ttl_d += v.first;

        // 喂两段。
        std::array<SealedSegment::FieldInput, 2> fs{{
            {"title", &tTPM},
            {"body",  &bTPM},
        }};
        if (g < 4) {
            segA->add("doc" + std::to_string(g), g, fs, ttl_d);
        } else {
            segB->add("doc" + std::to_string(g), g, fs, ttl_d);
        }

        // 喂基线（每字段单 InvertedIndex，全局 docid）。
        whole_title.add_doc(g, tTPM);
        whole_body.add_doc(g, bTPM);
        whole_chk.doc_lens[g] = ttl_d;
    }
    ASSERT_EQ(segA->doc_count(), 4u);
    ASSERT_EQ(segB->doc_count(), 4u);

    // 基线：与 TextPlugin::search_fields 同构的算法（每字段 search +
    // acc[ord]+=score，boost=1.0）。不调 TextPlugin（构造链路过重），用同样
    // 算法的纯函数实现作为对照——这恰是 search_fields 在单索引上的展开。
    auto baseline_search = [&](const std::unordered_map<std::string,
            std::vector<std::string>>& qft, std::size_t k) {
        std::unordered_map<std::uint64_t, double> acc;
        for (const auto& [fname, terms] : qft) {
            const InvertedIndex* inv = nullptr;
            if (fname == "title") inv = &whole_title;
            else if (fname == "body") inv = &whole_body;
            else continue;
            auto hits = inv->search(terms, k, whole_chk);
            for (const auto& h : hits) acc[h.ord] += h.score;
        }
        std::vector<std::pair<std::uint64_t, double>> ranked(acc.begin(),
                                                             acc.end());
        std::partial_sort(ranked.begin(),
            ranked.begin() +
                static_cast<std::ptrdiff_t>(std::min(k, ranked.size())),
            ranked.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
        if (ranked.size() > k) ranked.resize(k);
        std::vector<SearchHit> out;
        out.reserve(ranked.size());
        for (const auto& [ord, sc] : ranked) {
            out.push_back(SearchHit{"doc" + std::to_string(ord), ord, sc});
        }
        return out;
    };

    // 查询：title 找 rust（多文档命中，df 跨段聚合），body 找 memory（更广命中）。
    // 两路都要求 k=8 → 无 top-k 截断（hit 数 ~8，逐位对比稳定）。
    std::unordered_map<std::string, std::vector<std::string>> qft{
        {"title", {"rust"}},
        {"body",  {"memory"}},
    };
    const std::size_t k = kNAll;

    auto base = baseline_search(qft, k);
    std::vector<MultiFieldSegmentView> segs = {segA->multi_view(),
                                               segB->multi_view()};
    auto merged = multi_field_segment_search(segs, qft, k);

    ASSERT_EQ(base.size(), merged.size());
    for (std::size_t i = 0; i < base.size(); ++i) {
        EXPECT_EQ(base[i].key, merged[i].key) << "i=" << i;
        EXPECT_EQ(base[i].ord, merged[i].ord) << "i=" << i;
        EXPECT_FLOAT_EQ(static_cast<float>(base[i].score),
                        static_cast<float>(merged[i].score))
            << "i=" << i;
    }

    // 反证 G-on-the-fly 必要性：只跑 segA（单段）→ N=4/avgdl=某值，与两段
    // 全局 N=8/avgdl 不同 → BM25 idf + dl/avgdl 缩放必然不同 → 至少一条命中
    // 的分数与两段合并后同 doc 的分数不同（证明不跨段聚合会漂移分数）。
    std::vector<MultiFieldSegmentView> onlyA = {segA->multi_view()};
    auto oneSeg = multi_field_segment_search(onlyA, qft, k);
    bool diff_found = false;
    for (const auto& oh : oneSeg) {
        for (const auto& mh : merged) {
            if (oh.ord == mh.ord &&
                std::abs(static_cast<float>(oh.score) -
                         static_cast<float>(mh.score)) > 1e-6F) {
                diff_found = true;
                break;
            }
        }
        if (diff_found) break;
    }
    EXPECT_TRUE(diff_found)
        << "G-on-the-fly 必要性反证失败：单段本地统计应与跨段全局统计分数不同";
}

// ===========================================================================
// S29-6B：查询线程 term 快照缓存（TermSnapshotCache + per-shard gen 失效）
// 设计:docs/design/s29-6b-inverted-term-cache.md;风险清单 §3 逐条对位。
// ===========================================================================

// 命中路径与慢路径快照字节同源（snapshot_flat）→ 分数必须位级一致。
TEST(TermCache, RepeatQueryBitIdentical) {
    InvertedIndex idx;
    for (std::uint64_t i = 0; i < 50; ++i) {
        idx.add_doc(i, {{"alpha", tp(1 + static_cast<std::uint32_t>(i % 3), {0})},
                        {"beta", tp(1, {1})}});
    }
    FakeLiveChecker live;
    for (std::uint64_t i = 0; i < 50; ++i) live.doc_lens[i] = 2;
    const std::vector<std::string> q = {"alpha", "beta"};
    auto r1 = idx.search(q, 10, live);  // 慢路径（首查，填缓存）
    auto r2 = idx.search(q, 10, live);  // 零锁命中路径
    ASSERT_EQ(r1.size(), r2.size());
    ASSERT_FALSE(r1.empty());
    for (std::size_t i = 0; i < r1.size(); ++i) {
        EXPECT_EQ(r1[i].ord, r2[i].ord);
        EXPECT_EQ(r1[i].score, r2[i].score);
    }
}

// 漏 bump ⇒ 永久陈旧——add_doc 失效路径。
TEST(TermCache, InvalidationOnAddDoc) {
    InvertedIndex idx;
    idx.add_doc(0, {{"alpha", tp(1, {0})}});
    FakeLiveChecker live;
    live.doc_lens[0] = 1;
    ASSERT_EQ(idx.search({"alpha"}, 10, live).size(), 1u);  // 填缓存
    idx.add_doc(1, {{"alpha", tp(1, {0})}});
    live.doc_lens[1] = 1;
    EXPECT_EQ(idx.search({"alpha"}, 10, live).size(), 2u);  // gen 失效 → 见新文档
    EXPECT_EQ(idx.search({"alpha"}, 10, live).size(), 2u);  // 刷新后的命中路径
}

// 负缓存不得挡新词（add_doc 插新 term 也 bump shard）。
TEST(TermCache, NegativeEntryThenTermAppears) {
    InvertedIndex idx;
    idx.add_doc(0, {{"alpha", tp(1, {0})}});
    FakeLiveChecker live;
    live.doc_lens[0] = 1;
    live.doc_lens[1] = 1;
    EXPECT_TRUE(idx.search({"ghost"}, 10, live).empty());  // 缺席 → 负缓存
    EXPECT_TRUE(idx.search({"ghost"}, 10, live).empty());  // 负缓存命中
    idx.add_doc(1, {{"ghost", tp(1, {0})}});
    auto r = idx.search({"ghost"}, 10, live);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].ord, 1u);
}

// index_id 隔离：两索引同 term 交错查询不串味。
TEST(TermCache, CrossIndexIsolation) {
    InvertedIndex a;
    InvertedIndex b;
    a.add_doc(0, {{"alpha", tp(1, {0})}});
    b.add_doc(0, {{"alpha", tp(1, {0})}});
    b.add_doc(1, {{"alpha", tp(1, {0})}});
    FakeLiveChecker live;
    live.doc_lens[0] = 1;
    live.doc_lens[1] = 1;
    EXPECT_EQ(a.search({"alpha"}, 10, live).size(), 1u);
    EXPECT_EQ(b.search({"alpha"}, 10, live).size(), 2u);
    EXPECT_EQ(a.search({"alpha"}, 10, live).size(), 1u);
    EXPECT_EQ(b.search({"alpha"}, 10, live).size(), 2u);
}

// doc_freq 的 df-only/缺席条目 + 失效;df-only 不阻碍 search 行补齐。
TEST(TermCache, DocFreqCachedAndInvalidated) {
    InvertedIndex idx;
    idx.add_doc(0, {{"alpha", tp(1, {0})}});
    idx.add_doc(1, {{"alpha", tp(1, {0})}});
    EXPECT_EQ(idx.doc_freq("alpha"), 2u);
    EXPECT_EQ(idx.doc_freq("alpha"), 2u);  // df-only 命中
    EXPECT_EQ(idx.doc_freq("ghost"), 0u);
    EXPECT_EQ(idx.doc_freq("ghost"), 0u);  // 缺席负缓存命中
    idx.add_doc(2, {{"alpha", tp(1, {0})}});
    EXPECT_EQ(idx.doc_freq("alpha"), 3u);  // gen 失效 → 现值
    FakeLiveChecker live;
    for (std::uint64_t i = 0; i < 3; ++i) live.doc_lens[i] = 1;
    EXPECT_EQ(idx.search({"alpha"}, 10, live).size(), 3u);  // df-only → 行补齐
    EXPECT_EQ(idx.search({"alpha"}, 10, live).size(), 3u);  // 命中路径
}

// compact 删行 → 失效（df 与结果都要反映压实后状态）。
TEST(TermCache, CompactInvalidates) {
    InvertedIndex idx;
    for (std::uint64_t i = 0; i < 10; ++i) {
        idx.add_doc(i, {{"alpha", tp(1, {0})}});
    }
    FakeLiveChecker live;
    for (std::uint64_t i = 0; i < 10; ++i) live.doc_lens[i] = 1;
    EXPECT_EQ(idx.search({"alpha"}, 20, live).size(), 10u);
    EXPECT_EQ(idx.doc_freq("alpha"), 10u);
    live.doc_lens.clear();
    live.doc_lens[0] = 1;
    live.doc_lens[9] = 1;
    EXPECT_EQ(idx.compact(live, 0.5), 1u);
    EXPECT_EQ(idx.doc_freq("alpha"), 2u);  // 失效 → 压实后列长
    EXPECT_EQ(idx.search({"alpha"}, 20, live).size(), 2u);
}

// deserialize 全量失效（负缓存被重灌覆盖的场景）。
TEST(TermCache, DeserializeInvalidatesNegativeEntry) {
    InvertedIndex a;
    a.add_doc(0, {{"alpha", tp(1, {0})}});
    std::vector<std::byte> bytes;
    a.serialize(bytes);

    InvertedIndex b;
    FakeLiveChecker live;
    live.doc_lens[0] = 1;
    EXPECT_TRUE(b.search({"alpha"}, 10, live).empty());   // 负缓存
    ASSERT_TRUE(b.deserialize(bytes));
    EXPECT_EQ(b.search({"alpha"}, 10, live).size(), 1u);  // 失效 → 命中
}

// 运行期开关：关闭后行为等价（回退路径）。
TEST(TermCache, RuntimeSwitchOff) {
    InvertedIndex::set_query_cache_enabled(false);
    InvertedIndex idx;
    idx.add_doc(0, {{"alpha", tp(1, {0})}});
    FakeLiveChecker live;
    live.doc_lens[0] = 1;
    EXPECT_EQ(idx.search({"alpha"}, 10, live).size(), 1u);
    EXPECT_EQ(idx.search({"alpha"}, 10, live).size(), 1u);
    EXPECT_EQ(idx.doc_freq("alpha"), 1u);
    InvertedIndex::set_query_cache_enabled(true);
    EXPECT_EQ(idx.search({"alpha"}, 10, live).size(), 1u);
}

// 同查询钉住：已发放条目不得被同一查询内后续 upsert 覆写
// （评分视图正引用它们）;窗口全钉住时 upsert 拒绝(caller 回退私有槽)。
TEST(TermCache, PinnedEntriesSurviveSameQueryInserts) {
    auto& cache = bitcask::bm25::TermSnapshotCache::tls_instance();
    cache.begin_query();
    // 高位假 id,不与真实 index_id(进程级小整数单调)相撞。
    constexpr std::uint64_t kFakeId = 0xFFFFFFFF00000001ull;
    std::vector<std::pair<bitcask::bm25::TermSnapshotCache::Entry*, std::string>>
        got;
    std::size_t rejected = 0;
    for (int i = 0; i < 400; ++i) {  // > kSlots → 窗口必然填满钉住条目
        std::string term = "t" + std::to_string(i);
        auto* e = cache.upsert(kFakeId, term, 7);
        if (e == nullptr) {
            ++rejected;
            continue;
        }
        e->df = static_cast<std::uint64_t>(i);
        e->has_rows = true;
        got.emplace_back(e, term);
    }
    EXPECT_GE(rejected, 400u - bitcask::bm25::TermSnapshotCache::kSlots);
    for (auto& [e, term] : got) {
        EXPECT_EQ(e->term, term);
        EXPECT_EQ(e->index_id, kFakeId);
    }
    cache.begin_query();  // 解钉,后续测试正常复用槽
}

// 并发：读者热词查询（命中/失效交替 + BOW→WAND 跨阈值）vs 单写者持续
// add_doc——TSan 靶;功能断言宽松（hot 恒 ≥ 初始 200 docs）。
TEST(TermCacheConcurrency, SearchWhileIndexing) {
    InvertedIndex idx;
    for (std::uint64_t i = 0; i < 200; ++i) {
        idx.add_doc(i, {{"hot", tp(1, {0})}});
    }
    class AllLive : public LiveChecker {
    public:
        [[nodiscard]] bool is_live(std::uint64_t) const override { return true; }
        [[nodiscard]] std::uint32_t doc_len(std::uint64_t) const override {
            return 2;
        }
    };
    AllLive live;
    std::atomic<bool> stop{false};
    std::atomic<bool> ok{true};
    std::thread writer([&] {
        std::uint64_t ord = 200;
        std::uint64_t i = 0;
        // 封顶 20k 文档：防 hot posting 无界增长把测试拖成分钟级
        // （封顶后 gen 停变 → 读者转入纯命中路径,同样是覆盖目标）。
        while (!stop.load(std::memory_order_relaxed) && i < 20000) {
            idx.add_doc(ord++, {{"hot", tp(1, {0})},
                                {"cold" + std::to_string(i % 64), tp(1, {1})}});
            ++i;
        }
    });
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            for (int it = 0; it < 1500; ++it) {
                auto res = idx.search({"hot", "cold1"}, 10, live);
                if (res.empty()) {
                    ok.store(false);
                    return;
                }
                if (idx.doc_freq("hot") < 200) {
                    ok.store(false);
                    return;
                }
            }
        });
    }
    for (auto& t : readers) t.join();
    stop.store(true);
    writer.join();
    EXPECT_TRUE(ok.load());
}

// ===========================================================================
// S31(下游反馈):v1 deserialize 超长 term 跳过不炸段(容错 + 计数可见)。
// ===========================================================================
TEST(InvertedIndex, S31OversizedTermSkippedOnLoad) {
    InvertedIndex a;
    const std::string monster(1500, 'z');  // 绕过分析器直插(历史坏库形态)
    a.add_doc(0, {{"alpha", tp(1, {0})}, {monster, tp(1, {1})}});
    a.add_doc(1, {{"alpha", tp(2, {0, 1})}, {"beta", tp(1, {2})}});
    std::vector<std::byte> bytes;
    a.serialize(bytes);

    InvertedIndex b;
    ASSERT_TRUE(b.deserialize(bytes));  // 修复前:整段 false
    EXPECT_EQ(b.load_skipped_oversized_terms(), 1u);  // 可见性
    EXPECT_EQ(b.doc_freq("alpha"), 2u);               // 其余 term 完好
    EXPECT_EQ(b.doc_freq("beta"), 1u);
    EXPECT_EQ(b.doc_freq(monster), 0u);               // 超长者被剔除
    FakeLiveChecker live;
    live.doc_lens[0] = 2;
    live.doc_lens[1] = 3;
    EXPECT_EQ(b.search({"alpha"}, 10, live).size(), 2u);
}

// ===========================================================================
// C4:Block-Max MaxScore——三方随机对拍:无剪枝穷举参照 ≡ WAND ≡ MaxScore
// **位级相同**(两剪枝算法 admissible + 分数按原词序求和 + 同堆语义)。
// ===========================================================================

namespace {

// 无剪枝穷举参照:镜像 search_wand_impl 的取数与标量公式(idf 用 live_df/
// 全局 df、dl 每文档取一次、匹配词按原词序累加、>=θ 进堆/满后 >top 换)。
// 这是 top-k 语义的**规范实现**——剪枝算法必须与其逐位一致。
std::vector<SearchResult> exhaustive_topk_reference(
    const InvertedIndex& idx, const std::vector<std::string>& terms,
    std::size_t k, const LiveChecker& live,
    const Bm25Params& params, std::uint64_t N, std::uint64_t sum_dl,
    const std::vector<std::pair<std::string, std::uint64_t>>* global_df) {
    struct T {
        PostingList pl;
        std::vector<char> lv;
        float idf = 0.0F;
        bool present = false;
    };
    std::vector<T> ts(terms.size());
    const double avgdl =
        N > 0 ? static_cast<double>(sum_dl) / static_cast<double>(N) : 1.0;
    std::vector<std::uint64_t> all_ords;
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (!idx.snapshot_postings(terms[i], ts[i].pl)) continue;
        ts[i].present = true;
        ts[i].lv.resize(ts[i].pl.size());
        live.fill_is_live(ts[i].pl.ords, ts[i].lv);
        std::size_t live_df = 0;
        for (char c : ts[i].lv) live_df += static_cast<std::size_t>(c);
        if (live_df == 0) continue;
        double dfv = static_cast<double>(live_df);
        if (global_df) {
            for (const auto& [t, v] : *global_df) {
                if (t == terms[i]) {
                    if (v > 0) dfv = static_cast<double>(v);
                    break;
                }
            }
        }
        ts[i].idf = static_cast<float>(std::log(
            1.0 + (static_cast<double>(N) - dfv + 0.5) / (dfv + 0.5)));
        all_ords.insert(all_ords.end(), ts[i].pl.ords.begin(),
                        ts[i].pl.ords.end());
    }
    std::sort(all_ords.begin(), all_ords.end());
    all_ords.erase(std::unique(all_ords.begin(), all_ords.end()),
                   all_ords.end());

    using Entry = std::pair<float, std::uint64_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> heap;
    float threshold = 0.0F;
    for (auto d : all_ords) {
        if (!live.is_live(d)) continue;
        const auto dl = live.doc_len(d);
        float score = 0.0F;
        bool any = false;
        for (std::size_t i = 0; i < terms.size(); ++i) {
            if (!ts[i].present) continue;
            const auto idxp = ts[i].pl.find(d);
            if (idxp >= ts[i].pl.size()) continue;
            any = true;
            const auto tf = static_cast<float>(ts[i].pl.tfs[idxp]);
            const float tf_norm =
                tf * (params.k1 + 1.0F) /
                (tf + params.k1 *
                          (1.0F - params.b +
                           params.b * static_cast<float>(dl) /
                               static_cast<float>(avgdl)));
            score += ts[i].idf * (tf_norm + params.delta);
        }
        if (!any) continue;
        if (score >= threshold) {
            if (heap.size() < k) {
                heap.push({score, d});
            } else if (score > heap.top().first) {
                heap.pop();
                heap.push({score, d});
            }
            if (heap.size() >= k) threshold = heap.top().first;
        }
    }
    std::vector<SearchResult> out;
    out.reserve(heap.size());
    while (!heap.empty()) {
        out.push_back({heap.top().second, heap.top().first});
        heap.pop();
    }
    std::reverse(out.begin(), out.end());
    return out;
}

void expect_bitwise_equal(const std::vector<SearchResult>& a,
                          const std::vector<SearchResult>& b,
                          const std::string& what) {
    ASSERT_EQ(a.size(), b.size()) << what;
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].ord, b[i].ord) << what << " @" << i;
        EXPECT_EQ(a[i].score, b[i].score) << what << " @" << i;
    }
}

struct MaxScoreToggle {  // RAII:测试内切算法,退出恢复
    explicit MaxScoreToggle(bool on) { InvertedIndex::set_topk_use_maxscore(on); }
    ~MaxScoreToggle() { InvertedIndex::set_topk_use_maxscore(false); }
};

}  // namespace

TEST(MaxScore, ThreeWayRandomizedBitwiseEquivalence) {
    // 偏斜语料:hot(全命中)/mid×10/rare×100 + 变 tf——都路由 WAND 档
    // (查询含 hot ⇒ total ≥ docs ≥ 1024)。
    constexpr std::uint32_t kDocs = 4000;
    InvertedIndex idx;
    FakeLiveChecker live;
    std::vector<std::string> vocab = {"hot", "hot2"};
    for (int i = 0; i < 10; ++i) vocab.push_back("mid" + std::to_string(i));
    for (int i = 0; i < 100; ++i) vocab.push_back("rare" + std::to_string(i));
    for (std::uint32_t d = 0; d < kDocs; ++d) {
        TermPositions tp;
        tp["hot"] = {1 + d % 5, {0}};
        if (d % 2 == 0) tp["hot2"] = {1 + d % 3, {1}};
        tp["mid" + std::to_string(d % 10)] = {1 + d % 2, {2}};
        tp["rare" + std::to_string(d % 100)] = {1, {3}};
        idx.add_doc(d, tp);
        // dl 必须与索引时 add_doc 记录的 Σtf 一致——v5 impacts 前提
        // (块 min_dl 来自索引 dl,打分 dl 来自 checker;两者不一致会让
        // 块上界失效 → 「合法」误跳。生产路径 doc_len_writer 恒同源)。
        std::uint32_t dl = 0;
        for (auto& [t, pd] : tp) dl += pd.first;
        live.doc_lens[d] = dl;
    }
    // 三分之一文档死亡(live_df/idf 路径 + 死点跳过)。
    for (std::uint32_t d = 0; d < kDocs; d += 3) live.doc_lens.erase(d);

    std::mt19937 rng(0xC4);
    std::uniform_int_distribution<std::size_t> vpick(0, vocab.size() - 1);
    std::uniform_int_distribution<int> nterms(1, 8);
    std::uniform_int_distribution<int> kcase(0, 3);
    const std::size_t kk[4] = {1, 3, 10, 50};
    const Bm25Params params{1.2F, 0.75F};

    for (int trial = 0; trial < 200; ++trial) {
        std::vector<std::string> q = {"hot"};  // 保证 WAND 档
        const int n = nterms(rng);
        for (int i = 1; i < n; ++i) q.push_back(vocab[vpick(rng)]);
        if (trial % 7 == 0) q.push_back("ghost");     // 缺席词
        if (trial % 11 == 0) q.push_back(q.back());   // 重复词
        const std::size_t k = kk[static_cast<std::size_t>(kcase(rng))];

        // ext 注入(G-on-the-fly 语义)每 5 轮一次。
        std::vector<std::pair<std::string, std::uint64_t>> dfv;
        ExtStats ext;
        const ExtStats* extp = nullptr;
        if (trial % 5 == 0) {
            for (const auto& t : q) dfv.emplace_back(t, idx.doc_freq(t));
            ext.N = idx.live_doc_count();
            ext.sum_dl = idx.sum_doc_len();
            ext.df = &dfv;
            extp = &ext;
        }
        const std::uint64_t N = extp ? ext.N : idx.live_doc_count();
        const std::uint64_t sdl = extp ? ext.sum_dl : idx.sum_doc_len();

        const auto ref = exhaustive_topk_reference(idx, q, k, live, params, N,
                                                   sdl, extp ? ext.df : nullptr);
        std::vector<SearchResult> wand;
        {
            MaxScoreToggle t(false);
            wand = idx.search(q, k, live, &params, extp);
        }
        std::vector<SearchResult> ms;
        {
            MaxScoreToggle t(true);
            ms = idx.search(q, k, live, &params, extp);
        }
        expect_bitwise_equal(ref, wand, "ref-vs-wand t" + std::to_string(trial));
        expect_bitwise_equal(ref, ms, "ref-vs-ms t" + std::to_string(trial));
        if (::testing::Test::HasFailure()) break;
    }
}
