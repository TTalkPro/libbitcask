// S38：bitcask.meta 里的 Unicode 数据版本记录与重开比对告警。
//
// 为什么需要它：分词结果 = NFKC_Casefold 表 = Unicode 版本 = ICU 版本。
// 换个 ICU 版本打开旧索引，新写入的文档会被切成与老文档不同的 term，两边
// 互相搜不到——而这**没有任何显式症状**，只是召回悄悄少一块。所以建索引
// 时记版本、重开时比对告警。

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/detail/icu_util.hpp>
#include <bitcask/keydir_registry.hpp>
#include <bitcask/meta_file.hpp>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskOptions;

inline bitcask::keydir::KeyDirRegistry& test_registry() {
    static bitcask::keydir::KeyDirRegistry reg;
    return reg;
}

constexpr std::uint8_t kRuntimeIcu = bitcask::text::detail::icu_major_version();
constexpr std::uint8_t kRuntimeUni =
    bitcask::text::detail::unicode_major_version();

class MetaUnicodeVersionTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmpdir_ = fs::temp_directory_path() /
                  ("bc_uniover_" + std::to_string(::getpid()) + "_" +
                   std::to_string(counter_++));
        fs::create_directories(tmpdir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmpdir_, ec);
    }

    [[nodiscard]] bitcask::meta::MetaConfig read() const {
        auto mc = bitcask::meta::read_meta(tmpdir_.string());
        EXPECT_TRUE(mc.has_value()) << (mc ? "" : mc.error().message);
        return mc ? *mc : bitcask::meta::MetaConfig{};
    }

    // 把 meta 里记录的 ICU 主版本改成 forged，模拟"换了台 ICU 版本不同的
    // 机器"。走 write_meta 而非裸改字节——否则 CRC 对不上会先被拒开，
    // 测不到我们要测的那条路径。
    void forge_icu_major(std::uint8_t forged) const {
        auto mc = read();
        mc.icu_major = forged;
        ASSERT_TRUE(bitcask::meta::write_meta(tmpdir_.string(), mc).has_value());
    }

    // 开库并收集告警。
    [[nodiscard]] std::vector<std::string> open_and_collect_warnings(
        bool enable_search) const {
        std::vector<std::string> logs;
        std::mutex mu;
        CaskOptions opts;
        opts.read_write = true;
        opts.enable_search = enable_search;
        opts.log_fn = [&](CaskOptions::LogLevel lvl, std::string_view msg) {
            if (lvl != CaskOptions::LogLevel::kWarn) return;
            std::lock_guard<std::mutex> g(mu);
            logs.emplace_back(msg);
        };
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        EXPECT_TRUE(c.has_value());
        if (c) (*c)->close();
        std::lock_guard<std::mutex> g(mu);
        return logs;
    }

    static bool mentions_unicode_mismatch(const std::vector<std::string>& logs) {
        for (const auto& m : logs) {
            if (m.find("Unicode data version differs") != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    fs::path tmpdir_;
    static inline int counter_ = 0;
};

// 建索引目录时必须记下当前运行时的 ICU / Unicode 主版本。
TEST_F(MetaUnicodeVersionTest, IndexModeRecordsVersionOnCreate) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c.has_value());
    (*c)->close();

    const auto mc = read();
    EXPECT_EQ(mc.mode, bitcask::meta::Mode::kIndex);
    EXPECT_EQ(mc.icu_major, kRuntimeIcu);
    EXPECT_EQ(mc.unicode_major, kRuntimeUni);
    EXPECT_GT(mc.icu_major, 0u) << "记录不该是 0——0 是「未记录」的哨兵";
}

// KV 模式没有文本分析，记了也无从比对；留 0（未记录）语义更干净。
TEST_F(MetaUnicodeVersionTest, KvModeLeavesVersionUnrecorded) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = false;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c.has_value());
    (*c)->close();

    const auto mc = read();
    EXPECT_EQ(mc.mode, bitcask::meta::Mode::kKV);
    EXPECT_EQ(mc.icu_major, 0u);
    EXPECT_EQ(mc.unicode_major, 0u);
}

// 同版本重开：不得有告警（否则每次开库都刷屏，告警很快会被无视）。
TEST_F(MetaUnicodeVersionTest, SameVersionReopenIsSilent) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c.has_value());
    (*c)->close();

    EXPECT_FALSE(mentions_unicode_mismatch(
        open_and_collect_warnings(/*enable_search=*/true)));
}

