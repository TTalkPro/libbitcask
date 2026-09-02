#include <gtest/gtest.h>

#include <unicode/bytestream.h>
#include <unicode/normalizer2.h>
#include <unicode/stringpiece.h>
#include <unicode/ustring.h>

#include "bitcask/analyzer.hpp"
#include "bitcask/text_utils.hpp"
#include "bitcask/cjk_detect.hpp"
#include "bitcask/ngram_analyzer.hpp"
#include "bitcask/whitespace_analyzer.hpp"

using namespace bitcask::text;
using detail::is_cjk;

// ===========================================================================
// CJK Detection
// ===========================================================================

TEST(CjkDetect, BasicHan) {
    EXPECT_TRUE(is_cjk(U'中'));
    EXPECT_TRUE(is_cjk(U'文'));
    EXPECT_TRUE(is_cjk(U'京'));
}

TEST(CjkDetect, Hangul) {
    EXPECT_TRUE(is_cjk(U'한'));
    EXPECT_TRUE(is_cjk(U'국'));
}

TEST(CjkDetect, HiraganaKatakana) {
    EXPECT_TRUE(is_cjk(U'あ'));   // Hiragana
    EXPECT_TRUE(is_cjk(U'ア'));   // Katakana
}

TEST(CjkDetect, LatinNotCjk) {
    EXPECT_FALSE(is_cjk(U'A'));
    EXPECT_FALSE(is_cjk(U'z'));
    EXPECT_FALSE(is_cjk(U'0'));
}

TEST(CjkDetect, AsciiNotCjk) {
    EXPECT_FALSE(is_cjk(0x20));    // space
    EXPECT_FALSE(is_cjk(0x2E));    // '.'
}

// ===========================================================================
// AnalyzerFactory
// ===========================================================================

TEST(AnalyzerFactory, NgramDefault) {
    auto a = AnalyzerFactory::create(AnalyzerConfig{});
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->type(), AnalyzerType::Ngram);
    auto* ng = dynamic_cast<NgramAnalyzer*>(a.get());
    ASSERT_NE(ng, nullptr);
    EXPECT_EQ(ng->min_n(), 2);
    EXPECT_EQ(ng->max_n(), 3);
}

TEST(AnalyzerFactory, Whitespace) {
    auto a = AnalyzerFactory::create(AnalyzerConfig{.type = AnalyzerType::Whitespace});
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->type(), AnalyzerType::Whitespace);
}

TEST(AnalyzerFactory, InvalidConfigReturnsNull) {
    auto a = AnalyzerFactory::create(AnalyzerConfig{
        .type = AnalyzerType::Ngram, .min_n = 0, .max_n = 3});
    EXPECT_EQ(a, nullptr);

    a = AnalyzerFactory::create(AnalyzerConfig{
        .type = AnalyzerType::Ngram, .min_n = 5, .max_n = 2});
    EXPECT_EQ(a, nullptr);
}

// ===========================================================================
// NgramAnalyzer — CJK
// ===========================================================================

TEST(NgramAnalyzer, ChineseBigram) {
    NgramAnalyzer a(2, 2);
    auto tfs = a.analyze("北京市");

    // "北京市" (3 chars) → bigrams: "北京", "京市"
    EXPECT_EQ(tfs.size(), 2u);
    EXPECT_EQ(tfs.at("北京"), 1u);
    EXPECT_EQ(tfs.at("京市"), 1u);
}

TEST(NgramAnalyzer, ChineseBigramTrigram) {
    NgramAnalyzer a(2, 3);
    auto tfs = a.analyze("北京市");

    // bigrams: "北京", "京市"  |  trigrams: "北京市"
    EXPECT_EQ(tfs.size(), 3u);
    EXPECT_EQ(tfs.at("北京"), 1u);
    EXPECT_EQ(tfs.at("京市"), 1u);
    EXPECT_EQ(tfs.at("北京市"), 1u);
}

TEST(NgramAnalyzer, ChineseRepeatedChar) {
    NgramAnalyzer a(2, 2);
    auto tfs = a.analyze("哈哈哈");

    // bigrams: "哈哈" x2
    EXPECT_EQ(tfs.at("哈哈"), 2u);
}

