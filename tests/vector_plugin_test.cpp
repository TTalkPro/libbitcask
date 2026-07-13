// VectorPlugin 插件级测试（S18-11）：覆盖 S18 新表面——normalize_for_write
// 错误契约、flush/open 自治、重放幂等。查询语义的全量覆盖仍由 hnsw_test 与
// Cask 集成测试承担。
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <optional>
#include <string>

#include "bitcask/vector_plugin.hpp"

using namespace bitcask;
namespace fs = std::filesystem;

namespace {

// 查询翻译/存活用的最小只读身份表（fill 族走 LiveChecker 默认实现）。
struct FakeDocTable final : bm25::DocTable {
    [[nodiscard]] bool is_live(std::uint64_t) const override { return true; }
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

vec::VectorPluginConfig make_cfg(std::uint16_t dim,
                                 meta::VectorMetric metric) {
    vec::VectorPluginConfig c;
    c.dim = dim;
    c.metric = metric;
    return c;
}

}  // namespace

// S18-3：写入端归一化契约——三条错误消息逐字保留（Cask 边界翻译依赖），
// cosine 归一化到单位模长，kDot 原样透传。
TEST(VectorPlugin, NormalizeForWriteContract) {
    FakeDocTable dt;
    std::vector<float> buf;

    {
        vec::VectorPlugin p(make_cfg(0, meta::VectorMetric::kNone), dt);
        const float v[2] = {1.0F, 2.0F};
        auto r = p.normalize_for_write({v, 2}, buf);
        ASSERT_FALSE(r.has_value());
        EXPECT_STREQ(r.error(), "collection has no vector config");
        EXPECT_TRUE(p.normalize_for_write({}, buf).has_value());  // 空 = 合法
    }
    {
        vec::VectorPlugin p(
            make_cfg(4, meta::VectorMetric::kCosineNormalized), dt);
        const float bad[2] = {1.0F, 2.0F};
        auto r = p.normalize_for_write({bad, 2}, buf);
        ASSERT_FALSE(r.has_value());
        EXPECT_STREQ(r.error(), "vector dim mismatch");

        const float zero[4] = {0.0F, 0.0F, 0.0F, 0.0F};
        auto z = p.normalize_for_write({zero, 4}, buf);
        ASSERT_FALSE(z.has_value());
        EXPECT_STREQ(z.error(), "zero vector not allowed under cosine metric");

        const float v[4] = {3.0F, 0.0F, 4.0F, 0.0F};
        auto ok = p.normalize_for_write({v, 4}, buf);
        ASSERT_TRUE(ok.has_value());
        double sq = 0.0;
        for (float x : *ok) sq += static_cast<double>(x) * x;
        EXPECT_NEAR(sq, 1.0, 1e-6);
    }
    {
        vec::VectorPlugin p(make_cfg(4, meta::VectorMetric::kDot), dt);
        const float v[4] = {3.0F, 0.0F, 4.0F, 0.0F};
        auto ok = p.normalize_for_write({v, 4}, buf);
        ASSERT_TRUE(ok.has_value());
        EXPECT_EQ(ok->data(), v);  // passthrough：不拷贝不缩放
    }
}

// S18-6：insert → flush（base）→ 新插件按链状态 open → watermark 续接 +
// 自匹配 top1。
TEST(VectorPlugin, InsertFlushOpenRoundTrip) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_vecplugin_rt";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string dir_s = dir.string();

    FakeDocTable dt;
    vec::VectorPlugin a(make_cfg(4, meta::VectorMetric::kDot), dt);
    plugin::OpenContext ctx;
    ctx.dir = dir_s;
    ASSERT_EQ(a.open(ctx), plugin::PluginStatus::kOk);

    const float vs[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    for (std::uint64_t i = 0; i < 3; ++i) a.insert(i, {vs[i], 4});
    EXPECT_EQ(a.size(), 3u);

    plugin::FlushRequest req;
    req.watermark = 3;
    auto fr = a.flush(req);
    ASSERT_EQ(fr.status, plugin::PluginStatus::kOk);
    EXPECT_EQ(fr.covered_ord, 3u);
    EXPECT_TRUE(fs::exists(dir / "vec.ckpt"));
    EXPECT_TRUE(fs::exists(dir / "vec.vec"));  // 侧车

    const auto st = a.chain_state();
    vec::VectorPlugin b(make_cfg(4, meta::VectorMetric::kDot), dt);
    plugin::OpenContext c2;
    c2.dir = dir_s;
    c2.committed_base_watermark = st.base_gen;
    c2.committed_chain_watermark = st.chain_wm;
    c2.committed_chain_seq = st.next_seq - 1;
    ASSERT_EQ(b.open(c2), plugin::PluginStatus::kOk);
    EXPECT_EQ(b.watermark(), 3u);
    EXPECT_EQ(b.size(), 3u);
    auto r = b.search({vs[1], 4}, 1, 0, nullptr);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].key, "k1");  // 自匹配 top1
    fs::remove_all(dir);
}