// 版本不同 → 告警，且**仍然能打开**。跨版本索引可读可写，硬拒开会让
// 「升级发行版的 libicu」变成「所有库打不开」，代价远大于风险。
TEST_F(MetaUnicodeVersionTest, DifferentVersionWarnsButStillOpens) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c.has_value());
    (*c)->close();

    const std::uint8_t forged =
        static_cast<std::uint8_t>(kRuntimeIcu >= 2 ? kRuntimeIcu - 1
                                                   : kRuntimeIcu + 1);
    ASSERT_NO_FATAL_FAILURE(forge_icu_major(forged));

    const auto logs = open_and_collect_warnings(/*enable_search=*/true);
    ASSERT_TRUE(mentions_unicode_mismatch(logs))
        << "ICU 主版本不同必须告警；收到 " << logs.size() << " 条告警";

    // 告警要说清两侧版本，否则用户无从判断该不该重建。
    bool has_both = false;
    for (const auto& m : logs) {
        if (m.find("ICU " + std::to_string(forged)) != std::string::npos &&
            m.find("ICU " + std::to_string(kRuntimeIcu)) != std::string::npos) {
            has_both = true;
        }
    }
    EXPECT_TRUE(has_both) << "告警须同时给出索引侧与运行时侧的版本";
}

// S38 之前建的目录 icu_major == 0（未记录）：无从比对，必须静默，
// 绝不能把「没记录」误报成「版本不匹配」。
TEST_F(MetaUnicodeVersionTest, UnrecordedVersionIsSilent) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c.has_value());
    (*c)->close();

    ASSERT_NO_FATAL_FAILURE(forge_icu_major(0));  // 模拟旧目录
    EXPECT_FALSE(mentions_unicode_mismatch(
        open_and_collect_warnings(/*enable_search=*/true)));
}

// 关键不变式：**任何重写 meta 的路径都不得覆盖建索引时的原始记录**。
// write_meta 也被 v5→v6 懒升级等路径调用；若它在那里取「当前运行时版本」，
// 记录就会被悄悄刷成当次运行的版本，比对从此恒相等、告警永不触发——
// 那等于这个功能整个失效，且失效得毫无声息。
TEST_F(MetaUnicodeVersionTest, RewritingMetaPreservesOriginalRecord) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c.has_value());
    (*c)->close();

    const std::uint8_t forged =
        static_cast<std::uint8_t>(kRuntimeIcu >= 2 ? kRuntimeIcu - 1
                                                   : kRuntimeIcu + 1);
    ASSERT_NO_FATAL_FAILURE(forge_icu_major(forged));

    // 模拟懒升级：读出来、只改纪元、写回去。
    auto mc = read();
    mc.version = 6;
    ASSERT_TRUE(bitcask::meta::write_meta(tmpdir_.string(), mc).has_value());

    const auto after = read();
    EXPECT_EQ(after.version, 6);
    EXPECT_EQ(after.icu_major, forged)
        << "重写 meta 把建索引时的原始记录覆盖掉了——告警将永不触发";
    EXPECT_EQ(after.unicode_major, kRuntimeUni);
}

// 版本字段落在 CRC 覆盖区 [0,14) 内，故位翻转会被检出而非静默读错。
TEST_F(MetaUnicodeVersionTest, VersionBytesAreCrcProtected) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c.has_value());
    (*c)->close();

    const auto path = tmpdir_ / "bitcask.meta";
    std::vector<char> buf(18);
    {
        std::FILE* f = std::fopen(path.string().c_str(), "rb");
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(std::fread(buf.data(), 1, buf.size(), f), buf.size());
        std::fclose(f);
    }
    buf[12] = static_cast<char>(buf[12] ^ 0x01);  // 翻 icu_major 一位，不动 CRC
    {
        std::FILE* f = std::fopen(path.string().c_str(), "wb");
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(std::fwrite(buf.data(), 1, buf.size(), f), buf.size());
        std::fclose(f);
    }

    auto mc = bitcask::meta::read_meta(tmpdir_.string());
    ASSERT_FALSE(mc.has_value()) << "版本字节被改却未被 CRC 检出";
    EXPECT_NE(mc.error().message.find("CRC"), std::string::npos);
}

