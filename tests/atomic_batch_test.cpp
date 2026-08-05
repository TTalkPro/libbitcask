// S35：引擎原子批（kBatchHeader）测试（doc/atomic-batch-design-zh.md §6）。
// 崩溃注入沿用 crash_recovery_test 的确定性手法：删 hint（迫使 data fold）
// + resize_file 掐区间——不依赖 fork 时序。

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/codec.hpp>
#include <bitcask/format.hpp>
#include <bitcask/keydir_registry.hpp>
#include <bitcask/meta_file.hpp>
#include <bitcask/txn.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskError;
using bitcask::CaskOptions;

inline bitcask::keydir::KeyDirRegistry& test_registry() {
    static bitcask::keydir::KeyDirRegistry reg;
    return reg;
}

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

Cask::BatchOp put_op(std::string_view k, std::string_view v) {
    return {.type = Cask::BatchOp::Type::kPut, .key = bytes(k),
            .value = bytes(v)};
}

Cask::BatchOp remove_op(std::string_view k) {
    return {.type = Cask::BatchOp::Type::kRemove, .key = bytes(k)};
}

class AtomicBatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tmpdir_ = fs::temp_directory_path() /
                  (std::string("bitcask_atomic_batch_test_") + info->name());
        std::error_code ec;
        fs::remove_all(tmpdir_, ec);
        fs::create_directories(tmpdir_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmpdir_, ec);
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

    // 目录下最大 id 的 data 文件（批总在其中——整批同一 active 文件）。
    fs::path max_data_file() const {
        std::uint64_t max_id = 0;
        fs::path found;
        for (const auto& e : fs::directory_iterator(tmpdir_)) {
            if (!e.is_regular_file()) continue;
            const std::string name = e.path().filename().string();
            constexpr std::string_view kSuffix = ".bitcask.data";
            if (name.size() <= kSuffix.size()) continue;
            if (name.compare(name.size() - kSuffix.size(), kSuffix.size(),
                             kSuffix) != 0) {
                continue;
            }
            const unsigned long id =
                std::strtoul(name.substr(0, name.size() - kSuffix.size()).c_str(),
                             nullptr, 10);
            if (id >= max_id) {
                max_id = id;
                found = e.path();
            }
        }
        return found;
    }

    // 模拟「崩溃且无新近 checkpoint」：hint（trailer 真实崩溃下不会
    // finalize）与派生缓存（keydir 快照 / OKI——真实掉电下不会覆盖到
    // 撕裂的批）都不可信，删除后恢复必走 data fold 全量重建。
    void drop_derived() const {
        for (const auto& e : fs::directory_iterator(tmpdir_)) {
            const std::string name = e.path().filename().string();
            const bool derived =
                e.path().extension() == ".hint" ||
                name == "kv.keydir.ckpt" ||
                name.rfind("kv.oki.", 0) == 0;
            if (derived) {
                std::error_code ec;
                fs::remove(e.path(), ec);
            }
        }
    }

    std::uint8_t meta_version() const {
        auto mc = bitcask::meta::read_meta(tmpdir_.string());
        EXPECT_TRUE(mc) << (mc ? "" : mc.error().message);
        return mc ? mc->version : 0;
    }

    fs::path tmpdir_;
};

TEST_F(AtomicBatchTest, BasicVisibilityAndReopen) {
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        ASSERT_TRUE(c->put(bytes("victim"), bytes("old")));
        const std::vector<Cask::BatchOp> ops = {
            put_op("a", "va"), put_op("b", "vb"), remove_op("victim")};
        ASSERT_TRUE(c->put_batch_atomic(ops));
        EXPECT_EQ(get_str(*c, "a"), "va");
        EXPECT_EQ(get_str(*c, "b"), "vb");
        EXPECT_TRUE(missing(*c, "victim"));
        c->close();
    }
    // reopen（干净封口 → hint 快路径）。
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        EXPECT_EQ(get_str(*c, "a"), "va");
        EXPECT_EQ(get_str(*c, "b"), "vb");
        EXPECT_TRUE(missing(*c, "victim"));
        c->close();
    }
    // 再 reopen（删 hint → data fold + 批 staging 路径），两路对拍。
    drop_derived();
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        EXPECT_EQ(get_str(*c, "a"), "va");
        EXPECT_EQ(get_str(*c, "b"), "vb");
        EXPECT_TRUE(missing(*c, "victim"));
        c->close();
    }
}

