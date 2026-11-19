// S55：分析器配置进 meta（C2 尾段）、按记录打开、ngram 高亮偏移、高亮器区间排序合并。
// 下游反馈：feedbacks/2026-11-19-analyzer-not-recorded-ngram-no-highlight.md（keelson 转 bitcask-viewer）。
#include <gtest/gtest.h>

#include <bitcask/analyzer.hpp>
#include <bitcask/cask.hpp>
#include <bitcask/highlighter.hpp>
#include <bitcask/index_registry.hpp>
#include <bitcask/meta_file.hpp>
#include <bitcask/text_utils.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskError;
using bitcask::CaskOptions;
using bitcask::DocInput;
using bitcask::search::ColdText;
using bitcask::search::SearchLayerConfig;
using bitcask::text::AnalyzerConfig;
using bitcask::text::AnalyzerType;
using bitcask::text::TokenInfo;

bitcask::oki::IndexRegistry& test_registry() {
    static bitcask::oki::IndexRegistry reg;
    return reg;
}

std::span<const std::byte> bytes(std::string_view s) {
    return std::as_bytes(std::span<const char>(s.data(), s.size()));
}

AnalyzerConfig whitespace() {
    AnalyzerConfig a;
    a.type = AnalyzerType::Whitespace;
    return a;
}

class AnalyzerRecordTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() / (std::string("bitcask_s55_") + info->name());
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    CaskOptions options(const AnalyzerConfig& a, bool rw = true, bool from_meta = false) {
        CaskOptions opts;
        opts.read_write = rw;
        opts.enable_search = true;
        SearchLayerConfig cfg;
        cfg.analyzer_config = a;
        cfg.analyzer_from_meta = from_meta;
        opts.search_config = cfg;
        opts.log_fn = [this](CaskOptions::LogLevel lvl, std::string_view msg) {
            if (lvl != CaskOptions::LogLevel::kWarn) return;
            std::lock_guard<std::mutex> g(mu_);
            warns_.emplace_back(msg);
        };
        return opts;
    }

    // 建库、写一篇（+ 可选填充让它重开即冷）、关库。
    void create(const AnalyzerConfig& a, std::string_view text, int filler = 0) {
        auto c = Cask::open(dir_.string(), options(a), &test_registry());
        ASSERT_TRUE(c);
        DocInput d;
        d.text = bytes(text);
        ASSERT_TRUE((*c)->put_doc(bytes("target"), d));
        for (int i = 0; i < filler; ++i) {
            const std::string k = "zz" + std::to_string(i);
            const std::string v = "filler text number " + std::to_string(i);
            ASSERT_TRUE((*c)->put(bytes(k), bytes(v)));
        }
        (*c)->close();
    }

    [[nodiscard]] bitcask::meta::MetaConfig read() const {
        auto mc = bitcask::meta::read_meta(dir_.string());
        EXPECT_TRUE(mc.has_value()) << (mc ? "" : mc.error().message);
        return mc ? *mc : bitcask::meta::MetaConfig{};
    }

    // 模拟 S55 之前建的库：只留 C1 指纹（重写 meta 时不带 C2）。
    void strip_c2() const {
        auto mc = read();
        mc.analyzer.reset();
        ASSERT_TRUE(bitcask::meta::write_meta(dir_.string(), mc).has_value());
        ASSERT_EQ(fs::file_size(dir_ / "bitcask.meta"), 27U);
    }

    [[nodiscard]] bool warned_mismatch() {
        std::lock_guard<std::mutex> g(mu_);
        return std::any_of(warns_.begin(), warns_.end(), [](const std::string& m) {
            return m.find("analyzer configuration differs") != std::string::npos;
        });
    }

    fs::path dir_;
    std::mutex mu_;
    std::vector<std::string> warns_;
};

// ── S55-1 meta C2 ──────────────────────────────────────────────