TEST(NgramAnalyzer, EmptyInput) {
    NgramAnalyzer a(2, 3);
    auto tfs = a.analyze("");
    EXPECT_TRUE(tfs.empty());
}

TEST(NgramAnalyzer, SingleCjkChar) {
    NgramAnalyzer a(2, 3);
    auto tfs = a.analyze("中");
    // 单个 CJK 字符无法生成 bigram（min_n=2）
    EXPECT_TRUE(tfs.empty());
}

TEST(NgramAnalyzer, CjkPunctAsSeparator) {
    NgramAnalyzer a(2, 2);
    auto tfs = a.analyze("你好，世界");

    // "，" (U+FF0C fullwidth comma) splits CJK run:
    // "你好" → bigram "你好"
    // "世界" → bigram "世界"
    EXPECT_EQ(tfs.size(), 2u);
    EXPECT_EQ(tfs.at("你好"), 1u);
    EXPECT_EQ(tfs.at("世界"), 1u);
}

// ===========================================================================
// NgramAnalyzer — Latin
// ===========================================================================

TEST(NgramAnalyzer, LatinWhitespace) {
    NgramAnalyzer a(2, 3);
    auto tfs = a.analyze("hello world");

    EXPECT_EQ(tfs.at("hello"), 1u);
    EXPECT_EQ(tfs.at("world"), 1u);
}

TEST(NgramAnalyzer, CaseFold) {
    NgramAnalyzer a(2, 3);
    auto tfs = a.analyze("Hello WORLD");

    EXPECT_EQ(tfs.at("hello"), 1u);
    EXPECT_EQ(tfs.at("world"), 1u);
}

TEST(NgramAnalyzer, LatinRepeated) {
    NgramAnalyzer a(2, 3);
    auto tfs = a.analyze("foo foo bar");

    EXPECT_EQ(tfs.at("foo"), 2u);
    EXPECT_EQ(tfs.at("bar"), 1u);
}

// ===========================================================================
// NgramAnalyzer — Mixed CJK + Latin
// ===========================================================================

TEST(NgramAnalyzer, MixedText) {
    NgramAnalyzer a(2, 2);
    auto tfs = a.analyze("北京hello上海");

    // CJK run "北京" → "北京", CJK run "上海" → "上海"
    // Latin "hello" → "hello"
    EXPECT_EQ(tfs.at("北京"), 1u);
    EXPECT_EQ(tfs.at("hello"), 1u);
    EXPECT_EQ(tfs.at("上海"), 1u);
}

// ===========================================================================
// WhitespaceAnalyzer
// ===========================================================================

TEST(WhitespaceAnalyzer, Basic) {
    WhitespaceAnalyzer a;
    auto tfs = a.analyze("hello world foo");

    EXPECT_EQ(tfs.at("hello"), 1u);
    EXPECT_EQ(tfs.at("world"), 1u);
    EXPECT_EQ(tfs.at("foo"), 1u);
}

TEST(WhitespaceAnalyzer, CaseFold) {
    WhitespaceAnalyzer a;
    auto tfs = a.analyze("Hello WORLD");

    EXPECT_EQ(tfs.at("hello"), 1u);
    EXPECT_EQ(tfs.at("world"), 1u);
}

TEST(WhitespaceAnalyzer, Empty) {
    WhitespaceAnalyzer a;
    auto tfs = a.analyze("");
    EXPECT_TRUE(tfs.empty());
}

TEST(WhitespaceAnalyzer, CjkNotSegmented) {
    WhitespaceAnalyzer a;
    auto tfs = a.analyze("北京市");

    EXPECT_EQ(tfs.size(), 1u);
    EXPECT_EQ(tfs.at("北京市"), 1u);
}

// ===========================================================================
// Stop Words
// ===========================================================================

TEST(NgramAnalyzer, StopWordsDisabledByDefault) {
    NgramAnalyzer a(2, 3, false, {});
    auto tfs = a.analyze("this is a test");
    EXPECT_NE(tfs.find("this"), tfs.end());
    EXPECT_NE(tfs.find("is"), tfs.end());
}

