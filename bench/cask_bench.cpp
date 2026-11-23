// Cask end-to-end micro-benchmarks. These hit the data file on disk via
// pwrite / pread, so numbers depend on the underlying FS (typically tmpfs
// in /tmp). Useful for spotting regressions in the encode/decode + I/O
// path; not meant to compare against legacy bitcask without aligning the
// underlying storage layer first.

#include <benchmark/benchmark.h>

#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <utility>

#include "bitcask/cask.hpp"
#include <bitcask/index_registry.hpp>

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskOptions;

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
                ("bitcask_bench_" + std::to_string(::getpid()) + "_" +
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

}  // namespace

// -----------------------------------------------------------------------------
// Steady-state put. Pre-populates so we benchmark the overwrite path (most
// realistic — production workloads almost always overwrite existing keys).
// -----------------------------------------------------------------------------
static void BM_Cask_Put_Overwrite(benchmark::State& state) {
    TempDir td;
    auto c = Cask::open(td.path(), rw_opts(), &test_registry());
    if (!c) state.SkipWithError("Cask::open failed");
    auto& cask = **c;

    // S43-5：1024 是稳态热覆盖锚点；100k 保留同一个均匀写 gate 的
    // run 态 OKI probe 形态（每 put 一次，不是热哈希捷径）。
    const std::size_t kKeyspace = static_cast<std::size_t>(state.range(0));
    std::vector<std::string> keys;
    keys.reserve(static_cast<std::size_t>(kKeyspace));
    for (std::size_t i = 0; i < kKeyspace; ++i) {
        keys.push_back("k" + std::to_string(i));
    }

    const std::string value(128, 'v');  // 128-byte value, fixed
    for (const auto& k : keys) {
        auto r = cask.put(as_bytes(k), as_bytes(value));
        if (!r) state.SkipWithError("populate put failed");
    }

    std::mt19937 rng(0xCAFE);
    std::uniform_int_distribution<std::size_t> dist(0, kKeyspace - 1);

    for (auto _ : state) {
        auto& k = keys[static_cast<std::size_t>(dist(rng))];
        auto r = cask.put(as_bytes(k), as_bytes(value));
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(value.size()));
}
BENCHMARK(BM_Cask_Put_Overwrite)->Arg(1024)->Arg(100000);

