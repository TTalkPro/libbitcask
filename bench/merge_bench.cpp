// B1:Merge 吞吐基准。度量 S2（merge 输出批量 pwrite）+ tier-1 ④。
// 写两轮（旧值→覆写活值）制造一半死记录，计时 merge() 把活记录搬到新文件，
// 报 records/s + MB/s。setup/teardown 用 Pause/ResumeTiming 排除在计时外。
//
// Run:  ./bitcask_bench --benchmark_filter=Merge

#include <benchmark/benchmark.h>

#include <unistd.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "bitcask/cask.hpp"
#include <bitcask/index_registry.hpp>

namespace fs = std::filesystem;

namespace {

// S6-P0-pre：open() 现强制非空 registry。测试/bench 共享一个进程内 registry——
// 各用例用唯一目录名，互不冲突；同用例内 open→close→reopen 经 refcount 归零
// 重新从盘加载，与旧 nullptr 行为等价。
inline bitcask::oki::IndexRegistry& test_registry() {
    static bitcask::oki::IndexRegistry reg;
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

// S43-10：按扩展/文件名前缀统计落盘字节，避免把锁文件和 manifest 元数据
// 混入 merge 数据账。hint 预期恒为 0，用来锁定「新库不再生成 hint」。
std::uintmax_t disk_bytes(const fs::path& dir,
                          const std::function<bool(const std::string&)>& match) {
    std::uintmax_t sum = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const auto name = entry.path().filename().string();
        if (entry.is_regular_file(ec) && match(name)) {
            sum += entry.file_size(ec);
        }
    }
    return sum;
}

std::size_t disk_file_count(
    const fs::path& dir,
    const std::function<bool(const std::string&)>& match) {
    std::size_t count = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const auto name = entry.path().filename().string();
        if (entry.is_regular_file(ec) && match(name)) ++count;
    }
    return count;
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

// S43-10 磁盘核算：checkpoint 固化 OKI 后，记录 merge 前后的 data/OKI/hint
// 物理字节。写放大按「merge 输出增量 + OKI run 增量」除以搬移行的 value
// 字节计算；不含由同一进程打开产生的锁文件与 manifest 小元数据。
static void BM_Merge_DiskAccounting(benchmark::State& state) {
    constexpr std::size_t kRecords = 20000;
    constexpr std::size_t kValSize = 100;
    const std::string value(kValSize, 'v');

    state.PauseTiming();
    TempDir td;
    bitcask::CaskOptions opts;
    opts.read_write = true;
    opts.max_file_size = 256 * 1024;
    auto c = bitcask::Cask::open(td.path(), opts, &test_registry());
    if (!c) {
        state.SkipWithError("open failed");
        return;
    }
    auto& cask = **c;
    for (std::size_t i = 0; i < kRecords; ++i) {
        const std::string k = "k" + std::to_string(i);
        (void)cask.put(as_bytes(k), as_bytes(value),
                       static_cast<std::uint32_t>(1000 + i));
    }
    for (std::size_t i = 0; i < kRecords; ++i) {
        const std::string k = "k" + std::to_string(i);
        (void)cask.put(as_bytes(k), as_bytes(value),
                       static_cast<std::uint32_t>(5000 + i));
    }
    if (!cask.checkpoint()) {
        state.SkipWithError("pre-merge checkpoint failed");
        return;
    }

    const auto is_data = [](const std::string& name) {
        return name.ends_with(".bitcask.data");
    };
    const auto is_oki_run = [](const std::string& name) {
        return name.starts_with("kv.oki.seg-");
    };
    const auto is_hint = [](const std::string& name) {
        return name.ends_with(".hint");
    };
    const auto is_manifest = [](const std::string& name) {
        return name == "kv.oki.manifest";
    };
    const std::uintmax_t data_before = disk_bytes(td.path(), is_data);
    const std::uintmax_t oki_before = disk_bytes(td.path(), is_oki_run);
    const std::uintmax_t manifest_before = disk_bytes(td.path(), is_manifest);
    const auto needs = cask.needs_merge(/*now_sec*/ 9000);
    if (!needs.needs) {
        state.SkipWithError("no merge candidates");
        return;
    }
    state.ResumeTiming();

    auto result = cask.merge(needs.files, /*now_sec*/ 9000);

    state.PauseTiming();
    if (!result) {
        state.SkipWithError("merge failed");
        return;
    }
    cask.close();
    const std::uintmax_t data_after = disk_bytes(td.path(), is_data);
    const std::uintmax_t oki_after = disk_bytes(td.path(), is_oki_run);
    const std::uintmax_t manifest_after = disk_bytes(td.path(), is_manifest);
    const double data_delta = static_cast<double>(data_after > data_before
                                                     ? data_after - data_before
                                                     : 0);
    const double oki_delta = static_cast<double>(oki_after > oki_before
                                                     ? oki_after - oki_before
                                                     : 0);
    const double live_value_bytes =
        static_cast<double>(result->records_kept) *
        static_cast<double>(kValSize);
    state.counters["records_seen"] =
        static_cast<double>(result->records_seen);
    state.counters["records_kept"] =
        static_cast<double>(result->records_kept);
    state.counters["data_before_MB"] =
        static_cast<double>(data_before) / (1024.0 * 1024.0);
    state.counters["data_after_MB"] =
        static_cast<double>(data_after) / (1024.0 * 1024.0);
    state.counters["retired_data_MB"] =
        static_cast<double>(data_before > data_after
                                ? data_before - data_after
                                : 0) / (1024.0 * 1024.0);
    state.counters["oki_run_before_MB"] =
        static_cast<double>(oki_before) / (1024.0 * 1024.0);
    state.counters["oki_run_after_MB"] =
        static_cast<double>(oki_after) / (1024.0 * 1024.0);
    state.counters["oki_delta_MB"] = oki_delta / (1024.0 * 1024.0);
    state.counters["hint_before_files"] = static_cast<double>(
        disk_file_count(td.path(), is_hint));
    state.counters["hint_after_files"] = static_cast<double>(
        disk_file_count(td.path(), is_hint));
    state.counters["manifest_before_B"] =
        static_cast<double>(manifest_before);
    state.counters["manifest_after_B"] =
        static_cast<double>(manifest_after);
    state.counters["merge_output_bytes"] =
        static_cast<double>(result->bytes_written);
    state.counters["write_amplification"] =
        live_value_bytes > 0 ? (data_delta + oki_delta) / live_value_bytes
                             : 0.0;
    state.SetLabel("hint_files=0 records=" +
                   std::to_string(result->records_kept));
    state.ResumeTiming();
}
BENCHMARK(BM_Merge_DiskAccounting)
    ->Arg(20000)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);
