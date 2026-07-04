// S15-4：插件契约 Mock 测试——不链 search，验证 IndexPool 分发通路对
// CaskPlugin 的契约（为 P2 的 DocMap 插队首位、P4 的双插件扇出铺回归床）：
//   - on_put 按 ord 严格升序到达，多插件按注册序固定分发；
//   - wants_prepare()=false 的插件 prepare 不被调用、on_put 收 prep=nullptr；
//   - prepare 产物与事件按 (ord, 插件下标) 精确配对移交；
//   - Skip 空洞不触发 on_put；Delete 广播全插件；
//   - RunFn 在 reducer 静止点按 ord 序执行（与在途 on_put 不交错）；
//   - prepare 抛异常 → error_fn 计数 + on_put 仍到达（空 prep）+ ord 不 stall；
//   - 双 lane 并发写入下扇出/移交无 race（TSan 场景）。
//
// 分发闭包复刻 cask.cpp 装配点的宿主语义（make_doc_view/make_put_event +
// wants_prepare 过滤 + 注册序广播）——本测试同时守护该语义本身。

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "bitcask/plugin_api.hpp"
#include "bitcask/thread_pool.hpp"
#include "bitcask/index.hpp"  // S16-2：宿主 docmap（index::Index 身份/存活/meta）

namespace {

namespace bp = bitcask::plugin;
using bitcask::DeleteEntry;
using bitcask::IndexLane;
using bitcask::IndexOp;
using bitcask::IndexPool;
using bitcask::IndexTask;
using bitcask::PutEntry;
using bitcask::ReorderEntry;
using bitcask::RunFnEntry;
using bitcask::SkipEntry;
using bitcask::index::DocLoc;
using bitcask::index::DocSlot;
using bitcask::index::Index;

struct MockPrepared final : bp::Prepared {
    std::uint64_t ord = 0;
    int           plugin_idx = -1;
};

// 记录型 Mock 插件。数据事件只在 reducer 单写者上下文到达（契约），成员
// 无锁记录；主线程在 flush()（release/acquire 链）之后读取——无 race。
class MockPlugin final : public bp::CaskPlugin {
public:
    MockPlugin(int idx, bool wants_prep, std::vector<std::pair<std::uint64_t, int>>* seq)
        : idx_(idx), wants_prepare_(wants_prep), seq_(seq) {}

    std::string_view name() const override { return "mock"; }
    bp::PluginStatus open(const bp::OpenContext&) override { return bp::PluginStatus::kOk; }
    std::uint64_t watermark() const override { return 0; }
    bp::PluginStatus close() override { return bp::PluginStatus::kOk; }
    bp::FlushResult flush(const bp::FlushRequest&) override {
        return {bp::PluginStatus::kOk, 0, 0};
    }

    bool wants_prepare() const override { return wants_prepare_; }

    bp::PreparedPtr prepare(const bp::PutEvent& e) const override {
        prepare_calls_.fetch_add(1, std::memory_order_relaxed);
        if (throw_in_prepare_) throw std::runtime_error("prepare boom");
        auto p = std::make_unique<MockPrepared>();
        p->ord = e.ord;
        p->plugin_idx = idx_;
        return p;
    }

    void on_put(const bp::PutEvent& e, bp::PreparedPtr prep) override {
        put_ords_.push_back(e.ord);
        if (auto* mp = static_cast<MockPrepared*>(prep.get())) {
            // 配对契约：prep 必须是「本插件」在「同一 ord」上的 prepare 产物。
            paired_ok_ = paired_ok_ && mp->ord == e.ord && mp->plugin_idx == idx_;
            preps_received_.push_back(true);
        } else {
            preps_received_.push_back(false);
        }
        if (seq_) seq_->emplace_back(e.ord, idx_);
    }

    void on_delete(const bp::DeleteEvent& e) override { del_ords_.push_back(e.ord); }

    const int  idx_;
    const bool wants_prepare_;
    bool       throw_in_prepare_ = false;
    std::vector<std::pair<std::uint64_t, int>>* seq_;  // 跨插件全局分发序

