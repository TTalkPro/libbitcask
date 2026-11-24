// S33-5：CaskRangeIter 三方对拍 + 并发 stress。
// 对拍三方：make_range_iter(lo,hi) ×「CaskIter 全表 + 过滤 + 排序」×
// 「影子 std::map」。属性测试交错 put/覆盖写/remove/merge/close-reopen，
// 每轮随机窗口 + 全域窗口全比对——三方逐 key、逐 value 相等。
// 完整性不变量（OKI key 集 ⊇ keydir 活 key 集）由「全域 range 输出 ==
// 影子 map」直接蕴含（缺 key 即失败）。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskOptions;
using bitcask::RangeOptions;
using bitcask::CaskRangeIter;

namespace {

bitcask::oki::IndexRegistry& test_registry() {
    static bitcask::oki::IndexRegistry reg;
    return reg;
}

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string to_str(const std::vector<std::byte>& b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

using KvVec = std::vector<std::pair<std::string, std::string>>;

// 方式一：range 迭代器。
KvVec range_scan(Cask& c, std::string_view lo, std::string_view hi) {
    RangeOptions o;
    if (!lo.empty()) o.lo = bytes(lo);
    if (!hi.empty()) o.hi = bytes(hi);
    auto it = c.make_range_iter(o);
    EXPECT_TRUE(it.has_value()) << (it ? "" : it.error().detail);
    KvVec out;
    if (!it) return out;
    while (true) {
        auto e = (*it)->next();
        EXPECT_TRUE(e.has_value());
        if (!e || !e->has_value()) break;
        out.emplace_back(to_str((*e)->key), to_str((*e)->value));
    }
    return out;
}

// S33-6：同上，但走值预取路径（prefetch/prefetch_threads）。
KvVec range_scan_prefetch(Cask& c, std::string_view lo, std::string_view hi,
                          std::size_t prefetch, std::size_t threads) {
    RangeOptions o;
    if (!lo.empty()) o.lo = bytes(lo);
    if (!hi.empty()) o.hi = bytes(hi);
    o.prefetch = prefetch;
    o.prefetch_threads = threads;
    auto it = c.make_range_iter(o);
    EXPECT_TRUE(it.has_value()) << (it ? "" : it.error().detail);
    KvVec out;
    if (!it) return out;
    while (true) {
        auto e = (*it)->next();
        EXPECT_TRUE(e.has_value());
        if (!e || !e->has_value()) break;
        out.emplace_back(to_str((*e)->key), to_str((*e)->value));
    }
    return out;
}

// 方式二：CaskIter 全表 + 过滤 + 排序（O(全表) 参照实现）。
KvVec full_scan_filter(Cask& c, std::string_view lo, std::string_view hi) {
    KvVec out;
    auto it = c.make_iter();
    EXPECT_TRUE(it->start());
    while (true) {
        auto e = it->next();
        EXPECT_TRUE(e.has_value());
        if (!e || !e->has_value()) break;
        std::string k = to_str((*e)->key);
        if (!lo.empty() && k < lo) continue;
        if (!hi.empty() && k >= hi) continue;
        out.emplace_back(std::move(k), to_str((*e)->value));
    }
    it->release();
    std::sort(out.begin(), out.end());
    return out;
}

// 方式三：影子 map。
KvVec shadow_range(const std::map<std::string, std::string>& m,
                   std::string_view lo, std::string_view hi) {
    KvVec out;
    auto it = lo.empty() ? m.begin() : m.lower_bound(std::string(lo));
    for (; it != m.end(); ++it) {
        if (!hi.empty() && it->first >= hi) break;
        out.emplace_back(it->first, it->second);
    }
    return out;
}

void expect_three_way_equal(Cask& c,
                            const std::map<std::string, std::string>& shadow,
                            std::string_view lo, std::string_view hi,
                            const char* what) {
    auto a = range_scan(c, lo, hi);
    auto b = full_scan_filter(c, lo, hi);
    auto s = shadow_range(shadow, lo, hi);
    EXPECT_EQ(a, s) << what << " range vs shadow, lo=" << lo << " hi=" << hi;
    EXPECT_EQ(b, s) << what << " fullscan vs shadow, lo=" << lo
                    << " hi=" << hi;
}

class OkiRangeTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() /
               (std::string("bitcask_oki_range_") + info->name());
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    fs::path dir_;
};

}  // namespace

