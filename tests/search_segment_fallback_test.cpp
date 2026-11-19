// S52：旧段集打不开时从零重建——新段落在库目录（⛔ 进程当前目录），旧段文件与旁车下次
// checkpoint 清掉，新段编号避开旧号（⛔ 同名新段套上旧 `.live` 旁车），并记一条 warning。
// 下游反馈：feedbacks/2026-11-19-old-segments-fallback-writes-cwd.md（观澜，9.0.0 打开 7.0.1
// 封段的旧检索库，seg-0.seg / segments.manifest 写进了进程当前目录）。
// 「载不进」这里用改坏段文件魔数来制造：与下游的「8.1.0 之前封的段默认字段标识不同 ⇒
// open_v2 拒载」走的是同一条回退路径（SegmentSet::open 回 nullptr）。
#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
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

struct Logs {
    std::mutex mu;
    std::vector<std::string> lines;
    [[nodiscard]] bool contains(std::string_view needle) {
        std::lock_guard<std::mutex> g(mu);
        for (const auto& l : lines) {
            if (l.find(needle) != std::string::npos) return true;
        }
        return false;
    }
};

CaskOptions search_options(Logs* logs) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig cfg;
    cfg.analyzer_config.type = AnalyzerType::Whitespace;
    opts.search_config = cfg;
    if (logs != nullptr) {
        opts.log_fn = [logs](CaskOptions::LogLevel, std::string_view msg) {
            std::lock_guard<std::mutex> g(logs->mu);
            logs->lines.emplace_back(msg);
        };
    }
    return opts;
}

std::set<std::string> names_in(const fs::path& d) {
    std::set<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
        out.insert(it->path().filename().string());
    }
    return out;
}

// `seg-<n>.seg[.live]` ⇒ n；别的 ⇒ -1。
long long seg_id_of(const std::string& name) {
    std::string n = name;
    if (n.size() > 5 && n.ends_with(".live")) n.resize(n.size() - 5);
    if (!n.starts_with("seg-") || !n.ends_with(".seg")) return -1;
    const std::string digits = n.substr(4, n.size() - 8);
    if (digits.empty()) return -1;
    for (char c : digits) {
        if (c < '0' || c > '9') return -1;
    }
    return std::stoll(digits);
}

std::set<std::string> keys_for(Cask& c, std::string_view q) {
    std::set<std::string> out;
    auto r = c.search_text(q, 100);
    EXPECT_TRUE(r) << "search_text(" << q << ")";
    if (r) {
        for (const auto& h : r->hits) out.insert(h.key);
    }
    return out;
}

// 进程当前目录换到 dir，析构换回。
struct CwdGuard {
    fs::path old;
    explicit CwdGuard(const fs::path& dir) : old(fs::current_path()) { fs::current_path(dir); }
    ~CwdGuard() {
        std::error_code ec;
        fs::current_path(old, ec);
    }
    CwdGuard(const CwdGuard&) = delete;
    CwdGuard& operator=(const CwdGuard&) = delete;
};

class SegmentFallbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const auto base = fs::temp_directory_path() /
                          (std::string("bitcask_seg_fallback_") + info->name());
        dir_ = base / "idx";
        cwd_ = base / "cwd";
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(dir_, ec);
        fs::create_directories(cwd_, ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_.parent_path(), ec);
    }
    fs::path dir_;
    fs::path cwd_;
};

