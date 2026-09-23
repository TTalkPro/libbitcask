// 6.6.0：进程级线程数上限（feedback 2026-09-23 / keel 转 coxswain）。
//
// 冻结状态是进程级且不可逆的，故本文件独占一个可执行文件、全部断言放进
// 同一个 TEST（gtest_discover_tests 下 ctest 每条用例单独起进程，多条 TEST
// 的先后与同进程与否都不可依赖）。

#include <gtest/gtest.h>
#include "support/test_paths.hpp"

#include <bitcask/cask.hpp>
#include <bitcask/keydir_registry.hpp>
#include <bitcask/search_config.hpp>
#include <bitcask/thread_limits.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <tlhelp32.h>
#endif

namespace {

// 本进程当前线程数；平台不支持 → nullopt（数量断言跳过，语义断言照跑）。
std::optional<std::size_t> thread_count() {
#if defined(_WIN32)
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return std::nullopt;
    const DWORD pid = ::GetCurrentProcessId();
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    std::size_t n = 0;
    if (::Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) ++n;
        } while (::Thread32Next(snap, &te));
    }
    ::CloseHandle(snap);
    return n;
#elif defined(__linux__)
    std::error_code ec;
    std::size_t n = 0;
    for (auto it = std::filesystem::directory_iterator("/proc/self/task", ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        ++n;
    }
    if (ec) return std::nullopt;
    return n;
#else
    return std::nullopt;
#endif
}

}  // namespace

TEST(ThreadLimitsTest, SetBeforeFirstSearchOpenCapsIndexPoolThenFreezes) {
    using bitcask::ThreadLimits;
    namespace fs = std::filesystem;

    // ① 冻结前可反复设置，读回登记值。
    ASSERT_TRUE(bitcask::set_thread_limits({3, 3}));
    ASSERT_TRUE(bitcask::set_thread_limits({2, 2}));
    EXPECT_EQ(bitcask::thread_limits().index_workers, 2u);
    EXPECT_EQ(bitcask::thread_limits().search_slots, 2u);
    EXPECT_EQ(bitcask::resolve_thread_count(2), 2u);
    EXPECT_GE(bitcask::resolve_thread_count(0), 2u);

    const auto dir = bitcask::test::unique_tmpdir("bitcask_thread_limits");
    std::error_code ec;
    fs::remove_all(dir, ec);

    bitcask::keydir::KeyDirRegistry registry;
    bitcask::CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    bitcask::search::SearchLayerConfig sl_cfg;
    sl_cfg.analyzer_config.type = bitcask::text::AnalyzerType::Whitespace;
    opts.search_config = sl_cfg;

    const auto before = thread_count();
    auto c = bitcask::Cask::open(dir.string(), opts, &registry);
    ASSERT_TRUE(c) << c.error().detail;
    const auto after = thread_count();

    // ② 建池 = index_workers 条 map worker + 1 条 reducer。此前（缺省）是
    // hardware_concurrency + 1。给 TBB / OS 杂项留 2 条余量。
    if (before && after) {
        EXPECT_LE(*after - *before, 2u + 1u + 2u)
            << "before=" << *before << " after=" << *after;
    }

    // ③ 写 + 刷索引：池按上限工作（功能不受影响）。
    for (int i = 0; i < 200; ++i) {
        const std::string k = "k" + std::to_string(i);
        const std::string v = "hello world doc " + std::to_string(i);
        ASSERT_TRUE((*c)->put(std::as_bytes(std::span(k)),
                              std::as_bytes(std::span(v)), 1000));
    }
    (*c)->flush_index();
    auto r = (*c)->search_text("hello", 500);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->hits.size(), 200u);

    // ④ 已冻结：同值幂等成功，异值拒绝且不改登记值。
    EXPECT_TRUE(bitcask::set_thread_limits({2, 2}));
    EXPECT_FALSE(bitcask::set_thread_limits({4, 2}));
    EXPECT_FALSE(bitcask::set_thread_limits({0, 0}));
    EXPECT_EQ(bitcask::thread_limits().index_workers, 2u);
    EXPECT_EQ(bitcask::thread_limits().search_slots, 2u);

    (*c)->close();
    fs::remove_all(dir, ec);
}
