// B1:Merge 吞吐基准。度量 S2（merge 输出批量 pwrite）+ tier-1 ④。
// 写两轮（旧值→覆写活值）制造一半死记录，计时 merge() 把活记录搬到新文件，
// 报 records/s + MB/s。setup/teardown 用 Pause/ResumeTiming 排除在计时外。
//
// Run:  ./bitcask_bench --benchmark_filter=Merge

#include <benchmark/benchmark.h>

#include <unistd.h>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "bitcask/cask.hpp"
#include <bitcask/keydir_registry.hpp>

namespace fs = std::filesystem;

namespace {

// S6-P0-pre：open() 现强制非空 registry。测试/bench 共享一个进程内 registry——
// 各用例用唯一目录名，互不冲突；同用例内 open→close→reopen 经 refcount 归零
// 重新从盘加载，与旧 nullptr 行为等价。
inline bitcask::keydir::KeyDirRegistry& test_registry() {
    static bitcask::keydir::KeyDirRegistry reg;
    return reg;
}

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("bitcask_merge_bench_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path_);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path_, ec); }
    std::string path() const { return path_.string(); }
private:
    fs::path path_;
};

std::span<const std::byte> as_bytes(const std::string& s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

}  // namespace

static void BM_Merge_Throughput(benchmark::State& state) {
    const int kRecords = static_cast<int>(state.range(0));
    constexpr std::size_t kValSize = 100;
    const std::string value(kValSize, 'v');

    for (auto _ : state) {
        state.PauseTiming();
        TempDir td;
        bitcask::CaskOptions opts;
        opts.read_write = true;
        opts.max_file_size = 256 * 1024;  // 滚出多个 sealed 文件供 merge
        auto c = bitcask::Cask::open(td.path(), opts, &test_registry());
        if (!c) { state.SkipWithError("open failed"); break; }
        auto& cask = **c;
        // 第一轮旧值（将变死）+ 第二轮覆写为活值 → 约一半记录是死记录。
        for (int i = 0; i < kRecords; ++i) {
            const std::string k = "k" + std::to_string(i);
            (void)cask.put(as_bytes(k), as_bytes(value),
                           static_cast<std::uint32_t>(1000 + i));
        }
        for (int i = 0; i < kRecords; ++i) {
            const std::string k = "k" + std::to_string(i);
            (void)cask.put(as_bytes(k), as_bytes(value),
                           static_cast<std::uint32_t>(5000 + i));
        }
        state.ResumeTiming();

        auto mr = cask.merge({}, /*now_sec*/ 9000);  // 合并全部 eligible sealed 文件

        state.PauseTiming();
        if (!mr) { state.SkipWithError("merge failed"); break; }
        cask.close();
        state.ResumeTiming();
    }

    // 活记录 ≈ kRecords（每 key 最新版本被搬迁）。
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(kRecords));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(kRecords) *
                            static_cast<std::int64_t>(kValSize));
}
BENCHMARK(BM_Merge_Throughput)
    ->Arg(20000)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3);
