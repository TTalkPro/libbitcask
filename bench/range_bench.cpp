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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
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

// S33-6：进程 RSS（KB）。与 gate_bench 的同名助手同款口径。
std::size_t read_rss_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.starts_with("VmRSS:")) {
            std::size_t val = 0;
            std::sscanf(line.c_str(), "VmRSS: %zu kB", &val);
            return val;
        }
    }
    return 0;
}

// 目录内 OKI run 文件总字节。
std::uintmax_t oki_run_bytes(const std::string& dir) {
    std::uintmax_t sum = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().filename().string().starts_with("kv.oki.seg-")) {
            sum += fs::file_size(e.path(), ec);
        }
    }
    return sum;
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
// S33-6：值预取（RangeOptions::prefetch）——与上面同参的惰性版对比。
// Arg2 = 预取批大小（0/1 = 关闭），Arg3 = 预取线程数（0 = 自动上限 4）。
// 预取并行化的是 value 的 pread + decode；tmpfs / 已在页缓存的负载下线程
// 创建成本可能吃掉收益，真实收益形态是「大窗口 + 冷值」。
// -----------------------------------------------------------------------------
static void BM_Cask_RangeScan_OKI_Prefetch(benchmark::State& state) {
    const int total    = static_cast<int>(state.range(0));
    const int groups   = static_cast<int>(state.range(1));
    const auto prefetch = static_cast<std::size_t>(state.range(2));
    const auto threads  = static_cast<std::size_t>(state.range(3));
    TempDir td;
    auto c = Cask::open(td.path(), rw_opts(), &test_registry());
    if (!c) { state.SkipWithError("Cask::open failed"); return; }
    auto& cask = **c;

    // 值给大一点（1KiB）——预取的收益全在值读取上，64B 的值读取占比太低。
    const std::string value(1024, 'v');
    for (int i = 0; i < total; ++i) {
        if (!cask.put(as_bytes(mk_key(i % groups, i)), as_bytes(value))) {
            state.SkipWithError("populate put failed");
            return;
        }
    }
    if (!cask.checkpoint()) { state.SkipWithError("checkpoint failed"); return; }

    const std::string lo = "g000:";
    const std::string hi = "g000;";
    std::uint64_t matched = 0;
    for (auto _ : state) {
        bitcask::RangeOptions ro;
        ro.lo = as_bytes(lo);
        ro.hi = as_bytes(hi);
        ro.prefetch = prefetch;
        ro.prefetch_threads = threads;
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
    state.SetLabel("matched=" + std::to_string(matched) +
                   " prefetch=" + std::to_string(prefetch) +
                   " threads=" + std::to_string(threads));
    cask.close();
}
BENCHMARK(BM_Cask_RangeScan_OKI_Prefetch)
    ->Args({100000, 16, 0, 0})     // 惰性基准（同二进制内的对照）
    ->Args({100000, 16, 64, 0})    // 批 64 / 自动线程
    ->Args({100000, 16, 256, 4})   // 批 256 / 4 线程
    ->Args({100000, 256, 0, 0})    // 小窗口：惰性
    ->Args({100000, 256, 64, 0})   // 小窗口：预取（线程成本可能反超）
    ->Unit(benchmark::kMillisecond);

// -----------------------------------------------------------------------------
// S33-6：OKI 内存探针（S33-1 keydir 探针的扩展项）——三个阶段的口径：
//   1) memdelta：Arg0 个 key 写入后未 flush 时的行数/字节 + RSS 增量；
//   2) flush 后：run 文件字节 + 每 key 盘上字节（前缀差分的压缩效果）；
//   3) 全量重建：删 manifest 后重开触发重建，采样线程取 RSS 峰值。
// 计时段只含「重开（含重建）」——即重建耗时；内存数字进 counters。
// -----------------------------------------------------------------------------
static void BM_Oki_MemProbe(benchmark::State& state) {
    const int total = static_cast<int>(state.range(0));
    TempDir td;
    const std::string value(64, 'v');

    double delta_rows = 0, delta_bytes = 0, memdelta_rss_mb = 0;
    double run_bytes = 0, run_count = 0;
    double rebuild_peak_mb = 0;

    {
        auto c = Cask::open(td.path(), rw_opts(), &test_registry());
        if (!c) { state.SkipWithError("Cask::open failed"); return; }
        auto& cask = **c;
        const std::size_t rss0 = read_rss_kb();
        for (int i = 0; i < total; ++i) {
            if (!cask.put(as_bytes(mk_key(i % 16, i)), as_bytes(value))) {
                state.SkipWithError("populate put failed");
                return;
            }
        }
        // 阶段 1：memdelta 驻留（尚未 flush——阈值是 1M 行 / 64MiB）。
        auto& oki = cask.keydir().oki();
        delta_rows  = static_cast<double>(oki.delta_rows());
        delta_bytes = static_cast<double>(oki.delta_bytes());
        const std::size_t rss1 = read_rss_kb();
        memdelta_rss_mb =
            static_cast<double>(rss1 > rss0 ? rss1 - rss0 : 0) / 1024.0;

        // 阶段 2：checkpoint 搭车 flush → run 落盘。
        if (!cask.checkpoint()) { state.SkipWithError("checkpoint failed"); return; }
        run_count = static_cast<double>(oki.run_count());
        run_bytes = static_cast<double>(oki_run_bytes(td.path()));
        cask.close();
    }

    // 阶段 3：删 manifest → 重开触发全量重建；后台采样 RSS 峰值。
    for (auto _ : state) {
        state.PauseTiming();
        std::error_code ec;
        fs::remove(fs::path(td.path()) / "kv.oki.manifest", ec);
        const std::size_t rss_before = read_rss_kb();
        std::atomic<std::size_t> peak{rss_before};
        std::atomic<bool> stop{false};
        std::thread sampler([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const std::size_t cur = read_rss_kb();
                std::size_t old = peak.load(std::memory_order_relaxed);
                while (cur > old &&
                       !peak.compare_exchange_weak(old, cur,
                                                   std::memory_order_relaxed)) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        state.ResumeTiming();

        auto c = Cask::open(td.path(), rw_opts(), &test_registry());

        state.PauseTiming();
        stop.store(true);
        sampler.join();
        if (!c) { state.SkipWithError("reopen failed"); return; }
        rebuild_peak_mb =
            static_cast<double>(peak.load() > rss_before
                                    ? peak.load() - rss_before
                                    : 0) / 1024.0;
        (*c)->close();
        state.ResumeTiming();
    }

    state.counters["memdelta_rows"] = delta_rows;
    state.counters["memdelta_MB"] = delta_bytes / (1024.0 * 1024.0);
    state.counters["memdelta_rss_MB"] = memdelta_rss_mb;
    state.counters["run_count"] = run_count;
    state.counters["run_MB"] = run_bytes / (1024.0 * 1024.0);
    state.counters["run_B_per_key"] =
        total > 0 ? run_bytes / static_cast<double>(total) : 0.0;
    state.counters["rebuild_peak_rss_MB"] = rebuild_peak_mb;
}
BENCHMARK(BM_Oki_MemProbe)->Arg(100000)->Unit(benchmark::kMillisecond);

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

// ============================================================================
// S36-3：冷/热 get 锚点（设计 §2 预算门：热 ≤3%、冷 P99 ≤300µs SSD；
// 本机 /tmp = tmpfs，另立 tmpfs 锚点）。
// ============================================================================

namespace {

// 共享装配：100k key（"g%06d"，11B 内）× 64B 值 → checkpoint 固化 v2 run。
// 返回打开的 Cask；caller 决定是否逐出。
std::unique_ptr<Cask> setup_get_bench_cask(const TempDir& td, int nkeys) {
    CaskOptions o;
    o.read_write = true;
    auto c = Cask::open(td.path(), o, &test_registry());
    if (!c) return nullptr;
    const std::string value(64, 'v');
    for (int i = 0; i < nkeys; ++i) {
        char kb[16];
        std::snprintf(kb, sizeof(kb), "g%06d", i);
        if (!(*c)->put(as_bytes(std::string(kb)), as_bytes(value), 1000)) {
            return nullptr;
        }
    }
    if (!(*c)->checkpoint()) return nullptr;  // OKI flush → v2 run
    return std::move(*c);
}

}  // namespace

// （热 get 锚点复用 cask_bench.cpp 既有 BM_Cask_Get_Hot(_View)——S36-3
// 回归门 ≤3% 以它 A/B。）

// 冷 get：全部 key 逐出，get 走组合视图（memdelta miss → run bloom/稀疏
// 索引二分 → 块读 → 值 pread）。轮转 100k key ⟹ 4096 槽频度门指纹恒被
// 冲刷 → 几乎零回填，哈希持续冷（这正是门的设计行为：扫描型负载不污染
// 缓存）。arg1: 0 = 块缓存关（真·每次 2 pread），1 = 默认 256MB 块缓存
// （100k×64B 库的 run 块全驻留 → 稳态块零 IO，只剩值 pread）。
// counters: p50/p99（µs，样本 = 每次 get 的墙钟）。
static void BM_Cask_Get_ColdOki(benchmark::State& state) {
    const int nkeys = static_cast<int>(state.range(0));
    const bool block_cache = state.range(1) != 0;
    TempDir td;
    auto c = setup_get_bench_cask(td, nkeys);
    if (!c) { state.SkipWithError("setup failed"); return; }
    auto& kd = c->keydir();
    kd.oki().enable_point_query();  // Level B 点查（不开影子——bench 纯净）
    kd.oki().reset_block_cache_capacity(block_cache ? (256u << 20) : 0);
    for (int i = 0; i < nkeys; ++i) {
        char kb[16];
        std::snprintf(kb, sizeof(kb), "g%06d", i);
        if (!kd.evict(std::string_view(kb))) {
            state.SkipWithError("evict failed");
            return;
        }
    }

    std::vector<double> samples_us;
    samples_us.reserve(1u << 20);
    std::size_t i = 0;
    for (auto _ : state) {
        char kb[16];
        std::snprintf(kb, sizeof(kb), "g%06zu",
                      i++ % static_cast<std::size_t>(nkeys));
        const auto t0 = std::chrono::steady_clock::now();
        auto g = c->get(as_bytes(std::string(kb)));
        const auto t1 = std::chrono::steady_clock::now();
        if (!g) { state.SkipWithError("cold get failed"); break; }
        benchmark::DoNotOptimize(g->value.data());
        samples_us.push_back(
            std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    if (!samples_us.empty()) {
        std::sort(samples_us.begin(), samples_us.end());
        state.counters["p50_us"] = samples_us[samples_us.size() / 2];
        state.counters["p99_us"] = samples_us[samples_us.size() * 99 / 100];
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    c->close();
}
BENCHMARK(BM_Cask_Get_ColdOki)
    ->Args({100000, 0})
    ->Args({100000, 1});
