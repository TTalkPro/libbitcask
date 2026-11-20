// S63（feedbacks/2026-11-20-atomic-batch-no-read-isolation.md）：
// put_batch_atomic 对并发读者的一次性可见性契约。
//
// 旧版按 key 字节序逐条调用 apply_put/apply_remove，读者能看到「新头 +
// 旧尾」。本文件用并发读者卡住两个跨 key 观察顺序（头→尾、尾→头），并
// 覆盖含删除的批；merge 竞争路径由 pre_apply 测试缝确定性制造。

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>
#include <bitcask/meta_file.hpp>

#include <atomic>
#include <cstdint>
#include <utility>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "support/test_paths.hpp"

namespace {

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskError;
using bitcask::CaskOptions;
using Op = Cask::BatchOp;

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string text(std::span<const std::byte> v) {
    return {reinterpret_cast<const char*>(v.data()), v.size()};
}

int version_of(std::string_view v) {
    return std::stoi(std::string(v.substr(1)));
}

Op put(std::string_view k, std::string_view v) {
    return {.type = Op::Type::kPut, .key = bytes(k), .value = bytes(v)};
}

Op put_head(std::string_view v, Op::Expect expect, std::uint64_t ord) {
    return {.type = Op::Type::kPut, .key = bytes("head"),
            .value = bytes(v), .expect = expect, .expect_ord = ord};
}

Op remove_tail() {
    return {.type = Op::Type::kRemove, .key = bytes("tail")};
}

class AtomicBatchVisibilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = bitcask::test::unique_tmpdir(
            std::string("atomic_batch_visibility_") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void TearDown() override {
        c_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void open() {
        CaskOptions o;
        o.read_write = true;
        auto r = Cask::open(dir_.string(), o, &reg_);
        ASSERT_TRUE(r) << r.error().detail;
        c_ = std::move(*r);
    }

    static std::string value_of(Cask& c, std::string_view key) {
        auto r = c.get_owned(bytes(key));
        if (!r) return {};
        return {reinterpret_cast<const char*>(r->value.data()), r->value.size()};
    }

    static std::uint64_t ord_of(Cask& c, std::string_view key) {
        auto r = c.get_owned(bytes(key));
        return r ? r->ord : 0;
    }

    std::unique_ptr<Cask> c_;
    fs::path dir_;
    bitcask::oki::IndexRegistry reg_;
};

// 批必须把 head/tail 作为一次状态转换发布。按字节序 head 先生效的旧缺陷
// 会在「读到新 head 后又读到旧 tail」时被抓住。
TEST_F(AtomicBatchVisibilityTest, HeadThenTailNeverShowsHalfBatch) {
    open();
    ASSERT_TRUE(c_->put(bytes("head"), bytes("h0")));
    ASSERT_TRUE(c_->put(bytes("tail"), bytes("t0")));

    constexpr int kRounds = 700;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> stale_tail{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t) {
        readers.emplace_back([&] {
            std::size_t seen = 0;
            while (!stop.load(std::memory_order_acquire)) {
                const auto h = value_of(*c_, "head");
                const auto tv = value_of(*c_, "tail");
                const auto h2 = value_of(*c_, "head");
                if (h != h2) continue;  // 两次 get 之间插入了完整批
                if (h.size() == 2 && tv.size() == 2 &&
                    version_of(tv) < version_of(h)) {
                    ++stale_tail;
                }
                ++seen;
            }
            EXPECT_GT(seen, std::size_t{0});
        });
    }
    for (int i = 1; i <= kRounds; ++i) {
        const std::string h = "h" + std::to_string(i);
        const std::string body = "t" + std::to_string(i);
        std::vector<std::string> fill_keys;
        fill_keys.reserve(7);
        for (int fill = 0; fill < 7; ++fill) {
            fill_keys.push_back("fill" + std::to_string(fill));
        }
        std::vector<Op> ops;
        ops.reserve(9);
        ops.push_back(put_head(h, Op::Expect::kOrdEquals,
                               ord_of(*c_, "head")));
        for (int fill = 0; fill < 7; ++fill) {
            ops.push_back(put(fill_keys[static_cast<std::size_t>(fill)], body));
        }
        ops.push_back(put("tail", body));
        auto r = c_->put_batch_atomic(ops);
        ASSERT_TRUE(r) << r.error().detail;
    }
    stop.store(true, std::memory_order_release);
    for (auto& th : readers) th.join();
    EXPECT_EQ(stale_tail.load(), 0);
    EXPECT_EQ(value_of(*c_, "head"), "h700");
    EXPECT_EQ(value_of(*c_, "tail"), "t700");
}

// 可见性与 key 字节序无关：tail 先生效时也绝不能出现「新 tail + 旧 head」。
TEST_F(AtomicBatchVisibilityTest, TailThenHeadNeverShowsHalfBatch) {
    open();
    ASSERT_TRUE(c_->put(bytes("head"), bytes("h0")));
    ASSERT_TRUE(c_->put(bytes("tail"), bytes("t0")));

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> stale_head{0};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const auto tv = value_of(*c_, "tail");
            const auto h = value_of(*c_, "head");
            const auto tv2 = value_of(*c_, "tail");
            if (tv != tv2) continue;  // 两次 get 之间插入了完整批
            if (tv.size() == 2 && h.size() == 2 &&
                version_of(h) < version_of(tv)) {
                ++stale_head;
            }
        }
    });
    for (int i = 1; i <= 500; ++i) {
        const std::string head_val = "h" + std::to_string(i);
        const std::string body = "t" + std::to_string(i);
        std::vector<Op> ops;
        ops.reserve(4);
        ops.push_back(put_head(head_val,
                               Op::Expect::kOrdEquals, ord_of(*c_, "head")));
        ops.push_back(put("fill0", body));
        ops.push_back(put("fill1", body));
        ops.push_back(put("tail", body));
        auto r = c_->put_batch_atomic(ops);
        ASSERT_TRUE(r) << r.error().detail;
    }
    stop.store(true, std::memory_order_release);
    reader.join();
    EXPECT_EQ(stale_head.load(), 0);
}

