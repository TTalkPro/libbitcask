// S46（下游反馈 2026-11-17，feedbacks/2026-11-17-conditional-atomic-batch.md）：
// 原子批前置条件——按 key 的 LSN 做乐观并发。
//
// 覆盖：
//   1. kAbsent：新 key 成功；已存在则 kConditionFailed、failed_op 指向它、
//      整批零副作用（同批其它成员也不可见）。
//   2. kOrdEquals：用 get() 的 ord 改写成功；outcome.ords 就是随后 get() 的
//      ord，可直接接下一轮；过期的 ord 被拒。
//   3. 「不存在」的口径 = get()：删除后、expiry_at 过期后都算不存在。
//   4. kCheck 只校验不写；失败挡住整批；全是 kCheck 的批不写任何东西。
//   5. 批内同 key 多次：各自对照批前状态。
//   6. 形态校验：kCheck 不带条件 / 未知 expect → kInvalidOption，零副作用。
//   7. LSN 契约：merge 搬迁、重开（含删派生缓存后的 data 全量重建）后
//      ord 不变，旧 ord 仍能作为条件。
//   8. 并发读-改-写：多线程对同一计数器 CAS 自增，结果精确（无丢更新）；
//      对照组（无条件写）在同一负载下丢更新。
//   9. TxnCask::commit 透传条件与 outcome。

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>
#include <bitcask/txn.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "support/test_paths.hpp"

namespace {

namespace fs = std::filesystem;
using namespace bitcask;
using Op = Cask::BatchOp;
using Expect = Cask::BatchOp::Expect;

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

Op put_if(std::string_view k, std::string_view v, Expect e, std::uint64_t ord = 0) {
    return {.type = Op::Type::kPut, .key = bytes(k), .value = bytes(v),
            .expect = e, .expect_ord = ord};
}
Op put(std::string_view k, std::string_view v) {
    return put_if(k, v, Expect::kNone);
}
Op remove_if(std::string_view k, Expect e, std::uint64_t ord = 0) {
    return {.type = Op::Type::kRemove, .key = bytes(k), .expect = e,
            .expect_ord = ord};
}
Op check(std::string_view k, Expect e, std::uint64_t ord = 0) {
    return {.type = Op::Type::kCheck, .key = bytes(k), .expect = e,
            .expect_ord = ord};
}

class ConditionalBatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = test::unique_tmpdir(
            std::string("cond_batch_") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
    }
    void TearDown() override {
        c_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void open(std::uint64_t max_file_size = 0) {
        c_.reset();
        CaskOptions o;
        o.read_write = true;
        if (max_file_size) o.max_file_size = max_file_size;
        auto r = Cask::open(dir_.string(), o, &reg_);
        ASSERT_TRUE(r) << r.error().detail;
        c_ = std::move(*r);
        // 新库的首个 ord 是 0；先占掉它，下面 ord_of 才能用 0 表示「不存在」。
        if (!c_->get_owned(bytes("_warm"))) {
            ASSERT_TRUE(c_->put(bytes("_warm"), bytes("w")));
        }
    }

    // ord；不存在返回 0（open() 已占掉 ord 0）。
    std::uint64_t ord_of(std::string_view k) {
        auto r = c_->get_owned(bytes(k));
        if (!r) {
            EXPECT_EQ(r.error().kind, CaskError::kNotFound);
            return 0;
        }
        return r->ord;
    }
    std::string value_of(std::string_view k) {
        auto r = c_->get_owned(bytes(k));
        if (!r) return "<missing>";
        return {reinterpret_cast<const char*>(r->value.data()), r->value.size()};
    }

    oki::IndexRegistry reg_;
    fs::path dir_;
    std::unique_ptr<Cask> c_;
};

TEST_F(ConditionalBatchTest, AbsentCreatesOnceAndFailsAtomically) {
    open();
    Cask::BatchOutcome oc;
    std::vector<Op> b1{put_if("k", "v1", Expect::kAbsent)};
    ASSERT_TRUE(c_->put_batch_atomic(b1, 0, &oc));
    ASSERT_EQ(oc.ords.size(), 1u);
    EXPECT_EQ(oc.ords[0], ord_of("k"));
    EXPECT_EQ(oc.failed_op, Cask::BatchOutcome::kNoFailedOp);

    // 第二次「不存在才建」：失败，且同批的无条件成员也不落。
    std::vector<Op> b2{put("other", "x"), put_if("k", "v2", Expect::kAbsent)};
    auto r = c_->put_batch_atomic(b2, 0, &oc);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, CaskError::kConditionFailed);
    EXPECT_EQ(oc.failed_op, 1u);
    EXPECT_TRUE(oc.ords.empty());
    EXPECT_EQ(value_of("k"), "v1");
    EXPECT_EQ(value_of("other"), "<missing>");
}

