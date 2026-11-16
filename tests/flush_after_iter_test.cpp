// 下游反馈 2026-11-16（feedbacks/2026-11-16-flush-index-hangs-after-iter.md）回归：
// 迭代器用过之后再写一次，flush_index 永不返回（6.9.0 ～ 7.0.0）。
//
// 病因：CaskIter::start 借 ord 序列推进 fold 时代（advance_fold_epoch =
// advance_ord(peek_next_ord())），凭空跳掉一个 ord——既不写 data 也不进索引
// 通道。索引通道的重排缓冲按 ord 严格连续 apply，等不到那个号就永久停住；
// 下一次写把 submitted_ord_hwm 推过空洞后，flush 的谓词
// （applied_ord ≥ submitted_ord_hwm）再也不成立。
//
// 契约（本文件锁住）：
//   1. ord 只发给真正的写——开 / 放迭代器（fold、range 两种）不动 ord 序列；
//   2. 「迭代 → 释放 → 写 → flush_index」限时内返回，写入对检索可见；
//   3. 迭代器未释放时写 + flush 同样不卡（时代推进与迭代器生命周期无关）。

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

#include "bitcask/cask.hpp"
#include "bitcask/index_registry.hpp"

using namespace bitcask;
namespace fs = std::filesystem;

namespace {

bitcask::oki::IndexRegistry& test_registry() {
    static bitcask::oki::IndexRegistry reg;
    return reg;
}

std::span<const std::byte> sv_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

class FlushAfterIterTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string name =
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
        dir_ = fs::temp_directory_path() / ("bitcask_flush_after_iter_" + name);
        fs::remove_all(dir_);
        fs::create_directories(dir_);

        CaskOptions opts;
        opts.read_write = true;
        opts.enable_search = true;
        search::SearchLayerConfig scfg;
        scfg.analyzer_config.type = text::AnalyzerType::Whitespace;
        opts.search_config = scfg;
        auto c = Cask::open(dir_.string(), opts, &test_registry());
        ASSERT_TRUE(c) << c.error().detail;
        cask_ = std::move(*c);

        for (const char* k : {"a", "b", "c"}) put_doc(k, "apple pie");
        flush_or_die("setup");
    }
    void TearDown() override {
        if (cask_) cask_->close();
        cask_.reset();
        fs::remove_all(dir_);
    }

    void put_doc(std::string_view key, std::string_view text) {
        DocInput doc;
        doc.text = sv_bytes(text);
        ASSERT_TRUE(cask_->put_doc(sv_bytes(key), doc, 1000));
    }

    // flush_index 是无超时的阻塞调用：回归时它永不返回。放到旁路线程限时等，
    // 超时直接 abort——卡住的线程 join 不了，正常收尾（close 的有界 flush、
    // 静态析构）只会把「挂死」换成「很慢才红」。
    void flush_or_die(const char* what) {
        std::promise<void> done;
        auto fut = done.get_future();
        std::thread t([&] {
            cask_->flush_index();
            done.set_value();
        });
        if (fut.wait_for(std::chrono::seconds(20)) !=
            std::future_status::ready) {
            std::fprintf(stderr,
                         "FATAL: flush_index did not return within 20s (%s): "
                         "ord hole in index lane?\n", what);
            std::fflush(stderr);
            std::abort();
        }
        t.join();
    }

    void drain_fold_iter() {
        auto it = cask_->make_iter();
        ASSERT_TRUE(it->start());
        while (true) {
            auto e = it->next();
            ASSERT_TRUE(e);
            if (!*e) break;
        }
        it->release();
    }

    void drain_range_iter() {
        auto it = cask_->make_range_iter(RangeOptions{});
        ASSERT_TRUE(it) << it.error().detail;
        while (true) {
            auto e = (*it)->next();
            ASSERT_TRUE(e);
            if (!*e) break;
        }
    }

    [[nodiscard]] std::size_t hits(std::string_view q) {
        auto r = cask_->search_text(q, 10);
        EXPECT_TRUE(r);
        return r ? r->hits.size() : 0;
    }

    fs::path dir_;
    std::unique_ptr<Cask> cask_;
};

