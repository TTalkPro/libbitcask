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