    mutable std::atomic<std::size_t> prepare_calls_{0};
    std::vector<std::uint64_t> put_ords_;
    std::vector<bool>          preps_received_;
    std::vector<std::uint64_t> del_ords_;
    bool                       paired_ok_ = true;
};

// 宿主分发闭包（复刻 cask.cpp 装配点语义）。
static bitcask::MapFn host_map(std::vector<bp::CaskPlugin*> plugins) {
    return [plugins](const IndexTask& task) {
        std::vector<bp::PreparedPtr> preps(plugins.size());
        const bp::DocView  doc = bitcask::make_doc_view(task);
        const bp::PutEvent ev  = bitcask::make_put_event(task, &doc);
        for (std::size_t i = 0; i < plugins.size(); ++i) {
            if (plugins[i]->wants_prepare()) preps[i] = plugins[i]->prepare(ev);
        }
        return preps;
    };
}
static bitcask::ReduceFn host_reduce(std::vector<bp::CaskPlugin*> plugins) {
    return [plugins](ReorderEntry& entry) {
        std::visit([&plugins](auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, PutEntry>) {
                const bp::DocView  doc = bitcask::make_doc_view(e.task);
                const bp::PutEvent ev  = bitcask::make_put_event(e.task, &doc);
                for (std::size_t i = 0; i < plugins.size(); ++i) {
                    plugins[i]->on_put(ev, i < e.preps.size()
                                               ? std::move(e.preps[i])
                                               : bp::PreparedPtr{});
                }
            } else if constexpr (std::is_same_v<T, DeleteEntry>) {
                for (auto* p : plugins) p->on_delete(bp::DeleteEvent{e.ord, e.key});
            } else if constexpr (std::is_same_v<T, SkipEntry>) {
            } else if constexpr (std::is_same_v<T, RunFnEntry>) {
                if (e.fn) e.fn();
            }
        }, entry);
    };
}

static IndexTask mk_fields_task(std::string_view key, std::uint64_t ord,
                                std::string_view text) {
    auto t = IndexTask::make(IndexOp::Add, key, ord, text, 1, 0, 0, 0, 0);
    t.fields.assign({{"body", text}});
    return t;
}

// S16-2：复刻 cask.cpp reduce 闭包——宿主先 apply DocMap（put_doc/set_meta、
// 捕 prior_ord + remove），再广播。守护「DocMap 恒在所有插件之前」不变量。
static bitcask::ReduceFn host_reduce_docmap(std::vector<bp::CaskPlugin*> plugins,
                                             Index& docmap) {
    return [plugins = std::move(plugins), &docmap](ReorderEntry& entry) {
        std::visit([&](auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, PutEntry>) {
                const auto& t = e.task;
                docmap.put_doc(t.key(), t.ord,
                                DocSlot{DocLoc{t.file_id, t.offset, t.total_sz},
                                        t.tstamp, /*doc_len=*/0});
                if (!t.meta.empty()) docmap.set_meta(t.ord, t.meta);
                const bp::DocView  doc = bitcask::make_doc_view(t);
                const bp::PutEvent ev  = bitcask::make_put_event(t, &doc);
                for (std::size_t i = 0; i < plugins.size(); ++i) {
                    plugins[i]->on_put(ev, i < e.preps.size()
                                              ? std::move(e.preps[i])
                                              : bp::PreparedPtr{});
                }
            } else if constexpr (std::is_same_v<T, DeleteEntry>) {
                std::uint64_t prior = bp::kNoPriorOrd;
                if (auto slot = docmap.get(e.key)) {
                    prior = slot->ord;
                    docmap.remove(e.key, e.ord);
                }
                for (auto* p : plugins)
                    p->on_delete(bp::DeleteEvent{e.ord, e.key, prior});
            } else if constexpr (std::is_same_v<T, SkipEntry>) {
            } else if constexpr (std::is_same_v<T, RunFnEntry>) {
                if (e.fn) e.fn();
            }
        }, entry);
    };
}