// S43-10：bounded Zipf(s=1) 覆盖写。均匀 probe 已单独量化；这里记录
// 热点倾斜对端到端 put 的额外影响。CDF 用调和级数精确离散化，截断在
// 100k key；每次采样 O(log n)，避免把 benchmark 开销做成连续近似。
static void BM_Cask_Put_Zipfian(benchmark::State& state) {
    const std::size_t kKeyspace =
        static_cast<std::size_t>(state.range(0));
    TempDir td;
    auto c = Cask::open(td.path(), rw_opts(), &test_registry());
    if (!c) {
        state.SkipWithError("Cask::open failed");
        return;
    }
    auto& cask = **c;
    std::vector<std::string> keys;
    keys.reserve(static_cast<std::size_t>(kKeyspace));
    for (std::size_t i = 0; i < kKeyspace; ++i) {
        keys.push_back("k" + std::to_string(i));
    }
    const std::string value(128, 'v');
    for (const auto& k : keys) {
        if (!cask.put(as_bytes(k), as_bytes(value))) {
            state.SkipWithError("populate put failed");
            cask.close();
            return;
        }
    }

    std::vector<double> cdf;
    cdf.reserve(kKeyspace);
    double harmonic = 0.0;
    for (std::size_t rank = 1; rank <= kKeyspace; ++rank) {
        harmonic += 1.0 / static_cast<double>(rank);
        cdf.push_back(harmonic);
    }
    for (auto& x : cdf) x /= harmonic;
    cdf.back() = 1.0;

    std::mt19937 rng(0x21F3);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    for (auto _ : state) {
        const auto rank_it = std::upper_bound(
            cdf.begin(), cdf.end(), uniform(rng));
        const auto index = static_cast<std::size_t>(
            rank_it - cdf.begin());
        auto& k = keys[index < kKeyspace ? index : kKeyspace - 1];
        auto r = cask.put(as_bytes(k), as_bytes(value));
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(
        state.iterations() * static_cast<int64_t>(value.size()));
    cask.close();
}
BENCHMARK(BM_Cask_Put_Zipfian)->Arg(100000)->Unit(benchmark::kNanosecond);

// S43-5 性能口径：均匀覆盖写时每次 put 都会先做一次 OKI write probe。
// 这个基准把该 locate 从 pwrite/OKI/authority 应用中剥离出来，单独量化
// 100k 稳态 keyspace 的 run 态探测成本；端到端吞吐仍看上面的
// BM_Cask_Put_Overwrite。两者都不构成静默降级理由。
static void BM_Cask_WriteProbe_Uniform(benchmark::State& state) {
    const int nkeys = static_cast<int>(state.range(0));
    TempDir td;
    auto c = Cask::open(td.path(), rw_opts(), &test_registry());
    if (!c) { state.SkipWithError("Cask::open failed"); return; }
    auto& cask = **c;
    const std::string value(128, 'v');
    for (int i = 0; i < nkeys; ++i) {
        const std::string key = "k" + std::to_string(i);
        if (!cask.put(as_bytes(key), as_bytes(value))) {
            state.SkipWithError("populate put failed");
            cask.close();
            return;
        }
    }
    if (!cask.checkpoint()) {
        state.SkipWithError("oki checkpoint failed");
        cask.close();
        return;
    }

    std::size_t i = 0;
    std::size_t hits = 0;
    for (auto _ : state) {
        const std::string key = "k" + std::to_string(
            i++ % static_cast<std::size_t>(nkeys));
        const auto probe = cask.index_authority().state().probe_for_write(
            std::string_view(key.data(), key.size()));
        auto status = probe.status;
        auto offset = probe.loc.offset;
        benchmark::DoNotOptimize(status);
        benchmark::DoNotOptimize(offset);
        hits += probe.status ==
                bitcask::oki::OkiState::LocateStatus::kHit;
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["probe_hits"] = static_cast<double>(hits);
    state.counters["hit_ratio"] =
        state.iterations() > 0
            ? static_cast<double>(hits) / static_cast<double>(state.iterations())
            : 0.0;
    cask.close();
}
BENCHMARK(BM_Cask_WriteProbe_Uniform)->Arg(100000)->Unit(benchmark::kNanosecond);

// -----------------------------------------------------------------------------
// S11-W1：多线程并发写**同一** Cask handle 的吞吐。各线程写自己的 key 段。
// 本基准量化「多写者安全的代价」。**实测（6 核，2026-06-25）：聚合吞吐随线程数
// 不升反降**——
//   threads:1 ≈ 980k/s | 2 ≈ 200k/s | 4 ≈ 112k/s | 8 ≈ 46k/s
// 根因：put 临界区极短（~1µs：encode + pwrite(128B) + keydir），write_mu_ 串行化下，
// 多线程争锁的 **futex 唤醒/上下文切换开销（~µs 级）压过临界区本身** → 经典「短临界
// 区高争用」mutex 退化。这**不是** bug——它印证设计结论：
//   ① 单写线程吞吐不受 write_mu_ 影响（uncontended 锁 ~20ns，见 threads:1 ≈ 单写基线）；
//   ② **写扩展靠按目录分片多个 Cask 实例**（各自独立 WAL + 锁），而非往一个 handle
//      堆写线程（单 append WAL 本就串行，堆线程只增争用）；
//   ③ 多写**安全**（数据不坏，TSan 验证）——只是不该指望它提速。
// 混合读写负载里写是少数、读不争 write_mu_，本极端纯写争用不代表常态。
// -----------------------------------------------------------------------------
static TempDir* g_cw_dir = nullptr;
static std::unique_ptr<Cask> g_cw_cask;

static void BM_Cask_Put_Concurrent(benchmark::State& state) {
    if (state.thread_index() == 0) {
        g_cw_dir = new TempDir();
        auto c = Cask::open(g_cw_dir->path(), rw_opts(), &test_registry());
        if (!c) { state.SkipWithError("Cask::open failed"); return; }
        g_cw_cask = std::move(*c);
    }
    const int tid = state.thread_index();
    constexpr int kKeyspace = 1024;
    std::vector<std::string> keys;
    keys.reserve(static_cast<std::size_t>(kKeyspace));
    for (int i = 0; i < kKeyspace; ++i) {
        keys.push_back("t" + std::to_string(tid) + "_" + std::to_string(i));
    }
    const std::string value(128, 'v');
    // 注意：循环前**不**碰 g_cw_cask——google benchmark 仅在计时循环起点设线程
    // 屏障，循环前的代码各线程并发执行；只有 thread 0 在此前写 g_cw_cask。其余
    // 线程对 g_cw_cask 的访问全部落在计时循环内（屏障后，thread 0 的初始化已可见）。
    // 首轮为 insert、key 段绕回后转 overwrite——稳态即覆盖写。
    std::size_t idx = 0;
    for (auto _ : state) {
        auto& k = keys[idx++ & (kKeyspace - 1)];
        auto r = g_cw_cask->put(as_bytes(k), as_bytes(value));
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());

    if (state.thread_index() == 0) {
        g_cw_cask->close();
        g_cw_cask.reset();
        delete g_cw_dir;
        g_cw_dir = nullptr;
    }
}
// 1→8 写线程（6 核机重点看 1/2/4/8）；聚合吞吐 ~持平 = write_mu_ 串行如预期。
BENCHMARK(BM_Cask_Put_Concurrent)->ThreadRange(1, 8)->UseRealTime();

// -----------------------------------------------------------------------------
// S11-W4：parallel_scan 全表扫描随线程数的加速比。预填 5 万 key（128B），
// 计时 parallel_scan(n, fn)。被并行化的是读值的 keydir 查找 + pread + DocValue
// decode（页缓存热后为 CPU bound）；快照 key 串行。Arg = 工作线程数。
// -----------------------------------------------------------------------------
static void BM_Cask_ParallelScan(benchmark::State& state) {
    const std::size_t nthr = static_cast<std::size_t>(state.range(0));
    TempDir td;
    auto c = Cask::open(td.path(), rw_opts(), &test_registry());
    if (!c) { state.SkipWithError("Cask::open failed"); return; }
    auto& cask = **c;

    constexpr int kN = 50000;
    const std::string value(128, 'v');
    for (int i = 0; i < kN; ++i) {
        if (!cask.put(as_bytes("k" + std::to_string(i)), as_bytes(value))) {
            state.SkipWithError("populate put failed");
            break;
        }
    }

    std::atomic<std::uint64_t> sink{0};
    auto fn = [&sink](std::span<const std::byte>,
                      const bitcask::GetResultView& v) {
        sink.fetch_add(v.value.size(), std::memory_order_relaxed);
    };

    for (auto _ : state) {
        auto n = cask.parallel_scan(nthr, fn);
        benchmark::DoNotOptimize(n);
        if (!n) { state.SkipWithError("parallel_scan failed"); break; }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kN));
    cask.close();
}
BENCHMARK(BM_Cask_ParallelScan)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)
    ->Unit(benchmark::kMillisecond)->UseRealTime();