TEST_F(ConditionalBatchTest, OrdEqualsChainsAndRejectsStale) {
    open();
    ASSERT_TRUE(c_->put(bytes("k"), bytes("v0")));
    const std::uint64_t o0 = ord_of("k");
    ASSERT_NE(o0, 0u);

    Cask::BatchOutcome oc;
    std::vector<Op> b1{put_if("k", "v1", Expect::kOrdEquals, o0)};
    ASSERT_TRUE(c_->put_batch_atomic(b1, 0, &oc));
    const std::uint64_t o1 = oc.ords[0];
    EXPECT_GT(o1, o0);
    EXPECT_EQ(o1, ord_of("k"));

    // 不重读，直接用 outcome 的 ord 接下一轮。
    std::vector<Op> b2{put_if("k", "v2", Expect::kOrdEquals, o1)};
    ASSERT_TRUE(c_->put_batch_atomic(b2, 0, &oc));
    EXPECT_EQ(value_of("k"), "v2");

    // 过期的 ord（o0 / o1）被拒；kAbsent 对存在的 key 被拒。
    for (const std::uint64_t stale : {o0, o1}) {
        std::vector<Op> b{put_if("k", "bad", Expect::kOrdEquals, stale)};
        auto r = c_->put_batch_atomic(b, 0, &oc);
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().kind, CaskError::kConditionFailed);
        EXPECT_EQ(oc.failed_op, 0u);
    }
    std::vector<Op> b3{remove_if("k", Expect::kAbsent)};
    EXPECT_EQ(c_->put_batch_atomic(b3).error().kind, CaskError::kConditionFailed);
    EXPECT_EQ(value_of("k"), "v2");

    // 对不存在的 key 用 kOrdEquals：拒。
    std::vector<Op> b4{put_if("nokey", "x", Expect::kOrdEquals, o0)};
    EXPECT_EQ(c_->put_batch_atomic(b4).error().kind, CaskError::kConditionFailed);

    // 条件删除。
    const std::uint64_t cur = ord_of("k");
    std::vector<Op> b5{remove_if("k", Expect::kOrdEquals, cur)};
    ASSERT_TRUE(c_->put_batch_atomic(b5));
    EXPECT_EQ(value_of("k"), "<missing>");
}

TEST_F(ConditionalBatchTest, DeletedAndExpiredCountAsAbsent) {
    open();
    ASSERT_TRUE(c_->put(bytes("d"), bytes("v")));
    const std::uint64_t od = ord_of("d");
    ASSERT_TRUE(c_->remove(bytes("d")));
    std::vector<Op> b1{put_if("d", "v", Expect::kOrdEquals, od)};
    EXPECT_EQ(c_->put_batch_atomic(b1).error().kind, CaskError::kConditionFailed)
        << "被删的旧版本不能再作为 kOrdEquals 的依据";
    std::vector<Op> b2{put_if("d", "again", Expect::kAbsent)};
    ASSERT_TRUE(c_->put_batch_atomic(b2));
    EXPECT_EQ(value_of("d"), "again");

    // per-key 过期（expiry_at 已过）：get 视为不存在，条件口径一致。
    DocInput doc;
    doc.text = bytes("ttl");
    doc.expiry_at = 1;  // 1970-01-01T00:00:01Z
    std::vector<Op> b3{{.type = Op::Type::kPutDoc, .key = bytes("e"), .doc = &doc}};
    ASSERT_TRUE(c_->put_batch_atomic(b3));
    ASSERT_EQ(value_of("e"), "<missing>");
    std::vector<Op> b4{put_if("e", "fresh", Expect::kAbsent)};
    ASSERT_TRUE(c_->put_batch_atomic(b4));
    EXPECT_EQ(value_of("e"), "fresh");
}

TEST_F(ConditionalBatchTest, CheckOpsValidateWithoutWriting) {
    open();
    ASSERT_TRUE(c_->put(bytes("guard"), bytes("g")));
    const std::uint64_t og = ord_of("guard");

    // 校验通过：只有写成员落盘，guard 不被重写（ord 不变）；kCheck 的 ords = 0。
    Cask::BatchOutcome oc;
    std::vector<Op> b1{check("guard", Expect::kOrdEquals, og), put("a", "1"),
                       check("nobody", Expect::kAbsent), put("b", "2")};
    ASSERT_TRUE(c_->put_batch_atomic(b1, 0, &oc));
    ASSERT_EQ(oc.ords.size(), 4u);
    EXPECT_EQ(oc.ords[0], 0u);
    EXPECT_EQ(oc.ords[1], ord_of("a"));
    EXPECT_EQ(oc.ords[2], 0u);
    EXPECT_EQ(oc.ords[3], ord_of("b"));
    EXPECT_EQ(ord_of("guard"), og);
    EXPECT_EQ(value_of("nobody"), "<missing>");

    // guard 被别人改了 ⇒ 整批不写。
    ASSERT_TRUE(c_->put(bytes("guard"), bytes("g2")));
    std::vector<Op> b2{put("c", "3"), check("guard", Expect::kOrdEquals, og)};
    auto r = c_->put_batch_atomic(b2, 0, &oc);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, CaskError::kConditionFailed);
    EXPECT_EQ(oc.failed_op, 1u);
    EXPECT_EQ(value_of("c"), "<missing>");

    // 全是 kCheck：成功、什么都不写（ord 前后一致）。
    const std::uint64_t og2 = ord_of("guard");
    std::vector<Op> b3{check("guard", Expect::kOrdEquals, og2)};
    ASSERT_TRUE(c_->put_batch_atomic(b3, 0, &oc));
    EXPECT_EQ(oc.ords, std::vector<std::uint64_t>{0});
    EXPECT_EQ(ord_of("guard"), og2);
}