// 契约①②③④⑤：升序、注册序、prepare 过滤、配对移交、Skip/Delete 语义。
TEST(PluginContract, OrdOrderBroadcastAndPreparedPairing) {
    IndexPool pool(2, 10240);
    std::vector<std::pair<std::uint64_t, int>> seq;  // reducer 单线程记录
    MockPlugin a(0, /*wants_prep=*/true, &seq);
    MockPlugin b(1, /*wants_prep=*/false, &seq);
    std::vector<bp::CaskPlugin*> plugins{&a, &b};

    pool.start(host_map(plugins), host_reduce(plugins), [] {});

    // Add(fields){0}, Add(单文本){1}, Skip{2}, Delete{3}, Add(fields){4}
    pool.submit(mk_fields_task("k0", 0, "hello world"));
    pool.submit(IndexTask::make(IndexOp::Add, "k1", 1, "plain text", 1, 0, 0, 0, 0));
    pool.submit(IndexTask::make(IndexOp::Skip, "", 2, "", 0, 0, 0, 0, 0));
    pool.submit(IndexTask::make(IndexOp::Delete, "k0", 3, "", 0, 0, 0, 0, 0));
    pool.submit(mk_fields_task("k4", 4, "more text"));
    pool.flush();

    // ① ord 严格升序（Skip 洞不可见），两插件看到同一事件流。
    const std::vector<std::uint64_t> want_puts{0, 1, 4};
    EXPECT_EQ(a.put_ords_, want_puts);
    EXPECT_EQ(b.put_ords_, want_puts);

    // ② 同一 ord 内按注册序分发：seq = (ord,0),(ord,1) 成对交替。
    ASSERT_EQ(seq.size(), 6u);
    for (std::size_t i = 0; i < seq.size(); i += 2) {
        EXPECT_EQ(seq[i].first, seq[i + 1].first);
        EXPECT_EQ(seq[i].second, 0);
        EXPECT_EQ(seq[i + 1].second, 1);
    }

    // ③ prepare 过滤：a 每个 Add 一次；b 从不被调。
    EXPECT_EQ(a.prepare_calls_.load(), 3u);
    EXPECT_EQ(b.prepare_calls_.load(), 0u);

    // ④ 配对移交：a 收到的 prep 均为自己同 ord 的产物；b 恒 nullptr。
    EXPECT_TRUE(a.paired_ok_);
    EXPECT_EQ(a.preps_received_, (std::vector<bool>{true, true, true}));
    EXPECT_EQ(b.preps_received_, (std::vector<bool>{false, false, false}));

    // ⑤ Delete 广播全插件。
    EXPECT_EQ(a.del_ords_, (std::vector<std::uint64_t>{3}));
    EXPECT_EQ(b.del_ords_, (std::vector<std::uint64_t>{3}));

    pool.stop();
}

// 契约⑥：RunFn 在 reducer 静止点按 ord 序执行，与在途 on_put 不交错。
TEST(PluginContract, RunFnSerializedAtOrdPosition) {
    IndexPool pool(2, 10240);
    std::vector<std::pair<std::uint64_t, int>> seq;
    MockPlugin a(0, true, &seq);
    std::vector<bp::CaskPlugin*> plugins{&a};

    pool.start(host_map(plugins), host_reduce(plugins), [] {});

    pool.submit(mk_fields_task("k0", 0, "t0"));
    {
        IndexTask t;
        t.op  = IndexOp::RunFn;
        t.ord = 1;
        t.fn  = [&seq] { seq.emplace_back(1, /*RunFn 标记*/ -1); };
        pool.submit(std::move(t));
    }
    pool.submit(mk_fields_task("k2", 2, "t2"));
    pool.flush();

    // 精确顺位：on_put(0) → fn(1) → on_put(2)。
    ASSERT_EQ(seq.size(), 3u);
    EXPECT_EQ(seq[0], (std::pair<std::uint64_t, int>{0, 0}));
    EXPECT_EQ(seq[1], (std::pair<std::uint64_t, int>{1, -1}));
    EXPECT_EQ(seq[2], (std::pair<std::uint64_t, int>{2, 0}));
    pool.stop();
}

// 契约⑦：prepare 抛异常 → error_fn 计数 + on_put 仍到达（空 prep，插件自行
// 降级）+ ord 不 stall（S13-D7 语义）。
TEST(PluginContract, PrepareExceptionCountedAndDelivered) {
    IndexPool pool(2, 10240);
    std::atomic<std::size_t> errors{0};
    MockPlugin a(0, true, nullptr);
    a.throw_in_prepare_ = true;
    std::vector<bp::CaskPlugin*> plugins{&a};

    pool.start(host_map(plugins), host_reduce(plugins),
               [&] { errors.fetch_add(1); });

    pool.submit(mk_fields_task("k0", 0, "t0"));
    pool.submit(mk_fields_task("k1", 1, "t1"));
    pool.flush();

    EXPECT_EQ(errors.load(), 2u);
    EXPECT_EQ(a.put_ords_, (std::vector<std::uint64_t>{0, 1}));  // ord 不 stall
    EXPECT_EQ(a.preps_received_, (std::vector<bool>{false, false}));  // 空 prep 降级
    EXPECT_EQ(pool.applied_ord(), 1u);
    pool.stop();
}

