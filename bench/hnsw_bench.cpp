// V3.7 基准:HNSW 纯内核(脱离 Cask 的 insert/search 矩阵)。
// 红线(hnsw-design §6):100k/ef64 查询 < 1ms;插入 > 2k/s(384d,本机)。
// 跑法:--benchmark_filter='BM_Hnsw' --benchmark_repetitions=3
//       --benchmark_report_aggregates_only=true(取 median 对账 baseline)。

#include <benchmark/benchmark.h>

#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <random>
#include <span>
#include <vector>

#include <bitcask/hnsw.hpp>
#include <bitcask/diskann.hpp>  // S32-M5:DiskANN 召回三元组
#include <bitcask/ivf_rq.hpp>  // S32-M3:IVF 召回三元组

#include "ann_recall_harness.hpp"  // S32-M0c:召回三元组基建

namespace {

using bitcask::vec::HnswConfig;
using bitcask::vec::HnswIndex;
using bitcask::vec::HnswMetric;

constexpr std::size_t kDim = 384;

HnswConfig bench_cfg() {
    HnswConfig c;
    c.dim = kDim;
    c.metric = HnswMetric::kDot;  // M=16 / efC=200 / seed 取默认
    return c;
}

// 归一化随机高斯向量(纯随机高维是召回最坏形态,但作吞吐/延迟负载
// 正合适:无簇结构,导航跳数取上界一侧)。
std::vector<float> make_vecs(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    std::vector<float> v(n * kDim);
    for (std::size_t i = 0; i < n; ++i) {
        float* p = &v[i * kDim];
        double sq = 0.0;
        for (std::size_t d = 0; d < kDim; ++d) {
            p[d] = nd(rng);
            sq += static_cast<double>(p[d]) * p[d];
        }
        const auto inv = static_cast<float>(1.0 / std::sqrt(sq));
        for (std::size_t d = 0; d < kDim; ++d) p[d] *= inv;
    }
    return v;
}

// 查询基准的共享图:按规模建一次,进程内复用(建 100k 图 ~分钟级,
// 不能进计时区也不该每个 ef 档重建)。
const HnswIndex& shared_graph(std::size_t n) {
    static std::map<std::size_t, std::unique_ptr<HnswIndex>> cache;
    auto& slot = cache[n];
    if (!slot) {
        slot = std::make_unique<HnswIndex>(bench_cfg());
        auto vs = make_vecs(n, 0xBC37);
        for (std::size_t i = 0; i < n; ++i) {
            slot->insert(i, std::span<const float>(&vs[i * kDim], kDim));
        }
    }
    return *slot;
}

}  // namespace

// 插入吞吐:每轮全新图重插 n 条(计时含图构建全程,items/s 即插入速率)。
static void BM_Hnsw_Insert(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    auto vs = make_vecs(n, 0xBC37);
    for (auto _ : state) {
        HnswIndex idx(bench_cfg());
        for (std::size_t i = 0; i < n; ++i) {
            idx.insert(i, std::span<const float>(&vs[i * kDim], kDim));
        }
        benchmark::DoNotOptimize(idx.size());
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(n));
}
BENCHMARK(BM_Hnsw_Insert)
    ->Arg(10000)
    ->Arg(100000)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);

// 查询延迟:k=10,1000 条独立查询轮转(防单查询缓存驻留失真)。
// Args = {规模, ef}。
static void BM_Hnsw_Search(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto ef = static_cast<std::size_t>(state.range(1));
    const HnswIndex& g = shared_graph(n);
    auto qs = make_vecs(1000, 0x9337);
    std::size_t qi = 0;
    for (auto _ : state) {
        auto hits = g.search(
            std::span<const float>(&qs[(qi++ % 1000) * kDim], kDim), 10, ef);
        benchmark::DoNotOptimize(hits);
    }
}
BENCHMARK(BM_Hnsw_Search)
    ->Args({10000, 64})
    ->Args({10000, 256})
    ->Args({100000, 64})
    ->Args({100000, 256})
    ->Args({100000, 1024})  // S7-6：ef≥512 → int8 f32 精排批算走并行
    ->Unit(benchmark::kMicrosecond);