TEST_F(ConditionalBatchTest, SameKeyTwiceComparesAgainstPreBatchState) {
    open();
    // 两个成员都要求「批前不存在」：批前确实不存在 ⇒ 成功，批内 LWW。
    Cask::BatchOutcome oc;
    std::vector<Op> b1{put_if("k", "first", Expect::kAbsent),
                       put_if("k", "second", Expect::kAbsent)};
    ASSERT_TRUE(c_->put_batch_atomic(b1, 0, &oc));
    EXPECT_EQ(value_of("k"), "second");
    EXPECT_LT(oc.ords[0], oc.ords[1]);
    EXPECT_EQ(oc.ords[1], ord_of("k")) << "最后一个成员才是该 key 的当前版本";

    // 第二个成员拿第一个成员的 ord 作条件：对照的是批前状态 ⇒ 失败。
    const std::uint64_t cur = ord_of("k");
    std::vector<Op> b2{put_if("k", "x", Expect::kOrdEquals, cur),
                       put_if("k", "y", Expect::kOrdEquals, cur + 1)};
    auto r = c_->put_batch_atomic(b2, 0, &oc);
    ASSERT_FALSE(r);
    EXPECT_EQ(oc.failed_op, 1u);
    EXPECT_EQ(value_of("k"), "second");
}

TEST_F(ConditionalBatchTest, MalformedConditionsAreRejectedWithoutSideEffects) {
    open();
    std::vector<Op> b1{put("a", "1"), check("x", Expect::kNone)};
    auto r1 = c_->put_batch_atomic(b1);
    ASSERT_FALSE(r1);
    EXPECT_EQ(r1.error().kind, CaskError::kInvalidOption);

    Op bad = put("a", "1");
    bad.expect = static_cast<Expect>(7);
    std::vector<Op> b2{bad};
    auto r2 = c_->put_batch_atomic(b2);
    ASSERT_FALSE(r2);
    EXPECT_EQ(r2.error().kind, CaskError::kInvalidOption);
    EXPECT_EQ(value_of("a"), "<missing>");

    // 无条件的旧调用方式不受影响（outcome 也照样回填）。
    Cask::BatchOutcome oc;
    std::vector<Op> b3{put("a", "1")};
    ASSERT_TRUE(c_->put_batch_atomic(b3, 0, &oc));
    EXPECT_EQ(oc.ords[0], ord_of("a"));
}

TEST_F(ConditionalBatchTest, OrdSurvivesMergeAndReopen) {
    open(/*max_file_size=*/4096);
    ASSERT_TRUE(c_->put(bytes("stable"), bytes("s")));
    const std::uint64_t os = ord_of("stable");
    // 制造死数据并滚出多个文件，让 merge 真的搬迁 "stable"。
    const std::string pad(200, 'p');
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 60; ++i) {
            ASSERT_TRUE(c_->put(bytes("f" + std::to_string(i)), bytes(pad)));
        }
    }
    auto m = c_->merge();
    ASSERT_TRUE(m) << m.error().detail;
    ASSERT_GE(m->records_kept, 1u) << "merge 没有搬迁任何活记录，本用例不成立";
    EXPECT_EQ(ord_of("stable"), os) << "merge 搬迁必须原样保留 ord";
    std::vector<Op> b1{check("stable", Expect::kOrdEquals, os)};
    EXPECT_TRUE(c_->put_batch_atomic(b1));

    // 重开（走检查点 / hint）。
    open(4096);
    EXPECT_EQ(ord_of("stable"), os);

    // 删掉全部派生缓存 ⇒ data 全量重建。
    c_.reset();
    for (const auto& e : fs::directory_iterator(dir_)) {
        const std::string name = e.path().filename().string();
        if (e.path().extension() == ".hint" || name == "kv.keydir.ckpt" ||
            name.rfind("kv.oki.", 0) == 0) {
            fs::remove(e.path());
        }
    }
    open(4096);
    EXPECT_EQ(ord_of("stable"), os);
    std::vector<Op> b2{put_if("stable", "s2", Expect::kOrdEquals, os)};
    ASSERT_TRUE(c_->put_batch_atomic(b2));
    EXPECT_EQ(value_of("stable"), "s2");
}

