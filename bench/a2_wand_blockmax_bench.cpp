// S10-A2 WAND 块上界缓存微基准（不依赖 google benchmark）。
// 场景：多 term WAND 查询（> kWandThreshold=1024 postings 触发 WAND 路径），
// 每 term 有多 block（文档数 > 128），pivot 循环命中块跳跃分支。
//
// 编译：见 a1_cache_bench.cpp 同样指令（替换源文件即可）。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "bitcask/inverted.hpp"
#include "bitcask/live_checker.hpp"

using namespace bitcask::bm25;

namespace {

template <class Body>
double run_us(int warmup, int iters, Body&& body) {
    for (int i = 0; i < warmup; ++i) body();
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) body();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count() / iters;
}

class MapLiveChecker : public LiveChecker {
public:
    std::unordered_map<std::uint64_t, std::uint32_t> dl_;
    void fill_is_live(std::span<const std::uint64_t> ords,
                      std::span<char> out) const override {
        for (std::size_t i = 0; i < ords.size(); ++i) out[i] = dl_.count(ords[i]) ? 1 : 0;
    }
    void fill_doc_lens(std::span<const std::uint64_t> ords,
                       std::span<std::uint32_t> out) const override {
        for (std::size_t i = 0; i < ords.size(); ++i) {
            auto it = dl_.find(ords[i]);
            out[i] = it != dl_.end() ? it->second : 0;
        }
    }
    [[nodiscard]] bool is_live(std::uint64_t ord) const override {
        return dl_.count(ord) > 0;
    }
    [[nodiscard]] std::uint32_t doc_len(std::uint64_t ord) const override {
        auto it = dl_.find(ord);
        return it != dl_.end() ? it->second : 0;
    }
};

void bench_wand() {
    InvertedIndex idx;
    MapLiveChecker live;

    // 5000 docs，每 doc ~10 词 → 总 50k postings > kWandThreshold(1024) → 触发 WAND。
    constexpr int N = 5000;
    const std::vector<std::string> vocab = {
        "alpha", "bravo", "charlie", "delta", "echo", "foxtrot",
        "golf", "hotel", "india", "juliet", "kilo", "lima"
    };
    for (int d = 0; d < N; ++d) {
        std::unordered_map<std::string, std::pair<std::uint32_t, std::vector<std::uint32_t>>> tfs;
        // 每 doc 含 5-8 个词（从 vocab 模板化采样）
        int nterms = 5 + (d % 4);
        for (int t = 0; t < nterms; ++t) {
            const auto& term = vocab[(d + t) % vocab.size()];
            tfs[term] = {static_cast<std::uint32_t>(1 + t), {static_cast<std::uint32_t>(t)}};
        }
        idx.add_doc(d, tfs);
        live.dl_[d] = static_cast<std::uint32_t>(nterms * 3);
    }

    // 5-term WAND 查询（每 term 都有 postings > 128 → 多 block）
    std::vector<std::string> query5 = {"alpha", "bravo", "charlie", "delta", "echo"};
    std::vector<std::string> query10 = {"alpha", "bravo", "charlie", "delta", "echo",
                                        "foxtrot", "golf", "hotel", "india", "juliet"};

    Bm25Params params;
    // k=1：heap 极快满 → 块跳跃判定尽早触发 → block_upper 计算高频路径。
    auto us5 = run_us(50, 5000, [&] {
        auto r = idx.search(query5, 1, live, &params);
    });
    auto us10 = run_us(50, 5000, [&] {
        auto r = idx.search(query10, 1, live, &params);
    });

    std::printf("--- S10-A2 WAND 块上界缓存（5000 docs, 12 vocab, 多 block, k=1）---\n");
    std::printf("  5-term WAND:  %7.2f µs/q   QPS=%8.0f\n", us5, 1e6 / us5);
    std::printf("  10-term WAND: %7.2f µs/q   QPS=%8.0f\n", us10, 1e6 / us10);
}

}  // namespace

int main() {
    std::printf("=== S10-A2 WAND 块上界缓存微基准 ===\n\n");
    bench_wand();
    std::printf("=== 完成 ===\n");
    return 0;
}