// -----------------------------------------------------------------------------
// Hot get. Pre-populated dir; every get hits the keydir + reads back from
// the file. Page cache is warm after the first iteration.
// -----------------------------------------------------------------------------
static void BM_Cask_Get_Hot(benchmark::State& state) {
    TempDir td;
    auto c = Cask::open(td.path(), rw_opts(), &test_registry());
    if (!c) state.SkipWithError("Cask::open failed");
    auto& cask = **c;

    // S65：keyspace 做成参数——全部行都在 memdelta + 活跃文件里（不 checkpoint），
    // 大档钉的是「命中耗时随未落盘行数涨」（feedbacks/2026-11-23-point-get-hit-
    // cost-grows-with-unflushed-rows.md）。
    const int kKeyspace = static_cast<int>(state.range(0));
    std::vector<std::string> keys;
    keys.reserve(static_cast<std::size_t>(kKeyspace));
    for (int i = 0; i < kKeyspace; ++i) {
        keys.push_back("k" + std::to_string(i));
    }
    const std::string value(128, 'v');
    for (const auto& k : keys) {
        auto r = cask.put(as_bytes(k), as_bytes(value));
        if (!r) state.SkipWithError("populate put failed");
    }

    std::mt19937 rng(0xCAFE);
    std::uniform_int_distribution<int> dist(0, kKeyspace - 1);

    for (auto _ : state) {
        auto& k = keys[static_cast<std::size_t>(dist(rng))];
        auto r = cask.get_owned(as_bytes(k));
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(value.size()));
}
BENCHMARK(BM_Cask_Get_Hot)->Arg(1024)->Arg(100000)->Arg(400000);