// 契约⑧（TSan 场景）：双 lane 并发写入，扇出组 unique_ptr 跨线程移交无 race，
// 各 lane 事件流独立且分别升序。
TEST(PluginContract, TwoLanesConcurrentNoRace) {
    IndexPool pool(2, 10240);
    constexpr int kPerLane = 500;

    MockPlugin a0(0, true, nullptr), b0(1, false, nullptr);
    MockPlugin a1(0, true, nullptr), b1(1, false, nullptr);
    std::vector<bp::CaskPlugin*> lane0_plugins{&a0, &b0};
    std::vector<bp::CaskPlugin*> lane1_plugins{&a1, &b1};

    IndexLane* l0 = pool.register_lib(host_map(lane0_plugins),
                                      host_reduce(lane0_plugins), [] {}, 0);
    IndexLane* l1 = pool.register_lib(host_map(lane1_plugins),
                                      host_reduce(lane1_plugins), [] {}, 0);

    std::thread p0([&] {
        for (int i = 0; i < kPerLane; ++i)
            pool.submit(l0, mk_fields_task("a", static_cast<std::uint64_t>(i), "x"));
    });
    std::thread p1([&] {
        for (int i = 0; i < kPerLane; ++i)
            pool.submit(l1, mk_fields_task("b", static_cast<std::uint64_t>(i), "y"));
    });
    p0.join();
    p1.join();
    pool.flush(l0);
    pool.flush(l1);

    for (const MockPlugin* m : {&a0, &b0, &a1, &b1}) {
        ASSERT_EQ(m->put_ords_.size(), static_cast<std::size_t>(kPerLane));
        for (int i = 0; i < kPerLane; ++i) {
            EXPECT_EQ(m->put_ords_[static_cast<std::size_t>(i)],
                      static_cast<std::uint64_t>(i));
        }
    }
    EXPECT_TRUE(a0.paired_ok_);
    EXPECT_TRUE(a1.paired_ok_);
    EXPECT_EQ(b0.prepare_calls_.load(), 0u);

    pool.unregister_lib(l0);
    pool.unregister_lib(l1);
    pool.stop();
}

// S16-2：观察型插件——on_put/on_delete 内查宿主 docmap 状态，记录顺序不变量
// 快照（reducer 单写者上下文记录，主线程 flush 后读——无 race）。
class ObservingPlugin final : public bp::CaskPlugin {
public:
    explicit ObservingPlugin(const Index* dm) : docmap_(dm) {}

    std::string_view name() const override { return "observer"; }
    bp::PluginStatus open(const bp::OpenContext&) override { return bp::PluginStatus::kOk; }
    std::uint64_t watermark() const override { return 0; }
    bp::PluginStatus close() override { return bp::PluginStatus::kOk; }

    void on_put(const bp::PutEvent& e, bp::PreparedPtr) override {
        PutObs o;
        o.ord = e.ord;
        o.docmap_live = docmap_->is_live(e.ord);
        if (auto slot = docmap_->get(e.key)) {
            o.docmap_has_key = true;
            o.maps_to_this_ord = (slot->ord == e.ord);
            o.loc_match = slot->loc.file_id == e.loc.file_id
                       && slot->loc.offset  == e.loc.offset
                       && slot->loc.total_sz== e.loc.total_sz;
        }
        o.meta_size = docmap_->meta_blob(e.ord).size();
        puts_.push_back(o);
    }

    void on_delete(const bp::DeleteEvent& e) override {
        DelObs o;
        o.ord = e.ord;
        o.prior_ord = e.prior_ord;
        o.key_already_removed = !docmap_->get(e.key).has_value();
        dels_.push_back(o);
    }

