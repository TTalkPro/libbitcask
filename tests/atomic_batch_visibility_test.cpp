// S63（feedbacks/2026-11-20-atomic-batch-no-read-isolation.md）：
// put_batch_atomic 对并发读者的一次性可见性契约。
//
// 旧版按 key 字节序逐条调用 apply_put/apply_remove，读者能看到「新头 +
// 旧尾」。本文件用并发读者卡住两个跨 key 观察顺序（头→尾、尾→头），并
// 覆盖含删除的批；merge 竞争路径由 pre_apply 测试缝确定性制造。
//
// 读写交叠不靠调度碰巧（feedbacks/2026-11-23-two-timing-sensitive-tests-fail-under-full-load.md：
// 满载时读线程可能在写者写完之后才第一次被调度，`seen == 0` 假红，没红时
// 不变量也是空转）。并发用例统一走 run_overlapped()，见其注释。

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>
#include <bitcask/meta_file.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <latch>
#include <limits>
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

// 每个读者在写入期间至少要做成的一致观察数；写者写到这个数为止。
constexpr std::size_t kMinOverlap = 200;
// 写者的硬上限：到这里仍不够交叠只记一条说明（极端饥饿），不判失败。
constexpr int kMaxRounds = 100000;

// 并发读者 + 单写者，交叠由同步点保证而不是 sleep / 调度运气：
//   1. 读者各自先做成一次一致观察（observe() 返回 true）才 count_down，写者
//      等齐全部读者再开写——「每个读者至少观察过一次」因此是确定的；
//   2. 写者至少写 min_rounds 批，并一直写到每个读者在写入期间都做成
//      kMinOverlap 次观察（上限 kMaxRounds 批）——不变量确实在并发下被检查过；
//   3. 停写后读者再做成一次观察才退出（此时无写入，必定成功）。
// observe() 返回 false 表示这次读之间插入了完整批、不算数。write_round(i)
// 写第 i 批（从 1 起），返回 false 表示失败（调用方已记失败），写者提前结束。
// 返回实际写成的批数。
int run_overlapped(int n_readers, int min_rounds,
                   const std::function<bool()>& observe,
                   const std::function<bool(int)>& write_round) {
    std::latch ready(n_readers);
    std::atomic<bool> writing{false};
    std::atomic<bool> stop{false};
    std::vector<std::atomic<std::size_t>> overlap(static_cast<std::size_t>(n_readers));
    std::vector<std::thread> readers;
    readers.reserve(static_cast<std::size_t>(n_readers));
    for (int t = 0; t < n_readers; ++t) {
        readers.emplace_back([&, t] {
            bool counted = false;
            for (;;) {
                const bool done = stop.load(std::memory_order_acquire);
                const bool during = writing.load(std::memory_order_acquire);
                if (observe()) {
                    if (during) {
                        overlap[static_cast<std::size_t>(t)].fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    if (!counted) {
                        counted = true;
                        ready.count_down();
                    }
                    if (done) break;
                }
            }
        });
    }
    const auto min_overlap = [&] {
        std::size_t m = std::numeric_limits<std::size_t>::max();
        for (const auto& o : overlap) {
            m = std::min(m, o.load(std::memory_order_relaxed));
        }
        return m;
    };
    ready.wait();
    writing.store(true, std::memory_order_release);
    int rounds = 0;
    while (rounds < min_rounds ||
           (rounds < kMaxRounds && min_overlap() < kMinOverlap)) {
        if (!write_round(rounds + 1)) break;
        ++rounds;
    }
    writing.store(false, std::memory_order_release);
    stop.store(true, std::memory_order_release);
    for (auto& th : readers) th.join();
    if (min_overlap() < kMinOverlap) {
        std::fprintf(stderr,
                     "note: %d rounds, least-overlapped reader made %zu observations "
                     "while writing (< %zu); scheduler starved the readers\n",
                     rounds, min_overlap(), kMinOverlap);
    }
    return rounds;
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
//
// 判据：读 head → tail → head，两次 head 相同 ⇒ 期间没有批提交（每批都换
// head），tail 的代数必须与 head 相等；新头旧尾、旧头新尾都算撕裂。（旧写法
// 只在 `size() == 2` 时比较，即只查了 h0～h9 这前 9 批。）
TEST_F(AtomicBatchVisibilityTest, HeadThenTailNeverShowsHalfBatch) {
    open();
    ASSERT_TRUE(c_->put(bytes("head"), bytes("h0")));
    ASSERT_TRUE(c_->put(bytes("tail"), bytes("t0")));

    std::atomic<std::uint64_t> torn{0};
    const int rounds = run_overlapped(
        /*n_readers=*/3, /*min_rounds=*/700,
        [&] {
            const auto h = value_of(*c_, "head");
            const auto tv = value_of(*c_, "tail");
            const auto h2 = value_of(*c_, "head");
            if (h != h2) return false;  // 两次 get 之间插入了完整批
            if (h.empty() || tv.empty() || version_of(tv) != version_of(h)) {
                ++torn;
            }
            return true;
        },
        [&](int i) {
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
            if (!r) ADD_FAILURE() << "batch " << i << ": " << r.error().detail;
            return r.has_value();
        });
    ASSERT_GE(rounds, 700);
    EXPECT_EQ(torn.load(), 0);
    EXPECT_EQ(value_of(*c_, "head"), "h" + std::to_string(rounds));
    EXPECT_EQ(value_of(*c_, "tail"), "t" + std::to_string(rounds));
}

// 可见性与 key 字节序无关：tail 先生效时也绝不能出现「新 tail + 旧 head」。
// 判据同上，只是按 tail → head → tail 读。
TEST_F(AtomicBatchVisibilityTest, TailThenHeadNeverShowsHalfBatch) {
    open();
    ASSERT_TRUE(c_->put(bytes("head"), bytes("h0")));
    ASSERT_TRUE(c_->put(bytes("tail"), bytes("t0")));

    std::atomic<std::uint64_t> torn{0};
    const int rounds = run_overlapped(
        /*n_readers=*/1, /*min_rounds=*/500,
        [&] {
            const auto tv = value_of(*c_, "tail");
            const auto h = value_of(*c_, "head");
            const auto tv2 = value_of(*c_, "tail");
            if (tv != tv2) return false;  // 两次 get 之间插入了完整批
            if (tv.empty() || h.empty() || version_of(h) != version_of(tv)) {
                ++torn;
            }
            return true;
        },
        [&](int i) {
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
            if (!r) ADD_FAILURE() << "batch " << i << ": " << r.error().detail;
            return r.has_value();
        });
    ASSERT_GE(rounds, 500);
    EXPECT_EQ(torn.load(), 0);
}

// remove 与 put 在同一个状态转换里切换：读者不能看到「旧头 + 已删除的尾」
// 或「新头 + 旧尾」。
TEST_F(AtomicBatchVisibilityTest, RemoveSwitchesTogetherWithHead) {
    open();
    ASSERT_TRUE(c_->put(bytes("head"), bytes("h0")));
    ASSERT_TRUE(c_->put(bytes("tail"), bytes("t0")));

    // 偶数代写 tail、奇数代删 tail：偶数代 tail 必须在（且同代），奇数代必须不在。
    std::atomic<std::uint64_t> bad{0};
    const int rounds = run_overlapped(
        /*n_readers=*/1, /*min_rounds=*/300,
        [&] {
            auto hr = c_->get_owned(bytes("head"));
            if (!hr) return false;
            auto tr = c_->get_owned(bytes("tail"));
            auto hr2 = c_->get_owned(bytes("head"));
            if (!hr2 || hr2->value != hr->value) return false;
            const int generation = version_of(text(hr->value));
            const bool tail_missing = !tr &&
                tr.error().kind == CaskError::kNotFound;
            if (generation % 2 == 0 && tail_missing) ++bad;
            if (generation % 2 != 0 && tr &&
                version_of(text(tr->value)) != generation) {
                ++bad;
            }
            return true;
        },
        [&](int i) {
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
            if (!r) ADD_FAILURE() << "batch " << i << ": " << r.error().detail;
            return r.has_value();
        });
    ASSERT_GE(rounds, 300);
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
