// S10-A4 ad-hoc micro-bench（不依赖 google benchmark）。
// 量化 reduce_apply 字段名 intern 化的收益：
//   1. 多字段 on_write_fields 吞吐（5 字段/文档，Latin 短文本以压低 analyze 比重）
//   2. 全局 operator new 计数：冷启动期（intern 池填充）vs 稳态（intern 全命中）
//      稳态 alloc/doc 应显著低于冷启动——A4 前「每文档 × 每字段一次 owning string」
//      变为 A4 后「首次 N 字段 intern，之后零字段名分配」。
//
// 编译：见配套 run 脚本。链接：libbitcask.a + libtbb + utf8proc + zlib。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "bitcask/search_layer.hpp"

using namespace bitcask::search;
using namespace bitcask::text;

// ---- 全局分配计数（仅本 bench 用；非线程安全但本 bench 单线程）----
namespace {
std::atomic<std::size_t> g_alloc_count{0};
std::atomic<std::size_t> g_alloc_bytes{0};
bool g_counting = false;
}  // namespace

void* operator new(std::size_t n) {
    if (g_counting) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
        g_alloc_bytes.fetch_add(n, std::memory_order_relaxed);
    }
    void* p = std::malloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

struct CountSnapshot {
    std::size_t allocs;
    std::size_t bytes;
};

CountSnapshot snap() {
    return {g_alloc_count.load(std::memory_order_relaxed),
            g_alloc_bytes.load(std::memory_order_relaxed)};
}

struct AllocGuard {
    CountSnapshot before;
    explicit AllocGuard(bool on) {
        if (on) {
            g_alloc_count.store(0, std::memory_order_relaxed);
            g_alloc_bytes.store(0, std::memory_order_relaxed);
            g_counting = true;
        }
    }
    ~AllocGuard() { g_counting = false; }
};

SearchLayerConfig make_config() {
    SearchLayerConfig c;
    c.analyzer_config = AnalyzerConfig{.type = AnalyzerType::Ngram,
                                       .min_n = 2, .max_n = 3};
    c.bm25_params = bitcask::bm25::Bm25Params{1.2F, 0.75F};
    return c;
}

// SSO 阈值（libstdc++）= 15 字节。短字段名走 SSO（无堆分配）；
// 长字段名（>15B）堆分配——A4 的 intern 对后者才有真实分配收益。
const std::vector<std::pair<std::string, std::string>> kShortFields = {
    {"title", "the quick brown fox"},
    {"body", "jumps over the lazy dog"},
    {"tags", "animal story classic"},
    {"author", "anonymous writer"},
    {"category", "fiction literature"},
};
const std::vector<std::pair<std::string, std::string>> kLongFields = {
    {"title_extra_long_name", "the quick brown fox"},
    {"body_extended_field_id", "jumps over the lazy dog"},
    {"tags_quantified_label", "animal story classic"},
    {"author_identifier_key", "anonymous writer"},
    {"category_grouping_tag", "fiction literature"},
};

void bench_multi_field(const char* label,
                       const std::vector<std::pair<std::string, std::string>>& fields) {
    auto cfg = make_config();
    SearchLayer layer(cfg);

    constexpr int kWarmup = 1000;
    constexpr int kMeasure = 100000;

    {
        AllocGuard g(true);
        for (int i = 0; i < kWarmup; ++i) {
            std::string key = "doc" + std::to_string(i);
            layer.on_write_fields(key, i, fields, 1, 100, 50, 1000 + i);
        }
        auto s = snap();
        std::printf("  [%s] 冷启动 %d 文档（intern 池填充）:\n", label, kWarmup);
        std::printf("    alloc/doc = %.1f   bytes/doc = %.0f\n",
                    double(s.allocs) / kWarmup, double(s.bytes) / kWarmup);
    }

    {
        AllocGuard g(true);
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kMeasure; ++i) {
            std::string key = "doc" + std::to_string(kWarmup + i);
            layer.on_write_fields(key, kWarmup + i, fields, 1, 100, 50, 1000 + i);
        }
        auto t1 = std::chrono::steady_clock::now();
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        auto s = snap();

        std::printf("  [%s] 稳态 %d 文档（intern 全命中）:\n", label, kMeasure);
        std::printf("    总耗时      = %.0f ms\n", us / 1000);
        std::printf("    吞吐        = %.0f docs/s\n", kMeasure * 1e6 / us);
        std::printf("    avg/doc     = %.2f µs\n", us / kMeasure);
        std::printf("    alloc/doc   = %.1f\n", double(s.allocs) / kMeasure);
        std::printf("    bytes/doc   = %.0f\n", double(s.bytes) / kMeasure);
    }
    std::printf("\n");
}

// 对照组：单 text 字段（走 on_write，不经 ord_field_lens_ 路径）。
void bench_single_text() {
    auto cfg = make_config();
    SearchLayer layer(cfg);

    constexpr int kWarmup = 1000;
    constexpr int kMeasure = 100000;

    for (int i = 0; i < kWarmup; ++i) {
        std::string key = "doc" + std::to_string(i);
        layer.on_write(key, i, "the quick brown fox jumps over the lazy dog",
                       1, 100, 50, 1000 + i);
    }

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kMeasure; ++i) {
        std::string key = "doc" + std::to_string(kWarmup + i);
        layer.on_write(key, kWarmup + i, "the quick brown fox jumps over the lazy dog",
                       1, 100, 50, 1000 + i);
    }
    auto t1 = std::chrono::steady_clock::now();
    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    std::printf("  单 text 字段 %d 文档（对照）:\n", kMeasure);
    std::printf("    吞吐        = %.0f docs/s\n", kMeasure * 1e6 / us);
    std::printf("    avg/doc     = %.2f µs\n\n", us / kMeasure);
}

}  // namespace

int main() {
    std::printf("=== S10-A4 字段名 intern 微基准 ===\n\n");
    std::printf("--- 多字段（5 字段/文档，Latin 短文本）---\n\n");
    bench_multi_field("short-ssO", kShortFields);
    bench_multi_field("long-heap", kLongFields);
    std::printf("--- 单字段对照 ---\n");
    bench_single_text();
    std::printf("=== 完成 ===\n");
    return 0;
}
