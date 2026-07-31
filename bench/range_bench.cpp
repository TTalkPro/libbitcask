// S33-1：Range/prefix 扫描基线基准——OKI（有序 key 索引）落地前的现状锚点。
//
// 现状：keydir 是哈希表，CaskIter::start(key_prefix) 是 O(全表) 过滤
// （cask.hpp 头注释自承：省的是 value 读取与跨界拷贝，扫描本身仍全表）。
// 证据形态：固定总 key 数、把选择性从 1/16 收窄到 1/256，耗时应基本持平
// （匹配数降 16 倍而时间不降 = 全表扫描）。
// S33-5 的 make_range_iter 落地后在同参数下对比，预期改善一个数量级以上，
// 且耗时随选择性线性下降。
//
// 另含 BM_KeyDir_MemProbe：S33-1 内存估算探针的数字锚点（Level B 门禁数据
// 口径演示；真实决策数据须在生产规模负载上取）。

#include <benchmark/benchmark.h>

#include <unistd.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "bitcask/cask.hpp"
#include <bitcask/keydir_registry.hpp>

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskOptions;

namespace {

inline bitcask::keydir::KeyDirRegistry& test_registry() {
    static bitcask::keydir::KeyDirRegistry reg;
    return reg;
}

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("bitcask_range_bench_" + std::to_string(::getpid()) + "_" +
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

CaskOptions rw_opts() {
    CaskOptions o;
    o.read_write = true;
    return o;
}

// key 形态 "gNNN:kMMMMMM"——NNN 是前缀组号。真实负载常见 `prefix:id`，
// 也是 OKI run 前缀差分压缩的受益形态。
std::string mk_key(int group, int i) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "g%03d:k%06d", group, i);
    return buf;
}

}  // namespace

// -----------------------------------------------------------------------------
// Arg0 = 总 key 数，Arg1 = 前缀组数（选择性 = 1/组数）。
// 计时一次「扫某一个前缀组」：CaskIter::start(key_prefix) + 排空 next()。
// -----------------------------------------------------------------------------
static void BM_Cask_PrefixScan_Baseline(benchmark::State& state) {
    const int total  = static_cast<int>(state.range(0));
    const int groups = static_cast<int>(state.range(1));
    TempDir td;
    auto c = Cask::open(td.path(), rw_opts(), &test_registry());
    if (!c) { state.SkipWithError("Cask::open failed"); return; }
    auto& cask = **c;

    const std::string value(64, 'v');
    for (int i = 0; i < total; ++i) {
        if (!cask.put(as_bytes(mk_key(i % groups, i)), as_bytes(value))) {
            state.SkipWithError("populate put failed");
            return;
        }
    }

    // 组号固定取 0，格式与 mk_key 一致（g000:）。
    const std::string p = "g000:";
    std::uint64_t matched = 0;
    for (auto _ : state) {
        auto it = cask.make_iter();
        auto st = it->start(-1, -1, 0, false, as_bytes(p));
        if (!st) { state.SkipWithError("iter start failed"); return; }
        std::uint64_t n = 0;
        while (true) {
            auto e = it->next();
            if (!e) { state.SkipWithError("iter next failed"); return; }
            if (!e->has_value()) break;
            ++n;
        }
        it->release();
        matched = n;
        benchmark::DoNotOptimize(n);
    }
    // items = 匹配条数（对比 OKI 版时口径一致）；全表规模看 label。
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(matched));
    state.SetLabel("matched=" + std::to_string(matched) + "/" +
                   std::to_string(total));
    cask.close();
}
BENCHMARK(BM_Cask_PrefixScan_Baseline)
    ->Args({100000, 16})   // 选择性 1/16：~6250 条命中
    ->Args({100000, 256})  // 选择性 1/256：~390 条命中——时间若与上行持平即 O(全表) 实锤
    ->Unit(benchmark::kMillisecond);

// -----------------------------------------------------------------------------
// S33-5：OKI 有序 range 扫描——与上面的 O(全表) 基线同参数对比。
// checkpoint() 先行使 OKI 有 run 可归并（写后未 flush 的行走 memdelta 路径
// 同样正确，但基准锚定「已 flush」的稳态形态）。
// -----------------------------------------------------------------------------
static void BM_Cask_RangeScan_OKI(benchmark::State& state) {
    const int total  = static_cast<int>(state.range(0));
    const int groups = static_cast<int>(state.range(1));
    TempDir td;
    auto c = Cask::open(td.path(), rw_opts(), &test_registry());
    if (!c) { state.SkipWithError("Cask::open failed"); return; }
    auto& cask = **c;

    const std::string value(64, 'v');
    for (int i = 0; i < total; ++i) {
        if (!cask.put(as_bytes(mk_key(i % groups, i)), as_bytes(value))) {
            state.SkipWithError("populate put failed");
            return;
        }
    }
    if (!cask.checkpoint()) { state.SkipWithError("checkpoint failed"); return; }

    const std::string lo = "g000:";
    const std::string hi = "g000;";  // ':' + 1
    std::uint64_t matched = 0;
    for (auto _ : state) {
        bitcask::RangeOptions ro;
        ro.lo = as_bytes(lo);
        ro.hi = as_bytes(hi);
        auto it = cask.make_range_iter(ro);
        if (!it) { state.SkipWithError("make_range_iter failed"); return; }
        std::uint64_t n = 0;
        while (true) {
            auto e = (*it)->next();
            if (!e) { state.SkipWithError("range next failed"); return; }
            if (!e->has_value()) break;
            ++n;
        }
        matched = n;
        benchmark::DoNotOptimize(n);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(matched));
    state.SetLabel("matched=" + std::to_string(matched) + "/" +
                   std::to_string(total));
    cask.close();
}
BENCHMARK(BM_Cask_RangeScan_OKI)
    ->Args({100000, 16})   // 与基线同参：命中 ~6250
    ->Args({100000, 256})  // 命中 ~390——时间应随选择性线性下降（对照基线持平）
    ->Unit(benchmark::kMillisecond);

// -----------------------------------------------------------------------------
// S33-1 内存估算探针锚点：填 Arg0 个 key 后跑一次 key_length_histogram()，
// 把估算字节数写进 counter（本身也顺带量了探针的遍历耗时）。
// -----------------------------------------------------------------------------
static void BM_KeyDir_MemProbe(benchmark::State& state) {
    const int total = static_cast<int>(state.range(0));
    TempDir td;
    auto c = Cask::open(td.path(), rw_opts(), &test_registry());
    if (!c) { state.SkipWithError("Cask::open failed"); return; }
    auto& cask = **c;

    const std::string value(64, 'v');
    for (int i = 0; i < total; ++i) {
        if (!cask.put(as_bytes(mk_key(i % 16, i)), as_bytes(value))) {
            state.SkipWithError("populate put failed");
            return;
        }
    }

    bitcask::keydir::KeyDir::KeyLenHistogram h;
    for (auto _ : state) {
        h = cask.keydir().key_length_histogram();
        benchmark::DoNotOptimize(h);
    }
    state.counters["est_MB"] =
        static_cast<double>(h.estimated_bytes) / (1024.0 * 1024.0);
    state.counters["B_per_key"] =
        h.total > 0 ? static_cast<double>(h.estimated_bytes) /
                          static_cast<double>(h.total)
                    : 0.0;
    cask.close();
}
BENCHMARK(BM_KeyDir_MemProbe)->Arg(100000)->Unit(benchmark::kMillisecond);
