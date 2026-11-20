// S60 / S61：反馈 2026-11-13 文档能力缺口（锦书）的两项遗留——
//   S60（#1 后半）search_fields_highlight：字段命中归属 + 逐字段片段；
//   S61（#3）update_doc：只改命名字段 / meta 若干键的原子部分更新。
#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskError;
using bitcask::CaskOptions;
using bitcask::DocInput;
using bitcask::DocPatch;
using bitcask::search::SearchLayerConfig;
using bitcask::text::AnalyzerType;

bitcask::oki::IndexRegistry& test_registry() {
    static bitcask::oki::IndexRegistry reg;
    return reg;
}

std::span<const std::byte> bytes(std::string_view s) {
    return std::as_bytes(std::span<const char>(s.data(), s.size()));
}
template <class B>
std::string str(const B& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

CaskOptions search_options(bool read_write = true) {
    CaskOptions opts;
    opts.read_write = read_write;
    opts.enable_search = true;
    SearchLayerConfig cfg;
    cfg.analyzer_config.type = AnalyzerType::Whitespace;
    opts.search_config = cfg;
    return opts;
}

bitcask::meta::MetaFilter eq_filter(const std::string& k, bitcask::meta::MetaValue v) {
    bitcask::meta::MetaFilter f;
    f.conditions.push_back({k, bitcask::meta::MetaOp::Eq, std::move(v), {}});
    return f;
}

std::vector<std::byte> meta_blob(std::vector<bitcask::meta::MetaEntry> es) {
    std::sort(es.begin(), es.end(), [](auto& a, auto& b) { return a.key < b.key; });
    std::vector<std::byte> out;
    bitcask::meta::encode_meta(out, es);
    return out;
}

std::map<std::string, bitcask::meta::MetaValue> decode(std::span<const std::byte> m) {
    std::map<std::string, bitcask::meta::MetaValue> out;
    if (m.empty()) return out;
    auto d = bitcask::meta::decode_meta(m);
    EXPECT_TRUE(d);
    if (d) {
        for (auto& e : *d) out[e.key] = e.value;
    }
    return out;
}

class DocOpsTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() / (std::string("bitcask_s60_") + info->name());
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
        auto c = Cask::open(dir_.string(), search_options(), &test_registry());
        ASSERT_TRUE(c);
        c_ = std::move(*c);
    }
    void TearDown() override {
        if (c_) c_->close();
        c_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    void put_card(std::string_view key, std::string_view body, std::string_view title,
                  std::string_view alias, const std::vector<std::byte>& meta = {}) {
        DocInput d;
        d.text = bytes(body);
        d.fields.emplace_back("title", bytes(title));
        d.fields.emplace_back("alias", bytes(alias));
        d.meta = meta;
        ASSERT_TRUE(c_->put_doc(bytes(key), d));
    }
    fs::path dir_;
    std::unique_ptr<Cask> c_;
};

// ---------------- S60 ----------------