// V6.1.5: 零拷贝 get() benchmark——与上方 get_owned() 对比。
// 目标：get() (view) 耗时 < 90% get_owned() (owned)。
static void BM_Cask_Get_Hot_View(benchmark::State& state) {
    TempDir td;
    auto c = Cask::open(td.path(), rw_opts(), &test_registry());
    if (!c) state.SkipWithError("Cask::open failed");
    auto& cask = **c;

    // S65：keyspace 做成参数——全部行都在 memdelta + 活跃文件里（不 checkpoint），
    // 大档钉的是「命中耗时随未落盘行数涨」（feedbacks/2026-11-23-point-get-hit-
    // cost-grows-with-unflushed-rows.md）。
    const int kKeyspace = static_cast<int>(state.range(0));
    std::vector<std::string> keys;
    keys.reserve(static_cast<std::size_t>(kKeyspace));
    for (int i = 0; i < kKeyspace; ++i) {
        keys.push_back("k" + std::to_string(i));
    }
    const std::string value(128, 'v');
    for (const auto& k : keys) {
        auto r = cask.put(as_bytes(k), as_bytes(value));
        if (!r) state.SkipWithError("populate put failed");
    }

    std::mt19937 rng(0xCAFE);
    std::uniform_int_distribution<int> dist(0, kKeyspace - 1);

    for (auto _ : state) {
        auto& k = keys[static_cast<std::size_t>(dist(rng))];
        auto r = cask.get(as_bytes(k));  // zero-copy GetResultView
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(value.size()));
}
BENCHMARK(BM_Cask_Get_Hot_View)->Arg(1024)->Arg(100000)->Arg(400000);

// S66：同上，但建库后 close 再重开——行已落进 OKI run，点查走「bloom → 块首 key 二分
// → 块内定位 → sealed 映射读」。与上面两档并排看：行在 memdelta 与行在 run 两种状态
// （feedbacks/2026-11-23-point-get-after-flush-scans-run-block-linearly.md）。
static void BM_Cask_Get_Hot_View_Reopened(benchmark::State& state) {
    TempDir td;
    const int kKeyspace = static_cast<int>(state.range(0));
    std::vector<std::string> keys;
    keys.reserve(static_cast<std::size_t>(kKeyspace));
    for (int i = 0; i < kKeyspace; ++i) {
        keys.push_back("k" + std::to_string(i));
    }
    const std::string value(128, 'v');
    {
        auto c = Cask::open(td.path(), rw_opts(), &test_registry());
        if (!c) state.SkipWithError("Cask::open failed");
        for (const auto& k : keys) {
            auto r = (*c)->put(as_bytes(k), as_bytes(value));
            if (!r) state.SkipWithError("populate put failed");
        }
        (*c)->close();
    }
    auto c = Cask::open(td.path(), rw_opts(), &test_registry());
    if (!c) state.SkipWithError("Cask::reopen failed");
    auto& cask = **c;

    std::mt19937 rng(0xCAFE);
    std::uniform_int_distribution<int> dist(0, kKeyspace - 1);

    for (auto _ : state) {
        auto& k = keys[static_cast<std::size_t>(dist(rng))];
        auto r = cask.get(as_bytes(k));
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(value.size()));
}
BENCHMARK(BM_Cask_Get_Hot_View_Reopened)->Arg(1024)->Arg(100000)->Arg(400000);

