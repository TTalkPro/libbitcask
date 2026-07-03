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

}  // namespace