// S13-P7：并发查询扩展性（此前 bench 仅单线程，读者对节点自旋锁的
// exchange 造成的 hub 缓存行乒乓不可见；seqlock 修复后读侧零共享行写）。
// 每线程独立查询流轮转；对账指标 = 线程数↑时单查询延迟的退化幅度。
static void BM_Hnsw_SearchConcurrent(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto ef = static_cast<std::size_t>(state.range(1));
    const HnswIndex& g = shared_graph(n);
    static thread_local std::vector<float> qs;
    // 每线程独立随机查询集（种子掺线程号防同流缓存驻留）。
    qs = make_vecs(256, 0x9337u + static_cast<unsigned>(state.thread_index()));
    std::size_t qi = 0;
    for (auto _ : state) {
        auto hits = g.search(
            std::span<const float>(&qs[(qi++ % 256) * kDim], kDim), 10, ef);
        benchmark::DoNotOptimize(hits);
    }
}
BENCHMARK(BM_Hnsw_SearchConcurrent)
    ->Args({100000, 64})
    ->Threads(1)
    ->Threads(4)
    ->Threads(8)
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();

// ---------------------------------------------------------------------------
// S32-M0c:召回三元组基准（设计 vector-dual-engine-selection §7 M0;基建
// bench/ann_recall_harness.hpp,真值缓存盘上、两引擎共用同一标尺）。
// 输出:计时区 = 查询延迟/QPS(k=10);counters = recall@10/recall@100
// (同 ef,查询集全量)+ build_docs_per_s(建图吞吐)。
// 验收门:任何改动 recall@10 降幅 > 1pt 须显式声明并提供配置回退。
// Args = {规模, ef}。跑法:--benchmark_filter='BM_Hnsw_RecallQps'。
// ---------------------------------------------------------------------------
static void BM_Hnsw_RecallQps(benchmark::State& state) {
    namespace ba = bitcask::bench_ann;
    const auto n  = static_cast<std::size_t>(state.range(0));
    const auto ef = static_cast<std::size_t>(state.range(1));
    constexpr std::size_t kNq = 500;

    // v2 语料:簇规模随 n 缩放（~128 成员/簇,边际健康）;f32 + int8 双真值
    //（后者对 int8 评分引擎公平,差值 ≈ 量化代价）。
    const ba::TruthParams tp = ba::default_truth_params(n, kDim, kNq);
    static std::map<std::size_t, std::vector<float>> base_cache;
    auto& base = base_cache[n];
    if (base.empty()) {
        base = ba::make_corpus(n, kDim, tp.nc, tp.sigma, tp.base_seed,
                               tp.center_seed);
    }
    // query 与 base 共享簇心（center_seed 同）、成员噪声独立——查询落在
    // 簇附近才有可分的真近邻（真值边际健康,召回数字才有意义）。
    static std::map<std::size_t, std::vector<float>> query_cache;
    auto& queries = query_cache[n];
    if (queries.empty()) {
        queries = ba::make_corpus(kNq, kDim, tp.nc, tp.sigma, tp.query_seed,
                                  tp.center_seed);
    }
    const auto gt = ba::load_or_build_truth(
        tp, std::span<const float>(base), std::span<const float>(queries));
    ba::TruthParams tp_i8 = tp;
    tp_i8.int8_scored = true;
    const auto gt_i8 = ba::load_or_build_truth(
        tp_i8, std::span<const float>(base), std::span<const float>(queries));

    // 图按规模缓存（与 shared_graph 独立:语料是聚簇合成,非其随机高斯）;
    // 建图吞吐在首建时测取。
    static std::map<std::size_t, std::pair<std::unique_ptr<HnswIndex>, double>>
        graphs;
    auto& slot = graphs[n];
    if (!slot.first) {
        slot.first = std::make_unique<HnswIndex>(bench_cfg());
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n; ++i) {
            slot.first->insert(
                i, std::span<const float>(&base[i * kDim], kDim));
        }
        const std::chrono::duration<double> dt =
            std::chrono::steady_clock::now() - t0;
        slot.second = static_cast<double>(n) / dt.count();
    }
    const HnswIndex& g = *slot.first;

    // 计时区:k=10 查询轮转（QPS/延迟）。
    std::size_t qi = 0;
    for (auto _ : state) {
        auto hits = g.search(
            std::span<const float>(&queries[(qi++ % kNq) * kDim], kDim), 10,
            ef);
        benchmark::DoNotOptimize(hits);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));

    // 召回（计时区外,同 ef 全查询集）。
    auto run_k = [&](std::size_t k) {
        return ba::recall_at(gt, k, [&](std::size_t q) {
            auto hs = g.search(
                std::span<const float>(&queries[q * kDim], kDim), k, ef);
            std::vector<std::uint64_t> ords;
            ords.reserve(hs.size());
            for (const auto& h : hs) ords.push_back(h.ord);
            return ords;
        });
    };
    auto run_k_i8 = [&](std::size_t k) {
        return ba::recall_at(gt_i8, k, [&](std::size_t q) {
            auto hs = g.search(
                std::span<const float>(&queries[q * kDim], kDim), k, ef);
            std::vector<std::uint64_t> ords;
            ords.reserve(hs.size());
            for (const auto& h : hs) ords.push_back(h.ord);
            return ords;
        });
    };
    state.counters["recall@10"]     = run_k(10);
    state.counters["recall@100"]    = run_k(100);
    state.counters["recall@10_i8"]  = run_k_i8(10);
    state.counters["build_docs_per_s"] = slot.second;
}
BENCHMARK(BM_Hnsw_RecallQps)
    ->Args({10000, 64})
    ->Args({10000, 256})
    ->Args({100000, 64})
    ->Args({100000, 256})
    ->Unit(benchmark::kMicrosecond);

