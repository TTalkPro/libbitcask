// plugin_api 头自包含 + 基础契约测试（S15-1）。
// 首个 include 必须是 plugin_api.hpp——证明该头自包含（不依赖任何
// bitcask 头或前置 include）。本测试目标只链 bitcask_plugin_api，
// 编译通过即证明接口层对 search/bm25/vector 零依赖。
#include "bitcask/plugin_api.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace bp = bitcask::plugin;

namespace {

// 最小插件：只实现纯虚方法，验证默认实现（可选能力/维护/merge 事件）可用。
class MinimalPlugin final : public bp::CaskPlugin {
public:
    std::string_view name() const override { return "minimal"; }
    bp::PluginStatus open(const bp::OpenContext&) override {
        opened_ = true;
        return bp::PluginStatus::kOk;
    }
    std::uint64_t watermark() const override { return watermark_; }
    bp::PluginStatus close() override { return bp::PluginStatus::kOk; }

    void on_put(const bp::PutEvent& e, bp::PreparedPtr prep) override {
        last_ord_ = e.ord;
        last_had_prep_ = (prep != nullptr);
        last_had_doc_ = (e.doc != nullptr);
        last_value_.assign(e.value);
    }
    void on_delete(const bp::DeleteEvent& e) override { last_del_ord_ = e.ord; }

    bp::FlushResult flush(const bp::FlushRequest& req) override {
        return {bp::PluginStatus::kOk, last_ord_,
                req.force_rebase ? std::uint64_t{1} : std::uint64_t{0}};
    }

    bool opened_ = false;
    std::uint64_t watermark_ = 0;
    std::uint64_t last_ord_ = 0;
    std::uint64_t last_del_ord_ = 0;
    bool last_had_prep_ = false;
    bool last_had_doc_ = false;
    std::string last_value_;
};

// 带 prepare 能力的插件：验证 Prepared 类型擦除移交。
struct CountPrepared final : bp::Prepared {
    std::size_t token_count = 0;
};

class PreparingPlugin final : public bp::CaskPlugin {
public:
    std::string_view name() const override { return "preparing"; }
    bp::PluginStatus open(const bp::OpenContext&) override { return bp::PluginStatus::kOk; }
    std::uint64_t watermark() const override { return 0; }
    bp::PluginStatus close() override { return bp::PluginStatus::kOk; }

    bool wants_prepare() const override { return true; }
    bp::PreparedPtr prepare(const bp::PutEvent& e) const override {
        auto p = std::make_unique<CountPrepared>();
        p->token_count = e.value.size();
        return p;
    }

    void on_put(const bp::PutEvent&, bp::PreparedPtr prep) override {
        auto* p = static_cast<CountPrepared*>(prep.get());
        consumed_tokens_ += p ? p->token_count : 0;
    }
    void on_delete(const bp::DeleteEvent&) override {}
    bp::FlushResult flush(const bp::FlushRequest&) override {
        return {bp::PluginStatus::kOk, 0, 0};
    }

    std::size_t consumed_tokens_ = 0;
};

TEST(PluginApi, MinimalPluginLifecycleAndEvents) {
    MinimalPlugin p;
    EXPECT_FALSE(p.wants_prepare());          // 默认无预处理能力
    EXPECT_EQ(p.prepare(bp::PutEvent{}), nullptr);

    bp::OpenContext ctx{"/tmp/nonexistent", nullptr};
    EXPECT_EQ(p.open(ctx), bp::PluginStatus::kOk);
    EXPECT_TRUE(p.opened_);

    bp::PutEvent ev;
    ev.ord = 42;
    ev.key = "k";
    ev.value = "hello";
    ev.loc = {1, 128, 64};
    p.on_put(ev, nullptr);
    EXPECT_EQ(p.last_ord_, 42u);
    EXPECT_FALSE(p.last_had_prep_);
    EXPECT_FALSE(p.last_had_doc_);   // 纯 KV 写：doc == nullptr
    EXPECT_EQ(p.last_value_, "hello");

    p.on_delete(bp::DeleteEvent{43, "k"});
    EXPECT_EQ(p.last_del_ord_, 43u);

    // 默认实现（merge/维护事件）可直接调用，空操作不崩。
    p.on_relocate(bp::RelocateEvent{});
    p.on_merge_begin(bp::MergeBeginEvent{});
    p.on_merge_commit(bp::MergeCommitEvent{});
    p.on_merge_abort();
    p.maintain(bp::MaintainEvent{});

    auto fr = p.flush(bp::FlushRequest{bp::FlushRequest::Reason::kClose, true});
    EXPECT_EQ(fr.status, bp::PluginStatus::kOk);
    EXPECT_EQ(fr.covered_ord, 42u);
    EXPECT_EQ(fr.generation, 1u);
    EXPECT_EQ(p.close(), bp::PluginStatus::kOk);
}

TEST(PluginApi, PreparedHandoffTypeErased) {
    PreparingPlugin p;
    ASSERT_TRUE(p.wants_prepare());

    bp::PutEvent ev;
    ev.ord = 1;
    ev.value = "12345";
    bp::PreparedPtr prep = p.prepare(ev);   // map 相（此处同线程模拟）
    ASSERT_NE(prep, nullptr);
    p.on_put(ev, std::move(prep));          // reduce 相消费
    EXPECT_EQ(p.consumed_tokens_, 5u);
}

TEST(PluginApi, DocViewSpansBorrow) {
    // DocView 的 span 借用语义：视图指向 caller 缓冲，无拷贝。
    std::vector<bp::FieldKV> fields{{"title", "hello"}, {"body", "world"}};
    std::vector<float> vec{0.1f, 0.2f};
    std::vector<std::byte> meta{std::byte{7}};
    bp::DocView doc{"hello world", fields, vec, meta};

    bp::PutEvent ev;
    ev.doc = &doc;
    ASSERT_NE(ev.doc, nullptr);
    EXPECT_EQ(ev.doc->fields.size(), 2u);
    EXPECT_EQ(ev.doc->fields[0].first, "title");
    EXPECT_EQ(ev.doc->fields.data(), fields.data());  // 零拷贝借用
    EXPECT_EQ(ev.doc->vec.data(), vec.data());
    EXPECT_EQ(ev.doc->meta.data(), meta.data());
}

// PluginHost 可被独立实现（不依赖 Cask）——接口自包含性的另一半验证。
class RecordingHost final : public bp::PluginHost {
public:
    std::optional<std::string> read_at(bp::RecordLoc loc) override {
        reads_.push_back(loc.offset);
        return std::nullopt;
    }
    void run_serialized(std::function<void()> fn) override {
        // 契约：FIFO。测试实现直接同步执行。
        ++serialized_count_;
        fn();
    }
    void log(bp::LogLevel, std::string_view) override { ++logs_; }

    std::vector<std::uint64_t> reads_;
    int serialized_count_ = 0;
    int logs_ = 0;
};

TEST(PluginApi, HostInterfaceStandalone) {
    RecordingHost host;
    EXPECT_EQ(host.read_at({1, 99, 8}), std::nullopt);
    EXPECT_EQ(host.reads_.size(), 1u);

    std::atomic<int> ran{0};
    host.run_serialized([&] { ran.fetch_add(1); });
    EXPECT_EQ(ran.load(), 1);
    EXPECT_EQ(host.serialized_count_, 1);

    host.log(bp::LogLevel::kWarn, "test");
    EXPECT_EQ(host.logs_, 1);
}

}  // namespace
