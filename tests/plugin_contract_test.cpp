// 插件契约测试（S45-4 改写：索引管线去全局顺序化后的契约，设计
// designs/index-pipeline-unordered-design-zh.md §4.7）。不链 search。
//
// 宿主分发直接复用 IndexDispatcher（Cask 装配点同一份代码）——本测试守护的
// 是真实宿主语义，而非一份拷贝：
//   ① 分片插件：不同 key 并发到达、同 key 串行且按提交序；PutEvent::shard
//      与宿主分片一致；prepare 与 on_put 在同线程配对移交。
//   ② 串行插件：全部事件在同一线程到达；accepts_put/accepts_delete 过滤生效。
//   ③ prepare 抛异常 → 错误上报 + on_put 仍到达（空 prep）+ LSN 照常完成。
//   ④ 规则 R：裁决判过期的写不送达任何插件；docmap 不登记。
//   ⑤ DocMap 恒先于插件：on_put 时 docmap 已有本 ord 的 live 行；on_delete
//      时 prior_ord = 被删的最新版本、key 已移除；更旧版本的删除过期丢弃。
//   ⑥ run_serialized（独占维护）执行期间无 on_put/on_delete 并发。
//   ⑦ 双 lane 并发写入无 race（TSan 场景）。

#include <atomic>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "bitcask/index.hpp"
#include "bitcask/index_authority.hpp"
#include "bitcask/index_dispatch.hpp"
#include "bitcask/plugin_api.hpp"
#include "bitcask/thread_pool.hpp"

namespace {

using namespace std::chrono_literals;
namespace bp = bitcask::plugin;
using bitcask::IndexDispatcher;
using bitcask::IndexLane;
using bitcask::IndexOp;
using bitcask::IndexPool;
using bitcask::IndexTask;
using bitcask::LaneCallbacks;
using bitcask::index::Index;

struct MockPrepared final : bp::Prepared {
    std::uint64_t ord = 0;
    std::thread::id tid;
};

// 记录型插件：线程安全记录（分片插件的事件会并发到达）。
class RecordingPlugin final : public bp::CaskPlugin {
public:
    RecordingPlugin(Concurrency c, bool wants_prep) : conc_(c), wants_prep_(wants_prep) {}

    std::string_view name() const override { return "rec"; }
    bp::PluginStatus open(const bp::OpenContext&) override { return bp::PluginStatus::kOk; }
    std::uint64_t watermark() const override { return 0; }
    bp::PluginStatus close() override { return bp::PluginStatus::kOk; }
    bp::FlushResult flush(const bp::FlushRequest&) override { return {bp::PluginStatus::kOk, 0, 0}; }
    Concurrency concurrency() const override { return conc_; }
    bool wants_prepare() const override { return wants_prep_; }
    bool accepts_put(const bp::PutEvent& e) const override {
        return !only_vec_ || (e.doc && !e.doc->vec.empty());
    }
    bool accepts_delete() const override { return accept_delete_; }

    bp::PreparedPtr prepare(const bp::PutEvent& e) const override {
        if (throw_in_prepare_) throw std::runtime_error("prepare boom");
        auto p = std::make_unique<MockPrepared>();
        p->ord = e.ord;
        p->tid = std::this_thread::get_id();
        return p;
    }
    void on_put(const bp::PutEvent& e, bp::PreparedPtr prep) override {
        enter(e.key);
        std::lock_guard<std::mutex> lk(mu_);
        puts_.push_back(e.ord);
        per_key_[std::string(e.key)].push_back(e.ord);
        threads_.insert(std::this_thread::get_id());
        shards_[std::string(e.key)] = e.shard;
        if (auto* mp = static_cast<MockPrepared*>(prep.get())) {
            paired_ok_ = paired_ok_ && mp->ord == e.ord &&
                         mp->tid == std::this_thread::get_id();
            ++preps_;
        } else {
            ++null_preps_;
        }
        if (exclusive_running_ && exclusive_running_->load()) exclusive_violation_ = true;
        leave(e.key);
    }
    void on_delete(const bp::DeleteEvent& e) override {
        std::lock_guard<std::mutex> lk(mu_);
        dels_.push_back({e.ord, e.prior_ord});
        threads_.insert(std::this_thread::get_id());
        if (exclusive_running_ && exclusive_running_->load()) exclusive_violation_ = true;
    }

