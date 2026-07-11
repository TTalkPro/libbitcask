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
