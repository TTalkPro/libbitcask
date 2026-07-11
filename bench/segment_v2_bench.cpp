// segment_v2_bench.cpp — S30 mmap 段查询基准(S30-P5)。
// ① BOW 重复查询:TermSnapshotCache 命中(零解码) vs 关缓存(每查询解码)
//    ——量化 S29-6B 段协同收益;
// ② WAND 档:mmap 全量解码 interim vs 内存段快照——量化「WAND 块游标」
//    挂账的真实上限(差值 = 解码开销,决定是否值得做游标)。
#include <benchmark/benchmark.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "bitcask/inverted.hpp"
#include "bitcask/search_types.hpp"
#include "bitcask/segment_v2.hpp"

namespace {

using namespace bitcask;
using bm25::InvertedIndex;
using bm25::TermPositions;
using search::MmapSegment;
using search::SegV2DocRow;

class AllLive final : public bm25::LiveChecker {
public:
    [[nodiscard]] bool is_live(std::uint64_t) const override { return true; }
    [[nodiscard]] std::uint32_t doc_len(std::uint64_t) const override {
        return 8;
    }
};

// docs 篇 × terms 词/篇("t0..t{terms-1}"),写 v2 并 mmap 打开。
std::unique_ptr<MmapSegment> build_mmap(int docs, int terms,
                                        const char* tag) {
    auto idx = std::make_unique<InvertedIndex>();
    for (int i = 0; i < docs; ++i) {
        TermPositions tp;
        for (int t = 0; t < terms; ++t) {
            tp["t" + std::to_string(t)] = {1 + static_cast<std::uint32_t>(i % 3),
                                           {static_cast<std::uint32_t>(t)}};
        }
        idx->add_doc(static_cast<std::uint64_t>(i), tp);
    }
    const auto path = (std::filesystem::temp_directory_path() /
                       (std::string("segv2_bench_") + tag + ".seg"))
                          .string();
    std::filesystem::remove(path);
    std::vector<std::pair<std::string_view, const InvertedIndex*>> fields;
    fields.emplace_back(bitcask::search::kDefaultField, idx.get());
    static thread_local std::string key;
    if (!search::write_segment_v2(
            path, 1, fields, static_cast<std::uint32_t>(docs),
            [](std::uint32_t d) {
                key = "k" + std::to_string(d);
                return SegV2DocRow{key, d, {}};
            },
            static_cast<std::uint64_t>(docs) * 8)) {
        return nullptr;
    }
    return MmapSegment::open(path);
}

const std::vector<std::string>& bow_query() {
    static const std::vector<std::string> q = {"t0", "t1", "t2", "t3",
                                               "t4", "t5", "t6", "t7"};
    return q;
}

}  // namespace