TEST_F(DocOpsTest, FieldSnippetsUseOnlyThatFieldsTerms) {
    put_card("c1", "the rust compiler manages memory without a collector",
             "rust ownership", "crab");
    put_card("c2", "garbage collection reclaims memory", "gc basics", "collector");
    for (int i = 0; i < 5; ++i) put_card("f" + std::to_string(i), "nothing here", "other", "x");

    auto r = c_->search_fields_highlight("title:rust body memory", 10);
    ASSERT_TRUE(r) << r.error().detail;
    std::map<std::string, bitcask::search::SearchFieldHitEx> by_key;
    for (auto& h : *r) by_key[h.key] = h;
    ASSERT_TRUE(by_key.contains("c1"));
    const auto& h = by_key["c1"];
    std::map<std::string, std::string> snip;
    for (const auto& fs : h.field_snippets) {
        std::string all;
        for (const auto& s : fs.snippets) all += s.text + "|";
        snip[fs.field] = all;
    }
    ASSERT_TRUE(snip.contains("title")) << "title 命中应有片段";
    EXPECT_NE(snip["title"].find("<em>rust</em>"), std::string::npos) << snip["title"];
    ASSERT_TRUE(snip.contains("")) << "默认字段命中应有片段";
    EXPECT_NE(snip[""].find("<em>memory</em>"), std::string::npos) << snip[""];
    // 只用该字段的词：默认字段的片段里 rust 不是查询词（title:rust 只限 title），不该被标。
    EXPECT_EQ(snip[""].find("<em>rust</em>"), std::string::npos) << snip[""];
    // 命中与排序同 search_fields_attributed。
    auto base = c_->search_fields_attributed("title:rust body memory", 10);
    ASSERT_TRUE(base);
    ASSERT_EQ(base->size(), r->size());
    for (std::size_t i = 0; i < r->size(); ++i) {
        EXPECT_EQ((*r)[i].key, (*base)[i].key);
        EXPECT_EQ((*r)[i].score, (*base)[i].score);
        EXPECT_EQ((*r)[i].fields, (*base)[i].fields);
    }
}

TEST_F(DocOpsTest, FieldSnippetsWorkForColdDocsAndTags) {
    put_card("c1", "plain body text", "zebra crossing", "stripes");
    for (int i = 0; i < 5; ++i) put_card("f" + std::to_string(i), "nothing", "other", "x");
    c_->close();
    c_.reset();
    auto c = Cask::open(dir_.string(), search_options(), &test_registry());
    ASSERT_TRUE(c);
    c_ = std::move(*c);  // 新开进程：原文 LRU 全冷，片段照样从存储读回
    bitcask::search::HighlightOptions opts;
    opts.pre_tag = "[";
    opts.post_tag = "]";
    auto r = c_->search_fields_highlight("title:zebra alias:stripes", 10, opts);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->size(), 1U);
    ASSERT_EQ((*r)[0].field_snippets.size(), 2U);
    for (const auto& fs : (*r)[0].field_snippets) {
        ASSERT_FALSE(fs.snippets.empty()) << fs.field;
        const auto& t = fs.snippets[0].text;
        if (fs.field == "title") {
            EXPECT_NE(t.find("[zebra]"), std::string::npos) << t;
        }
        if (fs.field == "alias") {
            EXPECT_NE(t.find("[stripes]"), std::string::npos) << t;
        }
    }
}

TEST_F(DocOpsTest, FieldSnippetsRespectFilter) {
    put_card("a", "body", "zebra one", "x", meta_blob({{"kind", std::string("animal")}}));
    put_card("b", "body", "zebra two", "x", meta_blob({{"kind", std::string("road")}}));
    auto f = eq_filter("kind", std::string("road"));
    auto r = c_->search_fields_highlight("title:zebra", 10, {}, &f);
    ASSERT_TRUE(r) << r.error().detail;
    ASSERT_EQ(r->size(), 1U);
    EXPECT_EQ((*r)[0].key, "b");
    EXPECT_EQ((*r)[0].field_snippets.size(), 1U);
}

// ---------------- S61 ----------------