// remove 与 put 在同一个状态转换里切换：读者不能看到「旧头 + 已删除的尾」
// 或「新头 + 旧尾」。
TEST_F(AtomicBatchVisibilityTest, RemoveSwitchesTogetherWithHead) {
    open();
    ASSERT_TRUE(c_->put(bytes("head"), bytes("h0")));
    ASSERT_TRUE(c_->put(bytes("tail"), bytes("t0")));

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> bad{0};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            auto hr = c_->get_owned(bytes("head"));
            if (!hr) continue;
            auto tr = c_->get_owned(bytes("tail"));
            auto hr2 = c_->get_owned(bytes("head"));
            if (!hr2 || hr2->value != hr->value) continue;
            const int generation = version_of(text(hr->value));
            const bool tail_missing = !tr &&
                tr.error().kind == CaskError::kNotFound;
            if (generation % 2 == 0 && tail_missing) ++bad;
            if (generation % 2 != 0 && tr &&
                version_of(text(tr->value)) != generation) {
                ++bad;
            }
        }
    });
    for (int i = 1; i <= 300; ++i) {
        const std::string head_val = "h" + std::to_string(i);
        const std::string body = "t" + std::to_string(i);
        std::vector<Op> ops;
        ops.reserve(4);
        ops.push_back(put_head(head_val,
                               Op::Expect::kOrdEquals, ord_of(*c_, "head")));
        if (i % 2 == 0) {
            ops.push_back(put("fill", body));
            ops.push_back(put("tail", body));
        } else {
            ops.push_back(put("fill", "gone"));
            ops.push_back(remove_tail());
        }
        auto r = c_->put_batch_atomic(ops);
        ASSERT_TRUE(r) << r.error().detail;
    }
    stop.store(true, std::memory_order_release);
    reader.join();
    EXPECT_EQ(bad.load(), 0);
}

// merge 恰好把 biggest_file_id 推过批文件时，put_batch_atomic 重写被拒
// 成员。旧版逐条应用会导致重写后的成员先于其余成员可见；S63 后整批重试。
TEST_F(AtomicBatchVisibilityTest, MergeRaceRewritePublishesWholeBatch) {
    open();
    ASSERT_TRUE(c_->put(bytes("head"), bytes("h0")));
    ASSERT_TRUE(c_->put(bytes("tail"), bytes("t0")));

    auto& authority = c_->index_authority();
    bool advanced = false;
    authority.set_pre_apply_hook_for_test([&] {
        if (std::exchange(advanced, true)) return;
        (void)authority.increment_file_id();
    });

    Cask::BatchOutcome outcome;
    std::vector<Op> ops;
    ops.reserve(3);
    ops.push_back(put_head("h1", Op::Expect::kOrdEquals,
                           ord_of(*c_, "head")));
    ops.push_back(put("fill", "t1"));
    ops.push_back(put("tail", "t1"));
    auto r = c_->put_batch_atomic(ops, /*tstamp=*/0, &outcome);
    ASSERT_TRUE(r) << r.error().detail;
    authority.set_pre_apply_hook_for_test({});
    ASSERT_EQ(outcome.ords.size(), ops.size());

    EXPECT_EQ(value_of(*c_, "head"), "h1");
    EXPECT_EQ(value_of(*c_, "tail"), "t1");
    EXPECT_EQ(ord_of(*c_, "head"), outcome.ords[0]);
    EXPECT_EQ(ord_of(*c_, "tail"), outcome.ords[2]);
    EXPECT_GT(outcome.ords[2], outcome.ords[0]);
    // 重写成员的原始 ord 必须补 Skip；这里能 drain 完成即证明没有空号。
    c_->flush_index();
}

// 只含 kCheck 的批是「写锁下校验一组版本」承诺：不写字节、不分配 LSN、
// 也不触发 bitcask.meta 懒升级。
TEST_F(AtomicBatchVisibilityTest, CheckOnlyBatchDoesNotUpgradeMeta) {
    // 尚未打开目录：此时 meta 还不存在（或保持旧版本）。打开 RW 句柄本身
    // 不应升级；只读 open 同理。用旧版本 meta 可读的最简形态无法伪造，
    // 这里检查批前后 meta version 和 next ord 都不变。
    open();
    ASSERT_TRUE(c_->put(bytes("head"), bytes("h")));
    const auto before = bitcask::meta::read_meta(dir_.string());
    ASSERT_TRUE(before);
    const auto before_ord = c_->index_authority().peek_next_ord();

    std::vector<Op> ops;
    ops.push_back({.type = Op::Type::kCheck, .key = bytes("head"),
                   .expect = Op::Expect::kOrdEquals, .expect_ord = ord_of(*c_, "head")});
    ops.push_back({.type = Op::Type::kCheck, .key = bytes("tail"),
                   .expect = Op::Expect::kAbsent});
    auto r = c_->put_batch_atomic(ops);
    ASSERT_TRUE(r) << r.error().detail;

    const auto after = bitcask::meta::read_meta(dir_.string());
    ASSERT_TRUE(after);
    EXPECT_EQ(after->version, before->version);
    EXPECT_EQ(c_->index_authority().peek_next_ord(), before_ord);
}

}  // namespace
