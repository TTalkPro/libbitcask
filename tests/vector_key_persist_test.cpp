// S45-A：向量插件的 key + LSN 定位表随向量检查点持久化（kVecKeyLoc /
// kVecKeyLocDelta），重开时不再依赖 docmap 反查节点 key。
//
// 覆盖：
//   1. 编解码：快照不含墓碑；journal 只记窗口内条目；损坏段拒收。
//   2. 插件级：base + delta 链落盘后，用「什么都不认识」的 docmap 重开——
//      定位表仍完整（来自检查点），同版本重放不重复入图。
//   3. 兼容：base 缺 key 段（旧版检查点）→ 回退 docmap 反查。
//   4. Cask 端到端：模拟「docmap 检查点丢失、向量检查点完好」——重开后图节点
//      数不变、每个存活 key 的当前向量仍是 top-1。对照组：同时剥掉向量
//      检查点的 key 段（= 修改前的行为）必然长出重复节点。

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/detail/vector_key_loc.hpp>
#include <bitcask/index_registry.hpp>
#include <bitcask/search_checkpoint.hpp>
#include <bitcask/vector_plugin.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "support/test_paths.hpp"

namespace {

namespace fs = std::filesystem;
namespace sc = bitcask::search;
using bitcask::vec::detail::VectorKeyLocations;

// ---------------------------------------------------------------- 1. 编解码

TEST(VectorKeyPersist, SnapshotSkipsTombsAndRoundTrips) {
    VectorKeyLocations t;
    ASSERT_TRUE(t.admit_put("a", 3));
    ASSERT_TRUE(t.admit_put("b", 5));
    ASSERT_TRUE(t.admit_put("a", 9));
    t.on_delete("b", 7);  // 墓碑：不进快照
    std::vector<std::byte> snap;
    t.encode_snapshot(snap);

    std::map<std::string, std::uint64_t> got;
    ASSERT_TRUE(VectorKeyLocations::parse(
        snap, [&](std::uint64_t ord, std::string_view k) { got[std::string(k)] = ord; }));
    EXPECT_EQ(got, (std::map<std::string, std::uint64_t>{{"a", 9}}));

    VectorKeyLocations r;
    ASSERT_TRUE(r.load_section(snap));
    EXPECT_EQ(r.ord_of("a"), 9u);
    EXPECT_FALSE(r.admit_put("a", 9));  // 同版本重放
    EXPECT_FALSE(r.admit_put("a", 4));  // 更旧版本
    EXPECT_TRUE(r.admit_put("a", 10));
}

TEST(VectorKeyPersist, JournalRecordsOnlyWindowEntries) {
    VectorKeyLocations t;
    t.set_journal_window(100);
    ASSERT_TRUE(t.admit_put("old", 50));   // 窗口之下：不记 journal
    ASSERT_TRUE(t.admit_put("new", 120));
    ASSERT_TRUE(t.admit_put("new", 130));  // 同 key 两版都记（载入取 max）
    std::vector<std::byte> j;
    t.encode_journal(j);
    std::vector<std::pair<std::uint64_t, std::string>> got;
    ASSERT_TRUE(VectorKeyLocations::parse(j, [&](std::uint64_t o, std::string_view k) {
        got.emplace_back(o, std::string(k));
    }));
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[1], (std::pair<std::uint64_t, std::string>{130, "new"}));

    VectorKeyLocations r;
    ASSERT_TRUE(r.load_section(j));
    EXPECT_EQ(r.ord_of("new"), 130u);
    t.clear_journal();
    EXPECT_TRUE(t.journal_empty());
}

TEST(VectorKeyPersist, TruncatedSectionRejected) {
    VectorKeyLocations t;
    ASSERT_TRUE(t.admit_put("key-1", 1));
    std::vector<std::byte> snap;
    t.encode_snapshot(snap);
    snap.pop_back();
    VectorKeyLocations r;
    EXPECT_FALSE(r.load_section(snap));
    snap.push_back(std::byte{0});
    snap.push_back(std::byte{0});  // 尾部多余字节
    EXPECT_FALSE(r.load_section(snap));
}

// ---------------------------------------------------------------- 2/3. 插件级

