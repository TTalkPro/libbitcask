// avx512_verify_bench.cpp — Confirm AVX-512 instruction engagement.
//
// Run:  ./bitcask_bench --benchmark_filter=Avx512

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <vector>

#include "hnsw_kernels.hpp"

#include <bitcask/bm25_kernels.hpp>
#include <bitcask/detail/int8_kernels.hpp>
#include <bitcask/intersect.hpp>

using bitcask::bm25::detail::bm25_score_avx512;
using bitcask::bm25::detail::bm25_score_scalar;
using bitcask::vec::detail::dot_avx2;
using bitcask::vec::detail::dot_avx512;
using bitcask::vec::detail::dot_scalar;
using bitcask::vec::detail::l2_avx2;
using bitcask::vec::detail::l2_avx512;
using bitcask::vec::detail::l2_scalar;
using bitcask::vec::int8::Int8DotFn;
using bitcask::vec::int8::pick_int8_dot_kernel;
using bitcask::vec::int8::quantize;
using bitcask::vec::int8::QVector;

namespace {

bool g_cpu_reported = false;

#define BITCASK_PROBE(feat)                                                     \
    do {                                                                        \
        const bool ok = __builtin_cpu_supports(feat);                           \
        std::printf("  %-16s : %s\n", feat, ok ? "YES" : "no");                 \
        if (ok) any = true;                                                     \
    } while (0)

void report_cpu_caps() {
    if (g_cpu_reported) return;
    g_cpu_reported = true;
    __builtin_cpu_init();

    std::printf("\n=== AVX-512 CPU Capability Probe ===\n");
    bool any = false;
    BITCASK_PROBE("avx512f");
    BITCASK_PROBE("avx512vl");
    BITCASK_PROBE("avx512bw");
    BITCASK_PROBE("avx512dq");
    BITCASK_PROBE("avx512cd");
    BITCASK_PROBE("avx512vnni");
    BITCASK_PROBE("avx512bf16");
    std::printf("  -> AVX-512 %s on this CPU\n\n",
                any ? "AVAILABLE" : "NOT available");

    std::printf("=== Runtime Dispatcher Selection ===\n");
    const Int8DotFn k = pick_int8_dot_kernel();
    if (k == nullptr)
        std::printf("  int8 dot kernel    : scalar (no VNNI)\n");
    else if (__builtin_cpu_supports("avx512vnni"))
        std::printf("  int8 dot kernel    : avx512vnni (dot_vnni512)\n");
    else if (__builtin_cpu_supports("avxvnni"))
        std::printf("  int8 dot kernel    : avxvnni (dot_vnni)\n");
    else
        std::printf("  int8 dot kernel    : unknown\n");

    if (__builtin_cpu_supports("avx512f"))
        std::printf("  bm25 score dispatch: avx512f\n");
    else if (__builtin_cpu_supports("avx2"))
        std::printf("  bm25 score dispatch: avx2+fma\n");
    else
        std::printf("  bm25 score dispatch: scalar\n");

    if (__builtin_cpu_supports("avx512f"))
        std::printf("  hnsw distance      : avx512f\n");
    else if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
        std::printf("  hnsw distance      : avx2+fma\n");
    else
        std::printf("  hnsw distance      : scalar\n");

    if (__builtin_cpu_supports("avx512f"))
        std::printf("  intersect_u64      : avx512f\n");
    else if (__builtin_cpu_supports("avx2"))
        std::printf("  intersect_u64      : avx2\n");
    else
        std::printf("  intersect_u64      : scalar\n");
    std::printf("\n");
}

#undef BITCASK_PROBE

constexpr std::size_t kDim = 384;

std::vector<float> make_unit_vecs(std::size_t n, std::uint64_t seed) {
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
        const float inv = static_cast<float>(1.0 / std::sqrt(sq));
        for (std::size_t d = 0; d < kDim; ++d) p[d] *= inv;
    }
    return v;
}

void BM_Avx512_HnswDot_Scalar(benchmark::State& state) {
    report_cpu_caps();
    auto va = make_unit_vecs(256, 0xA1);
    auto vb = make_unit_vecs(256, 0xB2);
    volatile float sink = 0;
    for (auto _ : state) {
        for (std::size_t i = 0; i < 256; ++i)
            sink = dot_scalar(&va[i * kDim], &vb[i * kDim], kDim);
        benchmark::DoNotOptimize(sink);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * 256);
    state.SetLabel("dot_scalar (384d)");
}
BENCHMARK(BM_Avx512_HnswDot_Scalar)->Unit(benchmark::kMicrosecond);

