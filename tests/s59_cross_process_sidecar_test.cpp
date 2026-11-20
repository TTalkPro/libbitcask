// S59：跨进程 merge_only 旁车在写者开着时合并——旁车关库删掉输入文件，写者内存索引
// 仍指向旧位置（旁车的搬迁只进了旁车自己的权威），此前写者之后读被搬走的 key 全部
// kIo "open file_id=N"、迭代报 "fold location disappeared"，重开才好。现读到「定位所指
// 数据文件已不存在」时从 data 受控重建一次再重试。
// 两个 IndexRegistry = 两套独立权威，模拟两个进程（同进程共享 registry 的情形见 S58）。
#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>

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
using bitcask::RangeOptions;
using bitcask::search::SearchLayerConfig;
using bitcask::text::AnalyzerType;

bitcask::oki::IndexRegistry& writer_registry() {
    static bitcask::oki::IndexRegistry reg;
    return reg;
}
bitcask::oki::IndexRegistry& sidecar_registry() {  // 「另一个进程」
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

CaskOptions options(bool search, bool merge_only = false) {
    CaskOptions opts;
    opts.read_write = !merge_only;
    opts.merge_only = merge_only;
    if (search) {
        opts.enable_search = true;
        SearchLayerConfig cfg;
        cfg.analyzer_config.type = AnalyzerType::Whitespace;
        opts.search_config = cfg;
    }
    return opts;
}

constexpr int kKeys = 30;
std::string key(int i) { return "k" + std::to_string(i); }
std::string doc(int i, int rev) {
    return "rev" + std::to_string(rev) + " uniq" + std::to_string(i) + " lorem";
}

std::map<std::string, std::string> expected_all() {
    std::map<std::string, std::string> m;
    for (int i = 0; i < kKeys; ++i) m[key(i)] = doc(i, 10);
    m["live"] = "after seal uniqlive";
    return m;
}

class CrossProcessSidecar : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string name = info->name();
        for (auto& ch : name) {
            if (ch == '/') ch = '_';
        }
        dir_ = fs::temp_directory_path() / ("bitcask_s59_" + name);
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

TEST_P(CrossProcessSidecar, LiveWriterReadsAfterSidecarMergeDeletesInputs) {
    const bool search = GetParam();
    auto w = Cask::open(dir_.string(), options(search), &writer_registry());
    ASSERT_TRUE(w);
    for (int v = 1; v <= 10; ++v) {
        for (int i = 0; i < kKeys; ++i) ASSERT_TRUE((*w)->put(bytes(key(i)), bytes(doc(i, v))));
    }
    ASSERT_TRUE((*w)->close_write_file());
    ASSERT_TRUE((*w)->put(bytes("live"), bytes("after seal uniqlive")));

    // 写者的封口文件（排除当前写文件）交给旁车显式合并——跨进程旁车只看得到提交点清单，
    // 写者还没提交过时 needs_merge 为 false，这里不依赖它。
    std::vector<std::string> inputs;
    {
        const auto st = (*w)->status();
        std::uint32_t active = 0;
        for (const auto& f : st.files) active = std::max(active, f.file_id);
        for (const auto& f : st.files) {
            if (f.file_id != active) inputs.push_back(f.filename);
        }
    }
    ASSERT_FALSE(inputs.empty());
    {
        auto mo = Cask::open(dir_.string(), options(search, /*merge_only=*/true),
                             &sidecar_registry());
        ASSERT_TRUE(mo) << mo.error().detail;
        auto r = (*mo)->merge(inputs);
        ASSERT_TRUE(r) << r.error().detail;
        ASSERT_GT(r->records_kept, 0U);
        (*mo)->close();  // 退休队列在此排水：输入文件被删
    }
    for (const auto& f : inputs) ASSERT_FALSE(fs::exists(f)) << f << " 旁车关库应已删除";

    const auto want = expected_all();
    // 点查：被搬走的 key 全部读得到、值是最新版本。
    for (const auto& [k, v] : want) {
        auto g = (*w)->get_owned(bytes(k));
        ASSERT_TRUE(g) << k << ": " << g.error().detail;
        EXPECT_EQ(str(g->value), v) << k;
    }
    // 全表迭代（在删除之后才开始，不受 pin 保护）。
    {
        auto it = (*w)->make_iter();
        ASSERT_TRUE(it->start());
        std::map<std::string, std::string> got;
        while (true) {
            auto e = it->next();
            ASSERT_TRUE(e) << e.error().detail;
            if (!*e) break;
            got[str((*e)->key)] = str((*e)->value);
        }
        it->release();
        EXPECT_EQ(got, want);
    }
    // range 迭代。
    {
        auto it = (*w)->make_range_iter(RangeOptions{});
        ASSERT_TRUE(it) << it.error().detail;
        std::map<std::string, std::string> got;
        while (true) {
            auto e = (*it)->next();
            ASSERT_TRUE(e) << e.error().detail;
            if (!*e) break;
            got[str((*e)->key)] = str((*e)->value);
        }
        EXPECT_EQ(got, want);
    }
    if (search) {
        auto r = (*w)->search_text("uniq7", 10);
        ASSERT_TRUE(r);
        ASSERT_EQ(r->hits.size(), 1U);
        EXPECT_EQ(r->hits[0].key, key(7));
    }
    // 之后照常写读；重开不变。
    ASSERT_TRUE((*w)->put(bytes("after"), bytes("post merge uniqafter")));
    auto g = (*w)->get_owned(bytes("after"));
    ASSERT_TRUE(g);
    (*w)->close();

    auto c = Cask::open(dir_.string(), options(search), &writer_registry());
    ASSERT_TRUE(c);
    auto want2 = want;
    want2["after"] = "post merge uniqafter";
    for (const auto& [k, v] : want2) {
        auto g2 = (*c)->get_owned(bytes(k));
        ASSERT_TRUE(g2) << k << ": " << g2.error().detail;
        EXPECT_EQ(str(g2->value), v) << k;
    }
    if (search) {
        auto r = (*c)->search_text("uniq7", 10);
        ASSERT_TRUE(r);
        EXPECT_EQ(r->hits.size(), 1U);
    }
    (*c)->close();
}

// 删除之后第一个读者就是全表迭代（不先点查）：迭代器自身也要自愈，不报
// "fold location disappeared"。
TEST_P(CrossProcessSidecar, IterationFirstAfterSidecarMerge) {
    const bool search = GetParam();
    auto w = Cask::open(dir_.string(), options(search), &writer_registry());
    ASSERT_TRUE(w);
    for (int v = 1; v <= 10; ++v) {
        for (int i = 0; i < kKeys; ++i) ASSERT_TRUE((*w)->put(bytes(key(i)), bytes(doc(i, v))));
    }
    ASSERT_TRUE((*w)->close_write_file());
    ASSERT_TRUE((*w)->put(bytes("live"), bytes("after seal uniqlive")));
    std::vector<std::string> inputs;
    {
        const auto st = (*w)->status();
        std::uint32_t active = 0;
        for (const auto& f : st.files) active = std::max(active, f.file_id);
        for (const auto& f : st.files) {
            if (f.file_id != active) inputs.push_back(f.filename);
        }
    }
    {
        auto mo = Cask::open(dir_.string(), options(search, true), &sidecar_registry());
        ASSERT_TRUE(mo) << mo.error().detail;
        auto r = (*mo)->merge(inputs);
        ASSERT_TRUE(r) << r.error().detail;
        (*mo)->close();
    }
    for (const auto& f : inputs) ASSERT_FALSE(fs::exists(f));

    auto it = (*w)->make_iter();
    ASSERT_TRUE(it->start());
    std::map<std::string, std::string> got;
    while (true) {
        auto e = it->next();
        ASSERT_TRUE(e) << e.error().detail;
        if (!*e) break;
        got[str((*e)->key)] = str((*e)->value);
    }
    it->release();
    EXPECT_EQ(got, expected_all());
    (*w)->close();
}

INSTANTIATE_TEST_SUITE_P(KvAndSearch, CrossProcessSidecar, ::testing::Bool(),
                         [](const auto& p) { return p.param ? "Search" : "Kv"; });

}  // namespace
