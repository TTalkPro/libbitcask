// 下游反馈 2026-10-06（feedbacks/2026-10-06-meta-filter-query-gaps.md）回归：
//   1. 带 filter 的检索补取到 k（原先固定多取 max(k×4,64) 后过滤，严 filter
//      静默少返回：1000 篇里 100 篇满足，K=10 只回 6 条）；hybrid 文本路同病。
//   2. CaskIter / CaskRangeIter 可交出 meta（want_meta），与 value 同一次读。
//   3. phrase / bool / fields / near / fuzzy / wildcard 收 filter。
//   4. 迭代器收 filter：只按 meta 筛选的扫描（含有序 [lo, hi) 版）。
// 第 5 条（同分并列的 top-K 前缀稳定）另案处理，本文件不覆盖。
// 迭代器 filter 不吞墓碑（see_tombstones 时照常交出）由构造保证：求值只在
// 解码活记录的分支里，墓碑分支不经过（cask_iter.cpp）。
// 对拍基准：无 filter 全量检索（k ≥ 文档数）→ 按 meta 过滤 → 取前 K。

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "bitcask/cask.hpp"
#include "bitcask/keydir_registry.hpp"
#include "bitcask/meta_codec.hpp"
#include "bitcask/searcher.hpp"

using namespace bitcask;
namespace fs = std::filesystem;

namespace {

keydir::KeyDirRegistry& test_registry() {
    static keydir::KeyDirRegistry reg;
    return reg;
}

std::span<const std::byte> sv_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string to_str(const std::vector<std::byte>& b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

std::vector<std::byte> tag_blob(const std::string& tag) {
    std::vector<meta::MetaEntry> entries{{"tag", meta::MetaValue(tag)}};
    std::vector<std::byte> blob;
    meta::encode_meta(blob, entries);
    return blob;
}

meta::MetaFilter tag_filter(const std::string& tag) {
    meta::MetaFilter f;
    f.conditions.push_back({"tag", meta::MetaOp::Eq, meta::MetaValue(tag), {}});
    return f;
}

constexpr int kDocs = 1000;
constexpr int kKeepEvery = 10;  // 每 10 篇 1 篇 tag=keep → 共 100 篇

std::string key_of(int i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "k%04d", i);
    return buf;
}
bool is_keep(int i) { return i % kKeepEvery == 0; }

// param = 封口 RAM 预算：0 = 单 building 段；小预算 = 多封口段（跨段补取）。
class MetaFilterQueryTest : public ::testing::TestWithParam<std::size_t> {
protected:
    void SetUp() override {
        // ctest -j 每个用例一个进程：目录按用例名区分，防并发互踩。
        std::string name =
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
        std::replace(name.begin(), name.end(), '/', '_');
        dir_ = fs::temp_directory_path() / ("bitcask_meta_filter_query_" + name);
        fs::remove_all(dir_);
        fs::create_directories(dir_);

        CaskOptions opts;
        opts.read_write = true;
        opts.enable_search = true;
        search::SearchLayerConfig scfg;
        scfg.analyzer_config.type = text::AnalyzerType::Whitespace;
        scfg.seal_ram_budget_bytes = GetParam();
        opts.search_config = scfg;
        opts.vector_dim = 4;
        opts.vector_metric = meta::VectorMetric::kDot;
        auto c = Cask::open(dir_.string(), opts, &test_registry());
        ASSERT_TRUE(c) << c.error().detail;
        cask_ = std::move(*c);

        const auto keep = tag_blob("keep");
        const auto drop = tag_blob("drop");
        for (int i = 0; i < kDocs; ++i) {
            // 文本长度随 i 变 → BM25 分数有梯度（短文档分高），对拍不全靠平局序。
            std::string text = "apple pie";
            for (int j = 0; j < i % 7; ++j) text += " filler" + std::to_string(j);
            const float v[4] = {static_cast<float>(i % 13), 1.0F, 0.0F, 0.0F};
            DocInput doc;
            doc.text = sv_bytes(text);
            doc.meta = is_keep(i) ? std::span<const std::byte>(keep)
                                  : std::span<const std::byte>(drop);
            doc.vector = std::span<const float>(v, 4);
            ASSERT_TRUE(cask_->put_doc(sv_bytes(key_of(i)), doc, 1000));
        }
        cask_->flush_index();
    }
    void TearDown() override {
        if (cask_) cask_->close();
        cask_.reset();
        fs::remove_all(dir_);
    }