    // 同 key 并发检测。
    void enter(std::string_view k) {
        std::lock_guard<std::mutex> lk(key_mu_);
        if (!in_key_.insert(std::string(k)).second) same_key_overlap_ = true;
    }
    void leave(std::string_view k) {
        std::lock_guard<std::mutex> lk(key_mu_);
        in_key_.erase(std::string(k));
    }

    Concurrency conc_;
    bool wants_prep_;
    bool throw_in_prepare_ = false;
    bool only_vec_ = false;
    bool accept_delete_ = true;
    std::atomic<bool>* exclusive_running_ = nullptr;

    std::mutex mu_;
    std::vector<std::uint64_t> puts_;
    std::map<std::string, std::vector<std::uint64_t>> per_key_;
    std::map<std::string, unsigned> shards_;
    std::set<std::thread::id> threads_;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> dels_;
    std::size_t preps_ = 0, null_preps_ = 0;
    bool paired_ok_ = true;
    bool exclusive_violation_ = false;
    std::mutex key_mu_;
    std::set<std::string> in_key_;
    bool same_key_overlap_ = false;
};

// 观察型插件：on_put/on_delete 内查宿主 docmap 状态（⑤）。
class ObservingPlugin final : public bp::CaskPlugin {
public:
    explicit ObservingPlugin(const Index* dm) : docmap_(dm) {}
    // 分片插件：与宿主 docmap 更新同线程同步执行，观察到的正是「本事件
    // 应用后」的 docmap 状态。（串行插件在串行道稍后执行，可能观察到同 key
    // 更新的版本——契约只保证 docmap 先于插件，不保证插件执行时 docmap
    // 仍停在本版本。）
    Concurrency concurrency() const override { return Concurrency::kSharded; }
    std::string_view name() const override { return "observer"; }
    bp::PluginStatus open(const bp::OpenContext&) override { return bp::PluginStatus::kOk; }
    std::uint64_t watermark() const override { return 0; }
    bp::PluginStatus close() override { return bp::PluginStatus::kOk; }
    bp::FlushResult flush(const bp::FlushRequest&) override { return {bp::PluginStatus::kOk, 0, 0}; }
    void on_put(const bp::PutEvent& e, bp::PreparedPtr) override {
        PutObs o;
        o.ord = e.ord;
        o.docmap_live = docmap_->is_live(e.ord);
        if (auto slot = docmap_->get(e.key)) {
            o.maps_to_this_ord = slot->ord == e.ord;
            o.loc_match = slot->loc.file_id == e.loc.file_id &&
                          slot->loc.offset == e.loc.offset &&
                          slot->loc.total_sz == e.loc.total_sz;
        }
        o.meta_size = docmap_->meta_blob(e.ord).size();
        std::lock_guard<std::mutex> lk(mu_);
        puts_.push_back(o);
    }
    void on_delete(const bp::DeleteEvent& e) override {
        DelObs o{e.ord, e.prior_ord, !docmap_->get(e.key).has_value()};
        std::lock_guard<std::mutex> lk(mu_);
        dels_.push_back(o);
    }
    struct PutObs {
        std::uint64_t ord = 0;
        bool docmap_live = false, maps_to_this_ord = false, loc_match = false;
        std::size_t meta_size = 0;
    };
    struct DelObs {
        std::uint64_t ord = 0;
        std::uint64_t prior_ord = bp::kNoPriorOrd;
        bool key_already_removed = false;
    };
    const Index* docmap_;
    std::mutex mu_;
    std::vector<PutObs> puts_;
    std::vector<DelObs> dels_;
};

// 一个 lane = 一个宿主（dispatcher + pool 车道）。
struct Host {
    IndexDispatcher disp;
    IndexLane* lane = nullptr;
    std::atomic<int> errors{0};
    Host(IndexPool& pool, std::vector<bp::CaskPlugin*> plugins, Index* docmap = nullptr,
         std::uint64_t init = 0) {
        disp.set_plugins(plugins);
        disp.docmap = docmap;
        disp.on_error = [this] { ++errors; };
        LaneCallbacks cb;
        cb.shard_apply = [this](IndexTask& t, unsigned s) { return disp.shard_apply(t, s); };
        if (disp.has_serial()) cb.serial_apply = [this](IndexTask& t) { disp.serial_apply(t); };
        cb.on_error = [this] { ++errors; };
        lane = pool.register_lib(std::move(cb), init);
    }
};

IndexTask add(std::string_view key, std::uint64_t ord, std::string_view text = "t") {
    return IndexTask::make(IndexOp::Add, key, ord, text, 1, ord * 10, 8, 0, 0);
}
IndexTask del(std::string_view key, std::uint64_t ord) {
    return IndexTask::make(IndexOp::Delete, key, ord, "", 0, 0, 0, 0, 0);
}

// ① + ②
TEST(PluginContract, ShardedConcurrentSameKeySerialAndSerialSingleThread) {
    IndexPool pool(4);
    RecordingPlugin sharded(bp::CaskPlugin::Concurrency::kSharded, /*prep=*/true);
    RecordingPlugin serial(bp::CaskPlugin::Concurrency::kSerial, /*prep=*/true);
    Host h(pool, {&sharded, &serial});
    std::uint64_t ord = 0;
    constexpr int kKeys = 64, kVersions = 50;
    for (int v = 0; v < kVersions; ++v) {
        for (int k = 0; k < kKeys; ++k) pool.submit(h.lane, add("k" + std::to_string(k), ord++));
    }
    ASSERT_TRUE(pool.wait_lsn(h.lane, ord, 10s));
    EXPECT_EQ(sharded.puts_.size(), ord);
    EXPECT_EQ(serial.puts_.size(), ord);
    EXPECT_FALSE(sharded.same_key_overlap_);
    EXPECT_TRUE(sharded.paired_ok_) << "prepare 与 on_put 须在同线程、同 ord 配对";
    EXPECT_TRUE(serial.paired_ok_);
    EXPECT_GT(sharded.threads_.size(), 1u) << "分片插件应在多个分片线程上执行";
    EXPECT_EQ(serial.threads_.size(), 1u) << "串行插件只在串行道线程执行";
    for (auto& [k, ords] : sharded.per_key_) {
        EXPECT_TRUE(std::is_sorted(ords.begin(), ords.end())) << k;
        EXPECT_EQ(sharded.shards_[k], pool.shard_of(k)) << "PutEvent::shard 与宿主分片一致";
    }
    EXPECT_TRUE(pool.unregister_lib(h.lane));
}

TEST(PluginContract, SerialFiltersSkipUninterestingEvents) {
    IndexPool pool(2);
    RecordingPlugin vec(bp::CaskPlugin::Concurrency::kSerial, false);
    vec.only_vec_ = true;
    vec.accept_delete_ = false;
    Index docmap;
    Host h(pool, {&vec}, &docmap);
    pool.submit(h.lane, add("a", 0));
    auto t = add("b", 1);
    t.vec = {1.0f, 2.0f};
    pool.submit(h.lane, std::move(t));
    pool.submit(h.lane, del("b", 2));
    ASSERT_TRUE(pool.wait_lsn(h.lane, 3, 10s));
    ASSERT_EQ(vec.puts_.size(), 1u);
    EXPECT_EQ(vec.puts_[0], 1u);
    EXPECT_TRUE(vec.dels_.empty());
    EXPECT_TRUE(pool.unregister_lib(h.lane));
}

// ③
TEST(PluginContract, PrepareExceptionCountedAndDelivered) {
    IndexPool pool(2);
    RecordingPlugin p(bp::CaskPlugin::Concurrency::kSharded, true);
    p.throw_in_prepare_ = true;
    Host h(pool, {&p});
    for (std::uint64_t o = 0; o < 10; ++o) pool.submit(h.lane, add("k" + std::to_string(o), o));
    ASSERT_TRUE(pool.wait_lsn(h.lane, 10, 10s));
    EXPECT_EQ(p.puts_.size(), 10u);
    EXPECT_EQ(p.null_preps_, 10u);
    EXPECT_EQ(h.errors.load(), 10);
    EXPECT_TRUE(pool.unregister_lib(h.lane));
}

// ④
TEST(PluginContract, ArbitrationDropsStaleWrites) {
    IndexPool pool(2);
    RecordingPlugin p(bp::CaskPlugin::Concurrency::kSharded, false);
    Index docmap;
    Host h(pool, {&p}, &docmap);
    h.disp.is_current = [](std::string_view, std::uint64_t ord) { return ord % 2 == 0; };
    for (std::uint64_t o = 0; o < 20; ++o) pool.submit(h.lane, add("k" + std::to_string(o), o));
    ASSERT_TRUE(pool.wait_lsn(h.lane, 20, 10s));
    EXPECT_EQ(p.puts_.size(), 10u);
    EXPECT_EQ(h.disp.stale_dropped(), 10u);
    EXPECT_FALSE(docmap.get("k1").has_value()) << "过期写不登记 docmap";
    EXPECT_TRUE(docmap.get("k2").has_value());
    EXPECT_TRUE(pool.unregister_lib(h.lane));
}

// ⑤：DocMap 恒先于插件；prior_ord 反映最新版本；乱序到达的更旧删除过期。
TEST(PluginContract, DocmapVisibleBeforePluginAndPriorOrdIsLatest) {
    IndexPool pool(1);  // 单分片：本用例按提交序断言
    Index docmap;
    ObservingPlugin obs(&docmap);
    Host h(pool, {&obs}, &docmap);
    pool.submit(h.lane, add("k0", 0, "alpha"));
    pool.submit(h.lane, add("k0", 1, "beta"));
    {
        auto t = add("k2", 4, "delta");
        t.meta = {std::byte{0xAB}, std::byte{0xCD}};
        pool.submit(h.lane, std::move(t));
    }
    pool.submit(h.lane, del("k0", 2));
    pool.submit(h.lane, del("nope", 3));
    pool.submit(h.lane, add("k5", 6));
    pool.submit(h.lane, del("k5", 5));  // 乱序：删除(5) 晚于写(6) 到达 → 过期
    ASSERT_TRUE(pool.flush(h.lane, 10s));

    ASSERT_EQ(obs.puts_.size(), 4u);
    for (const auto& o : obs.puts_) {
        EXPECT_TRUE(o.docmap_live) << o.ord;
        EXPECT_TRUE(o.maps_to_this_ord) << o.ord;
        EXPECT_TRUE(o.loc_match) << o.ord;
    }
    EXPECT_EQ(obs.puts_[2].meta_size, 2u);
    ASSERT_EQ(obs.dels_.size(), 3u);
    EXPECT_EQ(obs.dels_[0].prior_ord, 1u) << "覆盖写后 prior_ord 应为最新 ord";
    EXPECT_TRUE(obs.dels_[0].key_already_removed);
    EXPECT_EQ(obs.dels_[1].prior_ord, bp::kNoPriorOrd);
    EXPECT_EQ(obs.dels_[2].prior_ord, bp::kNoPriorOrd) << "更旧的删除过期，不删任何版本";
    EXPECT_TRUE(docmap.get("k5").has_value()) << "过期删除不得删掉更新的写";
    EXPECT_TRUE(pool.unregister_lib(h.lane));
}

// ⑥
TEST(PluginContract, RunSerializedExcludesEvents) {
    IndexPool pool(4);
    std::atomic<bool> running{false};
    RecordingPlugin p(bp::CaskPlugin::Concurrency::kSharded, false);
    RecordingPlugin s(bp::CaskPlugin::Concurrency::kSerial, false);
    p.exclusive_running_ = &running;
    s.exclusive_running_ = &running;
    Host h(pool, {&p, &s});
    std::atomic<bool> stop{false};
    std::uint64_t ord = 0;
    std::thread feeder([&] {
        while (!stop.load()) {
            pool.submit(h.lane, add("k" + std::to_string(ord % 97), ord));
            ++ord;
        }
    });
    for (int i = 0; i < 10; ++i) {
        std::promise<void> done;
        pool.run_exclusive(h.lane, [&] {
            running = true;
            std::this_thread::sleep_for(5ms);
            running = false;
            done.set_value();
        });
        done.get_future().wait();
    }
    stop = true;
    feeder.join();
    ASSERT_TRUE(pool.wait_lsn(h.lane, ord, 30s));
    EXPECT_FALSE(p.exclusive_violation_);
    EXPECT_FALSE(s.exclusive_violation_);
    EXPECT_TRUE(pool.unregister_lib(h.lane));
}

// ⑦
TEST(PluginContract, TwoLanesConcurrentNoRace) {
    IndexPool pool(4);
    RecordingPlugin a(bp::CaskPlugin::Concurrency::kSharded, true);
    RecordingPlugin b(bp::CaskPlugin::Concurrency::kSharded, true);
    Index da, db;
    Host ha(pool, {&a}, &da);
    Host hb(pool, {&b}, &db);
    std::thread ta([&] { for (std::uint64_t o = 0; o < 2000; ++o) pool.submit(ha.lane, add("x" + std::to_string(o % 300), o)); });
    std::thread tb([&] { for (std::uint64_t o = 0; o < 2000; ++o) pool.submit(hb.lane, add("y" + std::to_string(o % 300), o)); });
    ta.join();
    tb.join();
    ASSERT_TRUE(pool.wait_lsn(ha.lane, 2000, 10s));
    ASSERT_TRUE(pool.wait_lsn(hb.lane, 2000, 10s));
    EXPECT_EQ(da.live_docs(), 300u);
    EXPECT_EQ(db.live_docs(), 300u);
    EXPECT_TRUE(pool.unregister_lib(ha.lane));
    EXPECT_TRUE(pool.unregister_lib(hb.lane));
}

}  // namespace

