#include <gtest/gtest.h>

#include <bitcask/cask.hpp>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bitcask::Cask;
using bitcask::CaskOptions;

class CrashRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tmpdir_ = std::filesystem::temp_directory_path() /
                  (std::string("bitcask_crash_recovery_test_") + info->name());
        std::error_code ec;
        std::filesystem::remove_all(tmpdir_, ec);
        std::filesystem::create_directories(tmpdir_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(tmpdir_, ec);
    }

    static std::string key_for(int i) {
        char buf[16]{};
        std::snprintf(buf, sizeof(buf), "key%04d", i);
        return std::string(buf);
    }

    static std::string value_for(int i) {
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "val%04d_fixed", i);
        return std::string(buf);
    }

    static std::vector<std::byte> bytes(std::string_view s) {
        const auto* p = reinterpret_cast<const std::byte*>(s.data());
        return std::vector<std::byte>(p, p + s.size());
    }

    std::filesystem::path max_data_file() const {
        std::uint64_t max_id = 0;
        std::filesystem::path found;
        for (const auto& e : std::filesystem::directory_iterator(tmpdir_)) {
            if (!e.is_regular_file()) continue;
            const std::string name = e.path().filename().string();
            constexpr std::string_view kSuffix = ".bitcask.data";
            if (name.size() <= kSuffix.size()) continue;
            if (name.compare(name.size() - kSuffix.size(), kSuffix.size(),
                             kSuffix) != 0) {
                continue;
            }
            const std::string id_str =
                name.substr(0, name.size() - kSuffix.size());
            char* end = nullptr;
            const unsigned long id = std::strtoul(id_str.c_str(), &end, 10);
            if (end != id_str.c_str() + id_str.size()) continue;
            if (id >= max_id) {
                max_id = id;
                found = e.path();
            }
        }
        return found;
    }

    std::filesystem::path tmpdir_;
};

TEST_F(CrashRecoveryTest, MidPutRestartFoldsCorrectly) {
    constexpr int kN = 50;

    const pid_t child = fork();
    ASSERT_NE(child, -1) << "fork failed";

    if (child == 0) {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts);
        if (!c) {
            std::fprintf(stderr, "child open failed: %s\n",
                         c.error().detail.c_str());
            _exit(1);
        }
        for (int i = 0; i < kN; ++i) {
            auto pr = (*c)->put(bytes(key_for(i)), bytes(value_for(i)),
                                static_cast<std::uint32_t>(1000 + i));
            if (!pr) {
                std::fprintf(stderr, "child put failed: %s\n",
                             pr.error().detail.c_str());
                _exit(1);
            }
            auto sr = (*c)->sync();
            if (!sr) {
                std::fprintf(stderr, "child sync failed: %s\n",
                             sr.error().detail.c_str());
                _exit(1);
            }
        }
        _exit(0);
    }

    int status = 0;
    const pid_t waited = waitpid(child, &status, 0);
    ASSERT_NE(waited, -1);
    ASSERT_TRUE(WIFEXITED(status) || WIFSIGNALED(status));

    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts);
    ASSERT_TRUE(c) << "reopen after crash failed: " << c.error().detail;

    auto it = (*c)->make_iter();
    auto start = it->start();
    ASSERT_TRUE(start);

    std::map<std::string, std::string> recovered;
    while (true) {
        auto entry = it->next();
        ASSERT_TRUE(entry);
        if (!*entry) break;
        std::string key(reinterpret_cast<const char*>((*entry)->key.data()),
                        (*entry)->key.size());
        std::string value(
            reinterpret_cast<const char*>((*entry)->value.data()),
            (*entry)->value.size());
        recovered.emplace(std::move(key), std::move(value));
    }

    EXPECT_GE(recovered.size(), 1u);
    EXPECT_EQ(recovered.size(), static_cast<std::size_t>(kN))
        << "all sync'd puts should be recoverable";

    for (const auto& [k, v] : recovered) {
        ASSERT_GE(k.size(), 3u);
        const int idx = std::atoi(k.substr(3).c_str());
        EXPECT_EQ(v, value_for(idx))
            << "corruption detected for key " << k;
    }

    EXPECT_EQ(recovered.count(key_for(0)), 1u);

    (*c)->close();
}

