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
                index::DocSlot{index::DocLoc{.offset   = ord * 100,
                                             .file_id  = 1,
                                             .total_sz = 50},
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

// S27-3 Slice C：FlushDeltaThenForcedBase + OrphanDeltaNotReplayedWhenChainSeqZero
// 退役——delta 链已删除，相关测试不再适用。

// ===========================================================================
// S27-3 Slice B1：Building 段镜像（fields_ 权威 / building_ 影子）单元测试。
// 验证 apply_* 同时写 fields_ + building_、flush 触发段封口、on_delete 段级
// 删除双路径（in_building / in_segment_set）。零行为变化：B1 阶段查询仍走
// fields_，building_ 是为 B2 切换查询路径预埋的影子。
// ===========================================================================

// 镜像一致性：apply_job 多字段后，fields_（权威）与 building_（影子）的
// doc_count / df / live_doc_count 一致。
TEST(TextPlugin, BuildingMirrorApplyJobConsistent) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_textplugin_b1_mirror";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string dir_s = dir.string();

    index::Index idx;
    text::TextPlugin p(make_cfg(), idx, idx, idx);
    plugin::OpenContext ctx;
    ctx.dir = dir_s;
    ASSERT_EQ(p.open(ctx), plugin::PluginStatus::kOk);

    constexpr std::size_t N = 5;
    for (std::uint64_t ord = 0; ord < N; ++ord) {
        const std::string key = "k" + std::to_string(ord);
        host_put_row(idx, key, ord);
        // owning 字符串保活 —— `"x" + std::to_string(ord)` 是 prvalue 临时,
        // 借给 string_view 后立即析构 → UAF(TSan 下暴露为乱码 token)。
        const std::string title_str = "alpha " + std::to_string(ord);
        const std::string body_str = "beta gamma " + std::to_string(ord);
        const std::pair<std::string_view, std::string_view> fields[] = {
            {"title", title_str},
            {"body",  body_str},
        };
        auto job = p.map_analyze(key, ord, fields,
                                 /*file_id=*/1, /*offset=*/ord * 100,
                                 /*total_sz=*/50, /*tstamp=*/1000);
        p.apply_job(job);
    }

    const auto* bld = p.building_segment();
    const auto* sset = p.segment_set();
    ASSERT_NE(bld, nullptr);
    ASSERT_NE(sset, nullptr);

    EXPECT_EQ(bld->doc_count(), N);
    EXPECT_EQ(bld->live_doc_count(), N);

    // 默认字段 df：title "alpha" + body "beta"/"gamma" 都走 catch-all 进入
    // 默认字段 → df == N。注：SealedSegment 的默认字段在 inv_（非 fields_ map），
    // 用 inverted() 取。Ngram 在 min_n=2/max_n=3/min_token_length=1 下对 4-5 字符
    // 拉丁词稳定 emit 为整词（Slice A 测试已验证）。
    const auto& bld_default = bld->inverted();
    EXPECT_EQ(bld_default.doc_freq("alpha"), N)
        << "building_ 影子：'alpha' df == N（catch-all 合并入默认字段）";
    EXPECT_EQ(bld_default.doc_freq("beta"), N)
        << "building_ 影子：'beta' df == N（body 经 catch-all 合并入默认字段）";
    EXPECT_EQ(bld_default.doc_freq("gamma"), N)
        << "building_ 影子：'gamma' df == N（body 经 catch-all 合并入默认字段）";

    // 未触发 flush → 段集仍空。
    EXPECT_EQ(sset->segment_count(), 0u);

    fs::remove_all(dir);
}

// Flush：apply_job → flush_building_now() → building_ 空、segment_set_ 1 段。
TEST(TextPlugin, FlushBuildingToSegmentSet) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_textplugin_b1_flush";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string dir_s = dir.string();

    index::Index idx;
    text::TextPlugin p(make_cfg(), idx, idx, idx);
    plugin::OpenContext ctx;
    ctx.dir = dir_s;
    ASSERT_EQ(p.open(ctx), plugin::PluginStatus::kOk);

    constexpr std::size_t N = 3;
    for (std::uint64_t ord = 0; ord < N; ++ord) {
        const std::string key = "k" + std::to_string(ord);
        host_put_row(idx, key, ord);
        const std::pair<std::string_view, std::string_view> fields[] = {
            {"title", "alpha"},
            {"body",  "beta gamma"},
        };
        auto job = p.map_analyze(key, ord, fields, 1, ord * 100, 50, 1000);
        p.apply_job(job);
    }

    // 显式触发 flush（不依赖 64K 阈值）。
    p.flush_building_now();

    EXPECT_EQ(p.building_segment()->doc_count(), 0u);
    const auto* sset = p.segment_set();
    ASSERT_NE(sset, nullptr);
    ASSERT_EQ(sset->segment_count(), 1u);

    const auto* seg = sset->segment(0);
    ASSERT_NE(seg, nullptr);
    EXPECT_EQ(seg->doc_count(), N);
    EXPECT_EQ(seg->live_doc_count(), N);

    fs::remove_all(dir);
}

