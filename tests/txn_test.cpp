// S34/S35：TxnCask 多键事务测试（doc/multikey-txn-impl-design-zh.md §9 +
// doc/atomic-batch-design-zh.md）。覆盖：正常提交、校验拒绝零副作用、
// recover/pending_txns 的 B2 后语义（恒空 + 遗留 "_txn:" key 不受触碰）、
// 崩溃下 commit 的引擎原子批 all-or-nothing（S37-2：fork → exec-self）。
// B2（2026-08-06）：意图重放已删除（从未随发布版本存在）——原六个意图
// 构造/重放用例（手写编码器对拍等）随实现一并退役，崩溃原子性由
// atomic_batch_test 的批语义用例承接。

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/keydir_registry.hpp>
#include <bitcask/txn.hpp>

#include "support/crash_child.hpp"

using bitcask::test::crash_exit;

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bitcask::Cask;
using bitcask::CaskError;
using bitcask::CaskOptions;
using bitcask::TxnCask;
using bitcask::TxnOp;

inline bitcask::keydir::KeyDirRegistry& test_registry() {
    static bitcask::keydir::KeyDirRegistry reg;
    return reg;
}

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

TxnOp put_op(std::string_view k, std::string_view v) {
    return {.type = TxnOp::Type::kPut, .key = bytes(k), .value = bytes(v)};
}

TxnOp remove_op(std::string_view k) {
    return {.type = TxnOp::Type::kRemove, .key = bytes(k)};
}

class TxnTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tmpdir_ = std::filesystem::temp_directory_path() /
                  (std::string("bitcask_txn_test_") + info->name());
        std::error_code ec;
        std::filesystem::remove_all(tmpdir_, ec);
        std::filesystem::create_directories(tmpdir_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(tmpdir_, ec);
    }

    std::unique_ptr<Cask> open_rw() {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        EXPECT_TRUE(c) << (c ? "" : c.error().detail);
        return c ? *std::move(c) : nullptr;
    }

    static std::string get_str(Cask& c, std::string_view key) {
        auto r = c.get_owned(bytes(key));
        EXPECT_TRUE(r) << (r ? "" : r.error().detail);
        if (!r) return {};
        return {reinterpret_cast<const char*>(r->value.data()),
                r->value.size()};
    }

    static bool missing(Cask& c, std::string_view key) {
        auto r = c.get_owned(bytes(key));
        return !r && r.error().kind == CaskError::kNotFound;
    }

    std::filesystem::path tmpdir_;
};

TEST_F(TxnTest, CommitBasic) {
    auto c = open_rw();
    ASSERT_TRUE(c);
    ASSERT_TRUE(c->put(bytes("gone"), bytes("old")));

    TxnCask txn(c.get());
    const std::vector<TxnOp> ops = {
        put_op("a", "1"), put_op("b", "2"), remove_op("gone")};
    ASSERT_TRUE(txn.commit(ops));

    EXPECT_EQ(get_str(*c, "a"), "1");
    EXPECT_EQ(get_str(*c, "b"), "2");
    EXPECT_TRUE(missing(*c, "gone"));

    auto pending = txn.pending_txns();
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending->empty());  // ④ 已清理
    c->close();
}

TEST_F(TxnTest, ValidationRejectsWithZeroSideEffects) {
    auto c = open_rw();
    ASSERT_TRUE(c);
    TxnCask txn(c.get());

    const auto expect_invalid = [&](const std::vector<TxnOp>& ops) {
        auto r = txn.commit(ops);
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().kind, CaskError::kInvalidOption);
    };
    expect_invalid({});                                         // 空批
    expect_invalid({put_op("", "v")});                          // 空 key
    expect_invalid({put_op("k", "1"), remove_op("k")});         // 重复 key
    expect_invalid({put_op("_txn:abc", "v")});                  // 保留前缀

    // 零副作用：keydir 空、无 pending。
    EXPECT_TRUE(missing(*c, "k"));
    auto pending = txn.pending_txns();
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending->empty());
    c->close();
}

TEST_F(TxnTest, RecoverOnCleanDirIsZero) {
    auto c = open_rw();
    ASSERT_TRUE(c);
    TxnCask txn(c.get());
    auto n = txn.recover();
    ASSERT_TRUE(n) << n.error().detail;
    EXPECT_EQ(*n, 0u);
    c->close();
}

// B2：遗留意图残留（开发期构建可能写过的 "_txn:" 前缀 key）——recover
// 恒 0、pending 恒空、残留不被触碰（手工清理路径仍是普通 KV API）。
TEST_F(TxnTest, RecoverIgnoresLegacyIntentLeftovers) {
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        ASSERT_TRUE(c->put(bytes("_txn:0000000000000001-deadbeef"),
                           bytes("opaque-legacy-bytes")));
        c->close();
    }
    auto c = open_rw();
    ASSERT_TRUE(c);
    TxnCask txn(c.get());
    auto n = txn.recover();
    ASSERT_TRUE(n) << n.error().detail;
    EXPECT_EQ(*n, 0u) << "B2：意图重放已删除，恒 0";
    auto pending = txn.pending_txns();
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending->empty());
    // 残留原样可读（手工清理走普通 KV API）。
    EXPECT_EQ(get_str(*c, "_txn:0000000000000001-deadbeef"),
              "opaque-legacy-bytes");
    ASSERT_TRUE(c->remove(bytes("_txn:0000000000000001-deadbeef")));
    c->close();
}

// 崩溃注入：commit 的原子性由引擎原子批承载（方案 C）——崩溃点在
// commit 返回之后，重开必须整批可见；kSyncOnCommit 保证 durable。
// （掐尾批不可见的另一半由 atomic_batch_test 的截断注入覆盖。）
BITCASK_CRASH_SCENARIO(txn_crash_after_commit) {
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(dir, opts, &test_registry());
    if (!c) crash_exit(1);
    TxnCask txn(c->get());
    const std::vector<TxnOp> ops{put_op("t1", "v1"), put_op("t2", "v2")};
    if (!txn.commit(ops)) crash_exit(2);
    crash_exit(0);  // 不 close：句柄、write lock 全部随进程消失
}

TEST_F(TxnTest, CrashAfterCommitIsAtomicallyVisible) {
    ASSERT_EQ(bitcask::test::spawn_crash_child("txn_crash_after_commit",
                                               tmpdir_.string()),
              0);

    auto c = open_rw();
    ASSERT_TRUE(c);
    TxnCask txn(c.get());
    auto n = txn.recover();
    ASSERT_TRUE(n) << n.error().detail;
    EXPECT_EQ(*n, 0u);
    EXPECT_EQ(get_str(*c, "t1"), "v1");
    EXPECT_EQ(get_str(*c, "t2"), "v2");
    c->close();
}

}  // namespace