    // 对拍：无 filter 全量结果（k ≥ 文档数，候选穷尽）按 keep 过滤后的前 k 个。
    // 同分并列的取舍由内核扫描 / 剪枝次序决定、不随 k 稳定（反馈第 5 条，另案），
    // 故按分数多重集比，不比 key 序列。
    template <class Run>
    void expect_matches_oracle(const char* what, Run run) {
        auto full = run(static_cast<std::size_t>(kDocs) * 2, nullptr);
        ASSERT_TRUE(full) << what;
        std::vector<double> oracle;
        for (auto& h : full->hits) {
            if (is_keep(std::stoi(h.key.substr(1)))) oracle.push_back(h.score);
        }
        ASSERT_EQ(oracle.size(), static_cast<std::size_t>(kDocs / kKeepEvery))
            << what << ": 全量检索应命中全部 keep 文档";
        const auto keep = tag_filter("keep");
        for (std::size_t k : {1u, 10u, 50u, 100u, 1000u}) {
            auto r = run(k, &keep);
            ASSERT_TRUE(r) << what << " k=" << k;
            const std::size_t want = std::min(k, oracle.size());
            ASSERT_EQ(r->hits.size(), want) << what << " k=" << k;
            std::multiset<double> got_scores;
            for (auto& h : r->hits) {
                EXPECT_TRUE(is_keep(std::stoi(h.key.substr(1))))
                    << what << " k=" << k << " key=" << h.key;
                got_scores.insert(h.score);
            }
            const std::multiset<double> want_scores(
                oracle.begin(), oracle.begin() + static_cast<std::ptrdiff_t>(want));
            EXPECT_EQ(got_scores, want_scores) << what << " k=" << k;
        }
    }

