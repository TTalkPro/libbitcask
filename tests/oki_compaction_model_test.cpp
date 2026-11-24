// S44：OKI 分级归并 + 读视图不可变发布的模型对拍与并发守门。
//
//   - 模型对拍（S44-0 底座，S44-3/4 加入后缀归并 / 两道并发）：随机 put /
//     remove / 同 ord 搬迁 / 条件墓碑 / flush / rebuild / reopen，与内存
//     模型逐 key 对比 locate 与迭代结果，并做 locate ⟺ 归并游标一致性检查；
//   - 设计 §4.7 的三个陷阱各有专项用例（墓碑保留、后缀连续、读视图成对）；
//   - 两条归并道、取消、背压、跨进程 load 重试。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <bitcask/oki_merge_cursor.hpp>
#include <bitcask/oki_run.hpp>
#include <bitcask/oki_state.hpp>

#include "support/oki_invariants.hpp"

namespace {

namespace fs = std::filesystem;
using bitcask::oki::CompactionMode;
using bitcask::oki::CompactionPolicyOptions;
using bitcask::oki::OkiMergeCursor;
using bitcask::oki::OkiState;
using bitcask::oki::RowLoc;
using Status = OkiState::LocateStatus;

std::span<const std::byte> bytes(const std::string& s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

CompactionPolicyOptions tiered(std::size_t max_runs = 6, double k = 1.0) {
    CompactionPolicyOptions o;
    o.mode = CompactionMode::kTiered;
    o.k = k;
    o.max_runs = max_runs;
    o.small_base_rows = 0;
    return o;
}

class OkiCompactionModelTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        // 目录名带进程级唯一后缀：不同构建树的同名用例可能同时在跑（例如
        // 两套 sanitizer 并行 ctest），只按用例名会写进同一目录互相干扰。
        dir_ = fs::temp_directory_path() /
               (std::string("bitcask_oki_s44_") + info->name() + "_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    std::unique_ptr<OkiState> fresh(const CompactionPolicyOptions& p) {
        auto st = std::make_unique<OkiState>();
        st->set_compaction_policy(p);
        EXPECT_TRUE(st->rebuild(dir_.string(), {}, 0));
        st->enable_point_query();
        return st;
    }
    std::unique_ptr<OkiState> reopen(const CompactionPolicyOptions& p) {
        auto st = std::make_unique<OkiState>();
        st->set_compaction_policy(p);
        st->load(dir_.string());
        EXPECT_TRUE(st->loaded());
        st->enable_point_query();
        return st;
    }

    fs::path dir_;
};

// ---------------------------------------------------------------------------
// 模型对拍
// ---------------------------------------------------------------------------

struct ModelRow {
    std::uint64_t ord = 0;
    bool tomb = false;
    RowLoc loc{};
};
using Model = std::map<std::string, ModelRow>;

::testing::AssertionResult check_against_model(const OkiState& st,
                                               const Model& model) {
    for (const auto& [k, m] : model) {
        const auto r = st.locate(k);
        if (m.tomb) {
            // 墓碑：仍以墓碑行存在，或已被全归并回收（缺席）。
            const bool ok = r.status == Status::kMiss ||
                            (r.status == Status::kHit && r.tomb && r.ord == m.ord);
            if (!ok) {
                return ::testing::AssertionFailure()
                       << "deleted key " << k << " visible: status "
                       << static_cast<int>(r.status) << " tomb " << r.tomb
                       << " ord " << r.ord << " (model ord " << m.ord << ")";
            }
        } else {
            if (r.status != Status::kHit || r.tomb || r.ord != m.ord ||
                r.loc.file_id != m.loc.file_id || r.loc.offset != m.loc.offset) {
                return ::testing::AssertionFailure()
                       << "live key " << k << ": status "
                       << static_cast<int>(r.status) << " tomb " << r.tomb
                       << " ord " << r.ord << " file " << r.loc.file_id
                       << " (model ord " << m.ord << " file " << m.loc.file_id << ")";
            }
        }
    }
    // 迭代结果 = 模型活集（位置一致）。
    auto v = st.make_read_view();
    if (!v) return ::testing::AssertionFailure() << "no view";
    auto mc = OkiMergeCursor::open(*v);
    if (!mc) return ::testing::AssertionFailure() << "cursor";
    std::size_t live = 0;
    for (const auto& [k, m] : model) live += m.tomb ? 0 : 1;
    std::size_t seen = 0;
    OkiState::DeltaRow row;
    while (true) {
        auto n = mc->next(row);
        if (!n) return ::testing::AssertionFailure() << "cursor read";
        if (!*n) break;
        auto it = model.find(row.key);
        if (it == model.end() || it->second.tomb || it->second.ord != row.ord ||
            it->second.loc.file_id != row.loc.file_id) {
            return ::testing::AssertionFailure()
                   << "iteration yields stale/unknown key " << row.key;
        }
        ++seen;
    }
    if (seen != live) {
        return ::testing::AssertionFailure()
               << "iteration " << seen << " keys, model live " << live;
    }
    return bitcask_test::locate_matches_cursor(st);
}

void run_model(const fs::path& dir, const CompactionPolicyOptions& policy,
               std::uint64_t seed, int steps,
               std::unique_ptr<OkiState> (*make)(const fs::path&,
                                                 const CompactionPolicyOptions&,
                                                 bool)) {
    std::mt19937_64 rng(seed);
    auto st = make(dir, policy, /*fresh=*/true);
    Model model;
    std::uint64_t next_ord = 0;
    std::uniform_int_distribution<int> keyd(0, 199);
    std::uniform_int_distribution<int> op(0, 99);
    std::uniform_int_distribution<std::uint32_t> filed(1, 50);
    for (int step = 0; step < steps; ++step) {
        const std::string k = "k" + std::to_string(keyd(rng));
        const int o = op(rng);
        auto it = model.find(k);
        const bool live = it != model.end() && !it->second.tomb;
        if (o < 50) {  // put
            const auto ord = next_ord++;
            const RowLoc loc{filed(rng), 10, ord * 16, 1000 + ord};
            st->apply_put(k, ord, loc);
            model[k] = ModelRow{ord, false, loc};
        } else if (o < 62) {  // remove
            const auto ord = next_ord++;
            st->apply_remove(k, ord);
            model[k] = ModelRow{ord, true, {}};
        } else if (o < 72 && live) {  // merge 搬迁：同 ord、新位置
            const RowLoc loc{100 + filed(rng), 10, it->second.ord * 16 + 8,
                             it->second.loc.tstamp};
            st->apply_relocation(k, it->second.ord, loc);
            it->second.loc = loc;
        } else if (o < 76 && live) {  // TTL 条件墓碑：受害者 ord
            st->apply_conditional_remove(k, it->second.ord);
            it->second.tomb = true;
            it->second.loc = {};
        } else if (o < 92) {  // flush（会调度后台归并）
            ASSERT_TRUE(st->flush(dir.string()));
        } else if (o < 95) {  // rebuild：从模型活集重建
            std::vector<OkiState::DeltaRow> rows;
            for (const auto& [mk, m] : model) {
                if (!m.tomb) rows.push_back(OkiState::DeltaRow{mk, m.ord, false, true, m.loc});
            }
            ASSERT_TRUE(st->rebuild(dir.string(), std::move(rows), next_ord));
            // rebuild 不保留墓碑：模型里的墓碑等价于缺席，保留不影响判定。
        } else if (o < 97) {  // reopen：落盘后换一个进程视角
            ASSERT_TRUE(st->flush(dir.string()));
            st.reset();  // 析构：宽限后取消在途归并
            st = make(dir, policy, /*fresh=*/false);
        } else {  // 校验（后台归并可能正在跑：内容等价，结果必须一致）
            ASSERT_TRUE(check_against_model(*st, model)) << "seed " << seed
                                                          << " step " << step;
        }
    }
    st->wait_compaction();
    ASSERT_TRUE(check_against_model(*st, model)) << "seed " << seed << " final";
    ASSERT_TRUE(st->flush(dir.string()));
    st.reset();
    st = make(dir, policy, false);
    ASSERT_TRUE(check_against_model(*st, model)) << "seed " << seed << " reopen";
}

std::unique_ptr<OkiState> make_state(const fs::path& dir,
                                     const CompactionPolicyOptions& p,
                                     bool is_fresh) {
    auto st = std::make_unique<OkiState>();
    st->set_compaction_policy(p);
    if (is_fresh) {
        EXPECT_TRUE(st->rebuild(dir.string(), {}, 0));
    } else {
        st->load(dir.string());
        EXPECT_TRUE(st->loaded());
    }
    st->enable_point_query();
    return st;
}

TEST_F(OkiCompactionModelTest, ModelTieredSmallCap) {
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
        run_model(dir_, tiered(/*max_runs=*/5), seed, 3000, &make_state);
        if (HasFatalFailure()) return;
    }
}

