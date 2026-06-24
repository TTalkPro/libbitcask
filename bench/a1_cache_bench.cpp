// S10-A1 ad-hoc micro-bench（不依赖 google benchmark）。
// 直接量化缓存命中 / 未命中路径的 per-query 耗时，对比 CJK vs Latin。
//
// 编译：见配套 run 脚本。链接：libbitcask.a + libtbb + utf8proc + zlib。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitcask/search_layer.hpp"

using namespace bitcask::search;
using namespace bitcask::text;

namespace {

struct BenchResult {
    const char* name;
    int iters;
    double total_us;
    double avg_us;
    double qps;
};

template <class Body>
BenchResult run_bench(const char* name, int warmup, int iters, Body&& body) {
    for (int i = 0; i < warmup; ++i) body();
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) body();
    auto t1 = std::chrono::steady_clock::now();
    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    return {name, iters, us, us / iters, iters * 1e6 / us};
}

SearchLayerConfig make_config(AnalyzerType at, std::uint32_t min_n = 2,
                              std::uint32_t max_n = 3) {
    SearchLayerConfig c;
    c.analyzer_config = AnalyzerConfig{.type = at, .min_n = min_n, .max_n = max_n};
    c.bm25_params = bitcask::bm25::Bm25Params{1.2F, 0.75F};
    c.cache_max_entries = 1024;
    return c;
}

void bench_latin_ngram() {
    auto cfg = make_config(AnalyzerType::Ngram);
    SearchLayer layer(cfg);
    constexpr int N = 5000;
    for (int i = 0; i < N; ++i) {
        std::string key = "doc" + std::to_string(i);
        std::string text = "the quick brown fox jumps over the lazy dog item " +
                           std::to_string(i);
        layer.on_write(key, i, text, 1, 100, 50, 1000 + i);
    }

    auto r_hit = run_bench("latin/ngram/hit", 100, 50000, [&] {
        auto r = layer.search_text("quick brown", 10);
        if (!r || r->empty()) std::abort();
    });

    std::vector<std::string> miss_queries;
    miss_queries.reserve(50000);
    for (int i = 0; i < 50000; ++i) {
        miss_queries.push_back("miss" + std::to_string(i) + " token" + std::to_string(i));
    }
    const std::size_t mq_size = miss_queries.size();
    std::size_t mi = 0;
    auto r_miss = run_bench("latin/ngram/miss", 10, 50000, [&] {
        auto r = layer.search_text(miss_queries[mi], 10);
        mi = (mi + 1) % mq_size;
    });

    std::printf("  %-22s avg=%7.2f µs/q   QPS=%10.0f\n", r_hit.name, r_hit.avg_us, r_hit.qps);
    std::printf("  %-22s avg=%7.2f µs/q   QPS=%10.0f\n", r_miss.name, r_miss.avg_us, r_miss.qps);
    std::printf("  -> 命中节省 %.2f µs/q (analyze 成本)\n\n",
                r_miss.avg_us - r_hit.avg_us);
}

void bench_cjk_ngram() {
    auto cfg = make_config(AnalyzerType::Ngram);
    SearchLayer layer(cfg);
    constexpr int N = 3000;
    const std::vector<std::string> cjk_texts = {
        "北京今天天气晴朗适合户外活动",
        "上海金融市场持续稳健发展",
        "广州深圳科技创新走廊建设加速",
        "杭州西湖文化景观闻名世界",
        "成都天府新区高质量发展",
    };
    for (int i = 0; i < N; ++i) {
        std::string key = "文档" + std::to_string(i);
        layer.on_write(key, i, cjk_texts[i % cjk_texts.size()], 1, 100, 50, 1000 + i);
    }

    auto r_hit = run_bench("cjk/ngram/hit", 100, 50000, [&] {
        auto r = layer.search_text("北京 天气", 10);
        if (!r || r->empty()) std::abort();
    });

    std::vector<std::string> miss_queries;
    miss_queries.reserve(50000);
    for (int i = 0; i < 50000; ++i) {
        miss_queries.push_back("未见词" + std::to_string(i) + "查询" + std::to_string(i));
    }
    const std::size_t mq_size = miss_queries.size();
    std::size_t mi = 0;
    auto r_miss = run_bench("cjk/ngram/miss", 10, 50000, [&] {
        auto r = layer.search_text(miss_queries[mi], 10);
        mi = (mi + 1) % mq_size;
    });

    std::printf("  %-22s avg=%7.2f µs/q   QPS=%10.0f\n", r_hit.name, r_hit.avg_us, r_hit.qps);
    std::printf("  %-22s avg=%7.2f µs/q   QPS=%10.0f\n", r_miss.name, r_miss.avg_us, r_miss.qps);
    std::printf("  -> 命中节省 %.2f µs/q (analyze 成本)\n\n",
                r_miss.avg_us - r_hit.avg_us);
}

