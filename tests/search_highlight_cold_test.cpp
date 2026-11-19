// S53：冷文档高亮——原文 LRU 只在写入 / 建索引时填，新开进程里每条命中都冷 ⇒ 缺省（kSkip）
// 片段为空；ColdText::kLoad 从存储读回正文补片段，与 LRU 命中同一条路（同分析器、同标签）。
// 下游反馈：feedbacks/2026-11-19-highlight-cold-docs-never-get-snippets.md（bitcask-viewer）。
#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskOptions;
using bitcask::DocInput;
using bitcask::search::ColdText;
using bitcask::search::HighlightOptions;
using bitcask::search::SearchLayerConfig;
using bitcask::text::AnalyzerType;

bitcask::oki::IndexRegistry& test_registry() {
    static bitcask::oki::IndexRegistry reg;
    return reg;
}

std::span<const std::byte> bytes(std::string_view s) {
    return std::as_bytes(std::span<const char>(s.data(), s.size()));
}

// 「冷」要靠规模：小库重开时从数据重放，重放会填原文 LRU（文档是热的）；写够一批之后目标文档
// 落进封好的段，重开直接载段、不重放 ⇒ 冷（实测 3 篇重开全热，1500 篇重开全冷）。
// 所以先写目标文档，再写一批填充。
constexpr int kFiller = 1500;

void write_filler(Cask& c) {
    for (int i = 0; i < kFiller; ++i) {
        const std::string k = "zz" + std::to_string(i);
        const std::string v = "filler text number " + std::to_string(i);
        ASSERT_TRUE(c.put(bytes(k), bytes(v)));
    }
}

CaskOptions search_options() {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig cfg;
    cfg.analyzer_config.type = AnalyzerType::Whitespace;
    opts.search_config = cfg;
    return opts;
}

// key ⇒ 拼起来的全部片段（没有片段 ⇒ ""）。
std::map<std::string, std::string> snippets_by_key(Cask& c, std::string_view q, ColdText cold,
                                                   const HighlightOptions& opts = {}) {
    std::map<std::string, std::string> out;
    auto r = c.search_text_highlight(q, 10, opts, cold);
    EXPECT_TRUE(r);
    if (!r) return out;
    for (const auto& h : r->hits) {
        std::string all;
        for (const auto& s : h.highlights) all += s.text + "|";
        out[h.key] = all;
    }
    return out;
}

class HighlightColdTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() / (std::string("bitcask_hl_cold_") + info->name());
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    // 三种文档：正文、纯 KV 写入、只有命名字段（正文空）。
    void write_and_close() {
        auto c = Cask::open(dir_.string(), search_options(), &test_registry());
        ASSERT_TRUE(c);
        DocInput d1;
        d1.text = bytes("the quick brown fox jumps");
        ASSERT_TRUE((*c)->put_doc(bytes("d1"), d1));
        ASSERT_TRUE((*c)->put(bytes("k2"), bytes("lazy dog watches the fox")));
        DocInput f3;
        f3.fields.emplace_back("title", bytes("a fox in the title"));
        ASSERT_TRUE((*c)->put_doc(bytes("f3"), f3));
        write_filler(**c);
        (*c)->close();
    }
    fs::path dir_;
};

TEST_F(HighlightColdTest, ReopenedLibraryHasNoSnippetsUnlessLoaded) {
    write_and_close();
    auto c = Cask::open(dir_.string(), search_options(), &test_registry());
    ASSERT_TRUE(c);

    // 缺省：每条都冷 ⇒ 命中照常、片段为空（原行为不变）。
    const auto skip = snippets_by_key(**c, "fox", ColdText::kSkip);
    ASSERT_EQ(skip.size(), 3U);
    for (const auto& [k, s] : skip) EXPECT_EQ(s, "") << k;
    // 三参版 ≙ kSkip。
    auto legacy = (*c)->search_text_highlight("fox", 10);
    ASSERT_TRUE(legacy);
    for (const auto& h : legacy->hits) EXPECT_TRUE(h.highlights.empty()) << h.key;

    // kLoad：读回正文补片段，三种文档都有、包住查询词。
    const auto load = snippets_by_key(**c, "fox", ColdText::kLoad);
    ASSERT_EQ(load.size(), 3U);
    EXPECT_NE(load.at("d1").find("<em>fox</em>"), std::string::npos) << load.at("d1");
    EXPECT_NE(load.at("k2").find("<em>fox</em>"), std::string::npos) << "纯 KV 写入：正文即 value";
    EXPECT_NE(load.at("f3").find("<em>fox</em>"), std::string::npos) << "正文空 ⇒ 第一个命名字段";

    // 读回的原文 ⛔ 回填 LRU：再查一次缺省仍然没有片段。
    const auto again = snippets_by_key(**c, "fox", ColdText::kSkip);
    for (const auto& [k, s] : again) EXPECT_EQ(s, "") << k;

    // 标签 / 片段数这些选项对冷路径同样生效。
    HighlightOptions opts;
    opts.pre_tag = "[[";
    opts.post_tag = "]]";
    const auto tagged = snippets_by_key(**c, "fox", ColdText::kLoad, opts);
    EXPECT_NE(tagged.at("d1").find("[[fox]]"), std::string::npos) << tagged.at("d1");
    (*c)->close();
}

// 热文档（本进程写的，LRU 里有）：kLoad 与 kSkip 给出同样的片段（冷路径只补空的）。
TEST_F(HighlightColdTest, HotSnippetsAreUntouchedByLoad) {
    auto c = Cask::open(dir_.string(), search_options(), &test_registry());
    ASSERT_TRUE(c);
    DocInput d;
    d.text = bytes("hot fox in memory");
    ASSERT_TRUE((*c)->put_doc(bytes("h1"), d));
    (*c)->flush_index();
    const auto skip = snippets_by_key(**c, "fox", ColdText::kSkip);
    const auto load = snippets_by_key(**c, "fox", ColdText::kLoad);
    ASSERT_EQ(skip.size(), 1U);
    EXPECT_NE(skip.at("h1").find("<em>fox</em>"), std::string::npos);
    EXPECT_EQ(skip, load);
    (*c)->close();
}

// 冷路径与 LRU 命中走同一条切片路：同一篇文档热 / 冷两次的片段逐字相同。
TEST_F(HighlightColdTest, ColdSnippetsMatchHotOnes) {
    std::string hot;
    {
        auto c = Cask::open(dir_.string(), search_options(), &test_registry());
        ASSERT_TRUE(c);
        DocInput d;
        d.text = bytes("Ｆｕｌｌwidth text with a fox and another fox near the end of a long sentence");
        ASSERT_TRUE((*c)->put_doc(bytes("w1"), d));
        (*c)->flush_index();
        hot = snippets_by_key(**c, "fox", ColdText::kSkip).at("w1");  // 先取热片段（填充会把它挤出 LRU）
        write_filler(**c);
        (*c)->close();
    }
    ASSERT_NE(hot, "");
    auto c = Cask::open(dir_.string(), search_options(), &test_registry());
    ASSERT_TRUE(c);
    EXPECT_EQ(snippets_by_key(**c, "fox", ColdText::kSkip).at("w1"), "") << "前置：重开后是冷的";
    EXPECT_EQ(snippets_by_key(**c, "fox", ColdText::kLoad).at("w1"), hot);
    (*c)->close();
}

}  // namespace
