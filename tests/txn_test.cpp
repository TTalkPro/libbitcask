// S34：TxnCask 多键事务测试（doc/multikey-txn-impl-design-zh.md §9）。
// 覆盖：正常提交、校验拒绝零副作用、意图前滚重放（手写编码器对拍钉死
// blob v1 格式）、REMOVE 重放、seq 序重放定序、fork 崩溃注入、巡检枚举。

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/keydir_registry.hpp>
#include <bitcask/txn.hpp>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

// --- 手写意图 blob 编码器：与 txn.cpp encode_ops 相互独立的第二实现，
// --- 对拍钉死 v1 盘上布局（设计 §3）。改任何一边布局本测试必红。
void le32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void le64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

struct HandOp {
    std::uint8_t type;  // 0 = put, 1 = remove
    std::string key;
    std::string value;
};

std::string hand_encode(std::uint64_t created_at_us,
                        const std::vector<HandOp>& ops) {
    std::string out;
    out.push_back(static_cast<char>(1));  // ver
    le64(out, created_at_us);
    le32(out, static_cast<std::uint32_t>(ops.size()));
    for (const auto& op : ops) {
        out.push_back(static_cast<char>(op.type));
        le32(out, static_cast<std::uint32_t>(op.key.size()));
        le32(out, op.type == 0 ? static_cast<std::uint32_t>(op.value.size()) : 0);
        out += op.key;
        if (op.type == 0) out += op.value;
    }
    return out;
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

// 模拟「崩在 ③ 之前」：只写意图记录（手写编码器构造）→ reopen →
// recover 前滚补齐数据 + 清理意图。
TEST_F(TxnTest, RecoverReplaysPendingIntent) {
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        const std::string blob = hand_encode(
            123456789, {{0, "x", "vx"}, {0, "y", "vy"}});
        ASSERT_TRUE(c->put(bytes("_txn:0000000000000001-deadbeef"),
                           bytes(blob)));
        c->close();  // 意图在,数据不在——等价于崩溃留下的状态
    }
    auto c = open_rw();
    ASSERT_TRUE(c);
    TxnCask txn(c.get());
    auto n = txn.recover();
    ASSERT_TRUE(n) << n.error().detail;
    EXPECT_EQ(*n, 1u);
    EXPECT_EQ(get_str(*c, "x"), "vx");
    EXPECT_EQ(get_str(*c, "y"), "vy");
    auto pending = txn.pending_txns();
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending->empty());
    c->close();
}

TEST_F(TxnTest, RecoverAppliesRemoveOps) {
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        ASSERT_TRUE(c->put(bytes("victim"), bytes("alive")));
        const std::string blob =
            hand_encode(1, {{0, "kept", "v"}, {1, "victim", ""}});
        ASSERT_TRUE(c->put(bytes("_txn:0000000000000001-deadbeef"),
                           bytes(blob)));
        c->close();
    }
    auto c = open_rw();
    ASSERT_TRUE(c);
    TxnCask txn(c.get());
    auto n = txn.recover();
    ASSERT_TRUE(n) << n.error().detail;
    EXPECT_EQ(*n, 1u);
    EXPECT_EQ(get_str(*c, "kept"), "v");
    EXPECT_TRUE(missing(*c, "victim"));
    c->close();
}

// 两条 pending 触碰同一 key：txn key 定宽 hex 字典序 = seq 序 = 提交序,
// seq 大者(后提交)终值胜(设计 §4)。
TEST_F(TxnTest, ReplayOrderIsSeqOrder) {
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        const std::string first = hand_encode(1, {{0, "k", "old"}});
        const std::string second = hand_encode(2, {{0, "k", "new"}});
        // 故意乱序写入——重放序由 key 字典序决定,与写入序无关。
        ASSERT_TRUE(c->put(bytes("_txn:0000000000000002-cafecafe"),
                           bytes(second)));
        ASSERT_TRUE(c->put(bytes("_txn:0000000000000001-deadbeef"),
                           bytes(first)));
        c->close();
    }
    auto c = open_rw();
    ASSERT_TRUE(c);
    TxnCask txn(c.get());
    auto n = txn.recover();
    ASSERT_TRUE(n) << n.error().detail;
    EXPECT_EQ(*n, 2u);
    EXPECT_EQ(get_str(*c, "k"), "new");
    c->close();
}

TEST_F(TxnTest, MalformedIntentStopsRecover) {
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        ASSERT_TRUE(c->put(bytes("_txn:0000000000000001-deadbeef"),
                           bytes("garbage")));
        c->close();
    }
    auto c = open_rw();
    ASSERT_TRUE(c);
    TxnCask txn(c.get());
    auto n = txn.recover();
    ASSERT_FALSE(n);
    EXPECT_EQ(n.error().kind, CaskError::kBadCrc);
    c->close();
}

TEST_F(TxnTest, PendingTxnsInspection) {
    auto c = open_rw();
    ASSERT_TRUE(c);
    const std::string blob = hand_encode(42, {{0, "p", "v"}, {1, "q", ""}});
    ASSERT_TRUE(c->put(bytes("_txn:00000000000000ff-01020304"), bytes(blob)));

    TxnCask txn(c.get());
    auto pending = txn.pending_txns();
    ASSERT_TRUE(pending) << pending.error().detail;
    ASSERT_EQ(pending->size(), 1u);
    EXPECT_EQ((*pending)[0].txn_key, "_txn:00000000000000ff-01020304");
    EXPECT_EQ((*pending)[0].created_at_us, 42u);
    EXPECT_EQ((*pending)[0].op_count, 2u);
    c->close();
}

// fork 崩溃注入:子进程完成 ①意图 + ②sync + 一半的 ③(两 key 只写一个),
// 然后 _exit 模拟崩溃——即设计 §2.2「崩在 3 的中间」。父进程 reopen 后
// recover 必须把两个 key 全部补齐并清理意图。
TEST_F(TxnTest, CrashMidApplyRecovers) {
    const pid_t child = fork();
    ASSERT_NE(child, -1) << "fork failed";

    if (child == 0) {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        if (!c) _exit(1);
        const std::string blob =
            hand_encode(7, {{0, "t1", "v1"}, {0, "t2", "v2"}});
        if (!(*c)->put(bytes("_txn:0000000000000007-deadbeef"), bytes(blob)))
            _exit(2);
        if (!(*c)->sync()) _exit(3);
        // ③ 的一半:只写 t1,t2 没写——然后"崩溃"。
        if (!(*c)->put(bytes("t1"), bytes("v1"))) _exit(4);
        _exit(0);  // 不 close:句柄、write lock 全部随进程消失
    }

    int status = 0;
    ASSERT_NE(waitpid(child, &status, 0), -1);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    auto c = open_rw();
    ASSERT_TRUE(c);
    TxnCask txn(c.get());
    auto n = txn.recover();
    ASSERT_TRUE(n) << n.error().detail;
    EXPECT_EQ(*n, 1u);
    EXPECT_EQ(get_str(*c, "t1"), "v1");
    EXPECT_EQ(get_str(*c, "t2"), "v2");
    auto pending = txn.pending_txns();
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending->empty());
    c->close();
}

}  // namespace