// -----------------------------------------------------------------------------
// A4:open 冷启动——keydir 段快照 vs 全量 fold(20k 记录)。
// 两者共用同一份预生成目录;FullFold 变体每轮删快照(close 会重写)。
// -----------------------------------------------------------------------------
namespace {
std::string prepare_open_dir() {
    static TempDir td;
    static bool done = false;
    if (!done) {
        auto c = Cask::open(td.path(), rw_opts(), &test_registry());
        const std::string value(64, 'v');
        for (int i = 0; i < 20000; ++i) {
            (void)(*c)->put(as_bytes("key" + std::to_string(i)), as_bytes(value));
        }
        (*c)->close();  // 写下快照
        done = true;
    }
    return td.path();
}
}  // namespace

static void BM_Cask_Open_Snapshot(benchmark::State& state) {
    auto dir = prepare_open_dir();
    for (auto _ : state) {
        auto c = Cask::open(dir, rw_opts(), &test_registry());
        if (!c) state.SkipWithError("open failed");
        benchmark::DoNotOptimize(c);
        (*c)->close();  // 重写快照,下一轮仍走快路径
    }
}
BENCHMARK(BM_Cask_Open_Snapshot)->Unit(benchmark::kMicrosecond);

static void BM_Cask_Open_FullFold(benchmark::State& state) {
    auto dir = prepare_open_dir();
    for (auto _ : state) {
        state.PauseTiming();
        std::error_code ec;
        fs::remove(fs::path(dir) / "kv.oki.manifest", ec);
        state.ResumeTiming();
        auto c = Cask::open(dir, rw_opts(), &test_registry());
        if (!c) state.SkipWithError("open failed");
        benchmark::DoNotOptimize(c);
        (*c)->close();
    }
}
BENCHMARK(BM_Cask_Open_FullFold)->Unit(benchmark::kMicrosecond);

// -----------------------------------------------------------------------------
// V3.5:vector 集合 open——hnsw 快照(BCVS)快路径 vs 全量 fold 重插。
// 1 万条 384d(归一化)向量文档;FullFold 变体每轮删 search.vec.ckpt(covers 门
// 关闭 → 整库 fold,重分词 + 重插 HNSW)。无红线,只记实测(设计 §6)。
// -----------------------------------------------------------------------------
namespace {
CaskOptions vec_opts() {
    CaskOptions o;
    o.read_write = true;
    o.enable_search = true;
    bitcask::search::SearchLayerConfig sc;
    sc.analyzer_config.type = bitcask::text::AnalyzerType::Whitespace;
    o.search_config = sc;
    o.vector_dim = 384;
    return o;
}

std::string prepare_vec_open_dir() {
    static TempDir td;
    static bool done = false;
    if (!done) {
        auto c = Cask::open(td.path(), vec_opts(), &test_registry());
        std::mt19937 rng(0xBC35);
        std::normal_distribution<float> nd(0.0f, 1.0f);
        std::vector<float> v(384);
        for (int i = 0; i < 10000; ++i) {
            double sq = 0.0;
            for (auto& x : v) {
                x = nd(rng);
                sq += static_cast<double>(x) * x;
            }
            const auto inv = static_cast<float>(1.0 / std::sqrt(sq));
            for (auto& x : v) x *= inv;
            bitcask::DocInput doc;
            const std::string key = "d" + std::to_string(i);
            const std::string text = "doc " + std::to_string(i);
            doc.text = as_bytes(text);
            doc.vector = std::span<const float>(v.data(), v.size());
            (void)(*c)->put_doc(as_bytes(key), doc);
        }
        (*c)->flush_index();
        (*c)->close();  // 落四块快照(bm25/sidecar/hnsw/keydir)
        done = true;
    }
    return td.path();
}
}  // namespace

