// TextPlugin 插件级测试（S18-11）：覆盖 S18 新表面——flush/open 自治、
// replay 路由、base/delta 决策。查询语义的全量覆盖仍由 search_layer_test
// （经 shim）与 Cask 集成测试承担。
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "bitcask/index.hpp"
#include "bitcask/text_plugin.hpp"

using namespace bitcask;
namespace fs = std::filesystem;

namespace {

text::TextPluginConfig make_cfg() {
    text::TextPluginConfig c;
    c.analyzer_config = text::AnalyzerConfig{};
    c.bm25_params = bm25::Bm25Params{1.2F, 0.75F};
    return c;
}

void host_put_row(index::Index& idx, const std::string& key,
                  std::uint64_t ord) {
    idx.put_doc(key, ord,
                index::DocSlot{index::DocLoc{1, ord * 100, 50},
                               static_cast<std::uint32_t>(1000 + ord), 0});
}

}  // namespace

// flush（base）→ 新插件按链状态提示 open → watermark 续接 + 查询命中。
TEST(TextPlugin, FlushOpenRoundTrip) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_textplugin_rt";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string dir_s = dir.string();

    index::Index idx;
    text::TextPlugin a(make_cfg(), idx, idx, idx);
    ASSERT_TRUE(a.has_analyzer());
    plugin::OpenContext ctx;
    ctx.dir = dir_s;
    ASSERT_EQ(a.open(ctx), plugin::PluginStatus::kOk);
    EXPECT_EQ(a.watermark(), 0u);  // 新目录：降级自建

    for (std::uint64_t ord = 0; ord < 3; ++ord) {
        const std::string key = "k" + std::to_string(ord);
        host_put_row(idx, key, ord);  // 宿主先落 docmap 行（S16-2 同构）
        a.apply_text(key, ord, "hello world " + std::to_string(ord));
    }
    plugin::FlushRequest req;
    req.watermark = 3;
    auto fr = a.flush(req);  // open 降级 → rebase 置位 → 首次 flush 恒 base
    ASSERT_EQ(fr.status, plugin::PluginStatus::kOk);
    EXPECT_EQ(fr.covered_ord, 3u);
    EXPECT_TRUE(fs::exists(dir / "bm25.ckpt"));

    // 新插件（新 docmap——宿主职责重建行，查询翻译需要）。
    const auto st = a.chain_state();
    index::Index idx2;
    for (std::uint64_t ord = 0; ord < 3; ++ord) {
        host_put_row(idx2, "k" + std::to_string(ord), ord);
    }
    text::TextPlugin b(make_cfg(), idx2, idx2, idx2);
    plugin::OpenContext c2;
    c2.dir = dir_s;
    c2.committed_base_watermark = st.base_gen;
    c2.committed_chain_watermark = st.chain_wm;
    c2.committed_chain_seq = st.next_seq - 1;
    ASSERT_EQ(b.open(c2), plugin::PluginStatus::kOk);
    EXPECT_EQ(b.watermark(), 3u);
    auto r = b.search_text("hello", 10);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 3u);
    fs::remove_all(dir);
}

// S18-8：replay 路由——单文本活写 prepare 返回 nullptr（reducer 内分析），
// 重放批则走 prepare 并行分析（kDefaultField 包装），on_put 消费产物。
TEST(TextPlugin, ReplayPrepareRouting) {
    index::Index idx;
    text::TextPlugin p(make_cfg(), idx, idx, idx);
    ASSERT_TRUE(p.has_analyzer());

    plugin::DocView dv;
    dv.text = "hello replay";
    plugin::PutEvent live;
    live.ord = 0;
    live.key = "k0";
    live.doc = &dv;
    live.loc = plugin::RecordLoc{1, 0, 50};
    live.tstamp = 1000;
    EXPECT_EQ(p.prepare(live), nullptr);  // 活写单文本：reducer 内分析

    plugin::PutEvent rep = live;
    rep.replay = true;
    auto prep = p.prepare(rep);
    ASSERT_NE(prep, nullptr);  // 重放批：prepare 并行分析

    host_put_row(idx, "k0", 0);
    p.on_put(rep, std::move(prep));
    auto r = p.search_text("replay", 10);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].key, "k0");
}

// S18-6：base/delta 决策——首次 base 后脏增量走 delta（.d1），force_rebase
// 收链回 base（.d 链清扫）。
TEST(TextPlugin, FlushDeltaThenForcedBase) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_textplugin_fd";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string dir_s = dir.string();

    index::Index idx;
    text::TextPlugin p(make_cfg(), idx, idx, idx);
    plugin::OpenContext ctx;
    ctx.dir = dir_s;
    ASSERT_EQ(p.open(ctx), plugin::PluginStatus::kOk);

    host_put_row(idx, "a", 0);
    p.apply_text("a", 0, "alpha doc");
    plugin::FlushRequest r1;
    r1.watermark = 1;
    ASSERT_EQ(p.flush(r1).status, plugin::PluginStatus::kOk);  // base
    EXPECT_TRUE(fs::exists(dir / "bm25.ckpt"));
    EXPECT_FALSE(fs::exists(dir / "bm25.ckpt.d1"));

    host_put_row(idx, "b", 1);
    p.apply_text("b", 1, "beta doc");
    plugin::FlushRequest r2;
    r2.watermark = 2;
    auto f2 = p.flush(r2);  // rebase 已清 + 脏 → delta
    ASSERT_EQ(f2.status, plugin::PluginStatus::kOk);
    EXPECT_EQ(f2.covered_ord, 2u);
    EXPECT_TRUE(fs::exists(dir / "bm25.ckpt.d1"));

    host_put_row(idx, "c", 2);
    p.apply_text("c", 2, "gamma doc");
    plugin::FlushRequest r3;
    r3.watermark = 3;
    r3.force_rebase = true;  // close 收链语义
    ASSERT_EQ(p.flush(r3).status, plugin::PluginStatus::kOk);
    EXPECT_FALSE(fs::exists(dir / "bm25.ckpt.d1"))
        << "base 落成后 delta 链应被清扫";
    fs::remove_all(dir);
}