// 「什么都不认识」的 docmap：重开时回退反查必然一无所获。
struct BlindDocTable final : bitcask::bm25::DocTable {
    [[nodiscard]] bool is_live(std::uint64_t) const override { return true; }
    [[nodiscard]] std::uint32_t doc_len(std::uint64_t) const override { return 1; }
    [[nodiscard]] std::optional<std::string> ord_to_ext(std::uint64_t) const override {
        return std::nullopt;
    }
    [[nodiscard]] bool eval_meta(std::uint64_t,
                                 const bitcask::meta::MetaFilter&) const override {
        return true;
    }
    [[nodiscard]] std::optional<std::uint64_t> ord_of(std::string_view) const override {
        return std::nullopt;
    }
};

struct Ev {
    std::vector<float> v;
    bitcask::plugin::DocView dv;
    bitcask::plugin::PutEvent ev;
    std::string key;
    Ev(std::string k, std::uint64_t ord, std::vector<float> vec)
        : v(std::move(vec)), key(std::move(k)) {
        dv.vec = std::span<const float>(v.data(), v.size());
        ev.doc = &dv;
        ev.key = key;
        ev.ord = ord;
    }
};

bitcask::vec::VectorPluginConfig cfg4() {
    bitcask::vec::VectorPluginConfig c;
    c.dim = 4;
    c.metric = bitcask::meta::VectorMetric::kCosineNormalized;
    return c;
}

class VectorKeyPersistPlugin : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = bitcask::test::unique_tmpdir(
            std::string("veckeyloc_") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        dir_s_ = dir_.string();
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // 写入 base（wm=10：k2@2 k5@5 k7@7）+ delta（wm=20：k5@11 k12@12）。
    void write_chain(std::uint64_t& base_wm, std::uint32_t& chain_seq) {
        BlindDocTable dt;
        bitcask::vec::VectorPlugin p(cfg4(), dt);
        bitcask::plugin::OpenContext ctx;
        ctx.dir = dir_s_;
        ASSERT_EQ(p.open(ctx), bitcask::plugin::PluginStatus::kOk);
        for (auto [k, o] : std::vector<std::pair<std::string, std::uint64_t>>{
                 {"k7", 7}, {"k2", 2}, {"k5", 5}}) {
            Ev e(k, o, {1, static_cast<float>(o), 0, 0});
            p.on_put(e.ev, nullptr);
        }
        bitcask::plugin::FlushRequest base;
        base.force_rebase = true;
        base.watermark = 10;
        auto r1 = p.flush(base);
        ASSERT_EQ(r1.status, bitcask::plugin::PluginStatus::kOk);
        for (auto [k, o] : std::vector<std::pair<std::string, std::uint64_t>>{
                 {"k12", 12}, {"k5", 11}}) {  // 乱序
            Ev e(k, o, {0, 1, static_cast<float>(o), 0});
            p.on_put(e.ev, nullptr);
        }
        bitcask::plugin::FlushRequest delta;
        delta.watermark = 20;
        auto r2 = p.flush(delta);
        ASSERT_EQ(r2.status, bitcask::plugin::PluginStatus::kOk);
        ASSERT_EQ(r2.chain_seq, 1u) << "第二次 flush 必须走 delta";
        EXPECT_EQ(p.size(), 5u);
        base_wm = 10;
        chain_seq = r2.chain_seq;
    }

    void replay_all(bitcask::vec::VectorPlugin& p) {
        for (auto [k, o] : std::vector<std::pair<std::string, std::uint64_t>>{
                 {"k2", 2}, {"k5", 5}, {"k7", 7}, {"k5", 11}, {"k12", 12}}) {
            Ev e(k, o, {1, 1, 1, static_cast<float>(o)});
            p.on_put(e.ev, nullptr);
        }
    }

    fs::path dir_;
    std::string dir_s_;
};

