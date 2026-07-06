#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

#include "bitcask/index.hpp"
#include "bitcask/text_plugin.hpp"
#include "bitcask/synonym_map.hpp"

using namespace bitcask::text;
using namespace bitcask::search;

namespace {

// S24-B4：docmap + TextPlugin 直连组合，替代原 shim SearchLayer（写法与
// highlighter_test 的 TextHost 一致；on_write 语义等价体见其注释）。
struct TextHost {
    bitcask::index::Index idx;
    TextPlugin plugin;

    explicit TextHost(const TextPluginConfig& cfg)
        : plugin(cfg, idx, idx, idx) {}

    void on_write(std::string_view key, std::uint64_t ord,
                  std::string_view text, std::uint32_t file_id,
                  std::uint64_t offset, std::uint32_t total_sz,
                  std::uint32_t tstamp) {
        idx.put_doc(key, ord,
                    bitcask::index::DocSlot{
                        bitcask::index::DocLoc{.offset   = offset,
                                               .file_id  = file_id,
                                               .total_sz = total_sz},
                        tstamp, /*doc_len=*/0});
        plugin.apply_text(key, ord, text);
    }

    auto search_text(std::string_view q, std::size_t k) const {
        return plugin.search_text(q, k);
    }
    auto search_phrase(std::string_view q, std::size_t k) const {
        return plugin.search_phrase(q, k);
    }
    auto search_near(std::string_view q, std::uint32_t slop,
                     std::size_t k) const {
        return plugin.search_near(q, slop, k);
    }
    auto search_fields(std::string_view q, std::size_t k) const {
        return plugin.search_fields(q, k);
    }
};

TextPluginConfig ws_config(std::shared_ptr<const SynonymMap> sm = nullptr) {
    TextPluginConfig c;
    c.analyzer_config =
        AnalyzerConfig{.type = AnalyzerType::Whitespace};
    c.bm25_params = bitcask::bm25::Bm25Params{1.2F, 0.75F};
    c.synonym_map = std::move(sm);
    return c;
}

}  // namespace

TEST(SynonymMap, AddGroupAndExpand) {
    SynonymMap map;
    map.add_group({"NYC", "New York", "Big Apple"});

    auto r1 = map.expand("NYC");
    EXPECT_EQ(r1.size(), 3u);
    EXPECT_TRUE(std::find(r1.begin(), r1.end(), "NYC") != r1.end());
    EXPECT_TRUE(std::find(r1.begin(), r1.end(), "New York") != r1.end());
    EXPECT_TRUE(std::find(r1.begin(), r1.end(), "Big Apple") != r1.end());

    auto r2 = map.expand("New York");
    EXPECT_EQ(r2.size(), 3u);
}

TEST(SynonymMap, ExpandUnknownTerm) {
    SynonymMap map;
    auto r = map.expand("unknown");
    EXPECT_EQ(r.size(), 0u);
}

TEST(SynonymMap, ExpandTermsDedup) {
    SynonymMap map;
    map.add_group({"NYC", "New York", "Big Apple"});
    map.add_group({"car", "automobile", "vehicle"});

    auto r = map.expand_terms({"NYC", "car"});
    EXPECT_EQ(r.size(), 6u);
}

TEST(SynonymMap, ExpandTermsWithUnknown) {
    SynonymMap map;
    map.add_group({"NYC", "New York"});

    auto r = map.expand_terms({"NYC", "hello"});
    EXPECT_EQ(r.size(), 3u);
    EXPECT_TRUE(std::find(r.begin(), r.end(), "NYC") != r.end());
    EXPECT_TRUE(std::find(r.begin(), r.end(), "New York") != r.end());
    EXPECT_TRUE(std::find(r.begin(), r.end(), "hello") != r.end());
}