static void BM_Cask_Open_VecSnapshot(benchmark::State& state) {
    auto dir = prepare_vec_open_dir();
    for (auto _ : state) {
        auto c = Cask::open(dir, vec_opts(), &test_registry());
        if (!c) state.SkipWithError("open failed");
        benchmark::DoNotOptimize(c);
        (*c)->close();
    }
}
BENCHMARK(BM_Cask_Open_VecSnapshot)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(5);

static void BM_Cask_Open_VecFullFold(benchmark::State& state) {
    auto dir = prepare_vec_open_dir();
    for (auto _ : state) {
        state.PauseTiming();
        std::error_code ec;
        fs::remove(fs::path(dir) / "search.vec.ckpt", ec);
        state.ResumeTiming();
        auto c = Cask::open(dir, vec_opts(), &test_registry());
        if (!c) state.SkipWithError("open failed");
        benchmark::DoNotOptimize(c);
        (*c)->close();
    }
}
BENCHMARK(BM_Cask_Open_VecFullFold)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(2);

// -----------------------------------------------------------------------------
// V3.7:hybrid 端到端(BM25 ∥ HNSW → RRF)。同语料 1 万条 384d;查询 =
// 轮转文本词 + 随机归一化向量,k=10。库在计时区外开关(度量纯查询)。
// -----------------------------------------------------------------------------
static void BM_Cask_SearchHybrid(benchmark::State& state) {
    auto dir = prepare_vec_open_dir();
    auto c = Cask::open(dir, vec_opts(), &test_registry());
    if (!c) { state.SkipWithError("open failed"); return; }
    std::mt19937 rng(0x9337);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    std::vector<float> qs(1000 * 384);
    for (int i = 0; i < 1000; ++i) {
        double sq = 0.0;
        float* p = &qs[static_cast<std::size_t>(i) * 384];
        for (int d = 0; d < 384; ++d) {
            p[d] = nd(rng);
            sq += static_cast<double>(p[d]) * p[d];
        }
        const auto inv = static_cast<float>(1.0 / std::sqrt(sq));
        for (int d = 0; d < 384; ++d) p[d] *= inv;
    }
    std::size_t qi = 0;
    for (auto _ : state) {
        const auto i = qi++ % 1000;
        const std::string tq = "doc " + std::to_string(i * 7 % 10000);
        auto r = (*c)->search_hybrid(
            tq, std::span<const float>(&qs[i * 384], 384), 10);
        if (!r) { state.SkipWithError("hybrid failed"); break; }
        benchmark::DoNotOptimize(r->hits);
    }
    (*c)->close();
}
BENCHMARK(BM_Cask_SearchHybrid)->Unit(benchmark::kMicrosecond);

// BM_Put_WalBatch 已删除（S19-4）：wal_batch_size 是 dead config，两档
// 测的是同一路径（S18 侦查坐实），基准前提不成立。

// -----------------------------------------------------------------------------
// S35：put_batch vs put_batch_atomic 开销对比。原子批多付：一条批头记录
// （27+13B）+ 成员 value 的 arena 预编码（span_bytes 需要精确编码长度）+
// 首次调用的 meta v6 重写（一次性，预热吸收）。arg = 批大小；每迭代提交
// 一整批（覆盖写同一 keyspace），Time/items = 单条摊销成本。
// -----------------------------------------------------------------------------
template <bool kAtomic>
static void BM_Cask_PutBatchImpl(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    TempDir td;
    auto c = Cask::open(td.path(), rw_opts(), &test_registry());
    if (!c) { state.SkipWithError("Cask::open failed"); return; }
    auto& cask = **c;

    std::vector<std::string> keys;
    keys.reserve(n);
    for (std::size_t i = 0; i < n; ++i) keys.push_back("bk" + std::to_string(i));
    const std::string value(128, 'v');

    std::vector<Cask::BatchItem> items;
    std::vector<Cask::BatchOp> ops;
    for (std::size_t i = 0; i < n; ++i) {
        if constexpr (kAtomic) {
            ops.push_back({.type = Cask::BatchOp::Type::kPut,
                           .key = as_bytes(keys[i]), .value = as_bytes(value)});
        } else {
            items.push_back({.key = as_bytes(keys[i]), .value = as_bytes(value)});
        }
    }
    // 预热：吸收 meta v6 懒升级与 active 文件创建的一次性成本。
    if constexpr (kAtomic) { (void)cask.put_batch_atomic(ops); }
    else { (void)cask.put_batch(items); }

    for (auto _ : state) {
        if constexpr (kAtomic) {
            auto r = cask.put_batch_atomic(ops);
            benchmark::DoNotOptimize(r);
        } else {
            auto r = cask.put_batch(items);
            benchmark::DoNotOptimize(r);
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(n));
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(n * value.size()));
    (*c)->close();
}
static void BM_Cask_PutBatch(benchmark::State& state) {
    BM_Cask_PutBatchImpl<false>(state);
}
static void BM_Cask_PutBatchAtomic(benchmark::State& state) {
    BM_Cask_PutBatchImpl<true>(state);
}
BENCHMARK(BM_Cask_PutBatch)->Arg(8)->Arg(64)->Arg(512);
BENCHMARK(BM_Cask_PutBatchAtomic)->Arg(8)->Arg(64)->Arg(512);