TEST_F(OkiRangeTest, BasicWindowsAcrossRunsAndMemdelta) {
    CaskOptions o;
    o.read_write = true;
    o.max_file_size = 512;  // 多 sealed 文件
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    std::map<std::string, std::string> shadow;

    auto put = [&](const std::string& k, const std::string& v) {
        ASSERT_TRUE((*c)->put(bytes(k), bytes(v), 1000));
        shadow[k] = v;
    };
    auto del = [&](const std::string& k) {
        ASSERT_TRUE((*c)->remove(bytes(k), 2000));
        shadow.erase(k);
    };

    for (int i = 0; i < 40; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "w%03d", i);
        put(buf, "v" + std::to_string(i));
    }
    del("w005");
    del("w017");
    put("w010", "overwritten");
    // checkpoint 把已写行 flush 进 run；其后的写只在 memdelta——窗口对拍
    // 必须跨 run/memdelta 归并正确。
    ASSERT_TRUE((*c)->checkpoint());
    put("w100", "late1");
    put("w020", "late-overwrite");
    del("w030");

    expect_three_way_equal(**c, shadow, "", "", "full");
    expect_three_way_equal(**c, shadow, "w010", "w020", "mid");
    expect_three_way_equal(**c, shadow, "w000", "w006", "with-del");
    expect_three_way_equal(**c, shadow, "w030", "", "tail-open");
    expect_three_way_equal(**c, shadow, "", "w003", "head-open");
    expect_three_way_equal(**c, shadow, "a", "b", "empty-window");
    expect_three_way_equal(**c, shadow, "w017", "w018", "deleted-only");
    (*c)->close();
}

// S43-6：CaskIter 的快照来源必须是 OKI ReadView，而不是仍在共享可变的
// shard 表。创建视图后的覆盖/删除/新写和 flush 都不得改变在途枚举。
TEST_F(OkiRangeTest, FoldIterPinsOkiViewAcrossWritesAndFlush) {
    CaskOptions o;
    o.read_write = true;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    auto& cask = **c;

    ASSERT_TRUE(cask.put(bytes("a1"), bytes("old-1"), 1000));
    ASSERT_TRUE(cask.put(bytes("a2"), bytes("old-2"), 1000));

    auto it = cask.make_iter();
    auto st = it->start();
    ASSERT_TRUE(st.has_value());
    ASSERT_TRUE(st.has_value());

    // 覆盖、删除、新增各一条，并把 memdelta 真实推进到 run。旧视图里的
    // run Reader 与 delta 拷贝都不可变；flush 只发布新视图。
    ASSERT_TRUE(cask.put(bytes("a1"), bytes("new-1"), 1001));
    ASSERT_TRUE(cask.remove(bytes("a2"), 1002));
    ASSERT_TRUE(cask.put(bytes("a3"), bytes("new-3"), 1003));
    ASSERT_TRUE(cask.checkpoint());

    std::map<std::string, std::string> got;
    while (true) {
        auto e = it->next();
        ASSERT_TRUE(e.has_value()) << (e ? "" : e.error().detail);
        if (!e || !e->has_value()) break;
        got.emplace(to_str((*e)->key), to_str((*e)->value));
    }
    it->release();

    const std::map<std::string, std::string> expected{
        {"a1", "old-1"},
        {"a2", "old-2"},
    };
    EXPECT_EQ(got, expected);
    cask.close();
}

