// S33-4：OKI memdelta / flush / 恢复 集成测试。
// 覆盖：干净关闭（快照+flush 搭车）→ run 内容与水位；重开不重建；
// crash 后 tail 重放（含"部分已 flush"的增量场景）；manifest 丢失 → 全量
// 重建；快照缺口（快照写后 flush 前崩溃窗口）→ 重建；墓碑行进 run 与
// 同 key 去重语义。

#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/keydir_registry.hpp>
#include <bitcask/oki_run.hpp>
#include <bitcask/oki_state.hpp>

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskOptions;

namespace {

bitcask::keydir::KeyDirRegistry& test_registry() {
    static bitcask::keydir::KeyDirRegistry reg;
    return reg;
}

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

// 全部 run 归并读出（同 key 取 max-ord）——OKI 语义层面的"盘上视图"。
std::map<std::string, std::pair<std::uint64_t, bool>>
read_all_runs(const std::string& dir) {
    std::map<std::string, std::pair<std::uint64_t, bool>> out;
    auto m = bitcask::oki::read_manifest(dir);
    EXPECT_TRUE(m.has_value()) << "manifest 必须存在";
    if (!m) return out;
    for (const auto& r : m->runs) {
        auto rd = bitcask::oki::OkiRunReader::open(
            bitcask::oki::mk_run_filename(dir, r.gen));
        EXPECT_TRUE(rd.has_value()) << "run gen " << r.gen;
        if (!rd) continue;
        auto c = rd->begin();
        bitcask::oki::OkiRunReader::Entry e;
        while (true) {
            auto n = c.next(e);
            EXPECT_TRUE(n.has_value());
            if (!n || !*n) break;
            auto it = out.find(e.key);
            if (it == out.end() || it->second.first < e.ord) {
                out[e.key] = {e.ord, e.tomb};
            }
        }
    }
    return out;
}

class OkiRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() /
               (std::string("bitcask_oki_recovery_") + info->name());
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    fs::path dir_;
};

}  // namespace

TEST_F(OkiRecoveryTest, CleanCloseFlushesRunsAndCoversAllKeys) {
    {
        CaskOptions o;
        o.read_write = true;
        auto c = Cask::open(dir_.string(), o, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 20; ++i) {
            ASSERT_TRUE((*c)->put(bytes("ok" + std::to_string(i)),
                                  bytes("v"), 1000));
        }
        ASSERT_TRUE((*c)->remove(bytes("ok3"), 2000));
        ASSERT_TRUE((*c)->remove(bytes("ok7"), 2000));
        const std::uint64_t next = (*c)->keydir().peek_next_ord();
        (*c)->close();

        auto m = bitcask::oki::read_manifest(dir_.string());
        ASSERT_TRUE(m.has_value());
        EXPECT_EQ(m->wm, next) << "close 后 wm 必须追平 next_ord（排他上界）";
    }
    auto view = read_all_runs(dir_.string());
    // 20 put + 2 tomb（同 key 去重后墓碑胜出）→ 20 个 key，其中 2 个 tomb。
    ASSERT_EQ(view.size(), 20u);
    int tombs = 0;
    for (const auto& [k, v] : view) {
        if (v.second) ++tombs;
    }
    EXPECT_EQ(tombs, 2);
    EXPECT_TRUE(view.at("ok3").second);
    EXPECT_TRUE(view.at("ok7").second);
    EXPECT_FALSE(view.at("ok0").second);
}

TEST_F(OkiRecoveryTest, ReopenAfterCleanCloseDoesNotRebuild) {
    {
        CaskOptions o;
        o.read_write = true;
        auto c = Cask::open(dir_.string(), o, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 10; ++i) {
            ASSERT_TRUE((*c)->put(bytes("rk" + std::to_string(i)),
                                  bytes("v"), 1000));
        }
        (*c)->close();
    }
    auto m1 = bitcask::oki::read_manifest(dir_.string());
    ASSERT_TRUE(m1.has_value());
    {
        CaskOptions o;
        o.read_write = true;
        auto c = Cask::open(dir_.string(), o, &test_registry());
        ASSERT_TRUE(c);
        EXPECT_TRUE((*c)->keydir().oki().loaded());
        EXPECT_EQ((*c)->keydir().oki().wm(), m1->wm);
        EXPECT_EQ((*c)->keydir().oki().delta_rows(), 0u)
            << "无缺口重开：tail 重放为空";
        (*c)->close();
    }
    auto m2 = bitcask::oki::read_manifest(dir_.string());
    ASSERT_TRUE(m2.has_value());
    ASSERT_EQ(m2->runs.size(), m1->runs.size()) << "重开不得触发重建";
    for (std::size_t i = 0; i < m1->runs.size(); ++i) {
        EXPECT_EQ(m2->runs[i].gen, m1->runs[i].gen);
    }
}

