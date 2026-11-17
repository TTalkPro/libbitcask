// S45-2/S45-4：乱序应用下的 key+LSN 裁决与幂等（designs/index-pipeline-unordered-design-zh.md §4.1）。
//
// 真并发：多个写线程对**同一组 key** 随机 put_doc / 覆盖 / 删除；提交前注入
// 随机抖动（Cask::set_submit_jitter_for_test），放大「同 key 两个版本的索引
// 任务乱序到达分片」的窗口。终态以 **KV 存储为预言机**逐 key 对拍索引：
//   - KV 中存在 ⇒ docmap 有该 key；其当前正文的唯一词恰好 1 命中；该 key
//     写过的其它任何词 0 命中（否则 = 幽灵）；当前向量 top-1 是该 key。
//   - KV 中不存在 ⇒ docmap 无该 key；该 key 写过的任何词 0 命中（否则 =
//     复活）。
// 对照组：关闭规则 R（OKI 裁决）后同一负载必须观察到违例。

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/index.hpp>
#include <bitcask/index_registry.hpp>
#include <bitcask/search_cache.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "support/test_paths.hpp"

namespace {

namespace fs = std::filesystem;

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

constexpr std::uint16_t kDim = 8;

struct RunResult {
    std::size_t violations = 0;
    std::uint64_t stale_dropped = 0;
    std::size_t vector_nodes = 0;  // close 前的 HNSW 节点数（含死节点）
    std::vector<std::string> samples;
};

bitcask::CaskOptions search_opts() {
    bitcask::CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    bitcask::search::SearchLayerConfig scfg;
    scfg.analyzer_config.type = bitcask::text::AnalyzerType::Whitespace;
    opts.search_config = scfg;
    opts.vector_dim = kDim;
    opts.vector_metric = bitcask::meta::VectorMetric::kCosineNormalized;
    return opts;
}

class UnorderedApplyTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = bitcask::test::unique_tmpdir(
            std::string("unordered_apply_") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    RunResult run(bool arbitration, std::uint64_t seed, std::size_t ops_per_thread,
                  int threads = 4) {
        RunResult res;
        bitcask::oki::IndexRegistry reg;
        auto oc = bitcask::Cask::open(dir_.string(), search_opts(), &reg);
        EXPECT_TRUE(oc) << (oc ? "" : oc.error().detail);
        if (!oc) return res;
        auto c = std::move(*oc);
        c->set_index_arbitration_for_test(arbitration);
        c->set_submit_jitter_for_test(200);

        constexpr std::size_t kKeys = 24;
        auto key_of = [](std::size_t i) { return "k" + std::to_string(i); };
        std::mutex mu;
        // key → 写过的全部 token；token → 向量。
        std::vector<std::vector<std::string>> tokens(kKeys);
        std::map<std::string, std::vector<float>> vec_of;

        std::vector<std::thread> ws;
        for (int t = 0; t < threads; ++t) {
            ws.emplace_back([&, t] {
                std::mt19937_64 rng(seed * 1000 + static_cast<std::uint64_t>(t));
                std::uniform_real_distribution<float> uf(-1.0f, 1.0f);
                for (std::size_t op = 0; op < ops_per_thread; ++op) {
                    const std::size_t ki = rng() % kKeys;
                    const std::string key = key_of(ki);
                    if (rng() % 100 < 70) {
                        const std::string tok = "tok_" + key + "_" + std::to_string(t) +
                                                "_" + std::to_string(op);
                        std::vector<float> v(kDim);
                        for (auto& x : v) x = uf(rng);
                        {
                            std::lock_guard<std::mutex> lk(mu);
                            tokens[ki].push_back(tok);
                            vec_of[tok] = v;
                        }
                        const std::string text = tok + " common";
                        bitcask::DocInput d;
                        d.text = bytes(text);
                        d.vector = v;
                        EXPECT_TRUE(c->put_doc(bytes(key), d));
                    } else {
                        EXPECT_TRUE(c->remove(bytes(key)));
                    }
                }
            });
        }
        for (auto& w : ws) w.join();

        // 收尾竞速：每个 key 最后一次操作 = 两线程近乎同时的 put / remove
        // （remove 晚 300µs 起步 ⇒ LSN 大概率更大），提交抖动放大到 2ms ⇒
        // 「删除的索引任务先于写到达」高概率发生在**最后一次**写上（随机期
        // 的乱序常被后续写覆盖，终态看不出来）。
        c->set_submit_jitter_for_test(2000);
        for (std::size_t ki = 0; ki < kKeys; ++ki) {
            const std::string key = key_of(ki);
            const std::string tok = "tok_" + key + "_final";
            std::vector<float> v(kDim, 0.0f);
            v[ki % kDim] = 1.0f;
            v[(ki + 1) % kDim] = static_cast<float>(ki + 1) / 100.0f;
            {
                std::lock_guard<std::mutex> lk(mu);
                tokens[ki].push_back(tok);
                vec_of[tok] = v;
            }
            std::thread a([&, tok, v] {
                const std::string text = tok + " common";
                bitcask::DocInput d;
                d.text = bytes(text);
                d.vector = v;
                EXPECT_TRUE(c->put_doc(bytes(key), d));
            });
            std::thread b([&] {
                std::this_thread::sleep_for(std::chrono::microseconds(300));
                EXPECT_TRUE(c->remove(bytes(key)));
            });
            a.join();
            b.join();
        }
        EXPECT_TRUE(c->drain_plugins());

        auto note = [&](std::string s) {
            ++res.violations;
            if (res.samples.size() < 8) res.samples.push_back(std::move(s));
        };
        const auto& dm = *c->docmap();
        for (std::size_t ki = 0; ki < kKeys; ++ki) {
            if (tokens[ki].empty()) continue;
            const std::string key = key_of(ki);
            auto got = c->get(bytes(key));
            std::string current;  // KV 当前正文的唯一词（空 = 已删）
            if (got) {
                const std::string_view txt(
                    reinterpret_cast<const char*>(got->value.data()), got->value.size());
                current = std::string(txt.substr(0, txt.find(' ')));
            }
            const bool alive = !current.empty();
            if (dm.get(key).has_value() != alive) {
                note(key + ": docmap presence != KV presence");
            }
            for (const auto& tok : tokens[ki]) {
                auto r = c->search_text(tok, 5);
                if (!r) {
                    note(key + ": search_text error");
                    continue;
                }
                const bool want = alive && tok == current;
                if (want && (r->hits.size() != 1 || r->hits[0].key != key)) {
                    note(tok + ": current version hits=" + std::to_string(r->hits.size()));
                } else if (!want && !r->hits.empty()) {
                    note(tok + (alive ? ": stale version visible (ghost)"
                                      : ": deleted key visible (resurrected)"));
                }
            }
            if (alive) {
                auto vr = c->search_vector(vec_of[current], 1, 4096);
                if (!vr) {
                    note(key + ": search_vector error");
                } else if (vr->hits.empty() || vr->hits[0].key != key) {
                    note(key + ": current vector not top-1");
                }
            }
        }
        res.stale_dropped = c->status().index_stale_dropped;
        res.vector_nodes = c->vector_plugin()->size();
        c->close();
        return res;
    }

    fs::path dir_;
};

TEST_F(UnorderedApplyTest, ConcurrentDisorderMatchesKvOracle) {
    std::uint64_t stale = 0;
    for (std::uint64_t seed : {1u, 2u, 3u}) {
        SCOPED_TRACE("seed=" + std::to_string(seed));
        fs::remove_all(dir_);
        const auto r = run(/*arbitration=*/true, seed, 600);
        EXPECT_EQ(r.violations, 0u);
        for (const auto& s : r.samples) ADD_FAILURE() << s;
        stale += r.stale_dropped;
    }
    EXPECT_GT(stale, 0u) << "乱序负载应触发过期丢弃（否则未覆盖到乱序路径）";
}

TEST_F(UnorderedApplyTest, ControlGroupWithoutArbitrationShowsViolations) {
    std::size_t total = 0;
    for (std::uint64_t seed : {1u, 2u, 3u, 4u}) {
        fs::remove_all(dir_);
        total += run(/*arbitration=*/false, seed, 600).violations;
        if (total > 0) break;
    }
    EXPECT_GT(total, 0u)
        << "关闭裁决后乱序负载应出现幽灵/复活——对照组失效，正例无区分力";
}

// 重放幂等：同一批写在重开后被恢复 fold 重放，索引与 KV 仍一致、文档不重复；
// 向量图不因重放长出重复节点（S45：重放经 OKI 的 key+LSN 裁决丢弃已删 /
// 已覆盖版本，当前版本由向量插件的 key 定位表判重；HNSW 图文件含乱序节点
// 也须能载入）。
TEST_F(UnorderedApplyTest, ReopenReplayIsIdempotent) {
    const auto r1 = run(/*arbitration=*/true, 7, 400);
    ASSERT_EQ(r1.violations, 0u);
    for (const auto& s : r1.samples) ADD_FAILURE() << s;
    bitcask::oki::IndexRegistry reg;
    for (int round = 0; round < 2; ++round) {
        auto oc = bitcask::Cask::open(dir_.string(), search_opts(), &reg);
        ASSERT_TRUE(oc) << oc.error().detail;
        auto c = std::move(*oc);
        ASSERT_TRUE(c->drain_plugins());
        auto all = c->search_text("common", 1000);
        ASSERT_TRUE(all);
        EXPECT_EQ(all->hits.size(), c->docmap()->live_docs()) << "round " << round;
        EXPECT_EQ(c->docmap()->live_docs(), c->status().key_count) << "round " << round;
        EXPECT_EQ(c->vector_plugin()->size(), r1.vector_nodes)
            << "round " << round << "：重开重放不得插入重复向量节点";
        c->close();
    }
}

// S45-6：查询缓存的「计算 → put」TOCTOU。计算开始前取代号，期间发生任何
// 失效（含空集失效）则 put 拒收——否则基于旧快照的结果会常驻缓存（真并发
// 下实测丢文档：AutoCompactConcurrentReadersNoRace 约 5% 失败）。
TEST(SearchCacheGeneration, PutAfterInvalidationIsRejected) {
    bitcask::search::SearchCache cache(16);
    const auto key = bitcask::search::CacheKey::make("text", "hello", 10);
    std::vector<bitcask::bm25::SearchResult> res(1);
    const std::string_view changed[] = {"unrelated"};

    const auto g0 = cache.generation();
    cache.invalidate_terms(changed);  // 与查询词无交集，仍推进代号
    cache.put(key, res, {"hello"}, g0);
    EXPECT_FALSE(cache.get(key).has_value()) << "失效前开始的计算不得写回";

    const auto g1 = cache.generation();
    EXPECT_GT(g1, g0);
    cache.invalidate_terms({});  // 空集（空缓存快路径）同样推进
    EXPECT_GT(cache.generation(), g1);

    const auto g2 = cache.generation();
    cache.put(key, res, {"hello"}, g2);
    EXPECT_TRUE(cache.get(key).has_value());
    cache.invalidate();
    EXPECT_FALSE(cache.get(key).has_value());
    cache.put(key, res, {"hello"});  // kAnyGen：不校验
    EXPECT_TRUE(cache.get(key).has_value());
}

}  // namespace