TEST_F(CrashRecoveryTest, TornWriteTruncated) {
    constexpr int kN = 10;
    constexpr std::string_view kGarbage =
        "GARBAGE_DATA_NOT_A_VALID_RECORD_HEADER_THIS_IS_LONGER_THAN_100_BYTES_"
        "AND_WILL_NEVER_LOOK_LIKE_A_REAL_BITCASK_RECORD";
    constexpr std::size_t kGarbageBytes = 100;
    static_assert(kGarbage.size() >= kGarbageBytes);

    {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts);
        ASSERT_TRUE(c);
        for (int i = 0; i < kN; ++i) {
            ASSERT_TRUE((*c)->put(bytes(key_for(i)), bytes(value_for(i)),
                                  static_cast<std::uint32_t>(2000 + i)));
        }
        ASSERT_TRUE((*c)->sync());
        (*c)->close();
    }

    const std::filesystem::path data_path = max_data_file();
    ASSERT_FALSE(data_path.empty()) << "no data file found after clean close";

    const std::filesystem::path hint_path =
        data_path.parent_path() /
        (data_path.stem().string() + ".hint");

    std::error_code ec;
    std::filesystem::remove(hint_path, ec);

    const std::uint64_t valid_size =
        std::filesystem::file_size(data_path, ec);
    ASSERT_FALSE(ec) << "failed to stat data file";

    {
        std::ofstream out(data_path, std::ios::binary | std::ios::app);
        ASSERT_TRUE(out.is_open());
        out.write(kGarbage.data(), static_cast<std::streamsize>(kGarbageBytes));
        ASSERT_TRUE(out.good());
    }

    const std::uint64_t corrupted_size =
        std::filesystem::file_size(data_path, ec);
    ASSERT_FALSE(ec);
    EXPECT_EQ(corrupted_size, valid_size + kGarbageBytes);

    {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts);
        ASSERT_TRUE(c) << "reopen with torn tail failed: " << c.error().detail;

        const std::uint64_t repaired_size =
            std::filesystem::file_size(data_path, ec);
        ASSERT_FALSE(ec);
        EXPECT_EQ(repaired_size, valid_size)
            << "torn tail was not truncated to last_valid_end";

        for (int i = 0; i < kN; ++i) {
            auto gr = (*c)->get_owned(bytes(key_for(i)));
            ASSERT_TRUE(gr) << "key " << key_for(i) << " missing after reopen";
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(
                                           gr->value.data()),
                                       gr->value.size()),
                      value_for(i));
        }

        (*c)->close();
    }
}

// R3:多数据文件纯 KV 冷启动并行 fold。小 max_file_size 强制滚动出多个
// data 文件，触发 load_keydir_from_disk 的并行路径（search_layer==null）。
// 含跨文件覆盖（同 key 多版本）以校验并行下 (file_id,tstamp,offset) LWW
// 冲突解析与到达序无关——最后写入的值必胜。
TEST_F(CrashRecoveryTest, MultiFileParallelFoldRecovers) {
    constexpr int kN = 400;
    // ~100B 值，4 KiB 文件 → 每文件 ~30 record，共 ~13 个 data 文件。
    auto big_value = [](int i) {
        std::string v = value_for(i);
        v.resize(100, 'x');
        return v;
    };

    {
        CaskOptions opts;
        opts.read_write = true;
        opts.max_file_size = 4096;
        auto c = Cask::open(tmpdir_.string(), opts);
        ASSERT_TRUE(c) << c.error().detail;

        std::uint32_t ts = 1000;
        // 第一轮：全部写入旧值（故意写错的 value，稍后被覆盖）。
        for (int i = 0; i < kN; ++i) {
            auto pr = (*c)->put(bytes(key_for(i)), bytes("STALE_VERSION"), ts++);
            ASSERT_TRUE(pr) << pr.error().detail;
        }
        // 第二轮：用更高 tstamp 覆盖成正确值，跨越多个后续文件。
        for (int i = 0; i < kN; ++i) {
            auto pr = (*c)->put(bytes(key_for(i)), bytes(big_value(i)), ts++);
            ASSERT_TRUE(pr) << pr.error().detail;
        }
        (*c)->close();
    }

    // 应当滚出多个 data 文件，否则没测到并行路径。
    int data_files = 0;
    for (const auto& e : std::filesystem::directory_iterator(tmpdir_)) {
        const std::string name = e.path().filename().string();
        if (name.size() > 13 &&
            name.compare(name.size() - 13, 13, ".bitcask.data") == 0) {
            ++data_files;
        }
    }
    ASSERT_GT(data_files, 1) << "expected multiple data files to exercise "
                                "parallel fold";

    {
        CaskOptions opts;
        opts.read_write = true;
        opts.max_file_size = 4096;
        auto c = Cask::open(tmpdir_.string(), opts);
        ASSERT_TRUE(c) << "parallel reopen failed: " << c.error().detail;

        for (int i = 0; i < kN; ++i) {
            auto gr = (*c)->get_owned(bytes(key_for(i)));
            ASSERT_TRUE(gr) << "key " << key_for(i) << " missing after reopen";
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(
                                           gr->value.data()),
                                       gr->value.size()),
                      big_value(i))
                << "LWW resolution wrong under parallel fold for " << key_for(i);
        }
        (*c)->close();
    }
}