// on_delete（building_ 内删除）：docid 在 building_ → mark_dead 翻位。
TEST(TextPlugin, OnDeleteFromBuilding) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_textplugin_b1_del_bld";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string dir_s = dir.string();

    index::Index idx;
    text::TextPlugin p(make_cfg(), idx, idx, idx);
    plugin::OpenContext ctx;
    ctx.dir = dir_s;
    ASSERT_EQ(p.open(ctx), plugin::PluginStatus::kOk);

    constexpr std::size_t N = 4;
    for (std::uint64_t ord = 0; ord < N; ++ord) {
        const std::string key = "k" + std::to_string(ord);
        host_put_row(idx, key, ord);
        const std::string title_str = "alpha " + std::to_string(ord);  // owning 保活
        const std::pair<std::string_view, std::string_view> fields[] = {
            {"title", title_str},
            {"body",  "beta"},
        };
        auto job = p.map_analyze(key, ord, fields, 1, ord * 100, 50, 1000);
        p.apply_job(job);
    }

    const auto* bld = p.building_segment();
    ASSERT_NE(bld, nullptr);
    EXPECT_EQ(bld->doc_count(), N);
    EXPECT_EQ(bld->live_doc_count(), N);

    // 删第 2 篇（ord=1 → building_ 段内 docid=1）。
    p.on_delete("k1", /*tomb_ord=*/100, /*prior_ord=*/1);

    EXPECT_FALSE(bld->is_live(1)) << "OnDelete 应翻 live_[1]=0";
    EXPECT_TRUE(bld->is_live(0));
    EXPECT_TRUE(bld->is_live(2));
    EXPECT_TRUE(bld->is_live(3));
    EXPECT_EQ(bld->live_doc_count(), N - 1);

    EXPECT_EQ(p.segment_set()->segment_count(), 0u)
        << "building_ 内删除不触发 flush";

    fs::remove_all(dir);
}

// on_delete（segment_set_ 内删除）：flush 后删 → 走 segment_set_->segment 路径。
TEST(TextPlugin, OnDeleteFromSegmentSet) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_textplugin_b1_del_seg";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string dir_s = dir.string();

    index::Index idx;
    text::TextPlugin p(make_cfg(), idx, idx, idx);
    plugin::OpenContext ctx;
    ctx.dir = dir_s;
    ASSERT_EQ(p.open(ctx), plugin::PluginStatus::kOk);

    constexpr std::size_t N = 3;
    for (std::uint64_t ord = 0; ord < N; ++ord) {
        const std::string key = "k" + std::to_string(ord);
        host_put_row(idx, key, ord);
        const std::string title_str = "alpha " + std::to_string(ord);  // owning 保活
        const std::pair<std::string_view, std::string_view> fields[] = {
            {"title", title_str},
        };
        auto job = p.map_analyze(key, ord, fields, 1, ord * 100, 50, 1000);
        p.apply_job(job);
    }
    EXPECT_EQ(p.building_segment()->doc_count(), N);

    p.flush_building_now();

    const auto* sset = p.segment_set();
    ASSERT_NE(sset, nullptr);
    ASSERT_EQ(sset->segment_count(), 1u);
    const auto* seg = sset->segment(0);
    ASSERT_NE(seg, nullptr);
    EXPECT_EQ(seg->doc_count(), N);
    EXPECT_EQ(seg->live_doc_count(), N);

    // 删第 2 篇（ord=1 → 已封口段内 docid=1）。
    p.on_delete("k1", /*tomb_ord=*/100, /*prior_ord=*/1);

    EXPECT_FALSE(seg->is_live(1)) << "段级删除应翻 live_[1]=0";
    EXPECT_TRUE(seg->is_live(0));
    EXPECT_TRUE(seg->is_live(2));
    EXPECT_EQ(seg->live_doc_count(), N - 1);

    EXPECT_EQ(p.building_segment()->doc_count(), 0u);
    EXPECT_EQ(p.building_segment()->live_doc_count(), 0u);

    fs::remove_all(dir);
}