// S63：原子批整批应用期间，并发点查只能等 OkiState::mu_。本项量化 400 行
// 大批 + 4 读者的点查 p99 / p99.9；每次 get 计一次样本（含 gget 返回值拷贝
// 的正常读路径），writer 只提交 fixed rounds，读样本在本地累计、writer 收尾
// 汇总。这是锁代价观测基准，不与 put_batch 对比吞吐。
static void BM_Cask_AtomicBatchPointReadP99(benchmark::State& state) {
    constexpr int kRounds = 150;
    constexpr int kReaders = 4;
    constexpr std::size_t kSampleCap = 250000;
    const auto n = static_cast<std::size_t>(state.range(0));
    const int readers = kReaders;
    TempDir td;
    auto c = Cask::open(td.path(), rw_opts(), &test_registry());
    if (!c) {
        state.SkipWithError("Cask::open failed");
        return;
    }
    auto& cask = **c;
    const std::string value(128, 'v');
    if (!cask.put(as_bytes("head"), as_bytes("h0")) ||
        !cask.put(as_bytes("tail"), as_bytes("t0"))) {
        state.SkipWithError("populate failed");
        cask.close();
        return;
    }

    std::atomic<bool> stop{false};
    std::vector<std::vector<std::uint64_t>> samples(
        static_cast<std::size_t>(readers));
    std::vector<std::thread> reader_threads;
    reader_threads.reserve(static_cast<std::size_t>(readers));
    for (int t = 0; t < readers; ++t) {
        reader_threads.emplace_back([&, t] {
            auto& dst = samples[static_cast<std::size_t>(t)];
            dst.reserve(kSampleCap);
            while (!stop.load(std::memory_order_acquire) &&
                   dst.size() < kSampleCap) {
                const auto t0 = std::chrono::steady_clock::now();
                auto r = cask.get_owned(as_bytes("head"));
                const auto mid = std::chrono::steady_clock::now();
                auto tr = cask.get_owned(as_bytes("tail"));
                const auto t1 = std::chrono::steady_clock::now();
                benchmark::DoNotOptimize(r);
                benchmark::DoNotOptimize(tr);
                dst.push_back(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        mid - t0).count()));
                dst.push_back(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        t1 - mid).count()));
            }
        });
    }

    for (int round = 1; round <= kRounds; ++round) {
        const std::string head = "h" + std::to_string(round);
        const std::string body = "t" + std::to_string(round);
        std::vector<Cask::BatchOp> ops;
        ops.reserve(n + 2);
        ops.push_back({.type = Cask::BatchOp::Type::kPut,
                       .key = as_bytes("head"), .value = as_bytes(head)});
        for (std::size_t i = 0; i < n; ++i) {
            const std::string key = "fill" + std::to_string(i);
            ops.push_back({.type = Cask::BatchOp::Type::kPut,
                           .key = as_bytes(key), .value = as_bytes(body)});
        }
        ops.push_back({.type = Cask::BatchOp::Type::kPut,
                       .key = as_bytes("tail"), .value = as_bytes(body)});
        auto r = cask.put_batch_atomic(ops);
        if (!r) {
            state.SkipWithError("atomic batch failed");
            break;
        }
    }
    stop.store(true, std::memory_order_release);
    for (auto& reader : reader_threads) reader.join();

    std::vector<std::uint64_t> all;
    for (const auto& part : samples) {
        all.insert(all.end(), part.begin(), part.end());
    }
    if (!all.empty()) {
        const auto percentile = [&](double p) {
            const auto nth = all.begin() +
                static_cast<std::ptrdiff_t>(std::min(
                    all.size() - 1,
                    static_cast<std::size_t>(
                        static_cast<double>(all.size()) * p)));
            std::nth_element(all.begin(), nth, all.end());
            return static_cast<double>(*nth) / 1000.0;
        };
        state.counters["read_p99_us"] = percentile(0.99);
        state.counters["read_p999_us"] = percentile(0.999);
        state.counters["read_samples"] = static_cast<double>(all.size());
    }
    state.counters["batch_rows"] = static_cast<double>(n + 2);
    state.counters["write_batches"] = static_cast<double>(kRounds);
    cask.close();
    state.SetItemsProcessed(kRounds * static_cast<std::int64_t>(n + 2));
}
BENCHMARK(BM_Cask_AtomicBatchPointReadP99)
    ->Arg(400)->Iterations(1)->Unit(benchmark::kMillisecond);


