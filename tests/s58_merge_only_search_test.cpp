// S58：检索库经 merge_only 旁车合并——
//   此前 Cask::merge 收尾的 flush_index() 等一个永远到不了的 LSN（旁车车道以打开时的
//   next_ord 注册、受控重建后 next_ord 才推进到数据末尾），调用线程永久阻塞；即使不挂，
//   也会用旁车的空检索索引成对保存检索 ckpt（bm25.ckpt / docmap.ckpt / 段），覆盖写者的。
//   现旁车按纯 KV 合并：不带检索插件、不做检索侧收尾；写者下次打开按 key+LSN 归正。
// 下游反馈：feedbacks/2026-11-20-merge-only-search-merge-hangs.md（bitcask）。
#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskOptions;
using bitcask::search::SearchLayerConfig;
using bitcask::text::AnalyzerType;

bitcask::oki::IndexRegistry& test_registry() {
    static bitcask::oki::IndexRegistry reg;
    return reg;
}

std::span<const std::byte> bytes(std::string_view s) {
    return std::as_bytes(std::span<const char>(s.data(), s.size()));
}

CaskOptions search_options(bool merge_only = false) {
    CaskOptions opts;
    opts.read_write = !merge_only;
    opts.merge_only = merge_only;
    opts.enable_search = true;
    SearchLayerConfig cfg;
    cfg.analyzer_config.type = AnalyzerType::Whitespace;
    opts.search_config = cfg;
    return opts;
}

constexpr int kKeys = 30;

std::string key(int i) { return "k" + std::to_string(i); }
std::string doc(int i, int rev) {
    return "task " + std::to_string(i) + " rev" + std::to_string(rev) + " uniq" +
           std::to_string(i) + " lorem ipsum dolor";
}

std::map<std::string, double> text_scores(Cask& c, std::string_view q) {
    std::map<std::string, double> out;
    auto r = c.search_text(q, 100);
    EXPECT_TRUE(r);
    if (r) {
        for (const auto& h : r->hits) out[h.key] = h.score;
    }
    return out;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// 检索侧持久化（成对保存点的产物）：bm25.ckpt / docmap.ckpt / index.manifest /
// bm25_segments/*。相对路径 → 内容。KV 侧（*.bitcask.data / kv.oki.*）旁车合并本就会改，不计。
std::map<std::string, std::string> search_side_files(const fs::path& d) {
    std::map<std::string, std::string> out;
    for (const char* n : {"bm25.ckpt", "docmap.ckpt", "index.manifest"}) {
        if (fs::exists(d / n)) out[n] = read_file(d / n);
    }
    if (fs::exists(d / "bm25_segments")) {
        for (const auto& e : fs::recursive_directory_iterator(d / "bm25_segments")) {
            if (e.is_regular_file()) {
                out[fs::relative(e.path(), d).string()] = read_file(e.path());
            }
        }
    }
    return out;
}

// 有界执行：超时即判失败并直接退出（挂住的线程收不回来，不能让整个套件陪着挂）。
template <class F>
auto bounded(F&& f, const char* what) {
    auto fut = std::async(std::launch::async, std::forward<F>(f));
    if (fut.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        ADD_FAILURE() << what << " 30 s 未返回（挂住）";
        std::fflush(nullptr);
        std::_Exit(1);
    }
    return fut.get();
}

class S58Test : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() / (std::string("bitcask_s58_") + info->name());
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    // 写者建库：每个 key 覆盖写 10 遍，封口，关库（留下可合并的死数据）。
    void build() {
        auto c = Cask::open(dir_.string(), search_options(), &test_registry());
        ASSERT_TRUE(c);
        for (int v = 1; v <= 10; ++v) {
            for (int i = 0; i < kKeys; ++i) ASSERT_TRUE((*c)->put(bytes(key(i)), bytes(doc(i, v))));
        }
        ASSERT_TRUE((*c)->close_write_file());
        (*c)->close();
    }
    fs::path dir_;
};

