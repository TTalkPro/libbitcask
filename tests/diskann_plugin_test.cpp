// DiskannPlugin 插件级测试（S32-M3）：base/delta 链轮回、窗口→段换代、删除
// 物理清理、重放幂等门。Cask 级 e2e 见 cask_docvalue_test.cpp S32M3*。
#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <set>
#include <string>

#include "bitcask/diskann_plugin.hpp"

using namespace bitcask;
namespace fs = std::filesystem;

namespace {

// 可配置存活集的身份表（删除语义测试用）。
struct FakeDocTable final : bm25::DocTable {
    std::set<std::uint64_t> dead;
    [[nodiscard]] bool is_live(std::uint64_t ord) const override {
        return dead.find(ord) == dead.end();
    }
    [[nodiscard]] std::uint32_t doc_len(std::uint64_t) const override {
        return 1;
    }
    [[nodiscard]] std::optional<std::string>
    ord_to_ext(std::uint64_t ord) const override {
        return "k" + std::to_string(ord);
    }
    [[nodiscard]] bool eval_meta(std::uint64_t,
                                 const meta::MetaFilter&) const override {
        return true;
    }
    [[nodiscard]] std::optional<std::uint64_t>
    ord_of(std::string_view) const override {
        return std::nullopt;
    }
};

vec::VectorPluginConfig da_cfg(std::uint16_t dim) {
    vec::VectorPluginConfig c;
    c.dim = dim;
    c.metric = meta::VectorMetric::kDot;
    return c;
}

std::vector<float> axis_vec(std::size_t dim, std::size_t axis) {
    std::vector<float> v(dim, 0.0f);
    v[axis % dim] = 1.0f;
    return v;
}

}  // namespace

// base（fresh 首 flush）→ delta（窗口）→ 重开链重放 → 检索三段等价。
TEST(DiskannPlugin, BaseDeltaReopenRoundTrip) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_dap_rt";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string dir_s = dir.string();
    const std::uint16_t dim = 8;

    FakeDocTable dt;
    vec::DiskannPlugin a(da_cfg(dim), dt);
    plugin::OpenContext ctx;
    ctx.dir = dir_s;
    ASSERT_EQ(a.open(ctx), plugin::PluginStatus::kOk);

    // 4 条 → 首 flush = base（fresh open rebase_needed）。
    for (std::uint64_t i = 0; i < 4; ++i) {
        auto v = axis_vec(dim, i);
        a.insert(i, std::span<const float>(v.data(), dim));
    }
    plugin::FlushRequest req;
    req.watermark = 4;
    ASSERT_EQ(a.flush(req).status, plugin::PluginStatus::kOk);
    EXPECT_TRUE(fs::exists(dir / "diskann.ckpt"));
    EXPECT_TRUE(fs::exists(dir / "diskann.bda"));
    EXPECT_EQ(a.sealed_size(), 4u);
    EXPECT_EQ(a.window_size(), 0u);  // base 换代后窗口清空

    // 2 条 → delta（链 seq 2）；窗口与 sealed 双路可查。
    for (std::uint64_t i = 4; i < 6; ++i) {
        auto v = axis_vec(dim, i);
        a.insert(i, std::span<const float>(v.data(), dim));
    }
    req.watermark = 6;
    ASSERT_EQ(a.flush(req).status, plugin::PluginStatus::kOk);
    EXPECT_EQ(a.chain_state().next_seq, 2u);
    EXPECT_TRUE(fs::exists(dir / "diskann.ckpt.d1"));
    EXPECT_EQ(a.sealed_size(), 4u);
    EXPECT_EQ(a.window_size(), 2u);
    for (std::uint64_t i : {0u, 3u, 5u}) {  // sealed 与 window 混查
        auto q = axis_vec(dim, i);
        auto r = a.search(std::span<const float>(q.data(), dim), 1, 0,
                          nullptr);
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(r->size(), 1u);
        EXPECT_EQ((*r)[0].key, "k" + std::to_string(i));
    }

    // 重开：base 载入 + .d1 重放进窗口。
    const auto st = a.chain_state();
    vec::DiskannPlugin b(da_cfg(dim), dt);
    plugin::OpenContext c2;
    c2.dir = dir_s;
    c2.committed_base_watermark = st.base_gen;
    c2.committed_chain_watermark = st.chain_wm;
    c2.committed_chain_seq = st.next_seq - 1;
    ASSERT_EQ(b.open(c2), plugin::PluginStatus::kOk);
    EXPECT_EQ(b.watermark(), 6u);
    EXPECT_EQ(b.sealed_size(), 4u);
    EXPECT_EQ(b.window_size(), 2u);
    for (std::uint64_t i : {0u, 5u}) {
        auto q = axis_vec(dim, i);
        auto r = b.search(std::span<const float>(q.data(), dim), 1, 0,
                          nullptr);
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(r->size(), 1u);
        EXPECT_EQ((*r)[0].key, "k" + std::to_string(i));
    }

    // 重放幂等门：ord ≤ watermark 的事件（fold 重叠区）被丢弃。
    auto v0 = axis_vec(dim, 0);
    b.insert(3, std::span<const float>(v0.data(), dim));
    EXPECT_EQ(b.window_size(), 2u) << "重放事件必须被幂等门丢弃";
    fs::remove_all(dir);
}