TEST_F(OkiRangeTest, PropertyThreeWayWithMergeAndReopen) {
    std::mt19937_64 rng(0x0C1);
    std::map<std::string, std::string> shadow;
    CaskOptions o;
    o.read_write = true;
    o.max_file_size = 1024;

    auto open_cask = [&] {
        auto c = Cask::open(dir_.string(), o, &test_registry());
        EXPECT_TRUE(c);
        return std::move(*c);
    };
    auto c = open_cask();

    auto rand_key = [&] {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "p%04d",
                      static_cast<int>(rng() % 500));
        return std::string(buf);
    };

    constexpr int kRounds = 12;
    constexpr int kOpsPerRound = 120;
    for (int round = 0; round < kRounds; ++round) {
        for (int i = 0; i < kOpsPerRound; ++i) {
            const auto k = rand_key();
            switch (rng() % 4) {
                case 0:
                case 1: {  // put / overwrite（权重 2）
                    const std::string v =
                        "v" + std::to_string(rng() % 100000);
                    ASSERT_TRUE(c->put(bytes(k), bytes(v), 1000));
                    shadow[k] = v;
                    break;
                }
                case 2: {  // remove（可能 miss——两边语义一致）
                    (void)c->remove(bytes(k), 2000);
                    shadow.erase(k);
                    break;
                }
                case 3: {  // 偶发结构事件
                    if (rng() % 6 == 0) {
                        auto merged = c->merge();
                        ASSERT_TRUE(merged) << merged.error().detail;
                    } else if (rng() % 6 == 1) {
                        c->close();
                        c = open_cask();
                    } else if (rng() % 6 == 2) {
                        ASSERT_TRUE(c->checkpoint());
                    }
                    break;
                }
            }
        }
        // 每轮：全域 + 3 个随机窗口对拍。
        expect_three_way_equal(*c, shadow, "", "", "round-full");
        for (int w = 0; w < 3; ++w) {
            auto a = rand_key();
            auto b = rand_key();
            if (b < a) std::swap(a, b);
            expect_three_way_equal(*c, shadow, a, b, "round-window");
        }
        if (::testing::Test::HasFailure()) {
            FAIL() << "对拍失败于 round " << round << "（种子 0x0C1）";
        }
    }
    c->close();
}

// S33-6：值预取只改变取值时机，输出必须与惰性路径逐 key 逐 value 相同。
// 覆盖批界（prefetch 小于/大于/整除窗口大小）、单线程与多线程、run 与
// memdelta 混合，以及「预取批内全是死 key」（删掉一整段后仍须继续推进
// 而不是提前 EOI）。
TEST_F(OkiRangeTest, PrefetchMatchesLazyAcrossBatchBoundaries) {
    CaskOptions o;
    o.read_write = true;
    o.max_file_size = 4096;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    auto& cask = **c;
    std::map<std::string, std::string> shadow;

    auto put = [&](const std::string& k, const std::string& v) {
        ASSERT_TRUE(cask.put(bytes(k), bytes(v), 1000));
        shadow[k] = v;
    };

    for (int i = 0; i < 200; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "k%04d", i);
        put(buf, "v" + std::to_string(i) +
                     std::string(static_cast<std::size_t>(i % 7), 'x'));
    }
    ASSERT_TRUE(cask.checkpoint());  // 一半进 run
    for (int i = 200; i < 260; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "k%04d", i);
        put(buf, "late" + std::to_string(i));
    }
    // 连续删一整段（≥ 最大预取批），逼出「整批死 key」的续跑路径：OKI 行
    // 仍在（陈旧），回查 keydir 全 kNotFound。
    for (int i = 100; i < 140; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "k%04d", i);
        ASSERT_TRUE(cask.remove(bytes(buf), 2000));
        shadow.erase(buf);
    }

    struct Window { const char* lo; const char* hi; };
    const Window windows[] = {{"", ""}, {"k0090", "k0150"}, {"k0250", ""},
                              {"", "k0005"}, {"k0300", "k0400"}};
    for (const auto& w : windows) {
        const auto lazy = range_scan(cask, w.lo, w.hi);
        const auto ref  = shadow_range(shadow, w.lo, w.hi);
        EXPECT_EQ(lazy, ref) << "lazy vs shadow lo=" << w.lo;
        for (std::size_t p : {std::size_t{2}, std::size_t{8}, std::size_t{64},
                              std::size_t{1000}}) {
            for (std::size_t t : {std::size_t{0}, std::size_t{1},
                                  std::size_t{3}}) {
                EXPECT_EQ(range_scan_prefetch(cask, w.lo, w.hi, p, t), lazy)
                    << "prefetch=" << p << " threads=" << t
                    << " lo=" << w.lo << " hi=" << w.hi;
            }
        }
    }
    // prefetch=1 是「关闭」的边界值，必须与 0 等价。
    EXPECT_EQ(range_scan_prefetch(cask, "", "", 1, 4), range_scan(cask, "", ""));
    cask.close();
}

