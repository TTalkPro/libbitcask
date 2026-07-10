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

// ---- S27-3 B2b 步骤 4:段集 recovery round-trip ----
// 覆盖三件事:① flush 先封口 building → kSegManifest 进 bm25.ckpt,新插件
// open 从**内嵌清单**开段集(单一 commit point 主路径);② 封口后、ckpt 前
// 的 mark_dead 经脏段重存持久化——reopen 无幽灵;③ key_to_location_ 从段集
// 重建——reopen 后对 ckpt 前文档的删除仍能段级定位。
TEST(TextPlugin, SegmentSetRecoveryRoundTrip) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_textplugin_seg_rt";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string dir_s = dir.string();

    constexpr std::size_t N = 5;
    text::TextPlugin::ChainState st{};
    {
        index::Index idx;
        text::TextPlugin a(make_cfg(), idx, idx, idx);
        plugin::OpenContext ctx;
        ctx.dir = dir_s;
        ASSERT_EQ(a.open(ctx), plugin::PluginStatus::kOk);
        for (std::uint64_t ord = 0; ord < N; ++ord) {
            const std::string key = "k" + std::to_string(ord);
            host_put_row(idx, key, ord);
            a.apply_text(key, ord, "common word" + std::to_string(ord));
        }
        a.flush_building_now();  // 先封口:随后的删除走「封口段 mark_dead」路径
        ASSERT_EQ(a.segment_set()->segment_count(), 1u);
        // 封口后、checkpoint 前删除 k1——live_ 位只翻内存,依赖脏段重存持久化。
        a.on_delete("k1", /*tomb_ord=*/100, /*prior_ord=*/1);
        ASSERT_EQ(a.segment_set()->segment(0)->live_doc_count(), N - 1);

        plugin::FlushRequest req;
        req.watermark = N;
        auto fr = a.flush(req);
        ASSERT_EQ(fr.status, plugin::PluginStatus::kOk);
        st = a.chain_state();
    }

    // 抹掉过渡期 segments.manifest——强制走 bm25.ckpt 内嵌 kSegManifest 主路径。
    fs::remove(dir / "bm25_segments" / "segments.manifest");

    index::Index idx2;
    for (std::uint64_t ord = 0; ord < N; ++ord) {
        host_put_row(idx2, "k" + std::to_string(ord), ord);
    }
    text::TextPlugin b(make_cfg(), idx2, idx2, idx2);
    plugin::OpenContext c2;
    c2.dir = dir_s;
    c2.committed_base_watermark = st.base_gen;
    c2.committed_chain_watermark = st.chain_wm;
    c2.committed_chain_seq = st.next_seq - 1;
    ASSERT_EQ(b.open(c2), plugin::PluginStatus::kOk);

    // ① 段集从内嵌清单恢复。
    const auto* sset = b.segment_set();
    ASSERT_NE(sset, nullptr);
    ASSERT_EQ(sset->segment_count(), 1u);
    const auto* seg = sset->segment(0);
    ASSERT_NE(seg, nullptr);
    EXPECT_EQ(seg->doc_count(), N);
    // ② ckpt 前的删除已持久化(无幽灵)。
    EXPECT_EQ(seg->live_doc_count(), N - 1) << "封口后 ckpt 前的删除复活成幽灵";
    EXPECT_FALSE(seg->is_live(1));
    // 查询侧:k1 专属词不可命中,其余命中。
    auto r_dead = b.search_text("word1", 10);
    ASSERT_TRUE(r_dead.has_value());
    EXPECT_TRUE(r_dead->empty()) << "已删除文档不应命中";
    auto r_live = b.search_text("word2", 10);
    ASSERT_TRUE(r_live.has_value());
    EXPECT_EQ(r_live->size(), 1u);
    // ③ key_to_location_ 已重建:对 ckpt 前文档的删除能段级定位。
    b.on_delete("k2", /*tomb_ord=*/101, /*prior_ord=*/2);
    EXPECT_FALSE(seg->is_live(2)) << "reopen 后 mark_dead 落空(key 定位未重建)";
    EXPECT_EQ(seg->live_doc_count(), N - 2);

    fs::remove_all(dir);
}