TEST_F(OkiCompactionModelTest, ModelTieredAggressiveK) {
    for (std::uint64_t seed = 11; seed <= 14; ++seed) {
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
        run_model(dir_, tiered(/*max_runs=*/24, /*k=*/4.0), seed, 3000, &make_state);
        if (HasFatalFailure()) return;
    }
}

TEST_F(OkiCompactionModelTest, ModelFullPolicy) {
    CompactionPolicyOptions full;
    full.mode = CompactionMode::kFull;
    for (std::uint64_t seed = 21; seed <= 24; ++seed) {
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
        run_model(dir_, full, seed, 3000, &make_state);
        if (HasFatalFailure()) return;
    }
}

// ---------------------------------------------------------------------------
// 设计 §4.7 陷阱 1 / 2：后缀归并保留墓碑、搬迁行跨后缀边界
// ---------------------------------------------------------------------------

TEST_F(OkiCompactionModelTest, SuffixMergeKeepsTombstoneAndRelocation) {
    auto st = fresh(tiered(24));
    // R0：100 个 key（底座）。
    for (std::uint64_t i = 0; i < 100; ++i) {
        st->apply_put("k" + std::to_string(i), i, RowLoc{1, 10, i * 16, 1000});
    }
    ASSERT_TRUE(st->flush(dir_.string()));
    // R1：删除 k5（put 在底座）；k7 搬迁到新位置（原行在底座）。
    st->apply_remove("k5", 100);
    st->apply_relocation("k7", 7, RowLoc{9, 10, 999, 1000});
    ASSERT_TRUE(st->flush(dir_.string()));
    // R2：一个新 key —— [100, 2, 1] 不触发；再来一个 [100, 2, 1, 1] → 并 R2..R3，
    // 再 [100, 2, 2] → 并 R1..R2：后缀不含底座。
    st->apply_put("x1", 101, RowLoc{2, 10, 0, 1000});
    ASSERT_TRUE(st->flush(dir_.string()));
    st->apply_put("x2", 102, RowLoc{2, 10, 16, 1000});
    ASSERT_TRUE(st->flush(dir_.string()));
    st->wait_compaction();

    const auto cs = st->compaction_stats();
    ASSERT_GE(cs.compactions, 1u);
    EXPECT_EQ(cs.full_compactions, 0u) << "本用例要求后缀不含底座";
    EXPECT_GE(cs.tomb_rows_written, 1u) << "非全归并必须保留墓碑行";
    EXPECT_EQ(st->run_count(), 2u);

    auto k5 = st->locate("k5");
    EXPECT_TRUE(k5.status == Status::kHit && k5.tomb) << "陷阱 1：已删除的 k5 复活";
    auto k7 = st->locate("k7");
    ASSERT_EQ(k7.status, Status::kHit);
    EXPECT_EQ(k7.loc.file_id, 9u) << "陷阱 2：搬迁后的新位置被旧位置压过";
    EXPECT_TRUE(bitcask_test::locate_matches_cursor(*st));

    // 重开后（另一进程视角）同样成立。
    st.reset();
    st = reopen(tiered(24));
    k5 = st->locate("k5");
    EXPECT_TRUE(k5.status == Status::kHit && k5.tomb);
    EXPECT_EQ(st->locate("k7").loc.file_id, 9u);
    EXPECT_TRUE(bitcask_test::locate_matches_cursor(*st));

    // 全归并（含底座）之后墓碑才真正消失。
    for (int i = 0; i < 6; ++i) {
        st->apply_put("y" + std::to_string(i), 200 + static_cast<std::uint64_t>(i),
                      RowLoc{3, 10, static_cast<std::uint64_t>(i) * 16, 1000});
        ASSERT_TRUE(st->flush(dir_.string()));
    }
    CompactionPolicyOptions full;
    full.mode = CompactionMode::kFull;
    full.full_limit = 1;
    st->set_compaction_policy(full);
    st->apply_put("z", 300, RowLoc{3, 10, 999, 1000});
    ASSERT_TRUE(st->flush(dir_.string()));
    st->wait_compaction();
    EXPECT_EQ(st->locate("k5").status, Status::kMiss);
    EXPECT_EQ(st->locate("k7").loc.file_id, 9u);
}