TEST_F(AnalyzerRecordTest, CreateRecordsFullConfigWithoutDictPath) {
    AnalyzerConfig a;
    a.type = AnalyzerType::Ngram;
    a.min_n = 1;
    a.max_n = 2;
    a.enable_stop_words = true;
    a.stop_words = {"的", "了", "the"};
    a.min_token_length = 2;
    a.max_token_bytes = 64;
    a.enable_stemming = true;
    a.dict_path = "/some/machine/path";
    create(a, "hello world");

    const auto mc = read();
    ASSERT_TRUE(mc.analyzer.has_value());
    const auto& r = *mc.analyzer;
    EXPECT_EQ(r.type, AnalyzerType::Ngram);
    EXPECT_EQ(r.min_n, 1U);
    EXPECT_EQ(r.max_n, 2U);
    EXPECT_TRUE(r.enable_stop_words);
    EXPECT_EQ(r.stop_words, (std::vector<std::string>{"的", "了", "the"}));
    EXPECT_EQ(r.min_token_length, 2U);
    EXPECT_EQ(r.max_token_bytes, 64U);
    EXPECT_TRUE(r.enable_stemming);
    EXPECT_TRUE(r.dict_path.empty()) << "机器相关路径不入 meta";
    EXPECT_EQ(bitcask::text::analyzer_fingerprint(r), mc.analyzer_fp);
}

TEST_F(AnalyzerRecordTest, RewritingMetaKeepsRecord) {
    create(whitespace(), "x");
    auto mc = read();
    mc.version = 6;  // 懒升级等路径：读回 → 改别的字段 → 写回
    ASSERT_TRUE(bitcask::meta::write_meta(dir_.string(), mc).has_value());
    const auto got = read();
    ASSERT_TRUE(got.analyzer.has_value());
    EXPECT_EQ(got.analyzer->type, AnalyzerType::Whitespace);
}

TEST_F(AnalyzerRecordTest, CorruptC2IsTreatedAsUnrecordedButKeepsC1) {
    create(whitespace(), "x");
    const auto fp = read().analyzer_fp;
    const auto path = dir_ / "bitcask.meta";
    std::string raw;
    {
        std::ifstream in(path, std::ios::binary);
        raw.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    ASSERT_GT(raw.size(), 33U);
    raw[32] = static_cast<char>(raw[32] ^ 0x5A);  // C2 payload 第一个字节（类型）
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(raw.data(), static_cast<std::streamsize>(raw.size()));
    }
    const auto mc = read();
    EXPECT_FALSE(mc.analyzer.has_value());
    EXPECT_EQ(mc.analyzer_fp, fp) << "C2 坏了不影响 C1";
}

TEST_F(AnalyzerRecordTest, RecordDisagreeingWithFingerprintIsNotWritten) {
    create(whitespace(), "x");
    auto mc = read();
    mc.analyzer->type = AnalyzerType::Ngram;  // 与 C1 指纹不符
    ASSERT_TRUE(bitcask::meta::write_meta(dir_.string(), mc).has_value());
    EXPECT_EQ(fs::file_size(dir_ / "bitcask.meta"), 27U) << "不一致的记录不写";
    EXPECT_FALSE(read().analyzer.has_value());
}

// ── S55-2 按记录打开 ─────────────────────────────────────────

TEST_F(AnalyzerRecordTest, FromMetaUsesRecordedAnalyzer) {
    create(whitespace(), "the quick brown fox", 0);
    // 选项给的是缺省 ngram，但要求按记录开 ⇒ 实际是 whitespace、不告警不一致。
    auto c = Cask::open(dir_.string(), options(AnalyzerConfig{}, /*rw=*/false, /*from_meta=*/true),
                        &test_registry());
    ASSERT_TRUE(c) << c.error().detail;
    EXPECT_EQ((*c)->options().search_config->analyzer_config.type, AnalyzerType::Whitespace);
    EXPECT_FALSE(warned_mismatch());
    (*c)->close();
}

TEST_F(AnalyzerRecordTest, FromMetaKeepsDictPathFromOptions) {
    create(whitespace(), "x");
    AnalyzerConfig a;
    a.dict_path = "/dict/here";
    auto c = Cask::open(dir_.string(), options(a, false, true), &test_registry());
    ASSERT_TRUE(c);
    EXPECT_EQ((*c)->options().search_config->analyzer_config.dict_path, "/dict/here");
    (*c)->close();
}