// -----------------------------------------------------------------------------
// S27-4 P3:DWPT builder 索引吞吐——单文本 put_doc 活写路径的分析+建段在
// apply 内(B=0 在 reducer 内联 = 串行瓶颈;B>=1 连分析一起下放 builder,
// round-robin 并行)。每轮:新库 → kDocs 篇 put_doc → flush_index(排干
// reducer + builder)= 端到端索引吞吐。arg = builder_threads。
// -----------------------------------------------------------------------------
static void BM_Cask_PutDocTextIndex(benchmark::State& state) {
    const auto B = static_cast<std::size_t>(state.range(0));
    constexpr int kDocs = 20000;
    constexpr int kWords = 30;
    // 预生成语料(词表 4k,30 词/篇)——生成成本不入计时。
    static const std::vector<std::string>* corpus = [] {
        auto* v = new std::vector<std::string>();
        v->reserve(kDocs);
        std::mt19937 rng(0x274B);
        std::uniform_int_distribution<int> wd(0, 3999);
        for (int i = 0; i < kDocs; ++i) {
            std::string t;
            t.reserve(kWords * 7);
            for (int w = 0; w < kWords; ++w) {
                t += "w" + std::to_string(wd(rng));
                t += ' ';
            }
            v->push_back(std::move(t));
        }
        return v;
    }();

    CaskOptions o;
    o.read_write = true;
    o.enable_search = true;
    bitcask::search::SearchLayerConfig sc;
    sc.analyzer_config.type = bitcask::text::AnalyzerType::Whitespace;
    sc.builder_threads = B;
    o.search_config = sc;

    std::size_t total = 0;
    for (auto _ : state) {
        state.PauseTiming();
        TempDir td;
        auto c = Cask::open(td.path(), o, &test_registry());
        if (!c) { state.SkipWithError("open failed"); break; }
        state.ResumeTiming();

        for (int i = 0; i < kDocs; ++i) {
            bitcask::DocInput doc;
            doc.text = as_bytes((*corpus)[static_cast<std::size_t>(i)]);
            const std::string key = "k" + std::to_string(i);
            (void)(*c)->put_doc(as_bytes(key), doc,
                                static_cast<std::uint32_t>(i));
        }
        (*c)->flush_index();  // 排干 map/reducer/builder → 端到端索引完成

        state.PauseTiming();
        (*c)->close();
        state.ResumeTiming();
        total += kDocs;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(total));
}
BENCHMARK(BM_Cask_PutDocTextIndex)
    ->Arg(0)->Arg(1)->Arg(2)->Arg(4)
    ->Unit(benchmark::kMillisecond)
    ->MeasureProcessCPUTime()
    ->UseRealTime();