// 「输出 run 已写、manifest 未提交」的崩溃残件：下次归并提交时被清理。
TEST_F(OkiCompactionModelTest, OrphanRunSweptOnNextCompaction) {
    auto st = fresh(tiered(24));
    for (std::uint64_t i = 0; i < 50; ++i) {
        st->apply_put("k" + std::to_string(i), i, RowLoc{1, 10, i * 16, 1000});
    }
    ASSERT_TRUE(st->flush(dir_.string()));
    const auto orphan = bitcask::oki::mk_run_filename(dir_.string(), 9999);
    { std::FILE* f = std::fopen(orphan.c_str(), "wb"); ASSERT_NE(f, nullptr); std::fclose(f); }
    for (std::uint64_t i = 0; i < 3; ++i) {
        st->apply_put("n" + std::to_string(i), 100 + i, RowLoc{2, 10, i * 16, 1000});
        ASSERT_TRUE(st->flush(dir_.string()));
    }
    st->wait_compaction();
    ASSERT_GE(st->compaction_stats().compactions, 1u);
    EXPECT_FALSE(fs::exists(orphan)) << "崩溃残留的 run 应被 sweep";
    EXPECT_EQ(st->locate("k3").status, Status::kHit);
    EXPECT_EQ(st->locate("n2").status, Status::kHit);
}