// ============================================================================
// S18-11：merge 广播契约（S18-7 设计 §3.9）——run_merge 直调 + mock 插件。
// 断言事件序 begin →（每条 live CAS 成功）relocate× N → commit；失败路径
// begin → abort。run_serialized 先于宿主保存点的 FIFO 由上方
// RunSerializedExcludesEvents + 维护线程 FIFO 单测 + merge 全系集成测试共同覆盖。
// ============================================================================

#include "bitcask/data_file.hpp"
#include "bitcask/merger.hpp"
#include "bitcask/oki_state.hpp"

#include <filesystem>

namespace {

class MergeEventRecorder final : public bp::CaskPlugin {
public:
    std::string_view name() const override { return "merge-recorder"; }
    bp::PluginStatus open(const bp::OpenContext&) override {
        return bp::PluginStatus::kOk;
    }
    std::uint64_t watermark() const override { return 0; }
    bp::PluginStatus close() override { return bp::PluginStatus::kOk; }
    void on_put(const bp::PutEvent&, bp::PreparedPtr) override {}
    void on_delete(const bp::DeleteEvent&) override {}
    bp::FlushResult flush(const bp::FlushRequest&) override {
        return {bp::PluginStatus::kOk, 0, 0};
    }
    void on_merge_begin(const bp::MergeBeginEvent&) override {
        events.push_back("begin");
    }
    void on_relocate(const bp::RelocateEvent& e) override {
        events.push_back("relocate:" + std::string(e.key) + "@" +
                         std::to_string(e.loc.file_id));
        relocated_file_ids.push_back(e.loc.file_id);
    }
    void on_merge_commit(const bp::MergeCommitEvent& e) override {
        events.push_back("commit");
        output_ids.assign(e.output_file_ids.begin(),
                          e.output_file_ids.end());
    }
    void on_merge_abort() override { events.push_back("abort"); }