TEST_F(DocOpsTest, UpdateDocPatchesFieldsAndMetaOnly) {
    put_card("c1", "the body stays", "old title", "alias one",
             meta_blob({{"x", std::int64_t{1}}, {"y", std::string("keep")}, {"z", true}}));
    auto before = c_->get_doc_owned(bytes("c1"));
    ASSERT_TRUE(before);

    DocPatch p;
    p.set_fields.emplace_back("title", bytes("new title"));
    p.set_fields.emplace_back("tags", bytes("fresh tag"));
    p.remove_fields.push_back("alias");
    p.set_meta.push_back({"x", std::int64_t{42}});
    p.set_meta.push_back({"w", 2.5});
    p.remove_meta.push_back("z");
    auto ord = c_->update_doc(bytes("c1"), p);
    ASSERT_TRUE(ord) << ord.error().detail;

    auto after = c_->get_doc_owned(bytes("c1"));
    ASSERT_TRUE(after);
    EXPECT_EQ(after->ord, *ord);
    EXPECT_GT(after->ord, before->ord);
    EXPECT_EQ(str(after->value), "the body stays") << "正文不动";
    ASSERT_EQ(after->fields.size(), 2U);
    EXPECT_EQ(after->fields[0].name, "title");  // 原顺序保留，替换就地
    EXPECT_EQ(str(after->fields[0].value), "new title");
    EXPECT_EQ(after->fields[1].name, "tags");   // 新字段追加
    EXPECT_EQ(str(after->fields[1].value), "fresh tag");
    const auto m = decode(after->meta);
    ASSERT_EQ(m.size(), 3U);
    EXPECT_EQ(std::get<std::int64_t>(m.at("x")), 42);
    EXPECT_EQ(std::get<std::string>(m.at("y")), "keep");
    EXPECT_EQ(std::get<double>(m.at("w")), 2.5);
    EXPECT_FALSE(m.contains("z"));

    // 索引跟着新版本走。
    auto s1 = c_->search_fields("title:new", 10);
    ASSERT_TRUE(s1);
    EXPECT_EQ(s1->hits.size(), 1U);
    auto s2 = c_->search_fields("title:old", 10);
    ASSERT_TRUE(s2);
    EXPECT_TRUE(s2->hits.empty());
    auto s3 = c_->search_fields("alias:alias", 10);
    ASSERT_TRUE(s3);
    EXPECT_TRUE(s3->hits.empty()) << "删掉的字段不再可检索";
    auto s4 = c_->search_fields("tags:fresh", 10);
    ASSERT_TRUE(s4);
    EXPECT_EQ(s4->hits.size(), 1U);
    auto mf = eq_filter("x", std::int64_t{42});
    auto s5 = c_->search_text("body", 10, &mf);
    ASSERT_TRUE(s5);
    EXPECT_EQ(s5->hits.size(), 1U) << "meta 过滤看到新值";

    // 重开后不变。
    c_->close();
    c_.reset();
    auto c = Cask::open(dir_.string(), search_options(), &test_registry());
    ASSERT_TRUE(c);
    c_ = std::move(*c);
    auto again = c_->get_doc_owned(bytes("c1"));
    ASSERT_TRUE(again);
    EXPECT_EQ(again->ord, *ord);
    EXPECT_EQ(decode(again->meta).size(), 3U);
}

TEST_F(DocOpsTest, UpdateDocKeepsVectorlessExpiryAndRejectsBadPatches) {
    DocInput d;
    d.text = bytes("body");
    d.expiry_at = 4102444800ULL;  // 2100-01-01
    ASSERT_TRUE(c_->put_doc(bytes("k"), d));
    DocPatch p;
    p.set_meta.push_back({"a", std::int64_t{1}});
    ASSERT_TRUE(c_->update_doc(bytes("k"), p));
    auto v = c_->get_doc_view(bytes("k"));
    ASSERT_TRUE(v);
    EXPECT_EQ(v->expiry_at, 4102444800ULL) << "过期时刻原样保留";

    EXPECT_EQ(c_->update_doc(bytes("missing"), p).error().kind, CaskError::kNotFound);

    DocPatch bad1;
    bad1.set_fields.emplace_back("", bytes("x"));
    EXPECT_EQ(c_->update_doc(bytes("k"), bad1).error().kind, CaskError::kInvalidOption);
    DocPatch bad2;
    bad2.set_fields.emplace_back("t", bytes("x"));
    bad2.remove_fields.push_back("t");
    EXPECT_EQ(c_->update_doc(bytes("k"), bad2).error().kind, CaskError::kInvalidOption);
    DocPatch bad3;
    bad3.set_meta.push_back({"m", std::int64_t{1}});
    bad3.set_meta.push_back({"m", std::int64_t{2}});
    EXPECT_EQ(c_->update_doc(bytes("k"), bad3).error().kind, CaskError::kInvalidOption);
    DocPatch bad4;
    bad4.set_meta.push_back({"m", std::int64_t{1}});
    bad4.remove_meta.push_back("m");
    EXPECT_EQ(c_->update_doc(bytes("k"), bad4).error().kind, CaskError::kInvalidOption);
}