// ---------------------------------------------------------------------------
// 设计 §4.7 陷阱 3：读视图成对（S44-1）
// ---------------------------------------------------------------------------

// 一个视图里，某 key 的胜出位置若是旧文件(1)，则同视图的 run / 冻结段里不得
// 已有同 ord 的新位置(2)。
std::size_t count_inversions(const OkiState::ReadView& v) {
    auto mc = OkiMergeCursor::open(v);
    if (!mc) return 0;
    std::size_t bad = 0;
    OkiState::DeltaRow row;
    while (true) {
        auto n = mc->next(row);
        if (!n || !*n) break;
        if (row.loc.file_id != 1) continue;
        for (const auto& run : v.runs) {
            auto e = run->find(bytes(row.key));
            if (e && e->has_value() && (*e)->ord == row.ord &&
                (*e)->loc.file_id == 2) {
                ++bad;
            }
        }
        for (const auto& seg : v.frozen) {
            for (const auto& r : *seg) {
                if (r.key == row.key && r.ord == row.ord && r.loc.file_id == 2) ++bad;
            }
        }
    }
    return bad;
}

TEST_F(OkiCompactionModelTest, ReadViewPairingControlGroup) {
    // 确定性对照组：旧 active 快照（含原始行）配新 runs（含同 ord 搬迁行）
    // 必然判反——证明检查器有分辨力。真实视图不得判反。
    auto st = fresh(tiered(24));
    st->apply_put("e", 1, RowLoc{1, 10, 0, 1000});
    auto stale = st->make_read_view();
    ASSERT_TRUE(stale.has_value());
    st->apply_relocation("e", 1, RowLoc{2, 10, 0, 1000});
    ASSERT_TRUE(st->flush(dir_.string()));
    auto now = st->make_read_view();
    ASSERT_TRUE(now.has_value());
    EXPECT_EQ(count_inversions(*now), 0u);
    OkiState::ReadView torn{now->runs, now->frozen, stale->delta, now->gens, now->cache};
    EXPECT_GT(count_inversions(torn), 0u) << "对照组未判反——检查器无分辨力";
    // 点查同理。
    EXPECT_EQ(st->locate("e").loc.file_id, 2u);
}

