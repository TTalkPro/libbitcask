#include <gtest/gtest.h>

#include "bitcask/index.hpp"
#include "bitcask/text_plugin.hpp"
#include "bitcask/highlighter.hpp"

#include <string_view>

using namespace bitcask;
using namespace bitcask::search;

namespace {

text::TextPluginConfig whitespace_config() {
    text::TextPluginConfig c;
    c.analyzer_config = text::AnalyzerConfig{
        .type = text::AnalyzerType::Whitespace};
    c.bm25_params = bm25::Bm25Params{1.2F, 0.75F};
    return c;
}

// S24-B4：docmap（宿主行）+ TextPlugin 直连组合，替代原 shim SearchLayer
// ——on_write(key, ord, text, file_id, offset, total_sz, tstamp) 等价体：
// 先落 docmap 行（doc_len 由 apply_text 分析后回填），再跑单文本核心。
struct TextHost {
    index::Index idx;
    text::TextPlugin plugin;

    explicit TextHost(const text::TextPluginConfig& cfg)
        : plugin(cfg, idx, idx, idx) {}

    void on_write(std::string_view key, std::uint64_t ord,
                  std::string_view text, std::uint32_t file_id,
                  std::uint64_t offset, std::uint32_t total_sz,
                  std::uint32_t tstamp) {
        idx.put_doc(key, ord,
                    index::DocSlot{index::DocLoc{.offset   = offset,
                                                 .file_id  = file_id,
                                                 .total_sz = total_sz},
                                   tstamp, /*doc_len=*/0});
        plugin.apply_text(key, ord, text);
    }

    auto search_text_highlight(std::string_view query, std::size_t k,
                               const HighlightOptions& opts = {}) const {
        return plugin.search_text_highlight(query, k, opts);
    }
};

}  // namespace

TEST(Highlighter, BasicEnglish) {
    TextHost layer(whitespace_config());
    layer.on_write("key1", 0, "hello world foo bar", 1, 100, 50, 1000);

    auto result = layer.search_text_highlight("foo", 10);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);

    auto& hit = result->at(0);
    EXPECT_EQ(hit.key, "key1");
    EXPECT_FALSE(hit.highlights.empty());
    EXPECT_TRUE(hit.highlights[0].text.find("<em>foo</em>") != std::string::npos);
}

TEST(Highlighter, MultipleTerms) {
    TextHost layer(whitespace_config());
    layer.on_write("key1", 0, "the quick brown fox jumps over the lazy dog", 1, 100, 50, 1000);

    auto result = layer.search_text_highlight("quick fox", 10);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);

    auto& hit = result->at(0);
    ASSERT_FALSE(hit.highlights.empty());
    bool has_quick = hit.highlights[0].text.find("<em>quick</em>") != std::string::npos;
    bool has_fox = hit.highlights[0].text.find("<em>fox</em>") != std::string::npos;
    EXPECT_TRUE(has_quick || has_fox) << "text: " << hit.highlights[0].text;
}

TEST(Highlighter, NoMatch) {
    TextHost layer(whitespace_config());
    layer.on_write("key1", 0, "hello world", 1, 100, 50, 1000);

    auto result = layer.search_text_highlight("xyz", 10);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->empty());
}

TEST(Highlighter, FragmentSize) {
    TextHost layer(whitespace_config());
    std::string long_text = "hello world foo bar baz qux quux corge grault garply waldo "
                           "fred plugh xyzzy thud hoge fuga piyo";
    layer.on_write("key1", 0, long_text, 1, 100, 50, 1000);

    HighlightOptions opts;
    opts.fragment_size = 30;
    opts.max_fragments = 1;

    auto result = layer.search_text_highlight("foo", 10, opts);
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->empty());

    EXPECT_LE(result->at(0).highlights[0].text.size(), opts.fragment_size + 64);
}

TEST(Highlighter, MultipleFragments) {
    TextHost layer(whitespace_config());
    layer.on_write("key1", 0, "foo bar foo baz foo qux", 1, 100, 50, 1000);

    HighlightOptions opts;
    opts.fragment_size = 15;
    opts.max_fragments = 3;

    auto result = layer.search_text_highlight("foo", 10, opts);
    ASSERT_TRUE(result.has_value());

    EXPECT_GE(result->at(0).highlights.size(), 1u);
}

TEST(Highlighter, SearchTextHighlightIntegration) {
    TextHost layer(whitespace_config());

    layer.on_write("doc1", 0, "hello world", 1, 100, 50, 1000);
    layer.on_write("doc2", 1, "hello bitcask search", 1, 200, 50, 1001);
    layer.on_write("doc3", 2, "world of search", 1, 300, 50, 1002);

    auto result = layer.search_text_highlight("hello search", 10);
    ASSERT_TRUE(result.has_value());

    bool found_doc1 = false;
    bool found_doc2 = false;
    for (auto& hit : *result) {
        if (hit.key == "doc1") {
            found_doc1 = true;
            EXPECT_FALSE(hit.highlights.empty());
        }
        if (hit.key == "doc2") {
            found_doc2 = true;
            EXPECT_FALSE(hit.highlights.empty());
        }
    }
    EXPECT_TRUE(found_doc1 || found_doc2);
}

// 回归 S9.20：单次出现的词不应产出多个相同片段。
// 修复前 select_best_fragments 每轮在不变的 range 集上选同一窗口，
// 产出 max_fragments(默认3) 个完全相同的片段。
TEST(Highlighter, NoDuplicateFragmentsForSingleOccurrence) {
    TextHost layer(whitespace_config());
    layer.on_write("key1", 0, "hello world foo bar", 1, 100, 50, 1000);

    auto result = layer.search_text_highlight("world", 10);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ(result->at(0).highlights.size(), 1u);  // 只 1 个片段，不重复
}

// 远距两次出现应产出 2 个不同片段（确认去重未退化成永远只 1 个）。
TEST(Highlighter, TwoFarApartOccurrencesGiveTwoFragments) {
    TextHost layer(whitespace_config());
    std::string text = "world ";
    for (int i = 0; i < 40; ++i) text += "xpad ";  // ~200 字节填充 > fragment_size(120)
    text += "world tail";
    layer.on_write("key1", 0, text, 1, 100, 50, 1000);

    auto result = layer.search_text_highlight("world", 10);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);
    const auto& hl = result->at(0).highlights;
    ASSERT_EQ(hl.size(), 2u);
    EXPECT_NE(hl[0].text, hl[1].text);  // 两个片段必须不同
}