// 并发：单写者持续写/删 + N 个 range 读者全域扫 + 一次 merge。
// 弱一致语义下不比对内容（写在途），断言结构性质：扫描输出严格升序、
// 无重复、迭代与写/merge 并发零错误零崩溃。TSan 树重点目标。
TEST_F(OkiRangeTest, ConcurrentWriterRangeReadersAndMerge) {
    CaskOptions o;
    o.read_write = true;
    o.max_file_size = 4096;
    auto copen = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(copen);
    auto& c = **copen;
#if defined(__SANITIZE_THREAD__)          // GCC
#  define BITCASK_TSAN_ACTIVE 1
#elif defined(__has_feature)              // clang
#  if __has_feature(thread_sanitizer)
#    define BITCASK_TSAN_ACTIVE 1
#  endif
#endif
    for (int i = 0; i < 200; ++i) {
        ASSERT_TRUE(c.put(bytes("c" + std::to_string(i)), bytes("v"), 1000));
    }
    ASSERT_TRUE(c.checkpoint());  // 让读者有 run 可归并
    // S43-4：public get 直连 OKI；旧 Level A 下 RO 语义的显式 fallback
    // 只证明读可用。Range 的可用性闸随后负责首次受控迁移，并发扫描不
    // 会把迁移窗口折叠成“没有索引”。
    {
        const auto warmup = c.get_owned(bytes("c0"));
        ASSERT_TRUE(warmup.has_value()) << warmup.error().detail;
    }

    std::atomic<bool> stop{false};
    std::atomic<int> scans{0};
    std::atomic<int> reader_errors{0};
    std::mutex err_mu;
    std::string first_err;  // 诊断：首个错误的分类与详情
    auto note_err = [&](const std::string& what) {
        std::lock_guard<std::mutex> lk(err_mu);
        if (first_err.empty()) first_err = what;
        reader_errors.fetch_add(1);
    };

    std::thread writer([&] {
        std::mt19937_64 rng(7);
        for (int i = 0; i < 4000 && !stop.load(); ++i) {
            const std::string k = "c" + std::to_string(rng() % 400);
            if (rng() % 5 == 0) {
                (void)c.remove(bytes(k), 2000);
            } else {
                (void)c.put(bytes(k), bytes("w" + std::to_string(i)), 1000);
            }
            if (i == 2000) {
                (void)c.merge();  // 与读者并发的 merge
            }
        }
        stop.store(true);
    });

    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto it = c.make_range_iter(RangeOptions{});
                if (!it) {
                    note_err("make_range_iter: " + it.error().detail);
                    return;
                }
                std::string prev;
                bool first = true;
                while (true) {
                    auto e = (*it)->next();
                    if (!e) {
                        note_err("next: " + e.error().detail +
                                 " kind=" +
                                 std::to_string(static_cast<int>(
                                     e.error().kind)) +
                                 " errnum=" +
                                 std::to_string(e.error().errnum) +
                                 " last=" + prev);
                        return;
                    }
                    if (!e->has_value()) break;
                    std::string k = to_str((*e)->key);
                    if (!first && !(prev < k)) {
                        note_err("order violation: " + prev + " !< " + k);
                        return;
                    }
                    prev = std::move(k);
                    first = false;
                }
                scans.fetch_add(1);
            }
        });
    }
    writer.join();
    for (auto& t : readers) t.join();
    EXPECT_EQ(reader_errors.load(), 0) << "first: " << first_err;
    EXPECT_GT(scans.load(), 0);
    c.close();
}

// 反馈 2026-09-18（oki-unflushed-memdelta-range-query）回归：memdelta 排序
// 视图缓存。钉两件事——
//   1) 任何写（put/覆盖/remove）都必须正确失效缓存：视图 × 影子三方对拍
//      在「缓存已建立 → 再写 → 再读」的序列下不得缺 key、不得给旧值；
//   2) 未 flush 的 memdelta（5 万行，远低于 1M 行 flush 阈值）上连续 range
//      查询不得按查询次数线性变贵：修订前每次 make_range_iter 全量拷贝 +
//      stable_sort memdelta（下游实测 5 万行 ~120ms/次），1000 次窄窗口
//      会拖到数十秒；修订后首查排序一次、后续命中缓存，秒级以内。
//      10s 上限是悬崖探针（数量级护栏），不是性能断言——性能标定归
//      bench/range_bench.cpp。
TEST_F(OkiRangeTest, SortedViewCacheInvalidatesOnWriteAndStaysFast) {
    CaskOptions o;
    o.read_write = true;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    std::map<std::string, std::string> shadow;

    auto put = [&](const std::string& k, const std::string& v) {
        ASSERT_TRUE((*c)->put(bytes(k), bytes(v), 1000));
        shadow[k] = v;
    };
    auto del = [&](const std::string& k) {
        ASSERT_TRUE((*c)->remove(bytes(k), 2000));
        shadow.erase(k);
    };

    // 建缓存：首批写 + 全域对拍（首次 make_range_iter 走 miss 路径建缓存）。
    for (int i = 0; i < 50; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "c%03d", i);
        put(buf, "v" + std::to_string(i));
    }
    expect_three_way_equal(**c, shadow, "", "", "cache-build");

    // 命中路径 + 写后失效，交替验证：同 key 覆盖、新 key、删除、删除后重写。
    expect_three_way_equal(**c, shadow, "", "", "cache-hit-1");
    put("c010", "overwritten");
    put("c999", "late");
    del("c020");
    expect_three_way_equal(**c, shadow, "", "", "after-write-1");
    expect_three_way_equal(**c, shadow, "c015", "c030", "after-write-window");
    del("c999");
    put("c020", "resurrected");
    expect_three_way_equal(**c, shadow, "", "", "after-write-2");
    expect_three_way_equal(**c, shadow, "", "", "cache-hit-2");
    (*c)->close();
}