void bench_mixed_hit_ratio() {
    auto cfg = make_config(AnalyzerType::Ngram);
    SearchLayer layer(cfg);
    constexpr int N = 5000;
    for (int i = 0; i < N; ++i) {
        std::string key = "doc" + std::to_string(i);
        std::string text = "the quick brown fox item " + std::to_string(i);
        layer.on_write(key, i, text, 1, 100, 50, 1000 + i);
    }

    // 模拟典型生产：5 个热查询（命中率高），N 个冷查询。
    // Hit ratio = 5 / (5 + cold) 分别跑 0%、50%、90%。
    const double ratios[] = {0.0, 0.5, 0.9};
    const int Q = 50000;
    for (double ratio : ratios) {
        std::vector<std::string> qs;
        qs.reserve(Q);
        const char* hot[] = {"quick brown", "fox jumps", "lazy dog", "the item", "brown dog"};
        for (int i = 0; i < Q; ++i) {
            if ((i % 100) < ratio * 100) {
                qs.push_back(hot[i % 5]);
            } else {
                qs.push_back("unique" + std::to_string(i) + " " + std::to_string(i));
            }
        }
        int idx = 0;
        auto r = run_bench(("hit_ratio_" + std::to_string(int(ratio * 100))).c_str(),
                           100, Q, [&] {
               auto res = layer.search_text(qs[idx], 10);
               idx = (idx + 1) % qs.size();
           });
        std::printf("  ratio=%3.0f%%  avg=%6.2f µs/q   QPS=%10.0f\n",
                    ratio * 100, r.avg_us, r.qps);
    }
    std::printf("\n");
}

void bench_cjk_long_query() {
    auto cfg = make_config(AnalyzerType::Ngram);
    SearchLayer layer(cfg);
    constexpr int N = 3000;
    const std::vector<std::string> cjk_texts = {
        "北京今天天气晴朗适合户外活动",
        "上海金融市场持续稳健发展",
        "广州深圳科技创新走廊建设加速",
        "杭州西湖文化景观闻名世界",
        "成都天府新区高质量发展",
    };
    for (int i = 0; i < N; ++i) {
        std::string key = "文档" + std::to_string(i);
        layer.on_write(key, i, cjk_texts[i % cjk_texts.size()], 1, 100, 50, 1000 + i);
    }

    // 长查询模拟用户粘贴大段文本（~200 CJK chars → analyze 重）。
    const std::string long_query =
        "北京今天天气晴朗适合户外活动上海金融市场持续稳健发展"
        "广州深圳科技创新走廊建设加速杭州西湖文化景观闻名世界"
        "成都天府新区高质量发展北京今天天气晴朗适合户外活动"
        "上海金融市场持续稳健发展广州深圳科技创新走廊建设加速"
        "杭州西湖文化景观闻名世界成都天府新区高质量发展";

    auto r_hit = run_bench("cjk/long/hit", 100, 50000, [&] {
        auto r = layer.search_text(long_query, 10);
    });

    std::vector<std::string> miss_queries;
    miss_queries.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        miss_queries.push_back(long_query + std::to_string(i));
    }
    const std::size_t mq_size = miss_queries.size();
    std::size_t mi = 0;
    auto r_miss = run_bench("cjk/long/miss", 10, 10000, [&] {
        auto r = layer.search_text(miss_queries[mi], 10);
        mi = (mi + 1) % mq_size;
    });

    std::printf("  %-22s avg=%7.2f µs/q   QPS=%10.0f\n", r_hit.name, r_hit.avg_us, r_hit.qps);
    std::printf("  %-22s avg=%7.2f µs/q   QPS=%10.0f\n", r_miss.name, r_miss.avg_us, r_miss.qps);
    std::printf("  -> 命中节省 %.2f µs/q (analyze 成本)\n\n",
                r_miss.avg_us - r_hit.avg_us);
}

}  // namespace

int main() {
    std::printf("=== S10-A1 缓存前置微基准 ===\n\n");
    std::printf("--- Latin ngram ---\n");
    bench_latin_ngram();
    std::printf("--- CJK ngram（短查询 ~4 字符）---\n");
    bench_cjk_ngram();
    std::printf("--- CJK ngram（长查询 ~200 字符）---\n");
    bench_cjk_long_query();
    std::printf("--- Mixed hit-ratio (Latin) ---\n");
    bench_mixed_hit_ratio();
    std::printf("=== 完成 ===\n");
    return 0;
}