// ---- S27-3 步骤 5:段生命周期 vs 查询并发(pin 机制定向压力)----
// 写者线程(模拟 reducer):apply → 周期性封口(flush_building_now)→ 删除
// 制造全死段 → compact 触发 drop;读者线程并发 search/explain。段封口切换、
// 列表增删、段对象析构全部与查询交叠——pin(shared_ptr 钉住)+ snapshot
// (列表锁)+ building 原子 shared_ptr 保证无 UAF/race(TSan/ASan 树跑本例)。
#include <thread>

TEST(TextPlugin, SegmentLifecycleVsQueryStress) {
    const fs::path dir =
        fs::temp_directory_path() / "bitcask_textplugin_seg_stress";
    fs::remove_all(dir);
    fs::create_directories(dir);

    index::Index idx;
    auto cfg = make_cfg();
    cfg.auto_compact_dead_ratio = 0.3;  // 开启:删除堆积触发段压实/drop
    text::TextPlugin p(cfg, idx, idx, idx);
    const std::string dir_s = dir.string();  // string_view 指向须存活
    plugin::OpenContext ctx;
    ctx.dir = dir_s;
    ASSERT_EQ(p.open(ctx), plugin::PluginStatus::kOk);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> anomalies{0};

    std::thread writer([&] {
        std::uint64_t ord = 0;
        int round = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            // 一批文档 → 封口成段 → 全部删除(制造全死段) → 下一轮
            // compact(经 apply 内 maybe_auto_compact)可 drop 之。
            const int base = round * 8;
            for (int i = 0; i < 8; ++i) {
                const std::string key = "k" + std::to_string(base + i);
                host_put_row(idx, key, ord);
                p.apply_text(key, ord, "stress common word" + std::to_string(i));
                ++ord;
            }
            p.flush_building_now();
            for (int i = 0; i < 8; ++i) {
                p.on_delete("k" + std::to_string(base + i), ord + 100,
                            static_cast<std::uint64_t>(ord - 8 + i));
            }
            ++round;
        }
    });

    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto r = p.search_text("common", 20);
                if (!r.has_value()) anomalies.fetch_add(1);
                (void)p.search_text("word3", 5);
                (void)p.explain("stress", "k1");
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    stop.store(true);
    writer.join();
    for (auto& r : readers) r.join();

    EXPECT_EQ(anomalies.load(), 0u);
    fs::remove_all(dir);
}

// ===========================================================================
// S27-4 P2:BuilderPool(B=1,串行等价)。on_put 路由派发 → builder 线程
// apply;drain() 补 read-your-writes;flush 前置 drain;LSN 守卫仲裁
// 在途 put vs 删除。
// ===========================================================================