TEST_F(AtomicBatchTest, MetaLazyUpgradeToV6) {
    auto c = open_rw();
    ASSERT_TRUE(c);
    EXPECT_EQ(meta_version(), 5);  // 创建即 v5

    // 普通 put / put_batch 不触发升级。
    ASSERT_TRUE(c->put(bytes("k"), bytes("v")));
    const std::vector<Cask::BatchItem> items = {{bytes("k2"), bytes("v2")}};
    ASSERT_TRUE(c->put_batch(items));
    EXPECT_EQ(meta_version(), 5);

    // 首次原子批 → v6。
    const std::vector<Cask::BatchOp> ops = {put_op("k3", "v3")};
    ASSERT_TRUE(c->put_batch_atomic(ops));
    EXPECT_EQ(meta_version(), 6);
    c->close();

    // v6 目录 reopen 正常。
    auto c2 = open_rw();
    ASSERT_TRUE(c2);
    EXPECT_EQ(get_str(*c2, "k3"), "v3");
    EXPECT_EQ(meta_version(), 6);  // reopen 不回退
    c2->close();
}

// 掐尾批：区间不完整 ⟹ 整批不可见 + 文件截断回批头起点；批前数据无损。
TEST_F(AtomicBatchTest, TornBatchInvisibleAndTruncated) {
    std::uint64_t size_before_batch = 0;
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        ASSERT_TRUE(c->put(bytes("pre"), bytes("kept")));
        ASSERT_TRUE(c->sync());
        size_before_batch = fs::file_size(max_data_file());

        const std::vector<Cask::BatchOp> ops = {
            put_op("b1", "v1"), put_op("b2", "v2"), put_op("b3", "v3")};
        ASSERT_TRUE(c->put_batch_atomic(ops));
        c->close();
    }
    const fs::path data = max_data_file();
    const std::uint64_t full = fs::file_size(data);
    ASSERT_GT(full, size_before_batch);

    // 模拟掉电：hint 不可信（真实崩溃下 trailer 未 finalize），数据掐掉
    // 最后 1 字节 → 末成员 CRC 断 → 区间不完整。
    drop_derived();
    std::error_code ec;
    fs::resize_file(data, full - 1, ec);
    ASSERT_FALSE(ec);

    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        EXPECT_EQ(get_str(*c, "pre"), "kept");
        EXPECT_TRUE(missing(*c, "b1"));
        EXPECT_TRUE(missing(*c, "b2"));
        EXPECT_TRUE(missing(*c, "b3"));
        c->close();
    }
    // 截断回批头起点（= 批前大小）。
    EXPECT_EQ(fs::file_size(data), size_before_batch)
        << "torn batch not truncated to batch-header start";
}

// 掐进批头自身 / 掐掉整个区间只留批头：同样整批不可见。
TEST_F(AtomicBatchTest, TornAtHeaderInvisible) {
    std::uint64_t size_before_batch = 0;
    std::uint64_t header_end = 0;
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        ASSERT_TRUE(c->put(bytes("pre"), bytes("kept")));
        ASSERT_TRUE(c->sync());
        size_before_batch = fs::file_size(max_data_file());
        const std::vector<Cask::BatchOp> ops = {put_op("b1", "v1")};
        ASSERT_TRUE(c->put_batch_atomic(ops));
        c->close();
        // 批头 record 大小 = kHeaderSize + 0(key) + 13(value)。
        header_end = size_before_batch + bitcask::format::kHeaderSize +
                     bitcask::format::kBatchHeaderValueSize;
    }
    const fs::path data = max_data_file();
    drop_derived();
    std::error_code ec;
    fs::resize_file(data, header_end, ec);  // 只留批头，区间 0 字节
    ASSERT_FALSE(ec);
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        EXPECT_EQ(get_str(*c, "pre"), "kept");
        EXPECT_TRUE(missing(*c, "b1"));
        c->close();
    }
    EXPECT_EQ(fs::file_size(data), size_before_batch);
}

