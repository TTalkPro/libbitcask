// S56：search_fields 打分口径——
//   ① 每字段跨段聚合 N / sum_dl / df（此前逐段本地统计：S45-4 后新文档散在各分片
//      building 段里各算各的 idf，重开前后分数 / 顺序都变，默认字段查询与 search_text
//      对不上）；
//   ② 字段打分的 dl 取该字段自身词数（此前取整篇总词数而 avgdl 是该字段的 ⇒ 短字段
//      被重罚，`labels:w^8` 压不过正文词频）。
// 下游反馈：feedbacks/2026-11-19-search-fields-local-stats-and-total-doc-len.md（bosun）。
#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>
#include <bitcask/segment.hpp>

#include <cmath>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskOptions;
using bitcask::DocInput;
using bitcask::search::SearchLayerConfig;
using bitcask::text::AnalyzerType;

bitcask::oki::IndexRegistry& test_registry() {
    static bitcask::oki::IndexRegistry reg;
    return reg;
}

std::span<const std::byte> bytes(std::string_view s) {
    return std::as_bytes(std::span<const char>(s.data(), s.size()));
}

CaskOptions search_options(bool catch_all = true) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig cfg;
    cfg.analyzer_config.type = AnalyzerType::Whitespace;
    cfg.index_catch_all = catch_all;
    opts.search_config = cfg;
    return opts;
}

using Scores = std::map<std::string, double>;

template <class R>
Scores to_scores(const R& r) {
    Scores out;
    for (const auto& h : r.hits) out[h.key] = h.score;
    return out;
}

Scores text_scores(Cask& c, std::string_view q) {
    auto r = c.search_text(q, 50);
    EXPECT_TRUE(r);
    return r ? to_scores(*r) : Scores{};
}

Scores field_scores(Cask& c, std::string_view q) {
    auto r = c.search_fields(q, 50);
    EXPECT_TRUE(r);
    return r ? to_scores(*r) : Scores{};
}

// 反馈里的文档形状：正文里拼上标题与标签，另以命名字段 title / labels 写一份。
void put_doc(Cask& c, std::string_view key, std::string_view title, std::string_view body,
             std::string_view labels) {
    const std::string text = std::string(title) + "\n" + std::string(body) + "\n" +
                             std::string(labels);
    DocInput d;
    d.text = bytes(text);
    d.fields.emplace_back("title", bytes(title));
    d.fields.emplace_back("labels", bytes(labels));
    ASSERT_TRUE(c.put_doc(bytes(key), d));
}

void write_repro(Cask& c) {
    put_doc(c, "body", "alpha beta", "zebra zebra zebra gamma delta", "");
    put_doc(c, "title", "zebra crossing", "gamma delta epsilon zeta eta", "");
    put_doc(c, "label", "alpha beta", "gamma delta epsilon zeta eta", "zebra");
    for (int i = 1; i <= 37; ++i) {
        put_doc(c, "fill" + std::to_string(i), "alpha beta", "gamma delta epsilon zeta eta",
                "misc");
    }
}

class SearchFieldsScoringTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() / (std::string("bitcask_s56_") + info->name());
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    fs::path dir_;
};