// 基本可见性 + 覆盖 + 删除:全部经 on_put 事件路由(builder 派发),drain
// 后查询结果与内联模式逐点一致。
TEST(TextPlugin, BuilderModeVisibilityOverwriteDelete) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_tp_builder_vis";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string dir_s = dir.string();

    index::Index idx;
    auto cfg = make_cfg();
    cfg.builder_threads = 1;
    text::TextPlugin p(cfg, idx, idx, idx);
    plugin::OpenContext ctx;
    ctx.dir = dir_s;
    ASSERT_EQ(p.open(ctx), plugin::PluginStatus::kOk);

    for (std::uint64_t ord = 0; ord < 3; ++ord) {
        const std::string key = "k" + std::to_string(ord);
        host_put_row(idx, key, ord);
        plugin::DocView dv;
        const std::string text = "hello world " + std::to_string(ord);
        dv.text = text;
        plugin::PutEvent e;
        e.ord = ord;
        e.key = key;
        e.doc = &dv;
        e.loc = plugin::RecordLoc{1, ord * 100, 50};
        e.tstamp = static_cast<std::uint32_t>(1000 + ord);
        p.on_put(e, nullptr);  // 单文本活写:prep 为空 → raw 路径派发
    }
    p.drain();
    {
        auto r = p.search_text("hello", 10);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->size(), 3u);
    }

    // 覆盖 k1(ord=3):旧版本 mark_dead,新词可查、旧独有词消失。
    {
        host_put_row(idx, "k1", 3);
        plugin::DocView dv;
        dv.text = "banana split";
        plugin::PutEvent e;
        e.ord = 3;
        e.key = "k1";
        e.doc = &dv;
        e.loc = plugin::RecordLoc{1, 300, 50};
        e.tstamp = 2000;
        p.on_put(e, nullptr);
        p.drain();
        auto rb = p.search_text("banana", 10);
        ASSERT_TRUE(rb.has_value());
        ASSERT_EQ(rb->size(), 1u);
        EXPECT_EQ((*rb)[0].key, "k1");
        auto rh = p.search_text("hello", 10);
        ASSERT_TRUE(rh.has_value());
        EXPECT_EQ(rh->size(), 2u);  // k0/k2
    }

    // 删除 vs 在途 put(LSN 守卫):k9 的 put(ord=9)入队后立刻删(tomb=10,
    // reducer 内联)——无论 apply 先后,墓碑 ord 胜出,drain 后不可见。
    {
        host_put_row(idx, "k9", 9);
        plugin::DocView dv;
        dv.text = "zebra quark";
        plugin::PutEvent e;
        e.ord = 9;
        e.key = "k9";
        e.doc = &dv;
        e.loc = plugin::RecordLoc{1, 900, 50};
        e.tstamp = 3000;
        p.on_put(e, nullptr);
        p.on_delete("k9", /*tomb_ord=*/10, /*prior_ord=*/9);
        p.drain();
        auto r = p.search_text("zebra", 10);
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->empty());
    }
    ASSERT_EQ(p.close(), plugin::PluginStatus::kOk);
    fs::remove_all(dir);
}

// 多字段 job 路径:prepare(map 阶段分析)产物经 on_put 派发 builder,
// search_fields 命中。
TEST(TextPlugin, BuilderModeMultiFieldJobDispatch) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_tp_builder_mf";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string dir_s = dir.string();

    index::Index idx;
    auto cfg = make_cfg();
    cfg.builder_threads = 1;
    text::TextPlugin p(cfg, idx, idx, idx);
    plugin::OpenContext ctx;
    ctx.dir = dir_s;
    ASSERT_EQ(p.open(ctx), plugin::PluginStatus::kOk);

    const std::vector<plugin::FieldKV> fields = {{"title", "quantum leap"},
                                                 {"body", "engine room"}};
    plugin::DocView dv;
    dv.fields = fields;
    plugin::PutEvent e;
    e.ord = 0;
    e.key = "doc0";
    e.doc = &dv;
    e.loc = plugin::RecordLoc{1, 0, 80};
    e.tstamp = 1000;
    auto prep = p.prepare(e);
    ASSERT_NE(prep, nullptr);
    host_put_row(idx, "doc0", 0);
    p.on_put(e, std::move(prep));
    p.drain();

    auto rt = p.search_fields("title:quantum", 10);
    ASSERT_TRUE(rt.has_value());
    ASSERT_EQ(rt->size(), 1u);
    EXPECT_EQ((*rt)[0].key, "doc0");
    auto rc = p.search_text("engine", 10);  // catch-all 合并
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(rc->size(), 1u);
    ASSERT_EQ(p.close(), plugin::PluginStatus::kOk);
    fs::remove_all(dir);
}