    fs::path dir_;
    std::unique_ptr<Cask> cask_;
};

// 第 1 条：反馈原表格（K=10/50/100/1000 → 10/50/100/100）。
TEST_P(MetaFilterQueryTest, SearchTextFillsKUnderSelectiveFilter) {
    const auto f = tag_filter("keep");
    const std::pair<std::size_t, std::size_t> table[] = {
        {10, 10}, {50, 50}, {100, 100}, {1000, 100}};
    for (auto [k, want] : table) {
        auto r = cask_->search_text("apple", k, &f);
        ASSERT_TRUE(r);
        EXPECT_EQ(r->hits.size(), want) << "k=" << k;
        for (auto& h : r->hits) EXPECT_TRUE(is_keep(std::stoi(h.key.substr(1))));
    }
    expect_matches_oracle("search_text", [&](std::size_t k, const meta::MetaFilter* flt) {
        return cask_->search_text("apple", k, flt);
    });
}

// 分页：每页都取满，十页合起来正好是全部 keep 文档的分数分布。
// 同分并列跨页可能重复 / 遗漏（反馈第 5 条，既有问题、另案），故按分数
// 多重集对拍，不比 key 集合。
TEST_P(MetaFilterQueryTest, SearchTextOffsetPagesThroughFilteredSet) {
    const auto f = tag_filter("keep");
    std::multiset<double> paged, want;
    for (std::size_t off = 0; off < 100; off += 10) {
        auto r = cask_->search_text("apple", 10, &f, off);
        ASSERT_TRUE(r);
        ASSERT_EQ(r->hits.size(), 10u) << "offset=" << off;
        for (auto& h : r->hits) {
            EXPECT_TRUE(is_keep(std::stoi(h.key.substr(1))));
            paged.insert(h.score);
        }
    }
    auto all = cask_->search_text("apple", 100, &f);
    ASSERT_TRUE(all);
    ASSERT_EQ(all->hits.size(), 100u);
    for (auto& h : all->hits) want.insert(h.score);
    EXPECT_EQ(paged, want);
    auto tail = cask_->search_text("apple", 10, &f, 100);
    ASSERT_TRUE(tail);
    EXPECT_TRUE(tail->hits.empty());
}

TEST_P(MetaFilterQueryTest, BatchAndHybridTextLegFillK) {
    const auto f = tag_filter("keep");
    const std::string_view qs[] = {"apple", "pie"};
    auto rs = cask_->search_text_batch(qs, 10, &f);
    ASSERT_EQ(rs.size(), 2u);
    for (auto& r : rs) {
        ASSERT_TRUE(r);
        EXPECT_EQ(r->hits.size(), 10u);
    }
    // hybrid 纯文本路（vec 空）：文本路补取到 K'，融合后取满 k。
    auto h = cask_->search_hybrid("apple", {}, 10, &f);
    ASSERT_TRUE(h) << h.error().detail;
    EXPECT_EQ(h->hits.size(), 10u);
    for (auto& hit : h->hits) EXPECT_TRUE(is_keep(std::stoi(hit.key.substr(1))));
    // 双路：结果只含 keep。
    const float q[4] = {1.0F, 1.0F, 0.0F, 0.0F};
    auto h2 = cask_->search_hybrid("apple", {q, 4}, 10, &f);
    ASSERT_TRUE(h2);
    EXPECT_EQ(h2->hits.size(), 10u);
    for (auto& hit : h2->hits) EXPECT_TRUE(is_keep(std::stoi(hit.key.substr(1))));
}

// 第 3 条：其余检索收 filter，同一套补取。
TEST_P(MetaFilterQueryTest, OtherSearchesAcceptFilter) {
    expect_matches_oracle("phrase", [&](std::size_t k, const meta::MetaFilter* flt) {
        return cask_->search_phrase("apple pie", k, 0, flt);
    });
    expect_matches_oracle("bool", [&](std::size_t k, const meta::MetaFilter* flt) {
        return cask_->bool_search("apple AND pie", k, 0, flt);
    });
    expect_matches_oracle("fields", [&](std::size_t k, const meta::MetaFilter* flt) {
        return cask_->search_fields("apple", k, flt);
    });
    expect_matches_oracle("near", [&](std::size_t k, const meta::MetaFilter* flt) {
        return cask_->search_near("apple pie", 0, k, flt);
    });
    expect_matches_oracle("fuzzy", [&](std::size_t k, const meta::MetaFilter* flt) {
        return cask_->search_fuzzy("aple", k, 1, flt);
    });
    expect_matches_oracle("wildcard", [&](std::size_t k, const meta::MetaFilter* flt) {
        return cask_->search_wildcard("app*", k, flt);
    });
}

TEST_P(MetaFilterQueryTest, FilterNoMatchReturnsEmpty) {
    const auto f = tag_filter("nobody");
    auto r = cask_->search_text("apple", 10, &f);
    ASSERT_TRUE(r);
    EXPECT_TRUE(r->hits.empty());
    auto p = cask_->search_phrase("apple pie", 10, 0, &f);
    ASSERT_TRUE(p);
    EXPECT_TRUE(p->hits.empty());
}

// 删掉一半 keep 文档：判活丢掉的候选同样靠补取填满。
TEST_P(MetaFilterQueryTest, DeletedCandidatesDoNotShrinkResult) {
    for (int i = 0; i < kDocs; i += 2 * kKeepEvery) {
        ASSERT_TRUE(cask_->remove(sv_bytes(key_of(i))));
    }
    cask_->flush_index();
    const auto f = tag_filter("keep");
    auto r = cask_->search_text("apple", 30, &f);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->hits.size(), 30u);
    auto all = cask_->search_text("apple", 1000, &f);
    ASSERT_TRUE(all);
    EXPECT_EQ(all->hits.size(), 50u);
}

TEST_P(MetaFilterQueryTest, SearcherFacadeAcceptsFilter) {
    text::Searcher ts(*cask_, *cask_->text_plugin());
    const auto f = tag_filter("keep");
    auto a = ts.search_phrase("apple pie", 10, &f);
    auto b = cask_->search_phrase("apple pie", 10, 0, &f);
    ASSERT_TRUE(a && b);
    ASSERT_EQ(a->hits.size(), 10u);
    ASSERT_EQ(a->hits.size(), b->hits.size());
    for (std::size_t i = 0; i < a->hits.size(); ++i) {
        EXPECT_EQ(a->hits[i].key, b->hits[i].key);
    }
    for (auto r : {ts.bool_search("apple", 10, &f), ts.search_fields("apple", 10, &f),
                   ts.search_near("apple pie", 0, 10, &f),
                   ts.search_fuzzy("aple", 10, 1, &f),
                   ts.search_wildcard("app*", 10, &f)}) {
        ASSERT_TRUE(r);
        EXPECT_EQ(r->hits.size(), 10u);
    }
}