TEST(NgramAnalyzer, StopWordsEnabledFiltersEnglish) {
    NgramAnalyzer a(2, 3, true, {});
    auto tfs = a.analyze("this is a test of the system");

    EXPECT_EQ(tfs.find("this"), tfs.end());
    EXPECT_EQ(tfs.find("is"), tfs.end());
    EXPECT_EQ(tfs.find("a"), tfs.end());
    EXPECT_EQ(tfs.find("of"), tfs.end());
    EXPECT_EQ(tfs.find("the"), tfs.end());

    EXPECT_NE(tfs.find("test"), tfs.end());
    EXPECT_NE(tfs.find("system"), tfs.end());
}

TEST(NgramAnalyzer, StopWordsFiltersChinese) {
    NgramAnalyzer a(2, 3, true, {});
    auto tfs = a.analyze("我是一个北京人");

    EXPECT_EQ(tfs.find("我"), tfs.end());
    EXPECT_EQ(tfs.find("是"), tfs.end());

    EXPECT_NE(tfs.find("北京"), tfs.end());
}

TEST(NgramAnalyzer, StopWordsCustomList) {
    NgramAnalyzer a(2, 3, true, {"bad", "term"});
    auto tfs = a.analyze("this bad term is good");

    EXPECT_EQ(tfs.find("bad"), tfs.end());
    EXPECT_EQ(tfs.find("term"), tfs.end());
    EXPECT_NE(tfs.find("this"), tfs.end());
    EXPECT_NE(tfs.find("good"), tfs.end());
}

TEST(AnalyzerFactory, StopWordsThroughConfig) {
    auto a = AnalyzerFactory::create(AnalyzerConfig{
        .type = AnalyzerType::Ngram,
        .min_n = 2,
        .max_n = 3,
        .enable_stop_words = true,
    });
    ASSERT_TRUE(a);

    auto tfs = a->analyze("the cat is on the mat");
    EXPECT_EQ(tfs.find("the"), tfs.end());
    EXPECT_EQ(tfs.find("is"), tfs.end());
    EXPECT_EQ(tfs.find("on"), tfs.end());
    EXPECT_NE(tfs.find("cat"), tfs.end());
    EXPECT_NE(tfs.find("mat"), tfs.end());
}

// S9.8：min_token_length 过滤短拉丁词。
TEST(WhitespaceAnalyzer, MinTokenLengthFiltersShortLatin) {
    WhitespaceAnalyzer a(3);
    auto tfs = a.analyze("a of cat hello");
    EXPECT_EQ(tfs.find("a"), tfs.end());     // 1 codepoint → 过滤
    EXPECT_EQ(tfs.find("of"), tfs.end());    // 2 → 过滤
    EXPECT_NE(tfs.find("cat"), tfs.end());   // 3 → 保留
    EXPECT_NE(tfs.find("hello"), tfs.end()); // 5 → 保留
}

// S9.8：默认 min_token_length=1 不过滤（向后兼容）。
TEST(WhitespaceAnalyzer, DefaultKeepsShortTokens) {
    WhitespaceAnalyzer a;
    auto tfs = a.analyze("a of cat");
    EXPECT_NE(tfs.find("a"), tfs.end());
    EXPECT_NE(tfs.find("of"), tfs.end());
    EXPECT_NE(tfs.find("cat"), tfs.end());
}

// S9.8 关键：min_token_length 只作用于拉丁整词，CJK n-gram 不受影响。
// 否则 min>=2/3 会删光中文 bi-gram 索引。
TEST(NgramAnalyzer, MinTokenLengthDoesNotAffectCjkNgrams) {
    NgramAnalyzer a(2, 3, false, {}, 3);  // min_token_length=3
    auto tfs = a.analyze("北京 a of");
    EXPECT_NE(tfs.find("北京"), tfs.end());  // CJK bi-gram（2 codepoint）必须保留
    EXPECT_EQ(tfs.find("a"), tfs.end());     // 拉丁短词过滤
    EXPECT_EQ(tfs.find("of"), tfs.end());
}