// 计数器：读值 + ord → +1 → 条件写，冲突重试。
std::uint64_t parse_u64(const GetResult& r) {
    std::uint64_t v = 0;
    std::memcpy(&v, r.value.data(), sizeof v);
    return v;
}

TEST_F(ConditionalBatchTest, ConcurrentReadModifyWriteLosesNothing) {
    open();
    constexpr int kThreads = 8;
    constexpr int kPerThread = 400;
    std::atomic<std::uint64_t> conflicts{0};
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                for (;;) {
                    std::uint64_t v = 0;
                    Op op;
                    auto g = c_->get_owned(bytes("ctr"));
                    if (g) {
                        v = parse_u64(*g) + 1;
                        op.expect = Expect::kOrdEquals;
                        op.expect_ord = g->ord;
                    } else if (g.error().kind == CaskError::kNotFound) {
                        v = 1;
                        op.expect = Expect::kAbsent;
                    } else {
                        ++errors;
                        return;
                    }
                    op.key = bytes("ctr");
                    op.value = {reinterpret_cast<const std::byte*>(&v), sizeof v};
                    std::vector<Op> b{op};
                    auto r = c_->put_batch_atomic(b);
                    if (r) break;
                    if (r.error().kind != CaskError::kConditionFailed) {
                        ++errors;
                        return;
                    }
                    ++conflicts;
                }
            }
        });
    }
    for (auto& t : ts) t.join();
    ASSERT_EQ(errors.load(), 0);
    auto g = c_->get_owned(bytes("ctr"));
    ASSERT_TRUE(g);
    EXPECT_EQ(parse_u64(*g), std::uint64_t{kThreads} * kPerThread)
        << "conflicts=" << conflicts.load();
}

// 对照组：同一负载不带条件 ⇒ 丢更新。证明上面那条测试确实有竞争。
// 竞争是概率性的：最多跑若干轮，任一轮出现丢更新即算对照成立。
TEST_F(ConditionalBatchTest, ControlUnconditionalReadModifyWriteLosesUpdates) {
    open();
    constexpr int kThreads = 8;
    constexpr int kPerThread = 400;
    bool lost = false;
    for (int round = 0; round < 10 && !lost; ++round) {
        const std::string key = "ctr" + std::to_string(round);
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t) {
            ts.emplace_back([&] {
                for (int i = 0; i < kPerThread; ++i) {
                    auto g = c_->get_owned(bytes(key));
                    std::uint64_t v = g ? parse_u64(*g) + 1 : 1;
                    std::vector<Op> b{{.type = Op::Type::kPut, .key = bytes(key),
                                       .value = {reinterpret_cast<const std::byte*>(&v),
                                                 sizeof v}}};
                    (void)c_->put_batch_atomic(b);
                }
            });
        }
        for (auto& t : ts) t.join();
        auto g = c_->get_owned(bytes(key));
        ASSERT_TRUE(g);
        lost = parse_u64(*g) < std::uint64_t{kThreads} * kPerThread;
    }
    EXPECT_TRUE(lost) << "对照组 10 轮都没有丢更新：竞争没被制造出来";
}

TEST_F(ConditionalBatchTest, TxnCommitPassesConditionsThrough) {
    open();
    TxnCask txn(c_.get(), TxnSyncPolicy::kNone);
    Cask::BatchOutcome oc;
    std::vector<TxnOp> t1{{.type = TxnOp::Type::kPut, .key = bytes("a"),
                           .value = bytes("1"), .expect = Expect::kAbsent}};
    ASSERT_TRUE(txn.commit(t1, &oc));
    const std::uint64_t oa = oc.ords[0];
    EXPECT_EQ(oa, ord_of("a"));

    std::vector<TxnOp> t2{{.type = TxnOp::Type::kPut, .key = bytes("b"), .value = bytes("2")},
                          {.type = TxnOp::Type::kCheck, .key = bytes("a"),
                           .expect = Expect::kOrdEquals, .expect_ord = oa + 1000}};
    auto r = txn.commit(t2, &oc);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, CaskError::kConditionFailed);
    EXPECT_EQ(oc.failed_op, 1u);
    EXPECT_EQ(value_of("b"), "<missing>");

    t2[1].expect_ord = oa;
    ASSERT_TRUE(txn.commit(t2, &oc));
    EXPECT_EQ(value_of("b"), "2");
}

}  // namespace