    bp::FlushResult flush(const bp::FlushRequest&) override {
        return {bp::PluginStatus::kOk, 0, 0};
    }

    struct PutObs {
        std::uint64_t ord = 0;
        bool docmap_live = false;
        bool docmap_has_key = false;
        bool maps_to_this_ord = false;
        bool loc_match = false;
        std::size_t meta_size = 0;
    };
    struct DelObs {
        std::uint64_t ord = 0;
        std::uint64_t prior_ord = bp::kNoPriorOrd;
        bool key_already_removed = false;
    };

    const Index* docmap_;
    std::vector<PutObs> puts_;
    std::vector<DelObs> dels_;
};

// S16-2 契约⑨：宿主 reduce 先 apply DocMap（身份/存活/meta），再广播给插件。
// 插件 on_put 内查 docmap 必已有本 ord 的 live slot；on_delete 时 prior_ord 已
// 捕获且 key 已 remove。复刻 cask.cpp reduce 闭包（host_reduce_docmap），
// 守护「DocMap 恒在所有插件之前」顺序不变量。
TEST(PluginContract, DocmapVisibleBeforePluginOnPut) {
    IndexPool pool(2, 10240);
    Index docmap;
    ObservingPlugin obs(&docmap);
    std::vector<bp::CaskPlugin*> plugins{&obs};
    pool.start(host_map(plugins), host_reduce_docmap(plugins, docmap), [] {});

    pool.submit(mk_fields_task("k0", 0, "alpha beta"));
    pool.submit(IndexTask::make(IndexOp::Add, "k1", 1, "gamma", 7, 99, 64, 5, 0));
    {
        auto t = IndexTask::make(IndexOp::Add, "k2", 4, "delta epsilon", 2, 200, 48, 6, 0);
        t.meta = {std::byte{0xAB}, std::byte{0xCD}};
        pool.submit(std::move(t));
    }
    pool.submit(IndexTask::make(IndexOp::Delete, "k0", 2, "", 0, 0, 0, 0, 0));
    pool.submit(IndexTask::make(IndexOp::Delete, "nope", 3, "", 0, 0, 0, 0, 0));
    pool.submit(IndexTask::make(IndexOp::Delete, "k2", 5, "", 0, 0, 0, 0, 0));
    pool.flush();

    ASSERT_EQ(obs.puts_.size(), 3u);
    EXPECT_EQ(obs.puts_[0].ord, 0u);
    EXPECT_TRUE(obs.puts_[0].docmap_live);
    EXPECT_TRUE(obs.puts_[0].docmap_has_key);
    EXPECT_TRUE(obs.puts_[0].maps_to_this_ord);
    EXPECT_TRUE(obs.puts_[0].loc_match);

    EXPECT_EQ(obs.puts_[1].ord, 1u);
    EXPECT_TRUE(obs.puts_[1].docmap_live);
    EXPECT_TRUE(obs.puts_[1].maps_to_this_ord);
    EXPECT_TRUE(obs.puts_[1].loc_match);

    EXPECT_EQ(obs.puts_[2].ord, 4u);
    EXPECT_TRUE(obs.puts_[2].docmap_live);
    EXPECT_TRUE(obs.puts_[2].maps_to_this_ord);
    EXPECT_EQ(obs.puts_[2].meta_size, 2u);

    ASSERT_EQ(obs.dels_.size(), 3u);
    EXPECT_EQ(obs.dels_[0].ord, 2u);
    EXPECT_EQ(obs.dels_[0].prior_ord, 0u);
    EXPECT_TRUE(obs.dels_[0].key_already_removed);

    EXPECT_EQ(obs.dels_[1].ord, 3u);
    EXPECT_EQ(obs.dels_[1].prior_ord, bp::kNoPriorOrd);

    EXPECT_EQ(obs.dels_[2].ord, 5u);
    EXPECT_EQ(obs.dels_[2].prior_ord, 4u);
    EXPECT_TRUE(obs.dels_[2].key_already_removed);

    pool.stop();
}