TEST_F(OkiCompactionModelTest, ReadViewNeverInvertedUnderConcurrentFlush) {
    auto st = fresh(tiered(24));
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> inverted{0}, locate_bad{0}, views{0};
    std::thread reader([&] {
        while (!stop.load()) {
            auto v = st->make_read_view();
            if (v) {
                inverted += count_inversions(*v);
                ++views;
            }
        }
    });
    std::thread prober([&] {
        std::mt19937 rng(7);
        while (!stop.load()) {
            const auto k = "e" + std::to_string(rng() % 300);
            // 点查：若命中旧位置，则同 ord 的新位置此刻必然还不存在于任何来源
            // ——无法直接验证，转而验证「命中位置不倒退」：一旦见过新位置，
            // 之后不得再见旧位置。
            thread_local std::map<std::string, bool> seen_new;
            const auto r = st->locate(k);
            if (r.status != Status::kHit) continue;
            if (r.loc.file_id == 2) seen_new[k] = true;
            else if (r.loc.file_id == 1 && seen_new[k]) ++locate_bad;
        }
    });
    for (std::uint64_t i = 0; i < 300; ++i) {
        const auto k = "e" + std::to_string(i);
        st->apply_put(k, i, RowLoc{1, 10, i * 16, 1000});
        std::this_thread::yield();
        st->apply_relocation(k, i, RowLoc{2, 10, i * 16, 1000});
        ASSERT_TRUE(st->flush(dir_.string()));
    }
    stop.store(true);
    reader.join();
    prober.join();
    st->wait_compaction();
    EXPECT_GT(views.load(), 0u);
    EXPECT_EQ(inverted.load(), 0u);
    EXPECT_EQ(locate_bad.load(), 0u) << "点查位置倒退：旧行压过同 ord 新行";
    EXPECT_TRUE(bitcask_test::locate_matches_cursor(*st));
}

// flush 期间（冻结段已发布、run 尚未落盘）读者可见全部行。
TEST_F(OkiCompactionModelTest, FrozenSegmentVisibleDuringFlushIo) {
    auto st = fresh(tiered(24));
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> short_views{0};
    std::atomic<std::size_t> published{0};
    std::thread reader([&] {
        while (!stop.load()) {
            const auto floor = published.load();
            auto v = st->make_read_view();
            if (!v) continue;
            auto mc = OkiMergeCursor::open(*v);
            std::size_t n = 0;
            OkiState::DeltaRow row;
            while (mc && *mc->next(row)) ++n;
            if (n < floor) ++short_views;
        }
    });
    std::uint64_t ord = 0;
    for (int r = 0; r < 60; ++r) {
        for (int i = 0; i < 50; ++i) {
            st->apply_put("f" + std::to_string(ord), ord, RowLoc{1, 10, ord * 16, 1000});
            ++ord;
        }
        published.store(ord);
        ASSERT_TRUE(st->flush(dir_.string()));
    }
    stop.store(true);
    reader.join();
    st->wait_compaction();
    EXPECT_EQ(short_views.load(), 0u);
}

// ---------------------------------------------------------------------------
// S44-4：两条归并道 / 取消 / 背压
// ---------------------------------------------------------------------------