// 版本记录能往返，且不干扰其余 meta 字段。
TEST_F(MetaUnicodeVersionTest, RoundTripAlongsideOtherFields) {
    bitcask::meta::MetaConfig mc;
    mc.mode = bitcask::meta::Mode::kIndex;
    mc.vector_dim = 128;
    mc.vector_metric = bitcask::meta::VectorMetric::kCosineNormalized;
    mc.vector_engine = bitcask::meta::VectorEngine::kIvfRq;
    mc.version = 6;
    mc.icu_major = 200;      // 刻意取不像真实版本的值，确保读的是这两个字节
    mc.unicode_major = 199;
    ASSERT_TRUE(bitcask::meta::write_meta(tmpdir_.string(), mc).has_value());

    const auto got = read();
    EXPECT_EQ(got.icu_major, 200u);
    EXPECT_EQ(got.unicode_major, 199u);
    EXPECT_EQ(got.vector_dim, 128);
    EXPECT_EQ(got.vector_metric, bitcask::meta::VectorMetric::kCosineNormalized);
    EXPECT_EQ(got.vector_engine, bitcask::meta::VectorEngine::kIvfRq);
    EXPECT_EQ(got.version, 6);
    EXPECT_EQ(got.mode, bitcask::meta::Mode::kIndex);
}


// ===========================================================================
// C1：analyzer 配置指纹（meta 尾段）。与 S38 同一失败形态：换分词配置重开，
// 新旧文档 term 集静默分叉。定长头已无空闲字节，故落在可选尾段（自带 CRC，
// 旧读端透明）。
// ===========================================================================

namespace {
bitcask::text::AnalyzerConfig ngram(std::uint32_t lo, std::uint32_t hi) {
    bitcask::text::AnalyzerConfig a;
    a.type = bitcask::text::AnalyzerType::Ngram;
    a.min_n = lo;
    a.max_n = hi;
    return a;
}

std::uintmax_t meta_size(const fs::path& dir) {
    return fs::file_size(dir / "bitcask.meta");
}
}  // namespace