TEST(SynonymMap, LoadFromFile) {
    auto path = std::filesystem::temp_directory_path() / "bitcask_synonym_test.txt";
    {
        std::ofstream ofs(path);
        ofs << "NYC, New York, Big Apple\n";
        ofs << "car, automobile\n";
    }

    SynonymMap map;
    map.load_from_file(path.string());

    auto r = map.expand("NYC");
    EXPECT_EQ(r.size(), 3u);

    auto r2 = map.expand("car");
    EXPECT_EQ(r2.size(), 2u);

    std::filesystem::remove(path);
}

TEST(SynonymMap, LoadFromFileEmptyLine) {
    auto path = std::filesystem::temp_directory_path() / "bitcask_synonym_empty.txt";
    {
        std::ofstream ofs(path);
        ofs << "\n";
        ofs << "a, b\n";
    }

    SynonymMap map;
    map.load_from_file(path.string());

    auto r = map.expand("a");
    EXPECT_EQ(r.size(), 2u);

    std::filesystem::remove(path);
}

TEST(TextPluginSynonym, SearchTextWithSynonym) {
    // 基线（无同义词词典）：只命中 1 篇。
    {
        TextHost layer(ws_config());
        layer.on_write("doc1", 0, "nyc is great", 1, 100, 50, 1000);
        layer.on_write("doc2", 1, "automobile is great", 1, 200, 50, 1001);
        auto r = layer.search_text("nyc", 10);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->size(), 1u);
    }
    // S11：同义词词典经 open-time config 注入（不可变）→ 展开命中 2 篇。
    auto sm = std::make_shared<SynonymMap>();
    sm->add_group({"nyc", "automobile"});
    TextHost layer(ws_config(sm));
    layer.on_write("doc1", 0, "nyc is great", 1, 100, 50, 1000);
    layer.on_write("doc2", 1, "automobile is great", 1, 200, 50, 1001);

    auto result_after = layer.search_text("nyc", 10);
    ASSERT_TRUE(result_after.has_value());
    EXPECT_EQ(result_after->size(), 2u);
}

TEST(TextPluginSynonym, SearchTextWithSynonymViaAlias) {
    auto sm = std::make_shared<SynonymMap>();
    sm->add_group({"hi", "hello"});
    TextHost layer(ws_config(sm));

    layer.on_write("doc1", 0, "hello world", 1, 100, 50, 1000);


    auto result = layer.search_text("hi", 10);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 1u);
    EXPECT_EQ(result->at(0).key, "doc1");
}

TEST(TextPluginSynonym, PhraseSearchDoesNotExpand) {
    auto sm = std::make_shared<SynonymMap>();
    sm->add_group({"hi", "hello"});
    TextHost layer(ws_config(sm));

    layer.on_write("doc1", 0, "hello world", 1, 100, 50, 1000);
    layer.on_write("doc2", 1, "hi world", 1, 200, 50, 1001);


    auto result = layer.search_phrase("hi world", 10);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 1u);
    EXPECT_EQ(result->at(0).key, "doc2");
}

TEST(TextPluginSynonym, NearSearchDoesNotExpand) {
    auto sm = std::make_shared<SynonymMap>();
    sm->add_group({"hi", "hello"});
    TextHost layer(ws_config(sm));

    layer.on_write("doc1", 0, "hello world", 1, 100, 50, 1000);
    layer.on_write("doc2", 1, "hi world", 1, 200, 50, 1001);


    auto result = layer.search_near("hi world", 0, 10);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 1u);
    EXPECT_EQ(result->at(0).key, "doc2");
}

TEST(TextPluginSynonym, SearchFieldsWithSynonym) {
    auto sm = std::make_shared<SynonymMap>();
    sm->add_group({"nyc", "automobile"});
    TextHost layer(ws_config(sm));

    layer.on_write("doc1", 0, "nyc is great", 1, 100, 50, 1000);
    layer.on_write("doc2", 1, "automobile is great", 1, 200, 50, 1001);


    auto result = layer.search_fields("nyc", 10);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 2u);
}