TEST_F(OkiRangeTest, UnflushedMemdeltaRepeatedRangeNotLinearPerQuery) {
    constexpr int kKeys = 50000;
    constexpr int kQueries = 1000;

    CaskOptions o;
    o.read_write = true;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    for (int i = 0; i < kKeys; ++i) {
        char k[16], v[16];
        std::snprintf(k, sizeof(k), "m%06d", i);
        std::snprintf(v, sizeof(v), "v%06d", i);
        ASSERT_TRUE((*c)->put(bytes(std::string_view(k)),
                              bytes(std::string_view(v)), 1000));
    }
    ASSERT_EQ((*c)->status().oki_delta_rows,
              static_cast<std::uint64_t>(kKeys))
        << "前置失效：memdelta 应未 flush（阈值 1M 行）";

    const auto t0 = std::chrono::steady_clock::now();
    for (int q = 0; q < kQueries; ++q) {
        // 窄窗口：每次只取 1 个 key——把「视图构建」成本与归并/取值成本
        // 分开，钉的正是视图构建不再逐查询重排 memdelta。
        char lo[16], hi[16];
        std::snprintf(lo, sizeof(lo), "m%06d", (q * 37) % kKeys);
        std::snprintf(hi, sizeof(hi), "m%06d", (q * 37) % kKeys + 1);
        auto out = range_scan(**c, lo, hi);
        ASSERT_EQ(out.size(), 1u);
    }
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(),
              10)
        << "1000 次窄窗口 range 耗时 "
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
               .count()
        << "ms——memdelta 视图疑似逐查询重排（悬崖回归）";
    (*c)->close();
}

// S64（下游反馈 2026-11-23，feedbacks/2026-11-23-range-iter-after-write-resorts-memdelta.md）：
// 写后首查不再整份重排 memdelta。此前每次写都让排序视图缓存作废，下一次开迭代器
// 把全部未落盘行拷一遍再排一遍——扫 1 行的 range 在 40 万未落盘行时 80 ms。
// 现在缓存是增量的（有序段 + 小尾巴），写后首查只整理新增的行。
// 钉工作量而不是墙钟：OkiState 记下视图整理处理过的行数。
TEST_F(OkiRangeTest, RangeAfterWriteDoesNotResortWholeMemdelta) {
    constexpr int kKeys = 50000;
    constexpr int kRounds = 300;

    CaskOptions o;
    o.read_write = true;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    for (int i = 0; i < kKeys; ++i) {
        char k[16];
        std::snprintf(k, sizeof(k), "m%06d", i);
        ASSERT_TRUE((*c)->put(bytes(std::string_view(k)), bytes("v"), 1000));
    }
    ASSERT_EQ((*c)->status().oki_delta_rows, static_cast<std::uint64_t>(kKeys));
    ASSERT_EQ(range_scan(**c, "m000007", "m000008").size(), 1u);  // 首建

    const auto before = bitcask::oki::OkiState::read_view_rows_processed_for_test();
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < kRounds; ++r) {
        // 写一条与被扫区间无关的 key，再扫 1 行。
        char k[16], lo[16], hi[16];
        std::snprintf(k, sizeof(k), "w%06d", r);
        ASSERT_TRUE((*c)->put(bytes(std::string_view(k)), bytes("x"), 1000));
        std::snprintf(lo, sizeof(lo), "m%06d", (r * 37) % kKeys);
        std::snprintf(hi, sizeof(hi), "m%06d", (r * 37) % kKeys + 1);
        ASSERT_EQ(range_scan(**c, lo, hi).size(), 1u);
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    const auto processed =
        bitcask::oki::OkiState::read_view_rows_processed_for_test() - before;
    std::cout << "[S64] " << kRounds << " rounds of put + 1-row range over " << kKeys
              << " unflushed rows: " << us / kRounds << " us/round, "
              << processed << " rows processed\n";
    // 旧实现每轮处理全部 5 万行（300 轮 = 1500 万行）。新实现每轮只碰尾巴（< 128 行）
    // 外加偶尔的小段归并。
    EXPECT_LT(processed, static_cast<std::uint64_t>(kKeys));
    // 写进去的 key 都在。
    EXPECT_EQ(range_scan(**c, "w", "x").size(), static_cast<std::size_t>(kRounds));
    (*c)->close();
}