TEST_F(S58Test, SidecarMergeOfSearchLibraryReturnsAndKeepsIndex) {
    build();
    std::map<std::string, double> before;
    {
        auto c = Cask::open(dir_.string(), search_options(), &test_registry());
        ASSERT_TRUE(c);
        before = text_scores(**c, "lorem");
        ASSERT_EQ(before.size(), static_cast<std::size_t>(kKeys));
        (*c)->close();
    }
    const auto search_before = search_side_files(dir_);
    ASSERT_TRUE(search_before.contains("bm25.ckpt") && search_before.contains("docmap.ckpt"))
        << "前提：写者关库已存检索 ckpt（否则下面的「不得改写」是空转）";

    std::vector<std::string> merged_inputs;
    {
        auto mo = Cask::open(dir_.string(), search_options(/*merge_only=*/true), &test_registry());
        ASSERT_TRUE(mo) << mo.error().detail;
        const auto nm = (*mo)->needs_merge();
        ASSERT_TRUE(nm.needs);
        merged_inputs = nm.files;
        auto r = bounded([&] { return (*mo)->merge(nm.files); }, "merge_only merge");
        ASSERT_TRUE(r) << r.error().detail;
        EXPECT_GT(r->records_kept, 0U);
        // 旁车上调检索也不能挂（它不拥有索引，结果不作要求）。
        (void)bounded([&] { return (*mo)->search_text("lorem", 10); }, "merge_only search");
        (*mo)->close();
    }
    {
        const auto search_after = search_side_files(dir_);
        EXPECT_EQ(search_after.size(), search_before.size());
        for (const auto& [name, bytes_before] : search_before) {
            auto it = search_after.find(name);
            ASSERT_NE(it, search_after.end()) << name << " 被旁车删了";
            EXPECT_TRUE(it->second == bytes_before) << "旁车不得改写写者的检索 ckpt：" << name;
        }
    }
    for (const auto& f : merged_inputs) EXPECT_FALSE(fs::exists(f)) << f << " 应已回收";

    // 写者重开：检索结果、分数与值逐一不变。
    auto c = Cask::open(dir_.string(), search_options(), &test_registry());
    ASSERT_TRUE(c);
    EXPECT_EQ(text_scores(**c, "lorem"), before);
    for (int i = 0; i < kKeys; ++i) {
        const auto hits = text_scores(**c, "uniq" + std::to_string(i));
        ASSERT_EQ(hits.size(), 1U) << i;
        EXPECT_EQ(hits.begin()->first, key(i));
        auto g = (*c)->get_owned(bytes(key(i)));
        ASSERT_TRUE(g) << i;
        const auto& v = g->value;
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(v.data()), v.size()), doc(i, 10));
    }
    EXPECT_TRUE(text_scores(**c, "rev1").empty()) << "旧版本不得复活";
    // 合并后的写者照常写、照常可检索。
    ASSERT_TRUE((*c)->put(bytes("new"), bytes("fresh uniqnew")));
    EXPECT_EQ(text_scores(**c, "uniqnew").size(), 1U);
    (*c)->close();
}

// 写者开着、旁车同进程（共享 registry ⇒ 共享索引权威，bitcask_merge_worker 的用法）：
// 旁车打开后写者继续写（新 ord 不会出现在旁车车道上——此前旁车合并必挂），合并后写者
// 照常读、照常检索，重开不变。
TEST_F(S58Test, SidecarMergeWithLiveWriterInSameProcess) {
    auto w = Cask::open(dir_.string(), search_options(), &test_registry());
    ASSERT_TRUE(w);
    for (int v = 1; v <= 10; ++v) {
        for (int i = 0; i < kKeys; ++i) ASSERT_TRUE((*w)->put(bytes(key(i)), bytes(doc(i, v))));
    }
    ASSERT_TRUE((*w)->close_write_file());
    // 写者在线分数（段 df 含被覆盖的旧版本——既定近似，merge / 重开归正）：旁车合并
    // 不碰写者的检索索引 ⇒ 合并前后逐位不变。
    const auto live_before = text_scores(**w, "lorem");
    ASSERT_EQ(live_before.size(), static_cast<std::size_t>(kKeys));
    auto mo = Cask::open(dir_.string(), search_options(/*merge_only=*/true), &test_registry());
    ASSERT_TRUE(mo) << mo.error().detail;
    ASSERT_TRUE((*w)->put(bytes("live"), bytes("written after sidecar open uniqlive")));
    const auto nm = (*mo)->needs_merge();
    ASSERT_TRUE(nm.needs) << "共享权威：旁车看得见写者的实时账本";
    auto r = bounded([&] { return (*mo)->merge(nm.files); }, "merge_only merge（写者开着）");
    ASSERT_TRUE(r) << r.error().detail;
    (*mo)->close();

    for (int i = 0; i < kKeys; ++i) {
        auto g = (*w)->get_owned(bytes(key(i)));
        ASSERT_TRUE(g) << i << ": " << g.error().detail;
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(g->value.data()), g->value.size()),
                  doc(i, 10));
        EXPECT_EQ(text_scores(**w, "uniq" + std::to_string(i)).size(), 1U) << i;
    }
    EXPECT_EQ(text_scores(**w, "uniqlive").size(), 1U);
    EXPECT_EQ(text_scores(**w, "lorem"), live_before);
    (*w)->close();

    auto c = Cask::open(dir_.string(), search_options(), &test_registry());
    ASSERT_TRUE(c);
    const auto reopened = text_scores(**c, "lorem");
    ASSERT_EQ(reopened.size(), live_before.size());
    for (const auto& [k, _] : live_before) EXPECT_TRUE(reopened.contains(k)) << k;
    EXPECT_EQ(text_scores(**c, "uniqlive").size(), 1U);
    EXPECT_TRUE(text_scores(**c, "rev1").empty());
    (*c)->close();
}

}  // namespace