// =========================================================================
// P2.5：nfkc_fold ASCII 快路径语义对拍
// =========================================================================

TEST(NfkcFold, AsciiFastPathEqualsLowercase) {
    using bitcask::text::detail::nfkc_fold;
    EXPECT_EQ(nfkc_fold("Hello, World! 123"), "hello, world! 123");
    EXPECT_EQ(nfkc_fold("ABCxyz"), "abcxyz");
    EXPECT_EQ(nfkc_fold("already lower"), "already lower");
    EXPECT_EQ(nfkc_fold(""), "");
    // 全部 ASCII 可打印字符：除 A-Z 外不变。
    std::string all;
    for (char c = 0x20; c < 0x7F; ++c) all.push_back(c);
    auto folded = nfkc_fold(all);
    ASSERT_EQ(folded.size(), all.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        char expect = (all[i] >= 'A' && all[i] <= 'Z')
                          ? static_cast<char>(all[i] - 'A' + 'a') : all[i];
        EXPECT_EQ(folded[i], expect) << "i=" << i;
    }
}

TEST(NfkcFold, NonAsciiPathUnchanged) {
    using bitcask::text::detail::nfkc_fold;
    // 全角 → 半角 + casefold（NFKC_Casefold 经典行为，走 ICU 慢路径）。
    EXPECT_EQ(nfkc_fold("ＨＥＬＬＯ"), "hello");
    EXPECT_EQ(nfkc_fold("Ｃａｆé"), "café");
    EXPECT_EQ(nfkc_fold("北京"), "北京");
    // 混合（含非 ASCII → 整串走 ICU，ASCII 部分行为一致）。
    EXPECT_EQ(nfkc_fold("Hello北京World"), "hello北京world");
}

TEST(ToCodepoints, AsciiFastPathOffsets) {
    using bitcask::text::detail::to_codepoints;
    auto cps = to_codepoints("a北b");
    ASSERT_EQ(cps.size(), 3u);
    EXPECT_EQ(cps[0].cp, U'a');
    EXPECT_EQ(cps[0].byte_off, 0u);
    EXPECT_EQ(cps[0].byte_len, 1u);
    EXPECT_EQ(cps[1].cp, U'北');
    EXPECT_EQ(cps[1].byte_off, 1u);
    EXPECT_EQ(cps[1].byte_len, 3u);
    EXPECT_EQ(cps[2].cp, U'b');
    EXPECT_EQ(cps[2].byte_off, 4u);
    EXPECT_EQ(cps[2].byte_len, 1u);
}

// =========================================================================
// P2.5b：CJK 恒等快路径
// =========================================================================

