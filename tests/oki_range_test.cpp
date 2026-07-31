// S33-5：CaskRangeIter 三方对拍 + 并发 stress。
// 对拍三方：make_range_iter(lo,hi) ×「CaskIter 全表 + 过滤 + 排序」×
// 「影子 std::map」。属性测试交错 put/覆盖写/remove/merge/close-reopen，
// 每轮随机窗口 + 全域窗口全比对——三方逐 key、逐 value 相等。
// 完整性不变量（OKI key 集 ⊇ keydir 活 key 集）由「全域 range 输出 ==
// 影子 map」直接蕴含（缺 key 即失败）。

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/keydir_registry.hpp>

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskOptions;
using bitcask::RangeOptions;

namespace {

bitcask::keydir::KeyDirRegistry& test_registry() {
    static bitcask::keydir::KeyDirRegistry reg;
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
                        ASSERT_TRUE(c->merge());
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
#ifdef BITCASK_TSAN_ACTIVE
    // TSan 下关闭 keydir 乐观读快路径（S29-6 回退开关）：seqlock 的
    // volatile 载入 × 写者搬移是 CI 既知豁免的误报同根因
    // （ci.yml TSan 豁免注释；专属 stress 为
    // KeyDirOptimisticRead.ConcurrentGetPutRemoveGrowStress）。本测试的
    // 对象是 OKI 层并发（归并/回查/merge 交互），不是乐观读——关闭后
    // 其余全部路径仍在 TSan 下受检，不新增豁免条目。
    c.keydir().set_optimistic_reads(false);
#endif
    for (int i = 0; i < 200; ++i) {
        ASSERT_TRUE(c.put(bytes("c" + std::to_string(i)), bytes("v"), 1000));
    }
    ASSERT_TRUE(c.checkpoint());  // 让读者有 run 可归并

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