TEST_F(SegmentFallbackTest, UnreadableSegmentsRebuildInsideTheIndexDir) {
    const fs::path segs = dir_ / "bm25_segments";

    // ① 建库写 10 篇，关掉 ⇒ 段落盘。
    {
        auto c = Cask::open(dir_.string(), search_options(nullptr), &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 10; ++i) {
            const std::string k = "a" + std::to_string(i);
            const std::string v = "apple common n" + std::to_string(i);
            ASSERT_TRUE((*c)->put(bytes(k), bytes(v)));
        }
        (*c)->close();
    }
    // ② 重开删一篇再关 ⇒ 旧段带 `.live` 旁车（同名新段会误套的那种文件）。
    {
        auto c = Cask::open(dir_.string(), search_options(nullptr), &test_registry());
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c)->remove(bytes("a3")));
        (*c)->close();
    }
    std::set<std::string> old_files;
    long long old_max = -1;
    for (const auto& n : names_in(segs)) {
        const auto id = seg_id_of(n);
        if (id < 0) continue;
        old_files.insert(n);
        if (id > old_max) old_max = id;
    }
    ASSERT_GE(old_max, 0) << "前置：段已落盘";
    bool has_sidecar = false;
    for (const auto& n : old_files) has_sidecar = has_sidecar || n.ends_with(".live");
    ASSERT_TRUE(has_sidecar) << "前置：删除之后旧段带旁车";

    // ③ 改坏所有旧段的魔数 ⇒ 载不进（与「旧默认字段标识 ⇒ 拒载」同一条回退路径）。
    for (const auto& n : old_files) {
        if (n.ends_with(".live")) continue;
        std::fstream f(segs / n, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.good());
        f.seekp(0);
        f.write("XXXX", 4);
    }

    // ④ 当前目录换成空目录再开：从零重建，写一篇，关掉。
    Logs logs;
    {
        CwdGuard cwd(cwd_);
        auto c = Cask::open(dir_.string(), search_options(&logs), &test_registry());
        ASSERT_TRUE(c);
        EXPECT_EQ(keys_for(**c, "apple").size(), 9U) << "全量重放之后照样搜得到（a3 已删）";
        EXPECT_TRUE(keys_for(**c, "apple").count("a3") == 0);
        ASSERT_TRUE((*c)->put(bytes("b1"), bytes("banana common")));
        (*c)->close();
    }
    EXPECT_TRUE(names_in(cwd_).empty())
        << "⭐ 进程当前目录里 ⛔ 多出 seg-N.seg / segments.manifest";
    EXPECT_TRUE(logs.contains("cannot be loaded")) << "回退要记一条 warning";

    const auto now = names_in(segs);
    for (const auto& n : old_files) {
        EXPECT_EQ(now.count(n), 0U) << "旧段文件 / 旁车下次 checkpoint 清掉：" << n;
    }
    bool any_new = false;
    for (const auto& n : now) {
        const auto id = seg_id_of(n);
        if (id < 0) continue;
        any_new = true;
        EXPECT_GT(id, old_max) << "新段编号避开旧号：" << n;
    }
    EXPECT_TRUE(any_new) << "新段落在库目录";

    // ⑤ 再开：不再回退，检索全对（⛔ 旧旁车套到新段上）。
    Logs logs2;
    {
        CwdGuard cwd(cwd_);
        auto c = Cask::open(dir_.string(), search_options(&logs2), &test_registry());
        ASSERT_TRUE(c);
        EXPECT_EQ(keys_for(**c, "apple").size(), 9U);
        EXPECT_EQ(keys_for(**c, "banana"), std::set<std::string>{"b1"});
        EXPECT_EQ(keys_for(**c, "common").size(), 10U);
        (*c)->close();
    }
    EXPECT_FALSE(logs2.contains("cannot be loaded")) << "重建后的库正常载入";
    EXPECT_TRUE(names_in(cwd_).empty());
}

// 新库（段目录为空）⛔ 记回退日志；SegmentSet::fresh 的名字解析只认 seg-<n>.seg[.live]。
TEST_F(SegmentFallbackTest, FreshLibraryLogsNothing) {
    Logs logs;
    {
        auto c = Cask::open(dir_.string(), search_options(&logs), &test_registry());
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c)->put(bytes("k"), bytes("hello world")));
        (*c)->close();
    }
    EXPECT_FALSE(logs.contains("cannot be loaded"));
}

}  // namespace