TEST_F(OkiCompactionModelTest, YoungLaneRunsWhileMainCompactionInFlight) {
    auto st = fresh(tiered(/*max_runs=*/24, /*k=*/1000.0));
    std::atomic<bool> release_main{false};
    std::atomic<bool> main_entered{false};
    OkiState* raw = st.get();  // reset() 先置空 unique_ptr 再析构：钩子不能经 st 访问
    st->set_compaction_row_hook_for_test([&, raw](bool young, std::uint64_t) {
        if (young) return;
        main_entered.store(true);
        while (!release_main.load() && !raw->cancel_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    Model model;
    std::uint64_t ord = 0;
    const auto put = [&](const std::string& k) {
        const RowLoc loc{1, 10, ord * 16, 1000};
        st->apply_put(k, ord, loc);
        model[k] = ModelRow{ord, false, loc};
        ++ord;
    };
    for (int i = 0; i < 2000; ++i) put("base" + std::to_string(i));
    ASSERT_TRUE(st->flush(dir_.string()));
    for (int i = 0; i < 10; ++i) put("trig" + std::to_string(i));
    ASSERT_TRUE(st->flush(dir_.string()));  // [2000, 10] → 主道全归并（被钩子挡住）
    for (int spin = 0; !main_entered.load() && spin < 5000; ++spin) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(main_entered.load());
    // 主道在途期间持续 flush 小 run：年轻道应收拾它们。
    for (int r = 0; r < 8; ++r) {
        for (int i = 0; i < 5; ++i) put("young" + std::to_string(r) + "_" + std::to_string(i));
        ASSERT_TRUE(st->flush(dir_.string()));
    }
    for (int spin = 0; st->compaction_stats().young_compactions == 0 && spin < 5000; ++spin) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GE(st->compaction_stats().young_compactions, 1u);
    EXPECT_TRUE(check_against_model(*st, model)) << "主道在途 + 年轻道提交后";
    release_main.store(true);
    st->wait_compaction();
    EXPECT_GE(st->compaction_stats().full_compactions, 1u);
    EXPECT_TRUE(check_against_model(*st, model));
    st.reset();
    st = reopen(tiered(24, 1000.0));
    EXPECT_TRUE(check_against_model(*st, model));
}

TEST_F(OkiCompactionModelTest, DestructorCancelsLongCompactionBounded) {
    Model model;
    {
        auto st = fresh(tiered(24, 1000.0));
        std::uint64_t ord = 0;
        for (int i = 0; i < 3000; ++i) {
            const auto k = "c" + std::to_string(i);
            const RowLoc loc{1, 10, ord * 16, 1000};
            st->apply_put(k, ord, loc);
            model[k] = ModelRow{ord, false, loc};
            ++ord;
        }
        ASSERT_TRUE(st->flush(dir_.string()));
        std::atomic<bool> entered{false};
        OkiState* raw = st.get();  // reset() 先置空 unique_ptr 再析构
        st->set_compaction_row_hook_for_test([&, raw](bool, std::uint64_t) {
            entered.store(true);
            while (!raw->cancel_requested()) {  // 「永不结束」的大归并
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        for (int i = 0; i < 10; ++i) {
            const auto k = "d" + std::to_string(i);
            const RowLoc loc{1, 10, ord * 16, 1000};
            st->apply_put(k, ord, loc);
            model[k] = ModelRow{ord, false, loc};
            ++ord;
        }
        ASSERT_TRUE(st->flush(dir_.string()));
        for (int spin = 0; !entered.load() && spin < 5000; ++spin) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_TRUE(entered.load());
        const auto t0 = std::chrono::steady_clock::now();
        st.reset();  // 宽限期后取消
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        EXPECT_LT(ms, 3000) << "close 必须有上界";
    }
    auto st = reopen(tiered(24, 1000.0));
    EXPECT_EQ(st->run_count(), 2u) << "取消的归并不得改动输入";
    EXPECT_TRUE(check_against_model(*st, model));
}

TEST_F(OkiCompactionModelTest, BackpressureBoundsRunCount) {
    auto policy = tiered(/*max_runs=*/4, /*k=*/1.0);
    policy.stall_runs = 4;  // 背压阈值 = 目标上限（默认是其 2 倍）
    auto st = fresh(policy);
    st->set_compaction_row_hook_for_test([](bool, std::uint64_t) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));  // 归并偏慢
    });
    Model model;
    std::uint64_t ord = 0;
    // 递减大小的 run：几何规则不并，只能靠硬上限 + 背压收敛。
    for (int r = 0; r < 12; ++r) {
        const int n = 4096 >> std::min(r, 11);
        for (int i = 0; i < n; ++i) {
            const auto k = "b" + std::to_string(ord);
            const RowLoc loc{1, 10, ord * 16, 1000};
            st->apply_put(k, ord, loc);
            model[k] = ModelRow{ord, false, loc};
            ++ord;
        }
        ASSERT_TRUE(st->flush(dir_.string()));
        EXPECT_LE(st->run_count(), policy.max_runs + 1)
            << "flush 返回后 run 数必须被背压压住（round " << r << "）";
    }
    st->wait_compaction();
    EXPECT_LE(st->run_count(), policy.max_runs);
    EXPECT_GE(st->compaction_stats().backpressure_waits, 1u);
    EXPECT_TRUE(check_against_model(*st, model));
}

// ---------------------------------------------------------------------------
// S44-5：跨进程 load 重试
// ---------------------------------------------------------------------------

// 交叠由条件保证，不靠调度运气（feedbacks/2026-11-23-two-timing-sensitive-tests-fail-under-full-load.md：
// 满载时写线程可能整段被饿住，1000 次 load 在一次归并都没发生时就做完了，
// `compactions >= 1` 假红；没红时 load 也没撞上 sweep，断言空转）：
//   - 写者做完第一次 flush 才开始 load；
//   - load 一直做到「≥ kMinLoads 次，且 load 期间写者提交了 ≥ kMinCompactions
//     次归并」——归并之后才有 sweep，load 撞 sweep 才是要测的；期限 kDeadline；
//   - 期限到仍没交叠够（极端饥饿）⇒ 跳过而不判失败；unloaded 照样严格检查。
TEST_F(OkiCompactionModelTest, ConcurrentLoaderNeverFallsBackToUnloaded) {
    constexpr int kMinLoads = 1000;
    constexpr std::uint64_t kMinCompactions = 3;
    constexpr auto kDeadline = std::chrono::seconds(120);

    auto writer = fresh(tiered(/*max_runs=*/4, /*k=*/1.0));
    std::atomic<bool> stop{false};
    std::atomic<bool> writer_failed{false};
    std::atomic<std::uint64_t> flushes{0};
    std::thread w([&] {
        std::uint64_t ord = 0;
        while (!stop.load()) {
            for (int i = 0; i < 20; ++i) {
                writer->apply_put("w" + std::to_string(ord % 500), ord,
                                  RowLoc{1, 10, ord * 16, 1000});
                ++ord;
            }
            if (!writer->flush(dir_.string())) {
                writer_failed.store(true);
                return;
            }
            flushes.fetch_add(1);
        }
    });
    while (flushes.load() == 0 && !writer_failed.load()) std::this_thread::yield();

    const auto compactions = [&] { return writer->compaction_stats().compactions; };
    const std::uint64_t base = compactions();
    int loads = 0, unloaded = 0;
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    while (!writer_failed.load() && std::chrono::steady_clock::now() < deadline &&
           (loads < kMinLoads || compactions() - base < kMinCompactions)) {
        OkiState r;  // 另一个「进程」：只读加载同一目录
        r.load(dir_.string());
        ++loads;
        if (!r.loaded()) ++unloaded;
    }
    const std::uint64_t overlapped = compactions() - base;
    stop.store(true);
    w.join();
    writer->wait_compaction();
    EXPECT_FALSE(writer_failed.load()) << "写者 flush 失败";
    EXPECT_EQ(unloaded, 0) << "load 撞上 sweep 后应重读 manifest 重试";
    if (loads < kMinLoads || overlapped < kMinCompactions) {
        GTEST_SKIP() << "前提没达成（调度饥饿）：" << loads << " 次 load 期间只有 "
                     << overlapped << " 次归并";
    }
}

}  // namespace