class MetaAnalyzerFpTest : public MetaUnicodeVersionTest {
protected:
    // 以给定 analyzer 配置开库（带 search_config），收集告警。
    std::vector<std::string> open_with(const bitcask::text::AnalyzerConfig& a,
                                       bool* opened = nullptr) const {
        std::vector<std::string> logs;
        std::mutex mu;
        CaskOptions opts;
        opts.read_write = true;
        opts.enable_search = true;
        bitcask::search::SearchLayerConfig cfg;
        cfg.analyzer_config = a;
        opts.search_config = cfg;
        opts.log_fn = [&](CaskOptions::LogLevel lvl, std::string_view msg) {
            if (lvl != CaskOptions::LogLevel::kWarn) return;
            std::lock_guard<std::mutex> g(mu);
            logs.emplace_back(msg);
        };
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        if (opened) *opened = c.has_value();
        EXPECT_TRUE(c.has_value());
        if (c) (*c)->close();
        std::lock_guard<std::mutex> g(mu);
        return logs;
    }
    static bool mentions_analyzer(const std::vector<std::string>& logs) {
        for (const auto& m : logs) {
            if (m.find("analyzer configuration differs") != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

// 带 search_config 建库 → 记下指纹（非零），meta 为 18B 头 + 9B 尾段。
TEST_F(MetaAnalyzerFpTest, RecordsFingerprintOnCreate) {
    (void)open_with(ngram(2, 3));
    const auto mc = read();
    EXPECT_NE(mc.analyzer_fp, 0u);
    EXPECT_EQ(mc.analyzer_fp, bitcask::text::analyzer_fingerprint(ngram(2, 3)));
    EXPECT_EQ(meta_size(tmpdir_), 27u);
}

// 无 search_config（不分词）/ KV 模式：不记，meta 保持 18 字节——与旧版
// 逐字节同形，旧二进制零感知。
TEST_F(MetaAnalyzerFpTest, NoAnalyzerLeavesMetaAt18Bytes) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;  // 无 search_config
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c.has_value());
    (*c)->close();
    EXPECT_EQ(read().analyzer_fp, 0u);
    EXPECT_EQ(meta_size(tmpdir_), 18u);
}

TEST_F(MetaAnalyzerFpTest, SameConfigReopenIsSilent) {
    (void)open_with(ngram(2, 3));
    EXPECT_FALSE(mentions_analyzer(open_with(ngram(2, 3))));
}

// 换 n 范围 / 换分词器类型 → 告警，且仍能打开（与 S38 同策略：只告警不拒开）。
TEST_F(MetaAnalyzerFpTest, DifferentConfigWarnsButStillOpens) {
    (void)open_with(ngram(2, 3));
    bool opened = false;
    EXPECT_TRUE(mentions_analyzer(open_with(ngram(1, 2), &opened)));
    EXPECT_TRUE(opened);

    bitcask::text::AnalyzerConfig ws;
    ws.type = bitcask::text::AnalyzerType::Whitespace;
    EXPECT_TRUE(mentions_analyzer(open_with(ws)));
}

// 不影响切词的差异不得告警（否则告警很快被无视）：Whitespace 不读 n；
// 停用词表是集合语义；dict_path 是机器相关路径。
TEST_F(MetaAnalyzerFpTest, IrrelevantDifferencesDoNotChangeFingerprint) {
    using bitcask::text::analyzer_fingerprint;
    bitcask::text::AnalyzerConfig a;
    a.type = bitcask::text::AnalyzerType::Whitespace;
    auto b = a;
    b.min_n = 5;
    b.max_n = 9;
    b.enable_stop_words = true;
    EXPECT_EQ(analyzer_fingerprint(a), analyzer_fingerprint(b));

    auto s1 = ngram(2, 3);
    s1.enable_stop_words = true;
    s1.stop_words = {"the", "a", "of"};
    auto s2 = s1;
    s2.stop_words = {"of", "the", "a", "the"};
    s2.dict_path = "/elsewhere/dict";
    EXPECT_EQ(analyzer_fingerprint(s1), analyzer_fingerprint(s2));

    // 停用词只在启用时计入。
    auto s3 = ngram(2, 3);
    s3.stop_words = {"x"};
    EXPECT_EQ(analyzer_fingerprint(s3), analyzer_fingerprint(ngram(2, 3)));

    // 真正影响切词的字段各自改变指纹。
    const auto base = analyzer_fingerprint(ngram(2, 3));
    auto m1 = ngram(2, 3); m1.min_token_length = 2;
    auto m2 = ngram(2, 3); m2.max_token_bytes = 64;
    auto m3 = ngram(2, 3); m3.enable_stemming = true;
    auto m4 = s1;          m4.stop_words.push_back("and");
    EXPECT_NE(analyzer_fingerprint(m1), base);
    EXPECT_NE(analyzer_fingerprint(m2), base);
    EXPECT_NE(analyzer_fingerprint(m3), base);
    EXPECT_NE(analyzer_fingerprint(m4), analyzer_fingerprint(s1));
}

// 旧二进制懒升级重写 meta 会截掉尾段（它只写 18 字节）→ 退化为未记录：
// 必须仍能读、且静默（不能把「没记录」误报成「配置不同」）。
TEST_F(MetaAnalyzerFpTest, TruncatedTailDegradesToUnrecorded) {
    (void)open_with(ngram(2, 3));
    fs::resize_file(tmpdir_ / "bitcask.meta", 18);
    EXPECT_EQ(read().analyzer_fp, 0u);
    EXPECT_FALSE(mentions_analyzer(open_with(ngram(1, 2))));
}

// 尾段损坏（CRC 不符）→ 同样按未记录处理，**不拒开**（诊断信息，非纪元门禁）。
TEST_F(MetaAnalyzerFpTest, CorruptTailIsIgnoredNotFatal) {
    (void)open_with(ngram(2, 3));
    const auto path = tmpdir_ / "bitcask.meta";
    std::vector<char> buf(27);
    {
        std::FILE* f = std::fopen(path.string().c_str(), "rb");
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(std::fread(buf.data(), 1, buf.size(), f), buf.size());
        std::fclose(f);
    }
    buf[20] = static_cast<char>(buf[20] ^ 0x10);  // 翻指纹一位，不动尾段 CRC
    {
        std::FILE* f = std::fopen(path.string().c_str(), "wb");
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(std::fwrite(buf.data(), 1, buf.size(), f), buf.size());
        std::fclose(f);
    }
    auto mc = bitcask::meta::read_meta(tmpdir_.string());
    ASSERT_TRUE(mc.has_value()) << "尾段损坏不应让目录打不开";
    EXPECT_EQ(mc->analyzer_fp, 0u);
}

// 与 S38 同一关键不变式：重写 meta（懒升级 v5→v6 等）不得丢失/覆盖原始指纹。
TEST_F(MetaAnalyzerFpTest, RewritingMetaPreservesFingerprint) {
    (void)open_with(ngram(2, 3));
    auto mc = read();
    const auto fp = mc.analyzer_fp;
    mc.version = 6;
    ASSERT_TRUE(bitcask::meta::write_meta(tmpdir_.string(), mc).has_value());
    EXPECT_EQ(read().analyzer_fp, fp);
    EXPECT_TRUE(mentions_analyzer(open_with(ngram(1, 2))));
}

}  // namespace