// 增量视图的正确性：随机 put / 覆盖 / 删除穿过多次「尾巴升段」「相邻段归并」与
// checkpoint（active 被重排，缓存整体作废），每隔几步与影子 map、全表扫描三方对拍。
TEST_F(OkiRangeTest, IncrementalSortedViewMatchesShadowAcrossPromotionsAndFlush) {
    CaskOptions o;
    o.read_write = true;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    std::map<std::string, std::string> shadow;
    std::mt19937 rng(20261123);
    auto key_of = [](unsigned n) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "k%04u", n);
        return std::string(buf);
    };
    for (int step = 1; step <= 6000; ++step) {
        const std::string k = key_of(static_cast<unsigned>(rng() % 900));
        if (rng() % 4 == 0) {
            ASSERT_TRUE((*c)->remove(bytes(k), 2000));
            shadow.erase(k);
        } else {
            const std::string v = "v" + std::to_string(step);
            ASSERT_TRUE((*c)->put(bytes(k), bytes(v), 1000));
            shadow[k] = v;
        }
        if (step % 2000 == 0) {
            ASSERT_TRUE((*c)->checkpoint());
        }
        // 读的间隔不规则：有时每步都读（尾巴逐行长大），有时隔几百步（一次并进一大段）。
        const bool read_now = step < 400 || step % 53 == 0 ||
                              (step > 3000 && step < 3300) || step % 2000 == 1;
        if (!read_now) continue;
        unsigned a = static_cast<unsigned>(rng() % 900);
        unsigned b = static_cast<unsigned>(rng() % 900);
        if (a > b) std::swap(a, b);
        expect_three_way_equal(**c, shadow, key_of(a), key_of(b + 1), "window");
        if (step % 530 == 0 || step % 2000 == 1) {
            expect_three_way_equal(**c, shadow, "", "", "full");
        }
        if (::testing::Test::HasFailure()) {
            FAIL() << "first divergence at step " << step;
        }
    }
    expect_three_way_equal(**c, shadow, "", "", "final");
    (*c)->close();
}

// S65（feedbacks/2026-11-23-point-get-hit-cost-grows-with-unflushed-rows.md）：memdelta 点查
// 索引换成开放寻址的平表、活跃文件的命中读走映射。点查对模型：6 万个 key（平表多次
// 扩容重排）、覆盖写（同 key 取最新）、删除、checkpoint（active 重排，整表重建）、
// 小文件上限下的多次 roll（活跃文件换代，旧文件转 sealed 映射）交错；每一步之后抽查，
// 最后全量核对，命中的值与 LSN 都要对。
TEST_F(OkiRangeTest, PointGetMatchesModelAcrossIndexGrowthRebuildAndRoll) {
    CaskOptions o;
    o.read_write = true;
    o.max_file_size = 256 * 1024;  // 多次 roll
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    std::map<std::string, std::string> shadow;
    std::mt19937 rng(65);
    auto key_of = [](unsigned n) {
        char buf[24];
        // 一半 key 超过 15 字节（std::string 小串优化的边界）
        std::snprintf(buf, sizeof(buf), (n % 2) ? "k%05u" : "key-long-prefix-%05u", n);
        return std::string(buf);
    };
    auto check = [&](const std::string& k) {
        auto r = (*c)->get_owned(bytes(k));
        auto it = shadow.find(k);
        if (it == shadow.end()) {
            ASSERT_FALSE(r) << k;
            ASSERT_EQ(r.error().kind, bitcask::CaskError::kNotFound) << k;
        } else {
            ASSERT_TRUE(r) << k << ": " << r.error().detail;
            ASSERT_EQ(to_str(r->value), it->second) << k;
        }
    };
    constexpr unsigned kSpace = 60000;
    for (int step = 1; step <= 150000; ++step) {
        const std::string k = key_of(static_cast<unsigned>(rng() % kSpace));
        if (rng() % 5 == 0) {
            ASSERT_TRUE((*c)->remove(bytes(k), 2000));
            shadow.erase(k);
        } else {
            const std::string v = "v" + std::to_string(step);
            ASSERT_TRUE((*c)->put(bytes(k), bytes(v), 1000));
            shadow[k] = v;
        }
        if (step % 40000 == 0) {
            ASSERT_TRUE((*c)->checkpoint());
        }
        if (step % 7 == 0) {
            check(k);
            check(key_of(static_cast<unsigned>(rng() % kSpace)));
            if (::testing::Test::HasFatalFailure()) FAIL() << "at step " << step;
        }
    }
    for (unsigned n = 0; n < kSpace; ++n) {
        check(key_of(n));
        if (::testing::Test::HasFatalFailure()) FAIL() << "final, key " << n;
    }
    // 重开后（索引从 data / run 恢复）结果相同。
    (*c)->close();
    c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    for (unsigned n = 0; n < kSpace; n += 7) {
        check(key_of(n));
        if (::testing::Test::HasFatalFailure()) FAIL() << "after reopen, key " << n;
    }
    (*c)->close();
}