// S32-M1：base rebase 窗口门——自 base 以来入图向量数达 rebase_min_docs
// 即在下次 flush 强制 base（链坍缩，next_seq 回 1）；未达阈值走 delta；
// 阈值 0 = 关（对照：恒 delta，仅链长门）。
TEST(VectorPlugin, RebaseMinDocsWindowTriggersBase) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_vecplugin_win";
    fs::remove_all(dir);
    fs::create_directories(dir);

    FakeDocTable dt;
    auto cfg = make_cfg(4, meta::VectorMetric::kDot);
    cfg.rebase_min_docs = 4;
    vec::VectorPlugin p(cfg, dt);
    plugin::OpenContext ctx;
    const std::string dir_s = dir.string();
    ctx.dir = dir_s;
    ASSERT_EQ(p.open(ctx), plugin::PluginStatus::kOk);

    const float v[4] = {1, 0, 0, 0};
    plugin::FlushRequest req;

    // 首次 flush：fresh open（无已提交状态）→ 必 base，窗口归零。
    p.insert(0, {v, 4});
    p.insert(1, {v, 4});
    req.watermark = 2;
    ASSERT_EQ(p.flush(req).status, plugin::PluginStatus::kOk);
    EXPECT_EQ(p.chain_state().next_seq, 1u);  // base：链坍缩

    // 窗口 2 < 4：delta。
    p.insert(2, {v, 4});
    p.insert(3, {v, 4});
    req.watermark = 4;
    ASSERT_EQ(p.flush(req).status, plugin::PluginStatus::kOk);
    EXPECT_EQ(p.chain_state().next_seq, 2u);  // delta：.d1 入链
    EXPECT_TRUE(fs::exists(dir / "vec.ckpt.d1"));

    // 窗口 2+2 = 4 ≥ 4：强制 base（非 force、非 rebuild、链远未满）。
    p.insert(4, {v, 4});
    p.insert(5, {v, 4});
    req.watermark = 6;
    ASSERT_EQ(p.flush(req).status, plugin::PluginStatus::kOk);
    EXPECT_EQ(p.chain_state().next_seq, 1u);   // base：链再坍缩
    EXPECT_FALSE(fs::exists(dir / "vec.ckpt.d1"));  // .d 链已回收

    // 对照：阈值 0 = 关，同节奏恒 delta。
    const fs::path dir0 = fs::temp_directory_path() / "bitcask_vecplugin_win0";
    fs::remove_all(dir0);
    fs::create_directories(dir0);
    auto cfg0 = make_cfg(4, meta::VectorMetric::kDot);
    cfg0.rebase_min_docs = 0;
    vec::VectorPlugin q(cfg0, dt);
    plugin::OpenContext c0;
    const std::string dir0_s = dir0.string();
    c0.dir = dir0_s;
    ASSERT_EQ(q.open(c0), plugin::PluginStatus::kOk);
    q.insert(0, {v, 4});
    req.watermark = 1;
    ASSERT_EQ(q.flush(req).status, plugin::PluginStatus::kOk);  // 首次 base
    for (std::uint64_t i = 1; i <= 8; ++i) {
        q.insert(i, {v, 4});
        req.watermark = i + 1;
        ASSERT_EQ(q.flush(req).status, plugin::PluginStatus::kOk);
    }
    EXPECT_EQ(q.chain_state().next_seq, 9u);  // 8 个 delta，无窗口收链

    fs::remove_all(dir);
    fs::remove_all(dir0);
}

// S32-M1：重开后链重放计入窗口——恢复即"欠账"可见，无需新写入即可在
// 下次 flush 收链（崩溃恢复窗口有界的另一半保证）。
TEST(VectorPlugin, RebaseWindowSurvivesReopen) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_vecplugin_wrr";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string dir_s = dir.string();

    FakeDocTable dt;
    auto cfg = make_cfg(4, meta::VectorMetric::kDot);
    cfg.rebase_min_docs = 100;  // a 写入期间不触发
    vec::VectorPlugin a(cfg, dt);
    plugin::OpenContext ctx;
    ctx.dir = dir_s;
    ASSERT_EQ(a.open(ctx), plugin::PluginStatus::kOk);

    const float v[4] = {0, 1, 0, 0};
    plugin::FlushRequest req;
    a.insert(0, {v, 4});
    req.watermark = 1;
    ASSERT_EQ(a.flush(req).status, plugin::PluginStatus::kOk);  // base
    a.insert(1, {v, 4});
    a.insert(2, {v, 4});
    req.watermark = 3;
    ASSERT_EQ(a.flush(req).status, plugin::PluginStatus::kOk);  // delta(2 条)
    const auto st = a.chain_state();
    ASSERT_EQ(st.next_seq, 2u);

    // b 以更小阈值重开：链重放 2 条 ≥ 2 → 无新写入，下次 flush 即收链。
    auto cfg_b = make_cfg(4, meta::VectorMetric::kDot);
    cfg_b.rebase_min_docs = 2;
    vec::VectorPlugin b(cfg_b, dt);
    plugin::OpenContext c2;
    c2.dir = dir_s;
    c2.committed_base_watermark = st.base_gen;
    c2.committed_chain_watermark = st.chain_wm;
    c2.committed_chain_seq = st.next_seq - 1;
    ASSERT_EQ(b.open(c2), plugin::PluginStatus::kOk);
    EXPECT_EQ(b.size(), 3u);
    req.watermark = 3;
    ASSERT_EQ(b.flush(req).status, plugin::PluginStatus::kOk);
    EXPECT_EQ(b.chain_state().next_seq, 1u);   // 重放欠账触发 base
    EXPECT_FALSE(fs::exists(dir / "vec.ckpt.d1"));  // 链已回收

    fs::remove_all(dir);
}

// 重放幂等：同 ord 重插被水位门丢弃（fold 重叠区安全的结构基础）。
TEST(VectorPlugin, ReplayInsertIdempotent) {
    FakeDocTable dt;
    vec::VectorPlugin p(make_cfg(4, meta::VectorMetric::kDot), dt);
    const float v[4] = {1, 0, 0, 0};
    p.insert(0, {v, 4});
    p.insert(1, {v, 4});
    EXPECT_EQ(p.size(), 2u);
    p.insert(0, {v, 4});  // 重放：ord ≤ 水位 → 丢弃
    p.insert(1, {v, 4});
    EXPECT_EQ(p.size(), 2u);
}