// ---------------------------------------------------------------------------
// S32-M3:IVF 段召回三元组——与 BM_Hnsw_RecallQps **同语料同真值**（缓存
// 共享），两引擎同标尺对账。Args = {规模, nprobe}。计时区 = 查询（QPS）；
// counters = recall@10/100 + build_docs_per_s（训练+分簇+落盘全程）。
// ---------------------------------------------------------------------------
static void BM_Ivf_RecallQps(benchmark::State& state) {
    namespace ba = bitcask::bench_ann;
    using bitcask::vec::IvfBuildSource;
    using bitcask::vec::IvfSegment;
    const auto n      = static_cast<std::size_t>(state.range(0));
    const auto nprobe = static_cast<std::uint32_t>(state.range(1));
    constexpr std::size_t kNq = 500;

    const ba::TruthParams tp = ba::default_truth_params(n, kDim, kNq);
    static std::map<std::size_t, std::vector<float>> base_cache;
    auto& base = base_cache[n];
    if (base.empty()) {
        base = ba::make_corpus(n, kDim, tp.nc, tp.sigma, tp.base_seed,
                               tp.center_seed);
    }
    static std::map<std::size_t, std::vector<float>> query_cache;
    auto& queries = query_cache[n];
    if (queries.empty()) {
        queries = ba::make_corpus(kNq, kDim, tp.nc, tp.sigma, tp.query_seed,
                                  tp.center_seed);
    }
    const auto gt = ba::load_or_build_truth(
        tp, std::span<const float>(base), std::span<const float>(queries));
    ba::TruthParams tp_i8 = tp;
    tp_i8.int8_scored = true;
    const auto gt_i8 = ba::load_or_build_truth(
        tp_i8, std::span<const float>(base), std::span<const float>(queries));

    static std::map<std::size_t, std::pair<std::unique_ptr<IvfSegment>, double>>
        segs;
    auto& slot = segs[n];
    if (!slot.first) {
        const std::string path =
            "/tmp/bitcask_bench_ivf_" + std::to_string(n) + ".biv";
        IvfBuildSource src;
        src.count = static_cast<std::uint32_t>(n);
        src.get = [&](std::uint32_t i, std::uint64_t& ord,
                      const float*& vec) {
            ord = i;
            vec = base.data() + static_cast<std::size_t>(i) * kDim;
        };
        const auto t0 = std::chrono::steady_clock::now();
        if (!IvfSegment::build(path, kDim, src, 0, 1)) {
            state.SkipWithError("ivf build failed");
            return;
        }
        const std::chrono::duration<double> dt =
            std::chrono::steady_clock::now() - t0;
        slot.first = std::make_unique<IvfSegment>();
        if (!slot.first->open(path, kDim, 1)) {
            state.SkipWithError("ivf open failed");
            return;
        }
        slot.second = static_cast<double>(n) / dt.count();
    }
    const IvfSegment& seg = *slot.first;

    std::size_t qi = 0;
    for (auto _ : state) {
        auto hits = seg.search(
            std::span<const float>(&queries[(qi++ % kNq) * kDim], kDim), 10,
            nprobe);
        benchmark::DoNotOptimize(hits);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));

    auto run_k = [&](std::size_t k) {
        return ba::recall_at(gt, k, [&](std::size_t q) {
            auto hs = seg.search(
                std::span<const float>(&queries[q * kDim], kDim), k, nprobe);
            std::vector<std::uint64_t> ords;
            ords.reserve(hs.size());
            for (const auto& h : hs) ords.push_back(h.ord);
            return ords;
        });
    };
    auto run_k_i8 = [&](std::size_t k) {
        return ba::recall_at(gt_i8, k, [&](std::size_t q) {
            auto hs = seg.search(
                std::span<const float>(&queries[q * kDim], kDim), k, nprobe);
            std::vector<std::uint64_t> ords;
            ords.reserve(hs.size());
            for (const auto& h : hs) ords.push_back(h.ord);
            return ords;
        });
    };
    state.counters["recall@10"]    = run_k(10);
    state.counters["recall@100"]   = run_k(100);
    state.counters["recall@10_i8"] = run_k_i8(10);
    state.counters["build_docs_per_s"] = slot.second;
    state.counters["nlist"] = seg.nlist();
}
// ---------------------------------------------------------------------------
// S32-M5:DiskANN 段召回三元组——与 HNSW/IVF 同语料同真值。Args = {规模, L}。
// ---------------------------------------------------------------------------
static void BM_Diskann_RecallQps(benchmark::State& state) {
    namespace ba = bitcask::bench_ann;
    using bitcask::vec::DiskannSegment;
    using bitcask::vec::IvfBuildSource;
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto L = static_cast<std::uint32_t>(state.range(1));
    constexpr std::size_t kNq = 500;

    const ba::TruthParams tp = ba::default_truth_params(n, kDim, kNq);
    static std::map<std::size_t, std::vector<float>> base_cache;
    auto& base = base_cache[n];
    if (base.empty()) {
        base = ba::make_corpus(n, kDim, tp.nc, tp.sigma, tp.base_seed,
                               tp.center_seed);
    }
    static std::map<std::size_t, std::vector<float>> query_cache;
    auto& queries = query_cache[n];
    if (queries.empty()) {
        queries = ba::make_corpus(kNq, kDim, tp.nc, tp.sigma, tp.query_seed,
                                  tp.center_seed);
    }
    const auto gt = ba::load_or_build_truth(
        tp, std::span<const float>(base), std::span<const float>(queries));
    ba::TruthParams tp_i8 = tp;
    tp_i8.int8_scored = true;
    const auto gt_i8 = ba::load_or_build_truth(
        tp_i8, std::span<const float>(base), std::span<const float>(queries));

    static std::map<std::size_t,
                    std::pair<std::unique_ptr<DiskannSegment>, double>>
        segs;
    auto& slot = segs[n];
    if (!slot.first) {
        const std::string path =
            "/tmp/bitcask_bench_diskann_" + std::to_string(n) + ".bda";
        IvfBuildSource src;
        src.count = static_cast<std::uint32_t>(n);
        src.get = [&](std::uint32_t i, std::uint64_t& ord,
                      const float*& vec) {
            ord = i;
            vec = base.data() + static_cast<std::size_t>(i) * kDim;
        };
        const auto t0 = std::chrono::steady_clock::now();
        if (!DiskannSegment::build(path, kDim, src, 0, 0, 1)) {
            state.SkipWithError("diskann build failed");
            return;
        }
        const std::chrono::duration<double> dt =
            std::chrono::steady_clock::now() - t0;
        slot.first = std::make_unique<DiskannSegment>();
        if (!slot.first->open(path, kDim, 1)) {
            state.SkipWithError("diskann open failed");
            return;
        }
        slot.second = static_cast<double>(n) / dt.count();
    }
    const DiskannSegment& seg = *slot.first;

    std::size_t qi = 0;
    for (auto _ : state) {
        auto hits = seg.search(
            std::span<const float>(&queries[(qi++ % kNq) * kDim], kDim), 10,
            L);
        benchmark::DoNotOptimize(hits);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));

    auto run_k = [&](const ba::GroundTruth& g2, std::size_t k) {
        return ba::recall_at(g2, k, [&](std::size_t q) {
            auto hs = seg.search(
                std::span<const float>(&queries[q * kDim], kDim), k, L);
            std::vector<std::uint64_t> ords;
            ords.reserve(hs.size());
            for (const auto& h : hs) ords.push_back(h.ord);
            return ords;
        });
    };
    state.counters["recall@10"]    = run_k(gt, 10);
    state.counters["recall@100"]   = run_k(gt, 100);
    state.counters["recall@10_i8"] = run_k(gt_i8, 10);
    state.counters["build_docs_per_s"] = slot.second;
}
BENCHMARK(BM_Diskann_RecallQps)
    ->Args({100000, 32})
    ->Args({100000, 64})
    ->Args({100000, 128})
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_Ivf_RecallQps)
    ->Args({100000, 8})
    ->Args({100000, 16})
    ->Args({100000, 32})
    ->Args({100000, 64})
    ->Unit(benchmark::kMicrosecond);
