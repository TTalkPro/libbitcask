// 跨进程隔离测试（exec-self 骨架，S37-2）——两类缺口：
//
// W1（TODO C2）：核心路径的 fd 必须带 O_CLOEXEC。宿主 fork+exec 子进程时，
//   不带 cloexec 的 data/hint/write.lock/ckpt fd 会被子进程继承：拖住已删除
//   文件的磁盘空间、子进程能读到库内容、锁 fd 的生命期被子进程延长。
// B3（TODO B3）：写锁的两进程争用路径此前零覆盖（S37-5 的 PID 复用 bug 恰是
//   该形态抓到的）。这里用真实的第二个进程争锁，而非进程内模拟。

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/keydir_registry.hpp>

#include "support/crash_child.hpp"
#include "support/test_paths.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using bitcask::test::crash_exit;

namespace {

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskError;
using bitcask::CaskOptions;

inline bitcask::keydir::KeyDirRegistry& test_registry() {
    static bitcask::keydir::KeyDirRegistry reg;
    return reg;
}

std::span<const std::byte> as_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

// 子进程退出码约定（父进程据此断言）。
constexpr int kNoLeak = 0;
constexpr int kLeakFound = 3;
constexpr int kGotWriteLocked = 10;
constexpr int kOpenedOk = 11;
constexpr int kOtherError = 12;

}  // namespace

// 子进程：列出本进程继承到的、指向库目录内的 fd。exec 之后仍存在的 fd
// 只可能是父进程没带 cloexec 的那些。按 (st_dev, st_ino) 比对而非读
// /proc/self/fd 符号链接——后者仅 Linux 有（FreeBSD 的 procfs 没有 fd 目录，
// /dev/fd 不挂 fdescfs 时只有 0-2），这样探针在所有 POSIX 平台都真跑。
BITCASK_CRASH_SCENARIO(cloexec_probe) {
    std::set<std::pair<dev_t, ino_t>> inside;
    const auto add = [&](const fs::path& p) {
        struct stat st{};
        if (::stat(p.c_str(), &st) == 0) inside.emplace(st.st_dev, st.st_ino);
    };
    add(dir);
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) add(e.path());

    const long open_max = ::sysconf(_SC_OPEN_MAX);
    const int fd_end =
        static_cast<int>(std::clamp<long>(open_max > 0 ? open_max : 1024, 64, 65536));
    bool leaked = false;
    for (int fd = 3; fd < fd_end; ++fd) {
        struct stat st{};
        if (::fstat(fd, &st) != 0) continue;
        if (inside.contains({st.st_dev, st.st_ino})) {
            std::fprintf(stderr, "[cloexec_probe] inherited fd %d (ino %llu)\n", fd,
                         static_cast<unsigned long long>(st.st_ino));
            leaked = true;
        }
    }
    crash_exit(leaked ? kLeakFound : kNoLeak);
}

// 子进程：以读写方式开同一目录（第二个写者）。成功后**不 close** 直接退出
// ——模拟持锁进程崩溃，留下 stale write.lock 供父进程验证接管。
BITCASK_CRASH_SCENARIO(lock_contend) {
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(dir, opts, &test_registry());
    if (c) crash_exit(kOpenedOk);
    if (c.error().kind == CaskError::kWriteLocked) crash_exit(kGotWriteLocked);
    std::fprintf(stderr, "[lock_contend] unexpected error: %s\n",
                 c.error().detail.c_str());
    crash_exit(kOtherError);
}

class ProcessIsolationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() /
               ("bitcask_prociso_" + std::to_string(bitcask::test::test_pid()) +
                "_" + info->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    fs::path dir_;
};

// 探针自检：父进程故意开一个**不带** cloexec 的 fd，子进程必须报告泄漏
// ——否则下一个用例的「零泄漏」可能只是探针失效。
TEST_F(ProcessIsolationTest, CloexecProbeDetectsDeliberateLeak) {
    const auto sentinel = (dir_ / "sentinel").string();
    const int fd = ::open(sentinel.c_str(), O_RDWR | O_CREAT, 0600);  // 无 O_CLOEXEC
    ASSERT_GE(fd, 0);
    const int rc = bitcask::test::spawn_crash_child("cloexec_probe", dir_.string());
    ::close(fd);
    EXPECT_EQ(rc, kLeakFound);
}

// W1：开着一个带检索的读写库（data / hint / write.lock / ckpt / 段文件都
// 已存在并持有句柄）时 spawn 子进程，子进程不得继承其中任何一个。
TEST_F(ProcessIsolationTest, LibraryFdsAreNotInheritedAcrossExec) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    opts.search_config = bitcask::search::SearchLayerConfig{};
    auto c = Cask::open(dir_.string(), opts, &test_registry());
    ASSERT_TRUE(c.has_value());
    for (int i = 0; i < 200; ++i) {
        const std::string k = "key" + std::to_string(i);
        const std::string text = "alpha beta " + std::to_string(i);
        bitcask::DocInput doc;
        doc.text = as_bytes(text);
        ASSERT_TRUE((*c)->put_doc(as_bytes(k), doc,
                                    static_cast<std::uint32_t>(1000 + i)));
    }
    ASSERT_TRUE((*c)->checkpoint());
    ASSERT_TRUE((*c)->put(as_bytes("tail"), as_bytes("v"), 5000u));  // 活跃 data fd
    ASSERT_TRUE((*c)->search_text("alpha", 5));  // 段已打开

    EXPECT_EQ(bitcask::test::spawn_crash_child("cloexec_probe", dir_.string()),
              kNoLeak);
    (*c)->close();
}

// B3：父进程持写锁 → 第二个进程开读写必须得到 kWriteLocked（而非静默成功
// 双写者）；父进程关库后第二个进程可以拿到锁。
TEST_F(ProcessIsolationTest, SecondProcessGetsWriteLockedThenAcquiresAfterClose) {
    CaskOptions opts;
    opts.read_write = true;
    {
        auto c = Cask::open(dir_.string(), opts, &test_registry());
        ASSERT_TRUE(c.has_value());
        ASSERT_TRUE((*c)->put(as_bytes("k"), as_bytes("v"), 1u));
        EXPECT_EQ(bitcask::test::spawn_crash_child("lock_contend", dir_.string()),
                  kGotWriteLocked);
        (*c)->close();
    }
    EXPECT_EQ(bitcask::test::spawn_crash_child("lock_contend", dir_.string()),
              kOpenedOk);
}

// B3：持锁子进程崩溃（不 close，write.lock 残留）→ 父进程重开必须识别为
// stale 锁并接管，且数据完好。
TEST_F(ProcessIsolationTest, StaleLockFromCrashedProcessIsTakenOver) {
    CaskOptions opts;
    opts.read_write = true;
    {
        auto c = Cask::open(dir_.string(), opts, &test_registry());
        ASSERT_TRUE(c.has_value());
        ASSERT_TRUE((*c)->put(as_bytes("k"), as_bytes("v"), 1u));
        (*c)->close();
    }
    ASSERT_EQ(bitcask::test::spawn_crash_child("lock_contend", dir_.string()),
              kOpenedOk);  // 子进程拿锁后崩溃
    ASSERT_TRUE(fs::exists(dir_ / "bitcask.write.lock"))
        << "子进程应留下 stale 写锁——否则本用例没测到接管路径";

    auto c = Cask::open(dir_.string(), opts, &test_registry());
    ASSERT_TRUE(c.has_value()) << c.error().detail;
    auto v = (*c)->get(as_bytes("k"));
    ASSERT_TRUE(v.has_value());
    (*c)->close();
}