TEST_F(FlushAfterIterTest, IteratorsDoNotConsumeOrds) {
    const auto before = cask_->index_authority().peek_next_ord();
    drain_fold_iter();
    drain_range_iter();
    drain_fold_iter();
    EXPECT_EQ(cask_->index_authority().peek_next_ord(), before)
        << "开迭代器不得占用 ord：ord 序列必须与索引通道任务一一对应";
}

TEST_F(FlushAfterIterTest, FoldIterThenDeleteThenFlush) {
    drain_fold_iter();
    ASSERT_TRUE(cask_->remove(sv_bytes("a"), 1001));
    flush_or_die("fold iter -> delete");
    EXPECT_EQ(hits("apple"), 2u);
}

TEST_F(FlushAfterIterTest, FoldIterThenPutDocThenFlush) {
    drain_fold_iter();
    put_doc("x", "cherry tart");
    flush_or_die("fold iter -> put_doc");
    EXPECT_EQ(hits("cherry"), 1u);
    EXPECT_EQ(hits("apple"), 3u);
}

TEST_F(FlushAfterIterTest, RangeIterThenWriteThenFlush) {
    drain_range_iter();
    put_doc("x", "cherry tart");
    ASSERT_TRUE(cask_->remove(sv_bytes("b"), 1001));
    flush_or_die("range iter -> put_doc + delete");
    EXPECT_EQ(hits("cherry"), 1u);
    EXPECT_EQ(hits("apple"), 2u);
}

// 多轮「扫 → 写 → flush」（锦书删块路径的形态），每轮都得回来。
TEST_F(FlushAfterIterTest, RepeatedScanWriteFlushRounds) {
    for (int round = 0; round < 8; ++round) {
        drain_fold_iter();
        drain_range_iter();
        put_doc("r" + std::to_string(round), "banana split");
        if (round % 2 == 1) {
            ASSERT_TRUE(cask_->remove(
                sv_bytes("r" + std::to_string(round - 1)), 1001));
        }
        flush_or_die("round");
    }
    EXPECT_EQ(hits("banana"), 4u);
}

// 迭代器还开着时写 + flush：时代推进不依赖 release。
TEST_F(FlushAfterIterTest, WriteAndFlushWhileIterOpen) {
    auto it = cask_->make_iter();
    ASSERT_TRUE(it->start());
    auto first = it->next();
    ASSERT_TRUE(first);
    ASSERT_TRUE(*first);
    put_doc("x", "cherry tart");
    flush_or_die("iter open -> put_doc");
    EXPECT_EQ(hits("cherry"), 1u);
    it->release();
    put_doc("y", "cherry cake");
    flush_or_die("iter released -> put_doc");
    EXPECT_EQ(hits("cherry"), 2u);
}

// 迭代之后关库重开：ord 序列与索引通道起点仍然对得上。
TEST_F(FlushAfterIterTest, IterThenWriteThenReopen) {
    drain_fold_iter();
    put_doc("x", "cherry tart");
    flush_or_die("before reopen");
    cask_->close();
    cask_.reset();

    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    search::SearchLayerConfig scfg;
    scfg.analyzer_config.type = text::AnalyzerType::Whitespace;
    opts.search_config = scfg;
    auto c = Cask::open(dir_.string(), opts, &test_registry());
    ASSERT_TRUE(c) << c.error().detail;
    cask_ = std::move(*c);

    drain_fold_iter();
    put_doc("y", "cherry cake");
    flush_or_die("after reopen");
    EXPECT_EQ(hits("cherry"), 2u);
    EXPECT_EQ(hits("apple"), 3u);
}

}  // namespace