TEST_F(AnalyzerRecordTest, FromMetaOnPreS55LibraryRecognisesDefaultConfigs) {
    create(whitespace(), "x");
    strip_c2();
    auto c = Cask::open(dir_.string(), options(AnalyzerConfig{}, false, true), &test_registry());
    ASSERT_TRUE(c) << c.error().detail;
    EXPECT_EQ((*c)->options().search_config->analyzer_config.type, AnalyzerType::Whitespace)
        << "只有 C1 指纹：认出是缺省 whitespace";
    (*c)->close();
}

TEST_F(AnalyzerRecordTest, FromMetaOnPreS55NonDefaultLibraryIsRejected) {
    AnalyzerConfig a;
    a.min_n = 1;
    a.max_n = 2;
    create(a, "x");
    strip_c2();
    auto c = Cask::open(dir_.string(), options(AnalyzerConfig{}, false, true), &test_registry());
    ASSERT_FALSE(c);
    EXPECT_EQ(c.error().kind, CaskError::kInvalidOption);
    EXPECT_NE(c.error().detail.find("does not record its analyzer"), std::string::npos)
        << c.error().detail;
}

TEST_F(AnalyzerRecordTest, WritableOpenBackfillsRecordOnPreS55Library) {
    AnalyzerConfig a;
    a.min_n = 1;
    a.max_n = 2;
    create(a, "x");
    strip_c2();
    {   // 只读打开：不碰 meta
        auto c = Cask::open(dir_.string(), options(a, /*rw=*/false), &test_registry());
        ASSERT_TRUE(c);
        (*c)->close();
        EXPECT_FALSE(read().analyzer.has_value());
    }
    {   // 可写打开、配置与指纹相符 ⇒ 补记
        auto c = Cask::open(dir_.string(), options(a, /*rw=*/true), &test_registry());
        ASSERT_TRUE(c);
        (*c)->close();
        const auto mc = read();
        ASSERT_TRUE(mc.analyzer.has_value());
        EXPECT_EQ(mc.analyzer->min_n, 1U);
        EXPECT_EQ(mc.analyzer->max_n, 2U);
    }
}

TEST_F(AnalyzerRecordTest, MismatchedWritableOpenDoesNotBackfill) {
    create(whitespace(), "x");
    strip_c2();
    auto c = Cask::open(dir_.string(), options(AnalyzerConfig{}, true), &test_registry());
    ASSERT_TRUE(c) << "不一致只告警（C1 既定取舍）";
    (*c)->close();
    EXPECT_TRUE(warned_mismatch());
    EXPECT_FALSE(read().analyzer.has_value()) << "配置不符 ⇒ ⛔ 把错的配置记进去";
}

// ── S55-4 ngram 偏移 ─────────────────────────────────────────

// 每个 token 的偏移落在归一化文本上、切出来正好是 term；term 集与 positions 版逐位一致。
TEST(NgramOffsets, OffsetsSliceNormalizedTextToTheTerm) {
    AnalyzerConfig cfg;  // ngram 2..3
    auto an = bitcask::text::AnalyzerFactory::create(cfg);
    ASSERT_TRUE(an);
    const std::string text = "光栅化着色器 Quick ＦＯＸ jumps，纹理";
    const std::string norm = bitcask::text::detail::nfkc_fold(text);
    const auto offs = an->analyze_with_offsets(norm);
    const auto pos = an->analyze_with_positions(norm);
    ASSERT_FALSE(offs.empty());
    ASSERT_EQ(offs.size(), pos.size());
    for (const auto& [term, infos] : offs) {
        ASSERT_TRUE(pos.count(term)) << term;
        ASSERT_EQ(infos.size(), pos.at(term).second.size()) << term;
        for (std::size_t i = 0; i < infos.size(); ++i) {
            const auto& t = infos[i];
            ASSERT_LT(t.start_byte, t.end_byte) << term;
            ASSERT_LE(t.end_byte, norm.size());
            EXPECT_EQ(norm.substr(t.start_byte, t.end_byte - t.start_byte), term);
            EXPECT_EQ(t.position, pos.at(term).second[i]) << term;
        }
    }
    EXPECT_TRUE(offs.count("着色器"));
    EXPECT_TRUE(offs.count("fox")) << "全角 ＦＯＸ 归一化后小写";
}