void BM_Avx512_HnswDot_Avx512(benchmark::State& state) {
    if (!__builtin_cpu_supports("avx512f")) {
        state.SkipWithError("no avx512f");
        return;
    }
    auto va = make_unit_vecs(256, 0xA1);
    auto vb = make_unit_vecs(256, 0xB2);
    volatile float sink = 0;
    for (auto _ : state) {
        for (std::size_t i = 0; i < 256; ++i)
            sink = dot_avx512(&va[i * kDim], &vb[i * kDim], kDim);
        benchmark::DoNotOptimize(sink);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * 256);
    state.SetLabel("dot_avx512 (384d) [avx512f]");
}
BENCHMARK(BM_Avx512_HnswDot_Avx512)->Unit(benchmark::kMicrosecond);

void BM_Avx512_HnswL2_Scalar(benchmark::State& state) {
    auto va = make_unit_vecs(256, 0xA1);
    auto vb = make_unit_vecs(256, 0xB2);
    volatile float sink = 0;
    for (auto _ : state) {
        for (std::size_t i = 0; i < 256; ++i)
            sink = l2_scalar(&va[i * kDim], &vb[i * kDim], kDim);
        benchmark::DoNotOptimize(sink);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * 256);
    state.SetLabel("l2_scalar (384d)");
}
BENCHMARK(BM_Avx512_HnswL2_Scalar)->Unit(benchmark::kMicrosecond);

void BM_Avx512_HnswL2_Avx512(benchmark::State& state) {
    if (!__builtin_cpu_supports("avx512f")) {
        state.SkipWithError("no avx512f");
        return;
    }
    auto va = make_unit_vecs(256, 0xA1);
    auto vb = make_unit_vecs(256, 0xB2);
    volatile float sink = 0;
    for (auto _ : state) {
        for (std::size_t i = 0; i < 256; ++i)
            sink = l2_avx512(&va[i * kDim], &vb[i * kDim], kDim);
        benchmark::DoNotOptimize(sink);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * 256);
    state.SetLabel("l2_avx512 (384d) [avx512f]");
}
BENCHMARK(BM_Avx512_HnswL2_Avx512)->Unit(benchmark::kMicrosecond);

struct Bm25Data {
    std::vector<std::uint32_t> tfs;
    std::vector<std::uint32_t> dls;
    std::vector<float>         contrib;
};

Bm25Data make_bm25_data(std::size_t n) {
    std::mt19937 rng(0xC4FE);
    Bm25Data d;
    d.tfs.resize(n);
    d.dls.resize(n);
    d.contrib.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        d.tfs[i] = static_cast<std::uint32_t>(rng() % 50 + 1);
        d.dls[i] = static_cast<std::uint32_t>(rng() % 500 + 10);
    }
    return d;
}

constexpr float kK1Plus1    = 2.2f;
constexpr float kK1Times1mB = 0.3f;
constexpr float kK1TimesB   = 0.9f;
constexpr float kDelta      = 1.0f;
constexpr float kIdf        = 2.5f;
constexpr float kInvAvgdl   = 1.0f / 250.0f;

void BM_Avx512_Bm25Score_Scalar(benchmark::State& state) {
    constexpr std::size_t N = 100000;
    auto d = make_bm25_data(N);
    for (auto _ : state) {
        bm25_score_scalar(d.tfs.data(), d.dls.data(),
                          kK1Plus1, kK1Times1mB, kK1TimesB,
                          kDelta, kIdf, kInvAvgdl,
                          d.contrib.data(), N);
        benchmark::DoNotOptimize(d.contrib[0]);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(N));
    state.SetLabel("bm25_score_scalar (100k postings)");
}
BENCHMARK(BM_Avx512_Bm25Score_Scalar)->Unit(benchmark::kMicrosecond);

void BM_Avx512_Bm25Score_Avx512(benchmark::State& state) {
    if (!__builtin_cpu_supports("avx512f")) {
        state.SkipWithError("no avx512f");
        return;
    }
    constexpr std::size_t N = 100000;
    auto d = make_bm25_data(N);
    for (auto _ : state) {
        bm25_score_avx512(d.tfs.data(), d.dls.data(),
                          kK1Plus1, kK1Times1mB, kK1TimesB,
                          kDelta, kIdf, kInvAvgdl,
                          d.contrib.data(), N);
        benchmark::DoNotOptimize(d.contrib[0]);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(N));
    state.SetLabel("bm25_score_avx512 (100k postings) [avx512f]");
}
BENCHMARK(BM_Avx512_Bm25Score_Avx512)->Unit(benchmark::kMicrosecond);

