// S57：bosun 的两份反馈（2026-11-19）——
//   ① jieba 词典路径不对时 cppjieba XCHECK 直接 abort 整个进程：open 来不及返回错误
//      （feedbacks/2026-11-19-jieba-missing-dict-aborts-process.md）；
//   ② needs_merge 只看上一个提交点的 v4 清单快照，本次打开之后的死字节看不见，
//      status 报着 95% 死、needs_merge 仍 false，重开才认
//      （feedbacks/2026-11-19-needs-merge-blind-to-dead-bytes-since-commit-point.md）。
#include <gtest/gtest.h>

#include <bitcask/analyzer.hpp>
#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>
#include <bitcask/jieba_analyzer.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <span>
#include <string>
#include <string_view>

#ifndef BITCASK_JIEBA_DICT_DIR
#define BITCASK_JIEBA_DICT_DIR "_deps/cppjieba-src/dict"
#endif

namespace {

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskError;
using bitcask::CaskOptions;
using bitcask::search::SearchLayerConfig;
using bitcask::text::AnalyzerConfig;
using bitcask::text::AnalyzerFactory;
using bitcask::text::AnalyzerType;

bitcask::oki::IndexRegistry& test_registry() {
    static bitcask::oki::IndexRegistry reg;
    return reg;
}

std::span<const std::byte> bytes(std::string_view s) {
    return std::as_bytes(std::span<const char>(s.data(), s.size()));
}

class S57Test : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() / (std::string("bitcask_s57_") + info->name());
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

CaskOptions jieba_options(std::string dict_path) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig cfg;
    cfg.analyzer_config.type = AnalyzerType::Jieba;
    cfg.analyzer_config.dict_path = std::move(dict_path);
    opts.search_config = cfg;
    return opts;
}

// ---- ① jieba 词典缺失 ----

TEST_F(S57Test, JiebaMissingDictFileIsReported) {
    using bitcask::text::jieba_missing_dict_file;
    EXPECT_EQ(jieba_missing_dict_file(BITCASK_JIEBA_DICT_DIR), "") << "真词典目录全在";
    EXPECT_EQ(jieba_missing_dict_file("/nonexistent"), "/nonexistent/jieba.dict.utf8");
    EXPECT_EQ(jieba_missing_dict_file(""), "/jieba.dict.utf8") << "空串（下游漏填）";
    // 只有第一个文件 ⇒ 报第二个。
    std::ofstream(dir_ / "jieba.dict.utf8") << "x 1 n\n";
    EXPECT_EQ(jieba_missing_dict_file(dir_.string()), (dir_ / "hmm_model.utf8").string());
    // 同名的是目录 ⇒ 也算缺（ifstream 在 Linux 上能「打开」目录）。
    fs::create_directories(dir_ / "hmm_model.utf8");
    EXPECT_EQ(jieba_missing_dict_file(dir_.string()), (dir_ / "hmm_model.utf8").string());
}

TEST_F(S57Test, FactoryReturnsNullInsteadOfAbort) {
    AnalyzerConfig c;
    c.type = AnalyzerType::Jieba;
    c.dict_path = "/nonexistent";
    EXPECT_EQ(AnalyzerFactory::create(c), nullptr) << "进程还活着才走得到这一行";
    c.dict_path = BITCASK_JIEBA_DICT_DIR;
    EXPECT_NE(AnalyzerFactory::create(c), nullptr);
}

TEST_F(S57Test, OpenWithBadDictPathFailsCleanly) {
    for (const std::string& bad : {std::string("/nonexistent"), std::string()}) {
        auto c = Cask::open(dir_.string(), jieba_options(bad), &test_registry());
        ASSERT_FALSE(c) << "dict_path=\"" << bad << "\" 应当打开失败";
        EXPECT_EQ(c.error().kind, CaskError::kInvalidOption);
        EXPECT_NE(c.error().detail.find(bad + "/jieba.dict.utf8"), std::string::npos)
            << c.error().detail;
    }
    // 对照：真词典目录照常开。
    auto c = Cask::open(dir_.string(), jieba_options(BITCASK_JIEBA_DICT_DIR), &test_registry());
    ASSERT_TRUE(c) << c.error().detail;
    ASSERT_TRUE((*c)->put(bytes("k"), bytes("我来到北京邮电大学")));
    (*c)->close();
}

// ---- ② needs_merge 用实时账本 ----

CaskOptions kv_options(bool search) {
    CaskOptions opts;
    opts.read_write = true;
    if (search) {
        opts.enable_search = true;
        SearchLayerConfig cfg;
        cfg.analyzer_config.type = AnalyzerType::Whitespace;
        opts.search_config = cfg;
    }
    return opts;
}

std::string doc(int i, int rev) {
    std::string s = "task " + std::to_string(i) + " rev" + std::to_string(rev);
    for (int j = 0; j < 20; ++j) s += " lorem ipsum dolor";
    return s;
}

void write_revs(Cask& c, int from, int to) {
    for (int v = from; v <= to; ++v) {
        for (int i = 1; i <= 50; ++i) {
            ASSERT_TRUE(c.put(bytes("k" + std::to_string(i)), bytes(doc(i, v))));
        }
    }
}

// status 里过了缺省碎片阈值（frag_threshold 40）的已封口文件名。
std::set<std::string> fragmented_sealed(Cask& c) {
    std::set<std::string> out;
    const auto st = c.status();
    std::uint32_t max_id = 0;
    for (const auto& f : st.files) max_id = std::max(max_id, f.file_id);
    for (const auto& f : st.files) {
        if (f.file_id != max_id && f.fragmented >= 40) out.insert(f.filename);
    }
    return out;
}

class NeedsMergeLive : public S57Test, public ::testing::WithParamInterface<bool> {};

TEST_P(NeedsMergeLive, SeesDeadBytesAfterReopen) {
    const auto opts = kv_options(GetParam());
    {
        auto c = Cask::open(dir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        write_revs(**c, 1, 1);
        (*c)->close();
    }
    auto c = Cask::open(dir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    EXPECT_FALSE((*c)->needs_merge().needs) << "刚重开：全活";
    write_revs(**c, 2, 21);  // 每个 key 覆盖 20 遍
    ASSERT_TRUE((*c)->close_write_file());
    const auto frag = fragmented_sealed(**c);
    ASSERT_FALSE(frag.empty()) << "status 应已报出死字节";
    const auto nm = (*c)->needs_merge();
    EXPECT_TRUE(nm.needs) << "status 报着死字节，needs_merge 必须看见（不用重开）";
    for (const auto& f : frag) {
        EXPECT_NE(std::find(nm.files.begin(), nm.files.end(), f), nm.files.end())
            << f << " 碎片过阈值却不在候选里";
    }

    // 合并后把输出整个写死：同一句柄上 needs_merge 仍要看见。
    auto mr = (*c)->merge(nm.files);
    ASSERT_TRUE(mr) << mr.error().detail;
    EXPECT_FALSE((*c)->needs_merge().needs) << "刚合并完";
    write_revs(**c, 22, 41);
    ASSERT_TRUE((*c)->close_write_file());
    EXPECT_FALSE(fragmented_sealed(**c).empty());
    EXPECT_TRUE((*c)->needs_merge().needs) << "合并之后的死字节同样要看见";
    (*c)->close();
}

INSTANTIATE_TEST_SUITE_P(KvAndSearch, NeedsMergeLive, ::testing::Bool(),
                         [](const auto& p) { return p.param ? "Search" : "Kv"; });

}  // namespace