// BOW(8 词 × 120 docs = 960 < 1024):缓存命中路径(默认)。
static void BM_MmapSeg_BOWQuery(benchmark::State& state) {
    auto seg = build_mmap(120, 8, "bow");
    AllLive live;
    for (auto _ : state) {
        auto r = seg->search(bitcask::search::kDefaultField, bow_query(), 10, live);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_MmapSeg_BOWQuery)->Unit(benchmark::kMicrosecond);

// 同查询,关缓存(每查询词典二分 + 全量解码)。
static void BM_MmapSeg_BOWQuery_NoCache(benchmark::State& state) {
    auto seg = build_mmap(120, 8, "bownc");
    AllLive live;
    InvertedIndex::set_query_cache_enabled(false);
    for (auto _ : state) {
        auto r = seg->search(bitcask::search::kDefaultField, bow_query(), 10, live);
        benchmark::DoNotOptimize(r);
    }
    InvertedIndex::set_query_cache_enabled(true);
}
BENCHMARK(BM_MmapSeg_BOWQuery_NoCache)->Unit(benchmark::kMicrosecond);

// WAND 档(2 词 × 20000 docs):mmap 全量解码 interim 的每查询成本。
static void BM_MmapSeg_WANDQuery(benchmark::State& state) {
    auto seg = build_mmap(20000, 2, "wand");
    AllLive live;
    const std::vector<std::string> q = {"t0", "t1"};
    for (auto _ : state) {
        auto r = seg->search(bitcask::search::kDefaultField, q, 10, live);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_MmapSeg_WANDQuery)->Unit(benchmark::kMicrosecond);

// 对照:同语料内存段 WAND(快照拷贝路径)——差值 = mmap 解码净开销。
static void BM_InMemSeg_WANDQuery(benchmark::State& state) {
    auto idx = std::make_unique<InvertedIndex>();
    for (int i = 0; i < 20000; ++i) {
        TermPositions tp;
        for (int t = 0; t < 2; ++t) {
            tp["t" + std::to_string(t)] = {1 + static_cast<std::uint32_t>(i % 3),
                                           {static_cast<std::uint32_t>(t)}};
        }
        idx->add_doc(static_cast<std::uint64_t>(i), tp);
    }
    AllLive live;
    const std::vector<std::string> q = {"t0", "t1"};
    for (auto _ : state) {
        auto r = idx->search(q, 10, live);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_InMemSeg_WANDQuery)->Unit(benchmark::kMicrosecond);

// ===========================================================================
// C4:WAND vs MaxScore A/B(结果位级相同——三方对拍守护;此处纯量性能)。
// 偏斜语料:t0 全命中(热),t1 半频,t2 1/8,t3.. 1/32——多词偏斜是
// MaxScore 的甜区(essential 收缩)。
// ===========================================================================

namespace {

std::unique_ptr<InvertedIndex> build_skewed(int docs) {
    auto idx = std::make_unique<InvertedIndex>();
    for (int i = 0; i < docs; ++i) {
        TermPositions tp;
        tp["t0"] = {1 + static_cast<std::uint32_t>(i % 3), {0}};
        if (i % 2 == 0) tp["t1"] = {1, {1}};
        if (i % 8 == 0) tp["t2"] = {2, {2}};
        if (i % 32 == 0) tp["t3"] = {1, {3}};
        if (i % 32 == 5) tp["t4"] = {1, {4}};
        if (i % 32 == 9) tp["t5"] = {1, {5}};
        idx->add_doc(static_cast<std::uint64_t>(i), tp);
    }
    return idx;
}

class SumDlLive final : public bm25::LiveChecker {  // dl 与索引一致(impacts 前提)
public:
    explicit SumDlLive(const InvertedIndex&) {}
    [[nodiscard]] bool is_live(std::uint64_t) const override { return true; }
    [[nodiscard]] std::uint32_t doc_len(std::uint64_t d) const override {
        std::uint32_t dl = 1 + static_cast<std::uint32_t>(d % 3);
        if (d % 2 == 0) dl += 1;
        if (d % 8 == 0) dl += 2;
        if (d % 32 == 0) dl += 1;
        if (d % 32 == 5) dl += 1;
        if (d % 32 == 9) dl += 1;
        return dl;
    }
};

void run_topk_bench(benchmark::State& state,
                    const std::vector<std::string>& q, bool maxscore) {
    static const auto idx = build_skewed(100000);
    SumDlLive live(*idx);
    InvertedIndex::set_topk_use_maxscore(maxscore);
    for (auto _ : state) {
        auto r = idx->search(q, 10, live);
        benchmark::DoNotOptimize(r);
    }
    InvertedIndex::set_topk_use_maxscore(false);
}

}  // namespace

static void BM_TopK_2term_Wand(benchmark::State& s) {
    run_topk_bench(s, {"t0", "t2"}, false);
}
static void BM_TopK_2term_MaxScore(benchmark::State& s) {
    run_topk_bench(s, {"t0", "t2"}, true);
}
static void BM_TopK_4term_Wand(benchmark::State& s) {
    run_topk_bench(s, {"t0", "t1", "t2", "t3"}, false);
}
static void BM_TopK_4term_MaxScore(benchmark::State& s) {
    run_topk_bench(s, {"t0", "t1", "t2", "t3"}, true);
}
static void BM_TopK_6term_Wand(benchmark::State& s) {
    run_topk_bench(s, {"t0", "t1", "t2", "t3", "t4", "t5"}, false);
}
static void BM_TopK_6term_MaxScore(benchmark::State& s) {
    run_topk_bench(s, {"t0", "t1", "t2", "t3", "t4", "t5"}, true);
}
BENCHMARK(BM_TopK_2term_Wand)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_TopK_2term_MaxScore)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_TopK_4term_Wand)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_TopK_4term_MaxScore)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_TopK_6term_Wand)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_TopK_6term_MaxScore)->Unit(benchmark::kMicrosecond);