TEST_F(OkiRecoveryTest, CrashTailReplayWithoutRebuild) {
    constexpr int kFirst = 15, kSecond = 10;
    const pid_t child = fork();
    ASSERT_NE(child, -1);
    if (child == 0) {
        CaskOptions o;
        o.read_write = true;
        auto c = Cask::open(dir_.string(), o, &test_registry());
        if (!c) _exit(1);
        for (int i = 0; i < kFirst; ++i) {
            if (!(*c)->put(bytes("ct" + std::to_string(i)), bytes("v"), 1000)) {
                _exit(1);
            }
        }
        if (!(*c)->checkpoint()) _exit(1);  // keydir 快照 + OKI flush 搭车
        for (int i = kFirst; i < kFirst + kSecond; ++i) {
            if (!(*c)->put(bytes("ct" + std::to_string(i)), bytes("v"), 1000)) {
                _exit(1);
            }
            if (!(*c)->sync()) _exit(1);
        }
        _exit(0);  // 崩溃：不 close（第二段行只在 data file 里）
    }
    int status = 0;
    ASSERT_NE(waitpid(child, &status, 0), -1);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    auto m_before = bitcask::oki::read_manifest(dir_.string());
    ASSERT_TRUE(m_before.has_value());

    {
        CaskOptions o;
        o.read_write = true;
        auto c = Cask::open(dir_.string(), o, &test_registry());
        ASSERT_TRUE(c);
        auto& oki = (*c)->keydir().oki();
        EXPECT_TRUE(oki.loaded());
        // 崩溃前 checkpoint 的 flush 覆盖第一段；第二段靠 tail 重放进
        // memdelta——不触发重建（manifest run 集保持，含新 flush 前）。
        EXPECT_GE(oki.delta_rows(), static_cast<std::size_t>(kSecond))
            << "第二段行必须经 tail 重放进 memdelta";
        (*c)->close();
    }
    auto m_after = bitcask::oki::read_manifest(dir_.string());
    ASSERT_TRUE(m_after.has_value());
    // 旧 run 仍在（未重建），新增 close-flush 的 run。
    for (const auto& r : m_before->runs) {
        bool found = false;
        for (const auto& r2 : m_after->runs) found |= (r2.gen == r.gen);
        EXPECT_TRUE(found) << "旧 run gen " << r.gen << " 不得被重建替换";
    }
    EXPECT_GT(m_after->runs.size(), m_before->runs.size());

    auto view = read_all_runs(dir_.string());
    for (int i = 0; i < kFirst + kSecond; ++i) {
        auto it = view.find("ct" + std::to_string(i));
        ASSERT_NE(it, view.end()) << "ct" << i;
        EXPECT_FALSE(it->second.second);
    }
}

TEST_F(OkiRecoveryTest, MissingManifestTriggersFullRebuild) {
    {
        CaskOptions o;
        o.read_write = true;
        auto c = Cask::open(dir_.string(), o, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 12; ++i) {
            ASSERT_TRUE((*c)->put(bytes("mm" + std::to_string(i)),
                                  bytes("v"), 1000));
        }
        ASSERT_TRUE((*c)->remove(bytes("mm5"), 2000));
        (*c)->close();
    }
    // 模拟 OKI 全丢（派生缓存语义）。
    {
        auto m = bitcask::oki::read_manifest(dir_.string());
        ASSERT_TRUE(m.has_value());
        for (const auto& r : m->runs) {
            fs::remove(bitcask::oki::mk_run_filename(dir_.string(), r.gen));
        }
        fs::remove(bitcask::oki::mk_manifest_filename(dir_.string()));
    }
    {
        CaskOptions o;
        o.read_write = true;
        auto c = Cask::open(dir_.string(), o, &test_registry());
        ASSERT_TRUE(c);
        EXPECT_TRUE((*c)->keydir().oki().loaded()) << "重建后必须就绪";
        EXPECT_EQ((*c)->keydir().oki().wm(),
                  (*c)->keydir().peek_next_ord());
        (*c)->close();
    }
    auto view = read_all_runs(dir_.string());
    // 重建只含活 key（被删的 mm5 缺席即语义）。
    EXPECT_EQ(view.size(), 11u);
    EXPECT_EQ(view.count("mm5"), 0u);
    EXPECT_EQ(view.count("mm0"), 1u);
}

TEST_F(OkiRecoveryTest, SnapshotGapTriggersRebuild) {
    {
        CaskOptions o;
        o.read_write = true;
        auto c = Cask::open(dir_.string(), o, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 8; ++i) {
            ASSERT_TRUE((*c)->put(bytes("sg" + std::to_string(i)),
                                  bytes("v"), 1000));
        }
        (*c)->close();  // 快照 + flush 都写了
    }
    // 模拟「快照写后、OKI flush 前崩溃」：把 manifest 回滚成空（wm=0），
    // 快照仍在——fold 会跳过字节水位前的行，OKI 无法靠 tail 重放补齐。
    ASSERT_TRUE(bitcask::oki::write_manifest(dir_.string(),
                                             bitcask::oki::OkiManifest{}));
    {
        CaskOptions o;
        o.read_write = true;
        auto c = Cask::open(dir_.string(), o, &test_registry());
        ASSERT_TRUE(c);
        EXPECT_TRUE((*c)->keydir().oki().loaded());
        EXPECT_EQ((*c)->keydir().oki().wm(),
                  (*c)->keydir().peek_next_ord())
            << "缺口必须触发全量重建并追平水位";
        (*c)->close();
    }
    auto m = bitcask::oki::read_manifest(dir_.string());
    ASSERT_TRUE(m.has_value());
    ASSERT_EQ(m->runs.size(), 1u) << "重建产出单一全量 run";
    auto view = read_all_runs(dir_.string());
    EXPECT_EQ(view.size(), 8u);
}