// ① 默认字段的 search_fields 与 search_text 任何时刻逐位一致；命名字段查询重开前后分数不变。
TEST_F(SearchFieldsScoringTest, MatchesSearchTextAndStableAcrossReopen) {
    std::map<std::string, Scores> before;
    const std::vector<std::string> queries = {"zebra", "title:zebra", "labels:zebra",
                                              "zebra title:zebra^3 labels:zebra^8"};
    {
        auto c = Cask::open(dir_.string(), search_options(), &test_registry());
        ASSERT_TRUE(c);
        write_repro(**c);
        (*c)->flush_index();
        const auto st = text_scores(**c, "zebra");
        ASSERT_EQ(st.size(), 3U);
        EXPECT_EQ(field_scores(**c, "zebra"), st) << "刚写完（building 段）";
        for (const auto& q : queries) before[q] = field_scores(**c, q);
        (*c)->close();
    }
    auto c = Cask::open(dir_.string(), search_options(), &test_registry());
    ASSERT_TRUE(c);
    const auto st = text_scores(**c, "zebra");
    EXPECT_EQ(field_scores(**c, "zebra"), st) << "重开后";
    for (const auto& q : queries) {
        const auto after = field_scores(**c, q);
        ASSERT_EQ(after.size(), before[q].size()) << q;
        for (const auto& [k, s] : before[q]) {
            EXPECT_NEAR(after.at(k), s, 1e-5 * std::max(1.0, s)) << q << " " << k;
        }
    }
    // 显式 checkpoint 后（段封口落盘、mmap 段惰性推字段长表）再比一次。
    ASSERT_TRUE((*c)->checkpoint());
    (*c)->close();
    c = Cask::open(dir_.string(), search_options(), &test_registry());
    ASSERT_TRUE(c);
    EXPECT_EQ(field_scores(**c, "zebra"), text_scores(**c, "zebra")) << "checkpoint + 重开";
    for (const auto& q : queries) {
        const auto after = field_scores(**c, q);
        for (const auto& [k, s] : before[q]) {
            EXPECT_NEAR(after.at(k), s, 1e-5 * std::max(1.0, s)) << q << " " << k;
        }
    }
    (*c)->close();
}

// ② labels 只有一个 token、全库只有一篇有 zebra：dl == avgdl ⇒ tf 归一 = 1，分数 = idf。
// N = 有 labels 的文档数 38（body / title 两篇 labels 为空不入字段），df = 1。
TEST_F(SearchFieldsScoringTest, NamedFieldUsesItsOwnLength) {
    auto c = Cask::open(dir_.string(), search_options(), &test_registry());
    ASSERT_TRUE(c);
    write_repro(**c);
    (*c)->flush_index();
    const double idf = std::log(1.0 + (38.0 - 1.0 + 0.5) / (1.0 + 0.5));
    const auto s = field_scores(**c, "labels:zebra");
    ASSERT_EQ(s.size(), 1U);
    EXPECT_NEAR(s.at("label"), idf, 1e-4);
    // boost 起文档里说的作用：labels^8 / title^3 命中排在正文 3 个 zebra 之前。
    auto r = (*c)->search_fields("zebra title:zebra^3 labels:zebra^8", 10);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->hits.size(), 3U);
    EXPECT_EQ(r->hits[0].key, "label");
    EXPECT_EQ(r->hits[1].key, "title");
    EXPECT_EQ(r->hits[2].key, "body");
    (*c)->close();
}

// ② 同一字段内容、不同正文长度 ⇒ 该字段分数相同（此前整篇越长罚得越重）。
TEST_F(SearchFieldsScoringTest, FieldScoreIndependentOfOtherFields) {
    auto c = Cask::open(dir_.string(), search_options(), &test_registry());
    ASSERT_TRUE(c);
    put_doc(**c, "short", "zebra", "a b", "x");
    put_doc(**c, "long", "zebra",
            "a b c d e f g h i j k l m n o p q r s t u v w x y z aa bb cc dd ee ff", "x");
    for (int i = 0; i < 10; ++i) put_doc(**c, "f" + std::to_string(i), "horse", "a b c", "x");
    (*c)->flush_index();
    const auto s = field_scores(**c, "title:zebra");
    ASSERT_EQ(s.size(), 2U);
    EXPECT_DOUBLE_EQ(s.at("short"), s.at("long"));
    (*c)->close();
}

// catch-all 关：默认字段只含正文，其 dl 也只算正文（命名字段的长度不再压默认字段分数）。
TEST_F(SearchFieldsScoringTest, DefaultFieldWithoutCatchAllUsesTextLength) {
    auto c = Cask::open(dir_.string(), search_options(/*catch_all=*/false), &test_registry());
    ASSERT_TRUE(c);
    auto put = [&](std::string_view key, std::string_view text, std::string_view title) {
        DocInput d;
        d.text = bytes(text);
        d.fields.emplace_back("title", bytes(title));
        ASSERT_TRUE((*c)->put_doc(bytes(key), d));
    };
    put("a", "zebra one", "t");
    put("b", "zebra one", "a very long title with many many words in it");
    for (int i = 0; i < 10; ++i) put("f" + std::to_string(i), "horse two", "t");
    (*c)->flush_index();
    const auto s = text_scores(**c, "zebra");
    ASSERT_EQ(s.size(), 2U);
    EXPECT_DOUBLE_EQ(s.at("a"), s.at("b"));
    EXPECT_EQ(field_scores(**c, "zebra"), s);
    (*c)->close();
}