// 并发：写者只追加新 key（从不删），读者全域扫描。读视图的 active 切面是到达序的
// 一个前缀，所以读到的 key 集合必须是「0..n-1 连续」——中间缺一个就是增量视图在
// 并发整理 / 回填时丢了行。TSan 下同时钉分批拷贝与缓存回填的数据竞争。
TEST_F(OkiRangeTest, IncrementalSortedViewConcurrentReadersSeeContiguousPrefix) {
    constexpr int kKeys = 20000;
    CaskOptions o;
    o.read_write = true;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    std::atomic<bool> done{false};
    std::atomic<int> bad{0};
    std::atomic<std::uint64_t> scans{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t) {
        readers.emplace_back([&] {
            std::size_t last = 0;
            while (true) {
                const bool final_pass = done.load(std::memory_order_acquire);
                auto out = range_scan(**c, "p", "q");
                for (std::size_t i = 0; i < out.size(); ++i) {
                    char k[16];
                    std::snprintf(k, sizeof(k), "p%06zu", i);
                    if (out[i].first != k) {
                        bad.fetch_add(1);
                        break;
                    }
                }
                if (out.size() < last) bad.fetch_add(1);  // 同一读者不得倒退
                last = out.size();
                scans.fetch_add(1);
                if (final_pass) {
                    if (out.size() != static_cast<std::size_t>(kKeys)) bad.fetch_add(1);
                    break;
                }
            }
        });
    }
    for (int i = 0; i < kKeys; ++i) {
        char k[16];
        std::snprintf(k, sizeof(k), "p%06d", i);
        ASSERT_TRUE((*c)->put(bytes(std::string_view(k)), bytes("v"), 1000));
    }
    done.store(true, std::memory_order_release);
    for (auto& th : readers) th.join();
    EXPECT_EQ(bad.load(), 0);
    EXPECT_GT(scans.load(), 3u);
    (*c)->close();
}

namespace {

std::vector<std::string> oki_files_in(const fs::path& d) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
        const auto n = it->path().filename().string();
        if (n.find("oki") != std::string::npos) out.push_back(n);
    }
    return out;
}