TEST(NgramOffsets, StopWordsFilteredLikePositions) {
    AnalyzerConfig cfg;
    cfg.enable_stop_words = true;
    cfg.stop_words = {"着色"};
    auto an = bitcask::text::AnalyzerFactory::create(cfg);
    const auto offs = an->analyze_with_offsets("着色器");
    EXPECT_FALSE(offs.count("着色"));
    EXPECT_TRUE(offs.count("色器"));
}

// ── S55-5 高亮器 ─────────────────────────────────────────────

TEST(HighlighterOrder, TermsAreTaggedRegardlessOfMapOrder) {
    const std::string text = "quick brown fox";
    std::unordered_map<std::string, std::vector<TokenInfo>> q;
    q["fox"] = {TokenInfo{2, 12, 15}};
    q["quick"] = {TokenInfo{0, 0, 5}};
    const auto r = bitcask::search::highlight(text, q, {});
    ASSERT_EQ(r.snippets.size(), 1U);
    EXPECT_EQ(r.snippets[0].text, "<em>quick</em> brown <em>fox</em>");
}

TEST(HighlighterOrder, OverlappingTokensMergeIntoOneTag) {
    const std::string text = "着色器纹理";  // 每个汉字 3 字节
    std::unordered_map<std::string, std::vector<TokenInfo>> q;
    q["着色"] = {TokenInfo{0, 0, 6}};
    q["色器"] = {TokenInfo{0, 3, 9}};
    q["着色器"] = {TokenInfo{0, 0, 9}};
    const auto r = bitcask::search::highlight(text, q, {});
    ASSERT_EQ(r.snippets.size(), 1U);
    // 片段止于最后一个命中的末尾（select_best_fragments 的既有口径），故不带后面的「纹理」。
    EXPECT_EQ(r.snippets[0].text, "<em>着色器</em>");
}

// ── 端到端：ngram 库的高亮（热 / 冷读回）──────────────────────

std::string snippet_of(Cask& c, std::string_view q, ColdText cold) {
    auto r = c.search_text_highlight(q, 10, {}, cold);
    EXPECT_TRUE(r);
    if (!r) return {};
    for (const auto& h : r->hits) {
        if (h.key == "target") {
            std::string all;
            for (const auto& s : h.highlights) all += s.text + "|";
            return all;
        }
    }
    return "<no hit>";
}

TEST_F(AnalyzerRecordTest, NgramLibraryHighlightsHotAndColdLoad) {
    const std::string text = "我们使用光栅化着色器渲染纹理 and a quick fox";
    {
        auto c = Cask::open(dir_.string(), options(AnalyzerConfig{}), &test_registry());
        ASSERT_TRUE(c);
        DocInput d;
        d.text = bytes(text);
        ASSERT_TRUE((*c)->put_doc(bytes("target"), d));
        // 热：本进程写的，原文在 LRU 里
        EXPECT_NE(snippet_of(**c, "着色器", ColdText::kSkip).find("<em>着色器</em>"), std::string::npos);
        const auto two = snippet_of(**c, "quick fox", ColdText::kSkip);
        EXPECT_NE(two.find("<em>quick</em> <em>fox</em>"), std::string::npos) << two;
        (*c)->close();
    }
    // 冷：只读、按记录打开（选项给 whitespace 也无妨），kLoad 读回正文按 ngram 切。
    // 原文缓存设 0 ⇒ 每条命中**确定**是冷的。不靠「写够填充让目标落进封好的段」：那个前提在高负载下
    // 不成立（重开时仍从数据重放、填回 LRU）——S53 的 HighlightColdTest 在 -j16 全量下约半数失败，
    // 改动前的 9.1.0 基线同样如此（见 TASK.md S55 落地记录）。
    auto ro = options(whitespace(), false, true);
    ro.search_config->doc_text_cache_max = 0;
    auto c = Cask::open(dir_.string(), ro, &test_registry());
    ASSERT_TRUE(c);
    EXPECT_EQ((*c)->options().search_config->analyzer_config.type, AnalyzerType::Ngram);
    EXPECT_EQ(snippet_of(**c, "着色器", ColdText::kSkip), "") << "冷 + 缺省：无片段（S53 语义不变）";
    const auto cold = snippet_of(**c, "着色器", ColdText::kLoad);
    EXPECT_NE(cold.find("<em>着色器</em>"), std::string::npos) << cold;
    (*c)->close();
}

}  // namespace