// S16-2 契约⑩：DeleteEvent.prior_ord 反映删除时刻 key 的**当前**（最新版本）
// ord——覆盖写后 prior_ord = 最新 ord，非原始 ord；不存在 key = kNoPriorOrd。
TEST(PluginContract, DeleteEventPriorOrdReflectsLatestVersion) {
    IndexPool pool(2, 10240);
    Index docmap;
    ObservingPlugin obs(&docmap);
    std::vector<bp::CaskPlugin*> plugins{&obs};
    pool.start(host_map(plugins), host_reduce_docmap(plugins, docmap), [] {});

    pool.submit(mk_fields_task("k0", 0, "v0"));
    pool.submit(mk_fields_task("k0", 1, "v1 overwritten"));
    pool.submit(IndexTask::make(IndexOp::Delete, "k0", 2, "", 0, 0, 0, 0, 0));
    pool.submit(mk_fields_task("k1", 3, "single"));
    pool.submit(IndexTask::make(IndexOp::Delete, "k1", 4, "", 0, 0, 0, 0, 0));
    pool.submit(IndexTask::make(IndexOp::Delete, "nope", 5, "", 0, 0, 0, 0, 0));
    pool.flush();

    ASSERT_EQ(obs.puts_.size(), 3u);
    EXPECT_EQ(obs.puts_[0].ord, 0u);
    EXPECT_EQ(obs.puts_[1].ord, 1u);
    EXPECT_TRUE(obs.puts_[1].docmap_live);
    EXPECT_TRUE(obs.puts_[1].maps_to_this_ord);

    ASSERT_EQ(obs.dels_.size(), 3u);
    EXPECT_EQ(obs.dels_[0].ord, 2u);
    EXPECT_EQ(obs.dels_[0].prior_ord, 1u) << "覆盖写后 prior_ord 应为最新 ord";
    EXPECT_TRUE(obs.dels_[0].key_already_removed);

    EXPECT_EQ(obs.dels_[1].ord, 4u);
    EXPECT_EQ(obs.dels_[1].prior_ord, 3u);

    EXPECT_EQ(obs.dels_[2].ord, 5u);
    EXPECT_EQ(obs.dels_[2].prior_ord, bp::kNoPriorOrd);

    pool.stop();
}

}  // namespace

// ============================================================================
// S18-11：merge 广播契约（S18-7 设计 §3.9）——run_merge 直调 + mock 插件。
// 断言事件序 begin →（每条 live CAS 成功）relocate× N → commit；失败路径
// begin → abort。run_serialized 先于宿主保存点的 FIFO 由上方
// RunFnSerializedAtOrdPosition + merge 全系集成测试共同覆盖。
// ============================================================================

#include "bitcask/data_file.hpp"
#include "bitcask/keydir.hpp"
#include "bitcask/merger.hpp"

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

    // 手工构造输入 data 文件 + keydir（3 条 live record）。
    const std::string data_path =
        bitcask::fileops::mk_data_filename(dir.string(), 1);
    bitcask::keydir::KeyDir kd;
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
            kd.put(key, /*file_id=*/1, w->total_size, w->offset,
                   static_cast<std::uint32_t>(1000 + i), /*now*/ 0,
                   /*newest*/ true, 0, 0, static_cast<std::uint64_t>(i));
        }
        ASSERT_TRUE(df->sync().has_value());
    }
    kd.increment_file_id_at_least(1);
    kd.mark_ready();

    MergeEventRecorder rec;
    bitcask::plugin::CaskPlugin* plugs[] = {&rec};
    const std::string inputs[] = {data_path};
    auto r = bitcask::merge::run_merge(inputs, dir.string(), kd,
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
    bitcask::keydir::KeyDir kd;
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
        kd.put(key, 1, w->total_size, w->offset, 1000, 0, true, 0, 0, 0);
        ASSERT_TRUE(df->sync().has_value());
    }
    kd.increment_file_id_at_least(1);
    kd.mark_ready();

    MergeEventRecorder rec;
    bitcask::plugin::CaskPlugin* plugs[] = {&rec};
    const std::string inputs[] = {data_path};
    const std::string bad_out = (dir / "no_such_subdir" / "x").string();
    auto r = bitcask::merge::run_merge(inputs, bad_out, kd,
                                       /*sync*/ false, plugs, /*now*/ 0);
    EXPECT_FALSE(r.has_value());
    ASSERT_EQ(rec.events.size(), 2u);
    EXPECT_EQ(rec.events[0], "begin");
    EXPECT_EQ(rec.events[1], "abort");
    fs::remove_all(dir);
}
