// 6.7.0：bitcask_shutdown——拆除进程级后台线程（索引池 / Search 池 / TBB
// worker），使宿主（FFM / P/Invoke / dlopen）可安全卸载动态库。
//
// tbb::finalize 是进程级且影响后续 TBB 使用的状态变化，故本文件独占一个
// 可执行文件、全部断言放进同一个 TEST（同 thread_limits_test 的理由）。
//
// 只走 C API（链 bitcask_shared）：被测的就是 .so 的导出行为。tests/support
// 的 test_paths.hpp 依赖未导出的 io 符号，这里不用。

#include <gtest/gtest.h>

#include "bitcask_c.h"

#include <oneapi/tbb/parallel_for.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <tlhelp32.h>
#endif

namespace {

namespace fs = std::filesystem;

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
    for (auto it = fs::directory_iterator("/proc/self/task", ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        ++n;
    }
    if (ec) return std::nullopt;
    return n;
#else
    return std::nullopt;
#endif
}

// 线程 join 返回时内核可能尚未回收它，/proc/self/task 里会短暂残留——
// 线程数断言一律「等收敛」而非即时读数。
std::optional<std::size_t> settled_min_thread_count() {
    std::optional<std::size_t> best;
    for (int i = 0; i < 20; ++i) {
        const auto n = thread_count();
        if (!n) return std::nullopt;
        if (!best || *n < *best) best = n;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return best;
}

// 至多等 5 秒，直到线程数 ≤ limit；返回最后一次读数。
std::optional<std::size_t> wait_thread_count_at_most(std::size_t limit) {
    std::optional<std::size_t> n;
    for (int i = 0; i < 500; ++i) {
        n = thread_count();
        if (!n || *n <= limit) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return n;
}

fs::path fresh_dir(const char* tag) {
    std::random_device rd;
    auto p = fs::temp_directory_path() /
             (std::string("bitcask_shutdown_") + tag + "_" +
              std::to_string(rd()) + std::to_string(rd()));
    std::error_code ec;
    fs::remove_all(p, ec);
    return p;
}

// 开一个带 search 的库、写几条文档、跑一次批量查询（建 Search 池、进 TBB
// 并行段）。返回打开的句柄。
bitcask_t* open_and_exercise(const fs::path& dir) {
    bitcask_options_t opts;
    bitcask_options_init(&opts);
    opts.read_write = 1;
    opts.enable_search = 1;
    opts.analyzer_type = BITCASK_ANALYZER_WHITESPACE;

    bitcask_t* cask = nullptr;
    bitcask_fault_t fault{};
    const std::string d = dir.string();
    EXPECT_EQ(bitcask_open(d.c_str(), &opts, &cask, &fault), BITCASK_OK)
        << fault.detail;
    if (!cask) return nullptr;

    const char* docs[3] = {"hello world", "hello there", "foo bar"};
    const char* keys[3] = {"d0", "d1", "d2"};
    for (int i = 0; i < 3; ++i) {
        bitcask_doc_input_t doc;
        std::memset(&doc, 0, sizeof(doc));
        doc.text.data = docs[i];
        doc.text.size = std::strlen(docs[i]);
        bitcask_slice_t key = {keys[i], std::strlen(keys[i])};
        EXPECT_EQ(bitcask_put_doc(cask, key, &doc, 0, &fault), BITCASK_OK)
            << fault.detail;
    }
    bitcask_flush_index(cask);

    const char* queries[2] = {"hello", "foo"};
    bitcask_search_result_t** results = nullptr;
    EXPECT_EQ(bitcask_search_text_batch(cask, queries, 2, 10, &results, &fault),
              BITCASK_OK)
        << fault.detail;
    if (results) {
        EXPECT_EQ(results[0]->count, 2u);
        EXPECT_EQ(results[1]->count, 1u);
        bitcask_search_result_batch_free(results, 2);
    }
    return cask;
}

}  // namespace

TEST(CApiShutdownTest, JoinsBackgroundThreadsAndStaysUsable) {
    // sanitizer 运行时（TSan 等）在首次建线程时才起自己的后台线程——先建
    // 一条空线程把它引出来，再取基线，免得把它算成本库残留。
    std::thread([] {}).join();
    const auto baseline = settled_min_thread_count();

    // ① 什么都没初始化过：直接成功（不为 finalize 去建 TBB 运行时）。
    bitcask_fault_t fault{};
    ASSERT_EQ(bitcask_shutdown(&fault), BITCASK_OK) << fault.detail;
    ASSERT_EQ(bitcask_shutdown(nullptr), BITCASK_OK);  // fault 可为 NULL

    // ② 有库打开 → BUSY，且什么都不动（库照常可用）。
    const auto dir1 = fresh_dir("a");
    bitcask_t* cask = open_and_exercise(dir1);
    ASSERT_NE(cask, nullptr);
    if (baseline) {
        const auto running = thread_count();
        ASSERT_TRUE(running);
        EXPECT_GT(*running, *baseline) << "索引池 / TBB worker 应已起线程";
    }
    fault = {};
    EXPECT_EQ(bitcask_shutdown(&fault), BITCASK_ERR_BUSY);
    EXPECT_EQ(fault.code, BITCASK_ERR_BUSY);
    EXPECT_NE(std::strstr(fault.detail, "still open"), nullptr) << fault.detail;
    {
        bitcask_search_result_t* r = nullptr;
        ASSERT_EQ(bitcask_search_text(cask, "hello", 10, &r, &fault), BITCASK_OK)
            << fault.detail;
        EXPECT_EQ(r->count, 2u);
        bitcask_search_result_free(r);
    }
    bitcask_close(cask);

    // ③ 全部关闭后 shutdown 成功，后台线程全部退出、回到基线。
    fault = {};
    ASSERT_EQ(bitcask_shutdown(&fault), BITCASK_OK) << fault.detail;
    if (baseline) {
        const auto after = wait_thread_count_at_most(*baseline);
        ASSERT_TRUE(after);
        EXPECT_LE(*after, *baseline) << "shutdown 后不应残留后台线程";
    }

    // ④ shutdown 后库仍可用：再开（含重放已有数据）、查询、关闭、再 shutdown。
    cask = open_and_exercise(fresh_dir("b"));
    ASSERT_NE(cask, nullptr);
    bitcask_close(cask);
    {
        bitcask_options_t opts;
        bitcask_options_init(&opts);
        opts.read_write = 1;
        opts.enable_search = 1;
        opts.analyzer_type = BITCASK_ANALYZER_WHITESPACE;
        const std::string d = dir1.string();
        ASSERT_EQ(bitcask_open(d.c_str(), &opts, &cask, &fault), BITCASK_OK)
            << fault.detail;
        bitcask_search_result_t* r = nullptr;
        ASSERT_EQ(bitcask_search_text(cask, "foo", 10, &r, &fault), BITCASK_OK)
            << fault.detail;
        EXPECT_EQ(r->count, 1u);
        bitcask_search_result_free(r);
        bitcask_close(cask);
    }
    ASSERT_EQ(bitcask_shutdown(&fault), BITCASK_OK) << fault.detail;

    // ⑤ 另一条仍存活的线程持有 TBB 线程局部状态 → finalize 失败 → BUSY
    //（不卸载、库仍可用）；该线程退出后再调成功。模拟 JVM / .NET 线程池里
    // 跑过查询的常驻线程。
    std::mutex mu;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    std::thread parked([&] {
        oneapi::tbb::parallel_for(0, 64, [](int) {});
        std::unique_lock lk(mu);
        entered = true;
        cv.notify_all();
        cv.wait(lk, [&] { return release; });
    });
    {
        std::unique_lock lk(mu);
        cv.wait(lk, [&] { return entered; });
    }
    fault = {};
    EXPECT_EQ(bitcask_shutdown(&fault), BITCASK_ERR_BUSY);
    EXPECT_EQ(fault.code, BITCASK_ERR_BUSY);
    EXPECT_NE(std::strstr(fault.detail, "TBB"), nullptr) << fault.detail;
    {
        std::lock_guard lk(mu);
        release = true;
    }
    cv.notify_all();
    parked.join();
    fault = {};
    EXPECT_EQ(bitcask_shutdown(&fault), BITCASK_OK) << fault.detail;
    // parked 退出时它释放的是最后一份 TBB 引用，走的是 TBB 的**非阻塞**拆除：
    // worker 收到退出通知后异步离场（只跑 libtbb 自己的代码，不碍卸载
    // libbitcask）。
    if (baseline) {
        const auto after = wait_thread_count_at_most(*baseline);
        ASSERT_TRUE(after);
        EXPECT_LE(*after, *baseline);
    }

    std::error_code ec;
    fs::remove_all(dir1, ec);
}