// flush 前置 drain(在途 job 计入 ckpt)+ 跨模式重开:B=1 写入 → flush →
// B=0 重开查询命中(段格式与模式无关)。
TEST(TextPlugin, BuilderModeFlushRoundTrip) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_tp_builder_rt";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string dir_s = dir.string();

    index::Index idx;
    auto cfg = make_cfg();
    cfg.builder_threads = 1;
    text::TextPlugin a(cfg, idx, idx, idx);
    plugin::OpenContext ctx;
    ctx.dir = dir_s;
    ASSERT_EQ(a.open(ctx), plugin::PluginStatus::kOk);

    for (std::uint64_t ord = 0; ord < 5; ++ord) {
        const std::string key = "k" + std::to_string(ord);
        host_put_row(idx, key, ord);
        plugin::DocView dv;
        const std::string text = "gamma ray " + std::to_string(ord);
        dv.text = text;
        plugin::PutEvent e;
        e.ord = ord;
        e.key = key;
        e.doc = &dv;
        e.loc = plugin::RecordLoc{1, ord * 100, 50};
        e.tstamp = static_cast<std::uint32_t>(1000 + ord);
        a.on_put(e, nullptr);
    }
    // 不显式 drain——flush 自身必须先排干(否则 covered_ord 撒谎)。
    plugin::FlushRequest req;
    req.watermark = 5;
    auto fr = a.flush(req);
    ASSERT_EQ(fr.status, plugin::PluginStatus::kOk);
    EXPECT_EQ(fr.covered_ord, 5u);
    const auto st = a.chain_state();
    ASSERT_EQ(a.close(), plugin::PluginStatus::kOk);

    index::Index idx2;
    for (std::uint64_t ord = 0; ord < 5; ++ord) {
        host_put_row(idx2, "k" + std::to_string(ord), ord);
    }
    text::TextPlugin b(make_cfg(), idx2, idx2, idx2);  // B=0 内联模式重开
    plugin::OpenContext c2;
    c2.dir = dir_s;
    c2.committed_base_watermark = st.base_gen;
    c2.committed_chain_watermark = st.chain_wm;
    c2.committed_chain_seq = st.next_seq - 1;
    ASSERT_EQ(b.open(c2), plugin::PluginStatus::kOk);
    EXPECT_EQ(b.watermark(), 5u);
    auto r = b.search_text("gamma", 10);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 5u);
    fs::remove_all(dir);
}

// 并发面:writer 经 on_put 持续派发(builder 线程 apply)+ 2 个查询线程
// 不 drain 直查(refresh 语义:结果可滞后,不可崩/不可见异常计数)。
TEST(TextPlugin, BuilderModeConcurrentQueryStress) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_tp_builder_stress";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string dir_s = dir.string();

    index::Index idx;
    auto cfg = make_cfg();
    cfg.builder_threads = 1;
    text::TextPlugin p(cfg, idx, idx, idx);
    plugin::OpenContext ctx;
    ctx.dir = dir_s;
    ASSERT_EQ(p.open(ctx), plugin::PluginStatus::kOk);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> anomalies{0};
    std::vector<std::thread> readers;
    readers.reserve(2);
    for (int t = 0; t < 2; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto r = p.search_text("stress", 5);
                if (!r.has_value()) anomalies.fetch_add(1);
            }
        });
    }
    constexpr std::uint64_t kDocs = 2000;
    for (std::uint64_t ord = 0; ord < kDocs; ++ord) {
        const std::string key = "s" + std::to_string(ord);
        host_put_row(idx, key, ord);
        plugin::DocView dv;
        const std::string text = "stress doc " + std::to_string(ord);
        dv.text = text;
        plugin::PutEvent e;
        e.ord = ord;
        e.key = key;
        e.doc = &dv;
        e.loc = plugin::RecordLoc{1, ord * 100, 50};
        e.tstamp = static_cast<std::uint32_t>(1000 + ord);
        p.on_put(e, nullptr);
        if (ord % 7 == 0) {  // 混入删除(reducer 内联,LSN 守卫仲裁)
            p.on_delete(key, ord + kDocs, ord);
        }
    }
    p.drain();
    stop.store(true);
    for (auto& r : readers) r.join();
    EXPECT_EQ(anomalies.load(), 0u);

    auto r = p.search_text("stress", static_cast<std::size_t>(kDocs));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), kDocs - (kDocs + 6) / 7);  // 删除的 ord%7==0 不可见
    ASSERT_EQ(p.close(), plugin::PluginStatus::kOk);
    fs::remove_all(dir);
}