// 向量库：update_doc 后向量原样保留（cosine 归一化后存，读回再写不漂）、向量检索照常。
TEST(DocOpsVector, UpdateDocKeepsVector) {
    const auto dir = fs::temp_directory_path() / "bitcask_s61_vec";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    auto opts = search_options();
    opts.vector_dim = 4;
    {
        auto c = Cask::open(dir.string(), opts, &test_registry());
        ASSERT_TRUE(c) << c.error().detail;
        const float v[4] = {1.0F, 2.0F, 3.0F, 4.0F};
        DocInput d;
        d.text = bytes("vector doc");
        d.vector = std::span<const float>(v, 4);
        ASSERT_TRUE((*c)->put_doc(bytes("v"), d));
        auto before = (*c)->get_doc_owned(bytes("v"));
        ASSERT_TRUE(before);
        ASSERT_EQ(before->vector.size(), 4U);
        DocPatch p;
        p.set_fields.emplace_back("title", bytes("added"));
        ASSERT_TRUE((*c)->update_doc(bytes("v"), p));
        auto after = (*c)->get_doc_owned(bytes("v"));
        ASSERT_TRUE(after);
        ASSERT_EQ(after->vector.size(), 4U);
        for (int i = 0; i < 4; ++i) {
            EXPECT_NEAR(after->vector[static_cast<std::size_t>(i)],
                        before->vector[static_cast<std::size_t>(i)], 1e-6F) << i;
        }
        auto hits = (*c)->search_vector(std::span<const float>(v, 4), 1);
        ASSERT_TRUE(hits) << hits.error().detail;
        ASSERT_EQ(hits->hits.size(), 1U);
        EXPECT_EQ(hits->hits[0].key, "v");
        (*c)->close();
    }
    fs::remove_all(dir, ec);
}

// 并发：N 个线程各自对同一文档的不同 meta 键做 update_doc，互不覆盖（原子读-改-写）。
TEST_F(DocOpsTest, ConcurrentUpdatesDoNotLoseEachOther) {
    put_card("c", "body", "t", "a");
    constexpr int kThreads = 4;
    constexpr int kEach = 25;
    std::atomic<int> failures{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < kEach; ++i) {
                DocPatch p;
                p.set_meta.push_back({"t" + std::to_string(t), std::int64_t{i}});
                for (;;) {
                    auto r = c_->update_doc(bytes("c"), p);
                    if (r) break;
                    if (r.error().kind != CaskError::kConditionFailed) {
                        ++failures;
                        break;
                    }
                }
            }
        });
    }
    for (auto& th : ts) th.join();
    EXPECT_EQ(failures.load(), 0);
    auto v = c_->get_doc_owned(bytes("c"));
    ASSERT_TRUE(v);
    const auto m = decode(v->meta);
    ASSERT_EQ(m.size(), static_cast<std::size_t>(kThreads)) << "每个线程的键都在（无丢更新）";
    for (int t = 0; t < kThreads; ++t) {
        EXPECT_EQ(std::get<std::int64_t>(m.at("t" + std::to_string(t))), kEach - 1);
    }
    ASSERT_EQ(v->fields.size(), 2U);
}

TEST_F(DocOpsTest, UpdateDocReadOnlyHandle) {
    put_card("c", "body", "t", "a");
    c_->close();
    c_.reset();
    auto ro = Cask::open(dir_.string(), search_options(/*read_write=*/false), &test_registry());
    ASSERT_TRUE(ro);
    DocPatch p;
    p.set_meta.push_back({"a", std::int64_t{1}});
    EXPECT_EQ((*ro)->update_doc(bytes("c"), p).error().kind, CaskError::kReadOnly);
    (*ro)->close();
}

}  // namespace
