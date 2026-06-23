// B4:Checkpoint 保存/加载基准。度量 S4（keydir 快照精确 reserve，消除 GB 级
// buffer 的 realloc churn）。save 计 serialize+落盘，load 计读盘+反序列化重建。
// 报 ms/op + keys/s。
//
// Run:  ./bitcask_bench --benchmark_filter=Checkpoint

#include <benchmark/benchmark.h>

#include <unistd.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "bitcask/keydir.hpp"

namespace fs = std::filesystem;
using bitcask::keydir::KeyDir;

namespace {

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("bitcask_ckpt_bench_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path_);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path_, ec); }
    std::string path() const { return path_.string(); }
private:
    fs::path path_;
};

void populate(KeyDir& kd, int n) {
    for (int i = 0; i < n; ++i) {
        kd.put("key_" + std::to_string(i), 1, 100,
               static_cast<std::uint64_t>(i), 1, 0, /*newest*/ false, 0, 0);
    }
}

const std::vector<std::pair<std::uint32_t, std::uint64_t>> kWatermarks{{1, 0}};

}  // namespace

static void BM_Checkpoint_KeydirSave(benchmark::State& state) {
    const int kKeys = static_cast<int>(state.range(0));
    KeyDir kd;
    populate(kd, kKeys);
    TempDir td;
    const std::string path = td.path() + "/kv.keydir.ckpt";

    for (auto _ : state) {
        if (!kd.save_snapshot(path, kWatermarks)) {
            state.SkipWithError("save_snapshot failed");
            break;
        }
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(kKeys));
}
BENCHMARK(BM_Checkpoint_KeydirSave)->Arg(100000)->Unit(benchmark::kMillisecond);

static void BM_Checkpoint_KeydirLoad(benchmark::State& state) {
    const int kKeys = static_cast<int>(state.range(0));
    KeyDir src;
    populate(src, kKeys);
    TempDir td;
    const std::string path = td.path() + "/kv.keydir.ckpt";
    if (!src.save_snapshot(path, kWatermarks)) {
        state.SkipWithError("setup save_snapshot failed");
        return;
    }

    for (auto _ : state) {
        KeyDir fresh;  // load_snapshot 仅限全新 KeyDir
        auto wms = fresh.load_snapshot(path);
        benchmark::DoNotOptimize(wms);
        if (!wms) {
            state.SkipWithError("load_snapshot failed");
            break;
        }
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(kKeys));
}
BENCHMARK(BM_Checkpoint_KeydirLoad)->Arg(100000)->Unit(benchmark::kMillisecond);