    std::vector<std::string> events;             // merge 线程单线程访问
    std::vector<std::uint32_t> relocated_file_ids;
    std::vector<std::uint32_t> output_ids;
};

}  // namespace

// 契约⑪：merge 事件序 begin → relocate×N → commit；relocate 携带新定位。
TEST(PluginMergeContract, BeginRelocateCommitOrder) {
    namespace fs = std::filesystem;
    const fs::path dir =
        fs::temp_directory_path() / "bitcask_plugin_merge_contract";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // 手工构造输入 data 文件 + OKI authority（3 条 live record）。
    const std::string data_path =
        bitcask::fileops::mk_data_filename(dir.string(), 1);
    bitcask::oki::IndexAuthority authority;
    {
        auto df = bitcask::fileops::DataFile::open(
            data_path, bitcask::fileops::DataFile::Mode::kCreate);
        ASSERT_TRUE(df.has_value());
        for (int i = 0; i < 3; ++i) {
            const std::string key = "mk" + std::to_string(i);
            const std::string val = "value-" + std::to_string(i);
            auto w = df->write(
                bitcask::format::RecordType::kDoc,
                static_cast<std::uint32_t>(1000 + i),
                static_cast<std::uint64_t>(i),
                std::as_bytes(std::span<const char>(key.data(), key.size())),
                std::as_bytes(std::span<const char>(val.data(), val.size())));
            ASSERT_TRUE(w.has_value());
        }
        ASSERT_TRUE(df->sync().has_value());
    }
    ASSERT_TRUE(authority.rebuild_from_data(dir.string()));
    (void)authority.increment_file_id_at_least(1);
    authority.state().enable_point_query();
    authority.mark_ready();

    MergeEventRecorder rec;
    bitcask::plugin::CaskPlugin* plugs[] = {&rec};
    const std::string inputs[] = {data_path};
    auto r = bitcask::merge::run_merge(
        std::span<const std::string>(inputs), dir.string(), authority,
                                       /*sync*/ false, plugs, /*now*/ 0);
    ASSERT_TRUE(r.has_value()) << r.error().detail;

    ASSERT_EQ(rec.events.size(), 5u);  // begin + 3×relocate + commit
    EXPECT_EQ(rec.events.front(), "begin");
    EXPECT_EQ(rec.events.back(), "commit");
    for (std::size_t i = 1; i <= 3; ++i) {
        EXPECT_EQ(rec.events[i].rfind("relocate:", 0), 0u)
            << "事件序中间必须是 relocate，实际: " << rec.events[i];
    }
    // 搬迁定位指向 merge 输出文件（≠ 输入 file_id 1），且 commit 携带之。
    ASSERT_EQ(rec.output_ids.size(), 1u);
    EXPECT_EQ(rec.output_ids[0], r->output_file_id);
    // S43-7：merge 输出 data-only。旧 ABI 字段保留但必须为空。
    EXPECT_TRUE(r->output_hint_path.empty());
    const std::string stem = r->output_data_path.substr(
        0, r->output_data_path.size() - std::strlen(".data"));
    EXPECT_FALSE(std::filesystem::exists(stem + ".hint"));
    for (auto fid : rec.relocated_file_ids) {
        EXPECT_EQ(fid, r->output_file_id);
        EXPECT_NE(fid, 1u);
    }
    fs::remove_all(dir);
}