// 段级：内存段与其 v2（mmap）落盘版的字段长一致，查询逐位一致；catch-all 默认字段
// （sum_dl == 段总长）不建表、直接用整篇总长。
TEST(FieldDocLensSegment, MemoryAndMmapAgree) {
    using bitcask::search::SealedSegment;
    using bitcask::search::kDefaultField;
    const auto dir = fs::temp_directory_path() / "bitcask_s56_segment";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    auto tp = [](std::initializer_list<std::pair<const char*, std::uint32_t>> ts) {
        bitcask::bm25::TermPositions out;
        std::uint32_t pos = 0;
        for (auto [t, tf] : ts) {
            auto& e = out[t];
            e.first = tf;
            for (std::uint32_t i = 0; i < tf; ++i) e.second.push_back(pos++);
        }
        return out;
    };
    SealedSegment mem;
    for (int i = 0; i < 300; ++i) {
        // 默认字段 = catch-all（正文 + 标题），命名字段 title 只有标题。
        const auto title = (i % 3 == 0) ? tp({{"zebra", 1}}) : tp({{"horse", 1}, {"pony", 1}});
        auto all = tp({{"w", static_cast<std::uint32_t>(1 + i % 7)}});
        for (const auto& [t, d] : title) all[t] = d;
        std::uint32_t total = 0;
        for (const auto& [t, d] : all) total += d.first;
        const SealedSegment::FieldInput in[] = {{kDefaultField, &all}, {"title", &title}};
        mem.add("k" + std::to_string(i), static_cast<std::uint64_t>(i + 1), in, total);
    }
    const auto path = (dir / "seg-1.seg").string();
    ASSERT_TRUE(mem.save_v2(path, 1));
    auto mm = SealedSegment::open_v2(path);
    ASSERT_TRUE(mm);

    for (std::uint64_t d = 0; d < 300; ++d) {
        EXPECT_EQ(mem.field_checker("title").doc_len(d), mm->field_checker("title").doc_len(d));
        EXPECT_EQ(mem.default_checker().doc_len(d), mm->default_checker().doc_len(d));
        EXPECT_EQ(mem.default_checker().doc_len(d), mem.doc_len(d)) << "catch-all ⇒ 整篇总长";
    }
    EXPECT_EQ(mem.field_checker("title").doc_len(0), 1U);
    EXPECT_EQ(mem.field_checker("title").doc_len(1), 2U);

    for (const auto* seg : {&mem, mm.get()}) {
        auto v = seg->multi_view();
        for (const auto& fv : v.fields) {
            if (fv.field_name != "title") continue;
            ASSERT_NE(fv.live, nullptr);
            const auto r = fv.inv->search({"zebra"}, 200, *fv.live);
            EXPECT_EQ(r.size(), 100U);
        }
    }
    auto mv = mem.multi_view();
    auto vv = mm->multi_view();
    const bitcask::search::FieldSegmentView* a = nullptr;
    const bitcask::search::FieldSegmentView* b = nullptr;
    for (const auto& fv : mv.fields) if (fv.field_name == "title") a = &fv;
    for (const auto& fv : vv.fields) if (fv.field_name == "title") b = &fv;
    ASSERT_TRUE(a && b);
    const auto ra = a->inv->search({"zebra", "pony"}, 50, *a->live);
    const auto rb = b->inv->search({"zebra", "pony"}, 50, *b->live);
    ASSERT_EQ(ra.size(), rb.size());
    for (std::size_t i = 0; i < ra.size(); ++i) {
        EXPECT_EQ(ra[i].ord, rb[i].ord);
        EXPECT_EQ(ra[i].score, rb[i].score);
    }
    mm.reset();
    fs::remove_all(dir, ec);
}

}  // namespace