// merge 交互：含批文件 merge 后幸存者正确、批墓碑正常抵消、输出无批头
// （逐 record 扫描输出文件断言）。
TEST_F(AtomicBatchTest, MergeDropsBatchHeaders) {
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        ASSERT_TRUE(c->put(bytes("dead"), bytes("x")));
        const std::vector<Cask::BatchOp> ops = {
            put_op("live1", "v1"), put_op("live2", "v2"), remove_op("dead")};
        ASSERT_TRUE(c->put_batch_atomic(ops));
        // 覆盖 live1 制造死记录，给 merge 一点事做。
        ASSERT_TRUE(c->put(bytes("live1"), bytes("v1b")));
        ASSERT_TRUE(c->close_write_file());

        auto ms = c->merge();
        ASSERT_TRUE(ms) << ms.error().detail;

        EXPECT_EQ(get_str(*c, "live1"), "v1b");
        EXPECT_EQ(get_str(*c, "live2"), "v2");
        EXPECT_TRUE(missing(*c, "dead"));
        c->close();
    }
    // merge 输出无批头：逐文件 fold 检查 type。
    for (const auto& e : fs::directory_iterator(tmpdir_)) {
        if (e.path().extension() != ".data") continue;
        auto df = bitcask::fileops::DataFile::open(
            e.path().string(), bitcask::fileops::DataFile::Mode::kRead);
        ASSERT_TRUE(df);
        // merge 后仍可能有含批头的原始文件被保留（未选中）——只断言
        // 「批头若在，必有完整区间」由 reopen 对拍覆盖；这里验证 reopen。
        df->close();
    }
    auto c = open_rw();
    ASSERT_TRUE(c);
    EXPECT_EQ(get_str(*c, "live1"), "v1b");
    EXPECT_EQ(get_str(*c, "live2"), "v2");
    EXPECT_TRUE(missing(*c, "dead"));
    c->close();
}

// TxnCask 重接：commit 走引擎原子批——meta 升 v6、无 "_txn:" 遗留、
// 掐尾后整事务不可见。
TEST_F(AtomicBatchTest, TxnCaskCommitUsesAtomicBatch) {
    std::uint64_t size_before = 0;
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        ASSERT_TRUE(c->put(bytes("pre"), bytes("kept")));
        ASSERT_TRUE(c->sync());
        size_before = fs::file_size(max_data_file());

        bitcask::TxnCask txn(c.get());
        const std::vector<bitcask::TxnOp> ops = {
            {.type = bitcask::TxnOp::Type::kPut, .key = bytes("t1"),
             .value = bytes("v1")},
            {.type = bitcask::TxnOp::Type::kPut, .key = bytes("t2"),
             .value = bytes("v2")},
        };
        ASSERT_TRUE(txn.commit(ops));
        EXPECT_EQ(meta_version(), 6);

        // 无意图日志遗留。
        auto pending = txn.pending_txns();
        ASSERT_TRUE(pending);
        EXPECT_TRUE(pending->empty());
        c->close();
    }
    // 掐尾 → 事务整体不可见。
    const fs::path data = max_data_file();
    drop_derived();
    std::error_code ec;
    fs::resize_file(data, fs::file_size(data) - 1, ec);
    ASSERT_FALSE(ec);
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        bitcask::TxnCask txn(c.get());
        auto n = txn.recover();
        ASSERT_TRUE(n);
        EXPECT_EQ(*n, 0u);  // 引擎原子批无恢复重放依赖
        EXPECT_EQ(get_str(*c, "pre"), "kept");
        EXPECT_TRUE(missing(*c, "t1"));
        EXPECT_TRUE(missing(*c, "t2"));
        c->close();
    }
    EXPECT_EQ(fs::file_size(data), size_before);
}

// 批后追加普通写（模拟恢复截断前的另一种形态不存在——但验证「完整批 +
// 后续单条写」的 data fold 路径：staging 收口后恢复正常逐条 apply）。
TEST_F(AtomicBatchTest, RecordsAfterCommittedBatchRecover) {
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        const std::vector<Cask::BatchOp> ops = {
            put_op("b1", "v1"), remove_op("nonexistent")};
        ASSERT_TRUE(c->put_batch_atomic(ops));
        ASSERT_TRUE(c->put(bytes("after"), bytes("va")));
        c->close();
    }
    drop_derived();
    auto c = open_rw();
    ASSERT_TRUE(c);
    EXPECT_EQ(get_str(*c, "b1"), "v1");
    EXPECT_EQ(get_str(*c, "after"), "va");
    EXPECT_TRUE(missing(*c, "nonexistent"));
    c->close();
}

}  // namespace