TEST_F(VectorKeyPersistPlugin, KeyTableReloadsFromCheckpointWithoutDocmap) {
    std::uint64_t wm = 0;
    std::uint32_t seq = 0;
    ASSERT_NO_FATAL_FAILURE(write_chain(wm, seq));

    BlindDocTable dt;
    bitcask::vec::VectorPlugin p(cfg4(), dt);
    bitcask::plugin::OpenContext ctx;
    ctx.dir = dir_s_;
    ctx.committed_base_watermark = wm;
    ctx.committed_chain_seq = seq;
    ASSERT_EQ(p.open(ctx), bitcask::plugin::PluginStatus::kOk);
    EXPECT_TRUE(p.key_loc_from_ckpt_for_test());
    EXPECT_EQ(p.size(), 5u);
    EXPECT_EQ(p.key_ord_for_test("k5"), 11u);  // delta 的更新版本胜出
    EXPECT_EQ(p.key_ord_for_test("k12"), 12u);
    EXPECT_EQ(p.key_ord_for_test("k2"), 2u);

    replay_all(p);  // 全部已在图里：一个都不得重复插入
    EXPECT_EQ(p.size(), 5u);
    Ev fresh("k30", 30, {0, 0, 0, 1});
    p.on_put(fresh.ev, nullptr);
    EXPECT_EQ(p.size(), 6u);
}

TEST_F(VectorKeyPersistPlugin, BaseWithoutKeySectionFallsBackToDocmap) {
    std::uint64_t wm = 0;
    std::uint32_t seq = 0;
    ASSERT_NO_FATAL_FAILURE(write_chain(wm, seq));

    // 把 vec.ckpt 改写成旧版布局：剥掉 kVecKeyLoc 段，其它原样。
    const std::string fp = (dir_ / "vec.ckpt").string();
    auto lc = sc::SearchCheckpoint::read(fp);
    ASSERT_TRUE(lc);
    std::vector<sc::CkptSection> secs;
    bool stripped = false;
    for (const auto& s : lc->sections) {
        if (s.type == static_cast<std::uint16_t>(sc::CkptSectionType::kVecKeyLoc)) {
            stripped = true;
            continue;
        }
        secs.push_back(sc::CkptSection{s.type, s.flags,
                                       std::span<const std::byte>(s.payload)});
    }
    ASSERT_TRUE(stripped);
    ASSERT_TRUE(sc::SearchCheckpoint::write(fp, lc->watermark, secs));

    BlindDocTable dt;
    bitcask::vec::VectorPlugin p(cfg4(), dt);
    bitcask::plugin::OpenContext ctx;
    ctx.dir = dir_s_;
    ctx.committed_base_watermark = wm;
    ctx.committed_chain_seq = seq;
    ASSERT_EQ(p.open(ctx), bitcask::plugin::PluginStatus::kOk);
    EXPECT_FALSE(p.key_loc_from_ckpt_for_test()) << "缺 key 段必须回退";
    EXPECT_EQ(p.size(), 5u);  // 图照常载入
    EXPECT_EQ(p.key_loc_size_for_test(), 0u);  // 盲 docmap 反查一无所获
}

// ---------------------------------------------------------------- 4. Cask 端到端

constexpr std::uint16_t kDim = 8;

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

bitcask::CaskOptions vec_opts() {
    bitcask::CaskOptions o;
    o.read_write = true;
    o.enable_search = true;
    bitcask::search::SearchLayerConfig s;
    s.analyzer_config.type = bitcask::text::AnalyzerType::Whitespace;
    o.search_config = s;
    o.vector_dim = kDim;
    o.vector_metric = bitcask::meta::VectorMetric::kCosineNormalized;
    return o;
}