// S54（勘误，feedbacks/2026-11-19-c-api-range-iter-no-index-comment-stale.md）：只读打开、
// 目录没有 OKI ⇒ open 期从 data 建内存读视图，range 照常遍历、⛔ 写回 OKI 文件；
// merge_only 句柄不建读视图 ⇒ kNoIndex（报文 ⛔ 再把只读算进去）。
TEST_F(OkiRangeTest, ReadOnlyWithoutOkiIteratesMergeOnlyHasNoIndex) {
    std::map<std::string, std::string> shadow;
    {
        CaskOptions o;
        o.read_write = true;
        auto c = Cask::open(dir_.string(), o, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 20; ++i) {
            const std::string k = "k" + std::string(i < 10 ? "0" : "") + std::to_string(i);
            const std::string v = "v" + std::to_string(i);
            ASSERT_TRUE((*c)->put(bytes(k), bytes(v), 1000));
            shadow[k] = v;
        }
        (*c)->close();
    }
    for (const auto& n : oki_files_in(dir_)) {
        std::error_code ec;
        fs::remove(dir_ / n, ec);
    }
    ASSERT_TRUE(oki_files_in(dir_).empty());

    {
        CaskOptions ro;
        ro.read_write = false;
        auto c = Cask::open(dir_.string(), ro, &test_registry());
        ASSERT_TRUE(c);
        EXPECT_EQ(range_scan(**c, "", ""), shadow_range(shadow, "", ""))
            << "只读、无 OKI ⇒ 内存读视图照常遍历（⛔ kNoIndex）";
        (*c)->close();
    }
    EXPECT_TRUE(oki_files_in(dir_).empty()) << "只读：内存视图，⛔ 写盘";

    {
        CaskOptions mo;
        mo.merge_only = true;
        auto c = Cask::open(dir_.string(), mo, &test_registry());
        ASSERT_TRUE(c);
        auto it = (*c)->make_range_iter(RangeOptions{});
        ASSERT_FALSE(it.has_value()) << "merge_only 不建读视图";
        EXPECT_EQ(it.error().kind, bitcask::CaskError::kNoIndex);
        EXPECT_EQ(it.error().detail.find("read-only or merge-only open"), std::string::npos)
            << "报文 ⛔ 再说只读打开会没有索引：" << it.error().detail;
        EXPECT_NE(it.error().detail.find("merge-only"), std::string::npos) << it.error().detail;
        (*c)->close();
    }
}

}  // namespace

// S76：range 取值按视图位置读 ⇒ 与 CaskIter 同为 ReadView 快照。迭代器建好之后的
// 改写 / 删除 / 新增都不可见（≤ 13.0.0 是 per-key 弱一致：删掉的跳过、改过的交新值）。
// 惰性与预取两条路径一致；close 之后 next() 报 kClosed（同 get）。
TEST_F(OkiRangeTest, S76RangeIsReadViewSnapshot) {
    CaskOptions o;
    o.read_write = true;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    for (int i = 0; i < 50; ++i) {
        char k[16];
        std::snprintf(k, sizeof k, "s%03d", i);
        ASSERT_TRUE((*c)->put(bytes(k), bytes(std::string("old") + k)));
    }
    const auto drain = [](CaskRangeIter& it) {
        KvVec out;
        while (true) {
            auto e = it.next();
            EXPECT_TRUE(e.has_value()) << (e ? "" : e.error().detail);
            if (!e || !e->has_value()) break;
            out.emplace_back(to_str((*e)->key), to_str((*e)->value));
        }
        return out;
    };
    for (std::size_t prefetch : {std::size_t{0}, std::size_t{16}}) {
        RangeOptions ro;
        ro.prefetch = prefetch;
        auto it = (*c)->make_range_iter(ro);
        ASSERT_TRUE(it);
        // 建好之后：改 s001、删 s002、新增 s0005（落在窗口里）
        ASSERT_TRUE((*c)->put(bytes("s001"), bytes("new")));
        ASSERT_TRUE((*c)->remove(bytes("s002")));
        ASSERT_TRUE((*c)->put(bytes("s0005"), bytes("added")));
        const auto got = drain(**it);
        ASSERT_EQ(got.size(), 50u) << "prefetch=" << prefetch;
        EXPECT_EQ(got[1], (std::pair<std::string, std::string>{"s001", "olds001"}));
        EXPECT_EQ(got[2], (std::pair<std::string, std::string>{"s002", "olds002"}));
        for (const auto& [k, v] : got) EXPECT_NE(k, "s0005");
        // 复原，给下一轮
        ASSERT_TRUE((*c)->put(bytes("s001"), bytes("olds001")));
        ASSERT_TRUE((*c)->put(bytes("s002"), bytes("olds002")));
        ASSERT_TRUE((*c)->remove(bytes("s0005")));
    }
    // 新建的迭代器看得到最新状态
    ASSERT_TRUE((*c)->put(bytes("s010"), bytes("latest")));
    const auto now = range_scan(**c, "s010", "s011");
    ASSERT_EQ(now.size(), 1u);
    EXPECT_EQ(now[0].second, "latest");

    auto it = (*c)->make_range_iter(RangeOptions{});
    ASSERT_TRUE(it);
    (*c)->close();
    auto e = (*it)->next();
    ASSERT_FALSE(e.has_value());
    EXPECT_EQ(e.error().kind, bitcask::CaskError::kClosed);
}