// 删除 → force rebase：base 重建物理清死（sealed 只剩活集）。
TEST(DiskannPlugin, RebaseDropsDeadPhysically) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_dap_dead";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::uint16_t dim = 8;

    FakeDocTable dt;
    vec::DiskannPlugin p(da_cfg(dim), dt);
    plugin::OpenContext ctx;
    const std::string dir_s = dir.string();
    ctx.dir = dir_s;
    ASSERT_EQ(p.open(ctx), plugin::PluginStatus::kOk);
    for (std::uint64_t i = 0; i < 8; ++i) {
        auto v = axis_vec(dim, i);
        p.insert(i, std::span<const float>(v.data(), dim));
    }
    plugin::FlushRequest req;
    req.watermark = 8;
    ASSERT_EQ(p.flush(req).status, plugin::PluginStatus::kOk);  // base(8)
    EXPECT_EQ(p.sealed_size(), 8u);

    // 删奇数 → 查询侧立即滤掉；rebase 后物理清除。
    for (std::uint64_t i = 1; i < 8; i += 2) dt.dead.insert(i);
    {
        auto q = axis_vec(dim, 1);
        auto r = p.search(std::span<const float>(q.data(), dim), 8, 0,
                          nullptr);
        ASSERT_TRUE(r.has_value());
        for (const auto& h : *r) EXPECT_EQ(h.ord % 2, 0u);
    }
    p.force_rebase();
    req.watermark = 8;
    ASSERT_EQ(p.flush(req).status, plugin::PluginStatus::kOk);
    EXPECT_EQ(p.sealed_size(), 4u) << "rebase 必须物理清死";
    for (std::uint64_t i : {0u, 6u}) {
        auto q = axis_vec(dim, i);
        auto r = p.search(std::span<const float>(q.data(), dim), 1, 0,
                          nullptr);
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(r->size(), 1u);
        EXPECT_EQ((*r)[0].ord, i);
    }
    fs::remove_all(dir);
}

// rebase_min_docs 双门槛（S32-M1 语义在 IVF 侧同款生效）。
TEST(DiskannPlugin, RebaseMinDocsWindow) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_dap_win";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::uint16_t dim = 8;

    FakeDocTable dt;
    auto cfg = da_cfg(dim);
    cfg.rebase_min_docs = 3;
    vec::DiskannPlugin p(cfg, dt);
    plugin::OpenContext ctx;
    const std::string dir_s = dir.string();
    ctx.dir = dir_s;
    ASSERT_EQ(p.open(ctx), plugin::PluginStatus::kOk);

    plugin::FlushRequest req;
    auto put = [&](std::uint64_t i) {
        auto v = axis_vec(dim, i);
        p.insert(i, std::span<const float>(v.data(), dim));
    };
    put(0);
    req.watermark = 1;
    ASSERT_EQ(p.flush(req).status, plugin::PluginStatus::kOk);  // 首 base
    put(1); put(2);
    req.watermark = 3;
    ASSERT_EQ(p.flush(req).status, plugin::PluginStatus::kOk);  // 2<3: delta
    EXPECT_EQ(p.chain_state().next_seq, 2u);
    put(3);
    req.watermark = 4;
    ASSERT_EQ(p.flush(req).status, plugin::PluginStatus::kOk);  // 3≥3: base
    EXPECT_EQ(p.chain_state().next_seq, 1u);
    EXPECT_EQ(p.sealed_size(), 4u);
    EXPECT_EQ(p.window_size(), 0u);
    fs::remove_all(dir);
}