namespace {
// UTF-8 编码单码点（测试辅助）。
std::string encode_utf8(char32_t cp) {
    std::string s;
    if (cp < 0x80) {
        s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return s;
}

// ICU oracle（绕过我们的快路径，直接问库）。S38 前这里是 utf8proc。
std::string nfkc_oracle(std::string_view input) {
    if (input.empty()) return {};
    UErrorCode ec = U_ZERO_ERROR;
    const auto* n2 = icu::Normalizer2::getNFKCCasefoldInstance(ec);
    if (U_FAILURE(ec) || n2 == nullptr) return {};
    std::string out;
    icu::StringByteSink<std::string> sink(&out);
    n2->normalizeUTF8(0,
                      icu::StringPiece(input.data(),
                                       static_cast<std::int32_t>(input.size())),
                      sink, nullptr, ec);
    if (U_FAILURE(ec)) return {};
    return out;
}
}  // namespace

// 表成员穷举验证：nfkc_casefold_inert 标记的每个码点，经 ICU
// NFKC_Casefold 后必须逐字节不变。表与 Unicode 数据不符即此测试红。
TEST(NfkcInert, TableOracleExhaustive) {
    using bitcask::text::detail::nfkc_casefold_inert;
    std::size_t checked = 0;
    for (char32_t cp = 0x80; cp <= 0xFFFF; ++cp) {
        if (!nfkc_casefold_inert(cp)) continue;
        auto u = encode_utf8(cp);
        ASSERT_EQ(nfkc_oracle(u), u) << "cp=U+" << std::hex << static_cast<int>(cp);
        ++checked;
    }
    EXPECT_GT(checked, 27000u);  // CJK 基本区+扩展 A+标点
}

// 黑盒对拍：从「表成员 ∪ 回退字符」混合字母表生成随机串，
// nfkc_fold（含快路径）必须与 ICU oracle 逐串一致。
TEST(NfkcInert, RandomizedAgainstOracle) {
    using bitcask::text::detail::nfkc_fold;
    const std::string alphabet[] = {
        "中", "文", "搜", "索", "引", "擎",          // 快路径成员
        "a", "B", "z", "9", " ", ",", ".",           // ASCII（含大写）
        "、", "。", "《", "》", "—",                  // 恒等标点
        "，", "！", "Ａ", "…", "é", "　",             // 回退触发（全角/分解/附标）
        // S38：**会与邻居组合**的起始字符。旧表（identity + ccc==0）把它们
        // 判成 inert，快路径原样透传，于是 U+1100 U+1161 没有合成 U+AC00。
        // 字母表里加上它们，随机串就能撞出那条路径。
        "\u1100", "\u1161", "\u11A8",               // 谚文 jamo（可合成 가/각）
        "\u09C7", "\u09BE",                         // 孟加拉元音符（可合成 U+09CB）
        "\u0061", "\u0301",                         // a + 组合锐音符 → á
    };
    std::uint64_t seed = 23;
    auto next = [&seed] {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return seed >> 33;
    };
    for (int iter = 0; iter < 3000; ++iter) {
        std::string s;
        auto n = next() % 24;
        for (std::uint64_t i = 0; i < n; ++i) {
            s += alphabet[next() % (sizeof(alphabet) / sizeof(alphabet[0]))];
        }
        ASSERT_EQ(nfkc_fold(s), nfkc_oracle(s)) << "s=" << s;
    }
}

// 定向用例：目标语料形态命中快路径；全角标点正确回退折叠。
TEST(NfkcInert, TargetedCases) {
    using bitcask::text::detail::nfkc_fold;
    EXPECT_EQ(nfkc_fold("北京GPU加速测试, 性能提升."),
              "北京gpu加速测试, 性能提升.");
    EXPECT_EQ(nfkc_fold("中文iPhone测试"), "中文iphone测试");
    EXPECT_EQ(nfkc_fold("《标题》、正文。"), "《标题》、正文。");
    EXPECT_EQ(nfkc_fold("全角，逗号"), "全角,逗号");      // 回退路径折叠
    EXPECT_EQ(nfkc_fold("ＧＰＵ测试"), "gpu测试");        // 全角字母回退折叠
}

// S38 回归：**会与邻居组合的起始字符不得进 inert 表**。
//
// 旧表的判据只有「NFKC_Casefold 恒等 + ccc == 0」，漏了第三条「不与邻居组合」。
// 于是 U+1100/U+1161 这类 ccc 均为 0 的谚文 jamo 双双被判 inert，上下文无关的
// 快路径把它们原样透传——同一个韩文词，预组合写法与分解写法产出不同 term，
// 互相搜不到。换成 ICU 的 Normalizer2::isInert（含第三条）后此测试才成立。
TEST(NfkcInert, ComposableStartersAreNotInert) {
    using bitcask::text::detail::nfkc_casefold_inert;
    using bitcask::text::detail::nfkc_fold;

    // 表层面：这些码点必须被排除在快路径之外。
    EXPECT_FALSE(nfkc_casefold_inert(0x1100));  // 谚文 CHOSEONG KIYEOK
    EXPECT_FALSE(nfkc_casefold_inert(0x1161));  // 谚文 JUNGSEONG A
    EXPECT_FALSE(nfkc_casefold_inert(0x09C7));  // 孟加拉 VOWEL SIGN E
    EXPECT_FALSE(nfkc_casefold_inert(0x09BE));  // 孟加拉 VOWEL SIGN AA
    EXPECT_FALSE(nfkc_casefold_inert(0x00E9));  // é（可再与组合符组合）

    // 行为层面：必须真的合成。
    EXPECT_EQ(nfkc_fold("\u1100\u1161"), "\uAC00");           // 가
    EXPECT_EQ(nfkc_fold("\u1100\u1161\u11A8"), "\uAC01");     // 각
    EXPECT_EQ(nfkc_fold("\u09C7\u09BE"), "\u09CB");
    EXPECT_EQ(nfkc_fold("\u0061\u0301"), "\u00E1");           // a + ́ → á

    // 预组合写法与分解写法必须归一到同一串（否则索引互相搜不到）。
    EXPECT_EQ(nfkc_fold("\u1100\u1161"), nfkc_fold("\uAC00"));
}

// S38：非法 UTF-8 必须**有信号**。
//
// 改造前有两种静默失败：utf8proc 路径下整段文本变空（该文档零 term），
// 而直接信任 ICU 的话它对非法字节原样透传（整段黏成一个乱码 term）。
// 两者都不产生任何错误信息。现在统一为：状态回报 + U+FFFD 替换。
TEST(NfkcFold, InvalidUtf8IsReported) {
    using bitcask::text::detail::FoldStatus;
    using bitcask::text::detail::nfkc_fold_checked;

    std::string out;

    // GB18030 的「测试」——合法 GBK，非法 UTF-8。
    EXPECT_EQ(nfkc_fold_checked(std::string("\xB2\xE2\xCA\xD4"), out),
              FoldStatus::kInvalidUtf8);
    EXPECT_EQ(out, "\uFFFD\uFFFD\uFFFD\uFFFD");  // 逐字节替换，不吞后续

    // 截断的多字节序列。
    EXPECT_EQ(nfkc_fold_checked(std::string("\xE6\xB5"), out),
              FoldStatus::kInvalidUtf8);

    // 孤立续字节。
    EXPECT_EQ(nfkc_fold_checked(std::string("a\x80" "b"), out),
              FoldStatus::kInvalidUtf8);
    EXPECT_EQ(out, "a\uFFFDb");

    // 合法输入不得被误判。
    EXPECT_EQ(nfkc_fold_checked("北京GPU测试", out), FoldStatus::kOk);
    EXPECT_EQ(out, "北京gpu测试");
    EXPECT_EQ(nfkc_fold_checked("", out), FoldStatus::kOk);
    EXPECT_TRUE(out.empty());
}

// S38：手写严格解码器 vs ICU。decode_one 是分词热路径上每码点都要过的函数，
// 且是本次自己实现的（不再用 utf8proc_iterate），必须逐码点 + 逐非法序列对拍。
TEST(Utf8Decode, StrictAgainstIcuOracle) {
    using bitcask::text::detail::decode_one;
    using bitcask::text::detail::validate_utf8;

    // ICU 的合法性判据：u_strFromUTF8 对病态输入置 U_INVALID_CHAR_FOUND。
    auto icu_valid = [](std::string_view sv) {
        UErrorCode ec = U_ZERO_ERROR;
        std::int32_t need = 0;
        u_strFromUTF8(nullptr, 0, &need, sv.data(),
                      static_cast<std::int32_t>(sv.size()), &ec);
        return ec == U_BUFFER_OVERFLOW_ERROR || U_SUCCESS(ec);
    };

    // (a) 全部标量值往返：编码后解回来必须是原码点，且吃掉全部字节。
    for (char32_t cp = 0; cp <= 0x10FFFF; ++cp) {
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        const auto u = encode_utf8(cp);
        ASSERT_FALSE(u.empty()) << "cp=" << static_cast<std::uint32_t>(cp);
        const auto [got, n] = decode_one(u);
        ASSERT_EQ(n, u.size()) << "cp=" << static_cast<std::uint32_t>(cp);
        ASSERT_EQ(got, cp);
    }

    // (b) 全部 1 字节与 2 字节序列穷举对拍合法性。
    for (unsigned b0 = 0; b0 < 0x100; ++b0) {
        const std::string s(1, static_cast<char>(b0));
        EXPECT_EQ(validate_utf8(s), icu_valid(s)) << "b0=" << b0;
    }
    for (unsigned b0 = 0x80; b0 < 0x100; ++b0) {
        for (unsigned b1 = 0; b1 < 0x100; ++b1) {
            std::string s;
            s.push_back(static_cast<char>(b0));
            s.push_back(static_cast<char>(b1));
            ASSERT_EQ(validate_utf8(s), icu_valid(s))
                << "b0=" << b0 << " b1=" << b1;
        }
    }

    // (c) 3 字节序列抽样（覆盖 overlong 与代理区两个经典陷阱）。
    for (unsigned b0 = 0xE0; b0 <= 0xEF; ++b0) {
        for (unsigned b1 = 0; b1 < 0x100; ++b1) {
            for (unsigned b2 = 0x80; b2 <= 0xBF; b2 += 0x0D) {
                std::string s;
                s.push_back(static_cast<char>(b0));
                s.push_back(static_cast<char>(b1));
                s.push_back(static_cast<char>(b2));
                ASSERT_EQ(validate_utf8(s), icu_valid(s))
                    << "b0=" << b0 << " b1=" << b1 << " b2=" << b2;
            }
        }
    }

    // (d) 定向：经典病态序列一律非法。
    const char* bad[] = {
        "\xC0\x80",          // overlong NUL
        "\xC1\xBF",          // overlong
        "\xE0\x80\x80",      // overlong
        "\xED\xA0\x80",      // 代理半区 U+D800
        "\xF0\x80\x80\x80",  // overlong
        "\xF5\x80\x80\x80",  // > U+10FFFF
        "\xF8\x88\x80\x80\x80",  // 5 字节形式（已废止）
        "\x80",              // 孤立续字节
        "\xBF",
        "\xFE", "\xFF",
    };
    for (const auto* p : bad) {
        EXPECT_FALSE(validate_utf8(p)) << "seq=" << p;
    }
}

// S31(下游反馈):max_token_bytes——超长 token(长 URL/模板块噪声)源头
// 丢弃,pos 仍递增(位置语义同 min 过滤);0 = 不限。
TEST(AnalyzerMaxTokenBytes, NgramAndWhitespaceDropOversized) {
    const std::string monster(1500, 'x');
    const std::string text = "aa " + monster + " bb";
    for (auto type : {AnalyzerType::Ngram, AnalyzerType::Whitespace}) {
        AnalyzerConfig c;
        c.type = type;
        auto an = AnalyzerFactory::create(c);
        ASSERT_NE(an, nullptr);
        auto tf = an->analyze_with_positions(text);
        EXPECT_EQ(tf.count(monster), 0u) << static_cast<int>(type);
        ASSERT_EQ(tf.count("aa"), 1u);
        ASSERT_EQ(tf.count("bb"), 1u);
        // pos 语义:aa=0,monster 占位=1,bb=2(丢词不塌缩位置)。
        EXPECT_EQ(tf["aa"].second[0], 0u);
        EXPECT_EQ(tf["bb"].second[0], 2u);
        // tf-only 路径同语义。
        auto tf2 = an->analyze(text);
        EXPECT_EQ(tf2.count(monster), 0u);
        EXPECT_EQ(tf2.count("aa"), 1u);
        // 0 = 不限(回退旧行为)。
        AnalyzerConfig c0 = c;
        c0.max_token_bytes = 0;
        auto an0 = AnalyzerFactory::create(c0);
        auto tf0 = an0->analyze_with_positions(text);
        EXPECT_EQ(tf0.count(monster), 1u);
    }
}

// ===========================================================================
// T22-4a：analyze()（tf-only 覆写）与 analyze_with_positions() 的对拍
//
// S29-8 的注释声称两版「term 集与 tf 值逐位一致」。该不变量是**索引路径**
// （positions 版，写入倒排）与 **BOW 查询路径**（tf 版，search_text）的一致
// 性前提：一旦分叉，查询算出的 term 集与索引里的对不上 → 评分错误且静默。
// 归并前两版各写一份过滤逻辑，此断言全靠复制粘贴维护；T22 把它变成结构
// 保证（共享 ngram_collect + materialize_and_filter），本组测试则把它变成
// **可执行断言**——即使将来有人重新拆开两版，这里也会立刻抓到。
// ===========================================================================

namespace {

// tf 版与 positions 版必须产出同一 term 集，且 tf == positions.size()。
void ExpectNgramTfMatchesPositions(const NgramAnalyzer& a,
                                   std::string_view text,
                                   std::string_view case_name) {
    const auto tfs = a.analyze(text);
    const auto tpm = a.analyze_with_positions(text);

    EXPECT_EQ(tfs.size(), tpm.size()) << "term 集大小分叉 @ " << case_name;
    for (const auto& [term, tf] : tfs) {
        auto it = tpm.find(term);
        ASSERT_NE(it, tpm.end())
            << "tf 版有而 positions 版无: '" << term << "' @ " << case_name;
        EXPECT_EQ(tf, it->second.first)
            << "tf 值分叉: '" << term << "' @ " << case_name;
        // positions 版自身的一致性：tf 必须等于记录的位置数。
        EXPECT_EQ(tf, it->second.second.size())
            << "tf 与 positions 数不符: '" << term << "' @ " << case_name;
    }
    for (const auto& [term, data] : tpm) {
        EXPECT_EQ(tfs.count(term), 1u)
            << "positions 版有而 tf 版无: '" << term << "' @ " << case_name;
    }
}

}  // namespace

TEST(NgramAnalyzer, TfMatchesPositionsAcrossInputShapes) {
    NgramAnalyzer a(2, 3);
    // 覆盖 ngram_tokenize 的每条分支：CJK run / 拉丁 run / 空白 / CJK 标点 /
    // ASCII 标点 / 混排 / 重复 n-gram（tf > 1）/ 单字 / 纯标点。
    const std::string_view cases[] = {
        "北京市",
        "哈哈哈哈",                    // 重复 → tf > 1
        "hello world",
        "北京市hello世界",             // CJK/拉丁混排
        "北京，上海。广州",            // CJK 标点分隔
        "a,b.c!d?e",                   // ASCII 标点
        "中",                          // 单 CJK 字（短于 min_n）
        "，。！",                      // 纯标点
        "  多  空格  混排  test  ",
        "",                            // 空输入
        "ab cd ab cd ab",              // 拉丁重复 → tf > 1
    };
    for (auto text : cases) {
        ExpectNgramTfMatchesPositions(a, text, text);
    }
}

TEST(NgramAnalyzer, TfMatchesPositionsWithStopWords) {
    // 停用词过滤在 materialize_and_filter 内，两版共享——但历史上是各写一遍。
    NgramAnalyzer a(2, 2, /*enable_stop_words=*/true,
                    {"the", "北京"}, /*min_token_length=*/1);
    const std::string_view cases[] = {
        "the quick brown fox",
        "北京市上海",
        "the the the",       // 全部被过滤 → 两版都应为空
    };
    for (auto text : cases) {
        ExpectNgramTfMatchesPositions(a, text, text);
    }
}

TEST(NgramAnalyzer, TfMatchesPositionsWithLengthFilters) {
    // min_token_length / max_token_bytes 是最容易单边改错的两个门槛
    // （S9.8 / S31）——原两版各写一遍同样的 if。
    for (std::uint32_t min_len : {1u, 3u, 5u}) {
        for (std::uint32_t max_bytes : {0u, 4u, 16u}) {
            NgramAnalyzer a(2, 3, /*enable_stop_words=*/false, {},
                            min_len, max_bytes);
            const std::string label = "min_len=" + std::to_string(min_len) +
                                      ",max_bytes=" + std::to_string(max_bytes);
            const std::string_view cases[] = {
                "a bb ccc dddd eeeee",
                "short verylongtokenhere x",
                "北京市 hello 世界 test",
            };
            for (auto text : cases) {
                ExpectNgramTfMatchesPositions(a, text, label);
            }
        }
    }
}