// 第 2、4 条：fold 迭代器 want_meta + filter。
TEST_P(MetaFilterQueryTest, FoldIterWantMetaAndFilter) {
    const auto keep_blob = tag_blob("keep");
    const auto drop_blob = tag_blob("drop");
    {
        auto it = cask_->make_iter();
        it->set_want_meta(true);
        ASSERT_TRUE(it->start());
        int n = 0;
        while (true) {
            auto e = it->next();
            ASSERT_TRUE(e);
            if (!*e) break;
            const int i = std::stoi(to_str((*e)->key).substr(1));
            EXPECT_EQ((*e)->meta, is_keep(i) ? keep_blob : drop_blob);
            EXPECT_EQ(to_str((*e)->value).rfind("apple pie", 0), 0u);
            ++n;
        }
        EXPECT_EQ(n, kDocs);
    }
    {
        const auto f = tag_filter("keep");
        auto it = cask_->make_iter();
        it->set_filter(&f);  // want_meta 默认关：meta 恒空
        ASSERT_TRUE(it->start());
        std::set<std::string> keys;
        while (true) {
            auto e = it->next();
            ASSERT_TRUE(e);
            if (!*e) break;
            EXPECT_TRUE((*e)->meta.empty());
            EXPECT_FALSE((*e)->value.empty());
            keys.insert(to_str((*e)->key));
        }
        EXPECT_EQ(keys.size(), static_cast<std::size_t>(kDocs / kKeepEvery));
        for (auto& k : keys) EXPECT_TRUE(is_keep(std::stoi(k.substr(1))));
    }
}

// 第 2、4 条：有序 range 迭代器 want_meta + filter（惰性与预取两条路径）。
TEST_P(MetaFilterQueryTest, RangeIterWantMetaAndFilter) {
    const auto keep_blob = tag_blob("keep");
    const auto f = tag_filter("keep");
    for (std::size_t prefetch : {0u, 256u}) {
        RangeOptions o;
        const std::string lo = key_of(100), hi = key_of(500);
        o.lo = sv_bytes(lo);
        o.hi = sv_bytes(hi);
        o.prefetch = prefetch;
        o.prefetch_threads = 2;
        o.want_meta = true;
        o.filter = &f;
        auto it = cask_->make_range_iter(o);
        ASSERT_TRUE(it) << it.error().detail;
        std::vector<std::string> keys;
        while (true) {
            auto e = (*it)->next();
            ASSERT_TRUE(e);
            if (!*e) break;
            EXPECT_EQ((*e)->meta, keep_blob);
            EXPECT_EQ(to_str((*e)->value).rfind("apple pie", 0), 0u);
            keys.push_back(to_str((*e)->key));
        }
        std::vector<std::string> want;
        for (int i = 100; i < 500; ++i) {
            if (is_keep(i)) want.push_back(key_of(i));
        }
        EXPECT_EQ(keys, want) << "prefetch=" << prefetch;
    }
    // want_meta 关、无 filter：行为与改前一致（meta 恒空，全量）。
    auto it = cask_->make_range_iter(RangeOptions{});
    ASSERT_TRUE(it);
    int n = 0;
    while (true) {
        auto e = (*it)->next();
        ASSERT_TRUE(e);
        if (!*e) break;
        EXPECT_TRUE((*e)->meta.empty());
        ++n;
    }
    EXPECT_EQ(n, kDocs);
}


std::string segment_mode_name(
    const ::testing::TestParamInfo<std::size_t>& p) {
    return p.param == 0 ? "SingleBuilding" : "MultiSegment";
}

INSTANTIATE_TEST_SUITE_P(Segments, MetaFilterQueryTest,
                         ::testing::Values(std::size_t{0}, std::size_t{16 * 1024}),
                         segment_mode_name);

}  // namespace
