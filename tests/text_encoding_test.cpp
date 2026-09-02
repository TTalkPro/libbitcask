// S38：ICU ucnv 编码转换层。重点在 GB18030——它是这条线的起因：改造前
// 把 GB18030 字节直接喂进索引层是**静默失败**（存得进、搜不出），现在
// 入口能校验（validate_utf8），这里提供把它转对的手段。

#include <gtest/gtest.h>

#include <string>

#include "bitcask/ngram_analyzer.hpp"
#include "bitcask/text_encoding.hpp"
#include "bitcask/text_utils.hpp"

using namespace bitcask::text;

namespace {

// GB18030 双字节区：测 = B2E2，试 = CAD4，中 = D6D0，文 = C4C4
const std::string kGbCeShi("\xB2\xE2\xCA\xD4", 4);          // 测试
// GB18030 四字节区：U+1D11E 𝄞 MUSICAL SYMBOL G CLEF = 94 32 BE 34
const std::string kGb4Byte("\x94\x32\xBE\x34", 4);
const std::string kUtf8CeShi("\xE6\xB5\x8B\xE8\xAF\x95", 6);  // 测试
const std::string kUtf8Clef("\xF0\x9D\x84\x9E", 4);           // 𝄞

}  // namespace

TEST(TextEncoding, EncodingSupportedAndCanonicalName) {
    EXPECT_TRUE(encoding_supported("GB18030"));
    EXPECT_TRUE(encoding_supported("gb18030"));   // 大小写不敏感
    EXPECT_TRUE(encoding_supported("GBK"));
    EXPECT_TRUE(encoding_supported("UTF-8"));
    EXPECT_FALSE(encoding_supported("NoSuchEncoding"));
    EXPECT_FALSE(encoding_supported(nullptr));

    // 别名归一到同一个转换器。
    EXPECT_EQ(canonical_encoding_name("GB18030"),
              canonical_encoding_name("gb18030"));
    EXPECT_FALSE(canonical_encoding_name("GB18030").empty());
    EXPECT_TRUE(canonical_encoding_name("NoSuchEncoding").empty());
}

TEST(TextEncoding, Gb18030TwoByteAndFourByteForms) {
    std::string out;

    // 双字节区。
    ASSERT_EQ(transcode_to_utf8(kGbCeShi, "GB18030", out), TranscodeStatus::kOk);
    EXPECT_EQ(out, kUtf8CeShi);

    // 四字节区（GB18030 相对 GBK 的增量，覆盖 BMP 外码点）。
    ASSERT_EQ(transcode_to_utf8(kGb4Byte, "GB18030", out), TranscodeStatus::kOk);
    EXPECT_EQ(out, kUtf8Clef);

    // 混合。
    ASSERT_EQ(transcode_to_utf8(kGbCeShi + kGb4Byte, "GB18030", out),
              TranscodeStatus::kOk);
    EXPECT_EQ(out, kUtf8CeShi + kUtf8Clef);

    // 转出来的必须是合法 UTF-8——否则等于把问题往下游推。
    EXPECT_TRUE(detail::validate_utf8(out));
}

TEST(TextEncoding, RoundTripIsByteExact) {
    std::string u8;
    ASSERT_EQ(transcode_to_utf8(kGbCeShi + kGb4Byte, "GB18030", u8),
              TranscodeStatus::kOk);

    std::string back;
    ASSERT_EQ(transcode_from_utf8(u8, "GB18030", back), TranscodeStatus::kOk);
    EXPECT_EQ(back, kGbCeShi + kGb4Byte);
}

// 严格模式是本层的默认，也是 S38 的核心纪律：ICU 转换器的**出厂回调是
// SUBSTITUTE**，坏字节会被悄悄换成 U+FFFD 而不报错。那正是要根除的那类失败。
TEST(TextEncoding, StrictModeRejectsMalformedInput) {
    std::string out;

    // 截断的 GB18030 双字节序列（只有前导字节）。
    EXPECT_EQ(transcode_to_utf8(std::string("\xB2", 1), "GB18030", out),
              TranscodeStatus::kMalformedInput);
    EXPECT_TRUE(out.empty());

    // 尾部截断。
    EXPECT_EQ(transcode_to_utf8(kGbCeShi + std::string("\xB2", 1), "GB18030", out),
              TranscodeStatus::kMalformedInput);
    EXPECT_TRUE(out.empty());

    // 宽容模式才做替换，且必须显式要求。
    EXPECT_EQ(transcode_to_utf8(std::string("\xB2", 1), "GB18030", out,
                                /*lenient=*/true),
              TranscodeStatus::kOk);
    EXPECT_EQ(out, "�");
}

TEST(TextEncoding, InvalidUtf8RejectedOnTheWayOut) {
    std::string out;
    // 非法 UTF-8 进 transcode_from_utf8 必须被拒，不能悄悄产出垃圾。
    EXPECT_EQ(transcode_from_utf8(kGbCeShi, "GB18030", out),
              TranscodeStatus::kMalformedInput);
    EXPECT_TRUE(out.empty());
}

TEST(TextEncoding, UnknownEncodingAndEmptyInput) {
    std::string out;
    EXPECT_EQ(transcode_to_utf8("abc", "NoSuchEncoding", out),
              TranscodeStatus::kUnknownEncoding);
    EXPECT_TRUE(out.empty());

    EXPECT_EQ(transcode_to_utf8("", "GB18030", out), TranscodeStatus::kOk);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(transcode_from_utf8("", "GB18030", out), TranscodeStatus::kOk);
    EXPECT_TRUE(out.empty());
}

TEST(TextEncoding, OtherLegacyEncodings) {
    std::string out;
    // Big5「測試」= B4 FA B8 D5
    ASSERT_EQ(transcode_to_utf8(std::string("\xB4\xFA\xB8\xD5", 4), "Big5", out),
              TranscodeStatus::kOk);
    EXPECT_EQ(out, "測試");  // 測試

    // windows-1252：0x93/0x94 是弯引号（Latin-1 里是控制字符，正是两者的分野）
    ASSERT_EQ(transcode_to_utf8(std::string("\x93hi\x94", 4), "windows-1252", out),
              TranscodeStatus::kOk);
    EXPECT_EQ(out, "“hi”");
}

// 端到端：这正是改造前失败的那条路。转码后，GB18030 的中文能被正常切成
// n-gram 并检索；不转码则整段被判非法 UTF-8。
TEST(TextEncoding, TranscodedTextIndexesCorrectly) {
    NgramAnalyzer az(2, 3);

    // 不转码：非法 UTF-8，nfkc_fold 明确回报状态（改造前是静默的）。
    std::string folded;
    EXPECT_EQ(detail::nfkc_fold_checked(kGbCeShi, folded),
              detail::FoldStatus::kInvalidUtf8);

    // 转码后：与直接喂 UTF-8 原文的分词结果完全一致。
    std::string u8;
    ASSERT_EQ(transcode_to_utf8(kGbCeShi, "GB18030", u8), TranscodeStatus::kOk);

    const auto from_gb = az.analyze(u8);
    const auto from_utf8 = az.analyze(kUtf8CeShi);
    EXPECT_EQ(from_gb, from_utf8);
    EXPECT_NE(from_gb.find("测试"), from_gb.end());
}