struct Int8Batch {
    std::vector<QVector> qa;
    std::vector<QVector> qb;
    explicit Int8Batch(std::size_t n) : qa(n), qb(n) {
        std::mt19937_64 rng(0x77);
        std::normal_distribution<float> nd(0.0f, 1.0f);
        for (std::size_t p = 0; p < n; ++p) {
            std::vector<float> va(kDim), vb(kDim);
            double sa = 0, sb = 0;
            for (std::size_t d = 0; d < kDim; ++d) {
                va[d] = nd(rng); vb[d] = nd(rng);
                sa += va[d] * va[d]; sb += vb[d] * vb[d];
            }
            float ia = static_cast<float>(1.0 / std::sqrt(sa));
            float ib = static_cast<float>(1.0 / std::sqrt(sb));
            for (std::size_t d = 0; d < kDim; ++d) { va[d] *= ia; vb[d] *= ib; }
            qa[p] = quantize(va.data(), kDim);
            qb[p] = quantize(vb.data(), kDim);
        }
    }
};

void BM_Avx512_Int8Dot_Scalar(benchmark::State& state) {
    Int8Batch batch(256);
    volatile float sink = 0;
    for (auto _ : state) {
        for (std::size_t i = 0; i < 256; ++i)
            sink = bitcask::vec::int8::dot_scalar(batch.qa[i], batch.qb[i], kDim);
        benchmark::DoNotOptimize(sink);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * 256);
    state.SetLabel("int8 dot_scalar (384d)");
}
BENCHMARK(BM_Avx512_Int8Dot_Scalar)->Unit(benchmark::kMicrosecond);

void BM_Avx512_Int8Dot_Vnni512(benchmark::State& state) {
    if (!__builtin_cpu_supports("avx512vnni")) {
        state.SkipWithError("no avx512vnni");
        return;
    }
    const Int8DotFn kernel = pick_int8_dot_kernel();
    if (kernel == nullptr) {
        state.SkipWithError("dispatcher returned null");
        return;
    }
    Int8Batch batch(256);
    volatile float sink = 0;
    for (auto _ : state) {
        for (std::size_t i = 0; i < 256; ++i)
            sink = kernel(batch.qa[i].codes.data(),
                          batch.qb[i].codes.data(),
                          batch.qb[i].sum_codes,
                          batch.qa[i].scale, batch.qb[i].scale, kDim);
        benchmark::DoNotOptimize(sink);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * 256);
    state.SetLabel("int8 dot_vnni512 (384d) [avx512vnni]");
}
BENCHMARK(BM_Avx512_Int8Dot_Vnni512)->Unit(benchmark::kMicrosecond);

std::vector<std::uint64_t> sorted_unique(std::size_t n, std::uint64_t base,
                                         std::uint64_t range, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<std::uint64_t> v(n);
    for (auto& x : v) x = base + rng() % range;
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

void BM_Avx512_IntersectU64_Scalar(benchmark::State& state) {
    auto a = sorted_unique(2'000'000, 5'000'000'000ULL, 4'000'000, 0xD1);
    auto b = sorted_unique(2'000'000, 5'000'000'000ULL, 4'000'000, 0xD2);
    std::vector<std::uint64_t> out;
    for (auto _ : state) {
        out.clear();
        out.resize(std::min(a.size(), b.size()));
        std::size_t i = 0, j = 0, k = 0;
        while (i < a.size() && j < b.size()) {
            if (a[i] < b[j]) ++i;
            else if (b[j] < a[i]) ++j;
            else { out[k++] = a[i]; ++i; ++j; }
        }
        out.resize(k);
        benchmark::DoNotOptimize(out[0]);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(a.size() + b.size()));
    state.SetLabel("scalar merge (2M x 2M)");
}
BENCHMARK(BM_Avx512_IntersectU64_Scalar)->Unit(benchmark::kMillisecond);

void BM_Avx512_IntersectU64_Dispatched(benchmark::State& state) {
    if (!__builtin_cpu_supports("avx512f")) {
        state.SkipWithError("no avx512f");
        return;
    }
    auto a = sorted_unique(2'000'000, 5'000'000'000ULL, 4'000'000, 0xD1);
    auto b = sorted_unique(2'000'000, 5'000'000'000ULL, 4'000'000, 0xD2);
    std::vector<std::uint64_t> out;
    for (auto _ : state) {
        bitcask::bm25::intersect_u64(a, b, out);
        benchmark::DoNotOptimize(out[0]);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(a.size() + b.size()));
    state.SetLabel("intersect_u64 -> avx512f (2M x 2M)");
}
BENCHMARK(BM_Avx512_IntersectU64_Dispatched)->Unit(benchmark::kMillisecond);

}  // namespace