class VectorKeyPersistCask : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = bitcask::test::unique_tmpdir(
            std::string("veckeyloc_cask_") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // 写 400 篇（含覆盖与删除），close；返回 close 前的节点数。
    std::size_t populate() {
        bitcask::oki::IndexRegistry reg;
        auto oc = bitcask::Cask::open(dir_.string(), vec_opts(), &reg);
        EXPECT_TRUE(oc) << (oc ? "" : oc.error().detail);
        if (!oc) return 0;
        auto c = std::move(*oc);
        std::mt19937_64 rng(7);
        std::uniform_real_distribution<float> uf(-1.0f, 1.0f);
        for (int i = 0; i < 400; ++i) {
            const std::string key = "k" + std::to_string(i % 250);  // 150 个覆盖
            std::vector<float> v(kDim);
            for (auto& x : v) x = uf(rng);
            const std::string text = "doc " + key;
            bitcask::DocInput d;
            d.text = bytes(text);
            d.vector = v;
            EXPECT_TRUE(c->put_doc(bytes(key), d));
            current_[key] = v;
        }
        for (int i = 0; i < 250; i += 5) {  // 50 个删除
            const std::string key = "k" + std::to_string(i);
            EXPECT_TRUE(c->remove(bytes(key)));
            current_.erase(key);
        }
        c->flush_index();
        const std::size_t nodes = c->vector_plugin()->size();
        c->close();
        return nodes;
    }

    // 模拟「docmap 检查点没落成、向量检查点落成了」：删掉 docmap 的全部
    // 检查点文件（base、.prev、增量链）。
    void drop_docmap_ckpt() {
        std::size_t removed = 0;
        for (const auto& e : fs::recursive_directory_iterator(dir_)) {
            const auto n = e.path().filename().string();
            if (n.rfind("docmap.ckpt", 0) == 0) {
                fs::remove(e.path());
                ++removed;
            }
        }
        ASSERT_GT(removed, 0u);
    }

    void strip_vec_key_sections() {
        for (const auto& e : fs::recursive_directory_iterator(dir_)) {
            const auto n = e.path().filename().string();
            if (n.rfind("vec.ckpt", 0) != 0) continue;
            auto lc = sc::SearchCheckpoint::read(e.path().string());
            if (!lc) continue;
            std::vector<sc::CkptSection> secs;
            for (const auto& s : lc->sections) {
                if (s.type == static_cast<std::uint16_t>(sc::CkptSectionType::kVecKeyLoc) ||
                    s.type ==
                        static_cast<std::uint16_t>(sc::CkptSectionType::kVecKeyLocDelta)) {
                    continue;
                }
                secs.push_back(sc::CkptSection{s.type, s.flags,
                                               std::span<const std::byte>(s.payload)});
            }
            ASSERT_TRUE(sc::SearchCheckpoint::write(e.path().string(), lc->watermark, secs));
        }
    }

    struct Reopened {
        std::size_t nodes = 0;
        bool from_ckpt = false;
        std::size_t not_top1 = 0;
    };
    Reopened reopen() {
        Reopened r;
        bitcask::oki::IndexRegistry reg;
        auto oc = bitcask::Cask::open(dir_.string(), vec_opts(), &reg);
        EXPECT_TRUE(oc) << (oc ? "" : oc.error().detail);
        if (!oc) return r;
        auto c = std::move(*oc);
        c->flush_index();
        r.nodes = c->vector_plugin()->size();
        const auto* hp =
            dynamic_cast<const bitcask::vec::VectorPlugin*>(c->vector_plugin());
        EXPECT_NE(hp, nullptr) << "默认引擎应为 HNSW";
        r.from_ckpt = hp != nullptr && hp->key_loc_from_ckpt_for_test();
        for (const auto& [key, v] : current_) {
            auto vr = c->search_vector(v, 1, 4096);
            if (!vr || vr->hits.empty() || vr->hits[0].key != key) ++r.not_top1;
        }
        EXPECT_EQ(c->docmap()->live_docs(), current_.size());
        c->close();
        return r;
    }

    fs::path dir_;
    std::map<std::string, std::vector<float>> current_;
};

TEST_F(VectorKeyPersistCask, DocmapCheckpointLostNoDuplicateVectorNodes) {
    const std::size_t nodes = populate();
    ASSERT_GT(nodes, 0u);
    drop_docmap_ckpt();
    const auto r = reopen();
    EXPECT_TRUE(r.from_ckpt) << "定位表必须来自向量检查点";
    EXPECT_EQ(r.nodes, nodes) << "重开重放不得插入重复向量节点";
    EXPECT_EQ(r.not_top1, 0u);
    // 再重开一轮：稳定。
    const auto r2 = reopen();
    EXPECT_EQ(r2.nodes, nodes);
    EXPECT_EQ(r2.not_top1, 0u);
}

// 对照组：向量检查点也不带 key 段（= 持久化 key 之前的行为），同样丢
// docmap 检查点 → 回退反查一无所获 → 当前版本被重复插入。证明上一用例
// 守住的正是 key 段。
TEST_F(VectorKeyPersistCask, ControlWithoutKeySectionsDuplicates) {
    const std::size_t nodes = populate();
    ASSERT_GT(nodes, 0u);
    drop_docmap_ckpt();
    strip_vec_key_sections();
    const auto r = reopen();
    EXPECT_FALSE(r.from_ckpt);
    EXPECT_GT(r.nodes, nodes) << "对照组应观察到重复节点";
    EXPECT_EQ(r.not_top1, 0u);  // 结果仍正确（查询按 ord 去重）
}

}  // namespace