// X1:迭代器存活期间调用 close() 不得 UAF。close() 会 reset keydir_（经
// registry 递减引用计数释放 KeyDir），而 CaskIter 内部的 IterHandle 持
// KeyDir* 裸指针；迭代器在 close() 后析构（release()→BarrierGuard 锁
// KeyDir mutex）若 KeyDir 已释放即 heap-use-after-free。CaskIter 现 pin
// 一份 KeyDir shared_ptr 兜住其生命周期。本测试在 TSan 下守护该契约。
TEST_F(CrashRecoveryTest, IteratorAliveAcrossCloseNoUaf) {
    constexpr int kN = 20;
    {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts);
        ASSERT_TRUE(c) << c.error().detail;
        for (int i = 0; i < kN; ++i) {
            ASSERT_TRUE((*c)->put(bytes(key_for(i)), bytes(value_for(i)),
                                  static_cast<std::uint32_t>(3000 + i)));
        }
        (*c)->close();
    }

    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts);
    ASSERT_TRUE(c) << c.error().detail;

    auto it = (*c)->make_iter();
    auto start = it->start();
    ASSERT_TRUE(start);
    // 部分迭代后即 close()——迭代器仍存活，随后离开作用域析构。
    auto first = it->next();
    ASSERT_TRUE(first);

    (*c)->close();  // reset keydir_ 于迭代器存活期间

    // it 在此后析构 → release() 锁 KeyDir mutex。pin 生效时不应 UAF/崩溃。
    it.reset();
    SUCCEED();
}

// T7:X1 显式 release 路径回归。IteratorAliveAcrossCloseNoUaf 走隐式析构
// （it.reset()→~CaskIter→release）；本例在 close() 后**显式** it->release()，
// 验证显式释放序（iter_->release → iter_.reset → keydir_pin_.reset）下 KeyDir
// 仍存活，且 release() 幂等（二次调用不崩）。
TEST_F(CrashRecoveryTest, IteratorExplicitReleaseAfterCloseNoUaf) {
    {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts);
        ASSERT_TRUE(c) << c.error().detail;
        for (int i = 0; i < 20; ++i) {
            ASSERT_TRUE((*c)->put(bytes(key_for(i)), bytes(value_for(i)),
                                  static_cast<std::uint32_t>(3000 + i)));
        }
        (*c)->close();
    }

    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts);
    ASSERT_TRUE(c) << c.error().detail;

    auto it = (*c)->make_iter();
    ASSERT_TRUE(it->start());
    ASSERT_TRUE(it->next());

    (*c)->close();    // KeyDir 经 pin 存活
    it->release();    // 显式 release：锁已释放出 registry 的 KeyDir mutex
    it->release();    // 幂等：二次 release 不得崩
    it.reset();       // ~CaskIter 再走一次 release（已空，no-op）
    SUCCEED();
}

// T8:X1 多 iterator 交错 release → 仅最后一个（keyfolders_ fetch_sub 返回 1）
// 触发 pending 应用 + MultiEntry 折叠；全程 KeyDir 经多 pin 在 close() 后存活。
// 真正要守护的是 KeyDir 内部协调（折叠时机 + 活 iterator 一致快照），非
// shared_ptr 计数本身。需 TSan 验证折叠期无 race。
TEST_F(CrashRecoveryTest, MultiIteratorInterleavedReleaseAfterClose) {
    auto str = [](const std::vector<std::byte>& v) {
        return std::string(reinterpret_cast<const char*>(v.data()), v.size());
    };

    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts);
    ASSERT_TRUE(c) << c.error().detail;
    auto& cask = **c;

    constexpr int kN = 5;
    for (int i = 0; i < kN; ++i) {
        ASSERT_TRUE(cask.put(bytes(key_for(i)), bytes(value_for(i)),
                             static_cast<std::uint32_t>(4000 + i)));
    }

    // it1 先 start；之后在 fold 态 overwrite k0 → SingleEntry 升级成 MultiEntry
    // （旧 revision 留给 it1，新 revision 给后启 iterator）。
    auto it1 = cask.make_iter();
    ASSERT_TRUE(it1->start());
    const std::string k0_new = "k0_NEW_REVISION";
    ASSERT_TRUE(cask.put(bytes(key_for(0)), bytes(k0_new), 4100));

    auto it2 = cask.make_iter();
    ASSERT_TRUE(it2->start());
    auto it3 = cask.make_iter();
    ASSERT_TRUE(it3->start());

    cask.close();  // keyfolders_==3，snapshot 跳过；KeyDir 经三 pin 存活

    // 交错 release：it1、it3 均非最后一个 → 不折叠。
    it1->release();
    it3->release();

    // it2 仍活（keyfolders_==1）：drain 全部 entry，应见 k0..k4 且 k0=新 revision。
    std::map<std::string, std::string> got;
    while (true) {
        auto e = it2->next();
        ASSERT_TRUE(e) << "post-close next() 失败：" << e.error().detail;
        if (!e->has_value()) break;
        got[str((*e)->key)] = str((*e)->value);
    }
    EXPECT_EQ(got.size(), static_cast<std::size_t>(kN));
    EXPECT_EQ(got[key_for(0)], k0_new)
        << "后启 iterator 应看到 MultiEntry 链头的新 revision";

    it2->release();  // keyfolders_ 1→0 → 最后一个 → 触发折叠（pinned KeyDir 上）
    SUCCEED();
}

}  // namespace