// 契约⑫：失败路径 begin → abort（不发 relocate/commit）。失败注入：输出
// 目录不存在 → 输出文件创建失败（缺失输入被 run_merge 宽容为无事可合，
// 不构成失败）。
TEST(PluginMergeContract, AbortOnFailure) {
    namespace fs = std::filesystem;
    const fs::path dir =
        fs::temp_directory_path() / "bitcask_plugin_merge_abort";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const std::string data_path =
        bitcask::fileops::mk_data_filename(dir.string(), 1);
    bitcask::oki::IndexAuthority authority;
    {
        auto df = bitcask::fileops::DataFile::open(
            data_path, bitcask::fileops::DataFile::Mode::kCreate);
        ASSERT_TRUE(df.has_value());
        const std::string key = "mk0";
        const std::string val = "value-0";
        auto w = df->write(
            bitcask::format::RecordType::kDoc, 1000, 0,
            std::as_bytes(std::span<const char>(key.data(), key.size())),
            std::as_bytes(std::span<const char>(val.data(), val.size())));
        ASSERT_TRUE(w.has_value());
        ASSERT_TRUE(df->sync().has_value());
    }
    ASSERT_TRUE(authority.rebuild_from_data(dir.string()));
    (void)authority.increment_file_id_at_least(1);
    authority.state().enable_point_query();
    authority.mark_ready();

    MergeEventRecorder rec;
    bitcask::plugin::CaskPlugin* plugs[] = {&rec};
    const std::string inputs[] = {data_path};
    const std::string bad_out = (dir / "no_such_subdir" / "x").string();
    auto r = bitcask::merge::run_merge(
        std::span<const std::string>(inputs), bad_out, authority,
                                       /*sync*/ false, plugs, /*now*/ 0);
    EXPECT_FALSE(r.has_value());
    ASSERT_EQ(rec.events.size(), 2u);
    EXPECT_EQ(rec.events[0], "begin");
    EXPECT_EQ(rec.events[1], "abort");
    fs::remove_all(dir);
}
