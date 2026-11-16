// OKI 回归：索引写入缺口 / 重建与合并语义（2026-11 OKI 审查修复）。
//
//   - 崩溃丢 memdelta 后，tail（最后一次提交之后的写）必须回到索引：
//     get 与 scan 都可见，且与 status.key_count 一致；
//   - run 全归并不得丢 v4 元数据 / data 水位；
//   - merge 删除墓碑 / TTL 过期记录后，data 重建不得复活旧值；
//   - data 重建在等 ord 时选 merge 副本（更大 file_id），不选退役输入；
//   - Level A 干净重开不做全量重建；
//   - 后台归并 / 无锁读视图 / 运行期重建与并发读写的一致性。
//
// 「崩溃」以 kill -9 等价方式模拟：句柄仍开着时复制目录（data 字节已在
// page cache，可见；未提交的 memdelta 丢失）。write.lock 不复制。

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>
#include <bitcask/oki_merge_cursor.hpp>
#include <bitcask/oki_run.hpp>
#include <bitcask/oki_state.hpp>

#include <thread>

namespace {

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskOptions;

bitcask::oki::IndexRegistry& test_registry() {
    static bitcask::oki::IndexRegistry registry;
    return registry;
}

std::span<const std::byte> bytes(const std::string& s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

class OkiRegressionTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        base_ = fs::temp_directory_path() /
                (std::string("bitcask_oki_regression_") + info->name());
        std::error_code ec;
        fs::remove_all(base_, ec);
        dir_ = base_ / "db";
        crash_ = base_ / "crash";
        fs::create_directories(dir_, ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(base_, ec);
    }

    static std::unique_ptr<Cask> open(const fs::path& d, CaskOptions o) {
        o.read_write = true;
        auto c = Cask::open(d.string(), o, &test_registry());
        EXPECT_TRUE(c.has_value()) << (c ? "" : c.error().detail);
        return c ? std::move(*c) : nullptr;
    }

    static void crash_copy(const fs::path& src, const fs::path& dst) {
        std::error_code ec;
        fs::remove_all(dst, ec);
        fs::create_directories(dst);
        for (const auto& e : fs::directory_iterator(src)) {
            const auto n = e.path().filename().string();
            if (n.find(".lock") != std::string::npos) continue;
            fs::copy(e.path(), dst / e.path().filename(),
                     fs::copy_options::recursive);
        }
    }

    static std::size_t scan_count(Cask& c) {
        std::atomic<std::size_t> n{0};
        auto r = c.parallel_scan(1, [&](auto, const auto&) { ++n; });
        EXPECT_TRUE(r.has_value());
        return n.load();
    }

    std::vector<std::string> data_files() const {
        std::vector<std::string> out;
        for (const auto& e : fs::directory_iterator(dir_)) {
            if (e.path().filename().string().ends_with(".bitcask.data")) {
                out.push_back(e.path().string());
            }
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            return std::stoul(fs::path(a).filename().string()) <
                   std::stoul(fs::path(b).filename().string());
        });
        return out;
    }

    void crash_tail(CaskOptions o, bool doc, int before_rounds = 1) {
        auto put = [&](Cask& c, const std::string& k) {
            if (doc) {
                bitcask::DocInput in;
                const std::string t = "hello world " + k;
                in.text = bytes(t);
                ASSERT_TRUE(c.put_doc(bytes(k), in).has_value());
            } else {
                ASSERT_TRUE(c.put(bytes(k), bytes("v" + k)).has_value());
            }
        };
        int committed = 0;
        for (int r = 0; r < before_rounds; ++r) {
            auto c = open(dir_, o);
            ASSERT_TRUE(c);
            for (int i = 0; i < 10; ++i) {
                put(*c, "a" + std::to_string(r) + "-" + std::to_string(i));
                ++committed;
            }
            c->close();
        }
        {
            auto c = open(dir_, o);
            ASSERT_TRUE(c);
            for (int i = 0; i < 50; ++i) put(*c, "b" + std::to_string(i));
            crash_copy(dir_, crash_);
            c->close();
        }
        auto r = open(crash_, o);
        ASSERT_TRUE(r);
        const std::size_t total = static_cast<std::size_t>(committed) + 50;
        EXPECT_EQ(scan_count(*r), total) << "scan 丢 tail";
        for (int i = 0; i < 50; ++i) {
            EXPECT_TRUE(r->get(bytes("b" + std::to_string(i))).has_value())
                << "b" << i << "：data 在、索引缺失";
        }
        EXPECT_EQ(r->status().key_count, total);
        r->close();
    }

    fs::path base_, dir_, crash_;
};

TEST_F(OkiRegressionTest, CrashTailLevelAKv) {
    crash_tail(CaskOptions{}, false);
}

TEST_F(OkiRegressionTest, CrashTailLevelASearch) {
    CaskOptions o;
    o.enable_search = true;
    crash_tail(o, true);
}

TEST_F(OkiRegressionTest, CrashTailLevelBKv) {
    CaskOptions o;
    o.keydir_cache_entries = 128;
    crash_tail(o, false);
}

TEST_F(OkiRegressionTest, CrashTailLevelBSearch) {
    CaskOptions o;
    o.keydir_cache_entries = 128;
    o.enable_search = true;
    crash_tail(o, true);
}

// 第 9 次提交触发 run 全归并；随后会话崩溃。原实现归并丢 v4 元数据，
// 重开退出 data-only 恢复，兜底路径又不补 tail → 50 个 key 永久缺失。
TEST_F(OkiRegressionTest, CrashTailAfterRunCompaction) {
    CaskOptions o;
    o.keydir_cache_entries = 128;
    crash_tail(o, false, /*before_rounds=*/9);
}

TEST_F(OkiRegressionTest, CompactionKeepsManifestMetadata) {
    CaskOptions o;
    o.keydir_cache_entries = 128;
    for (int round = 0; round < 12; ++round) {
        auto c = open(dir_, o);
        ASSERT_TRUE(c);
        ASSERT_TRUE(c->put(bytes("k" + std::to_string(round)), bytes("v")));
        c->close();
        auto m = bitcask::oki::read_manifest(dir_.string());
        ASSERT_TRUE(m.has_value());
        EXPECT_TRUE(m->metadata.has_value()) << "round " << round;
        EXPECT_FALSE(m->data_wms.empty()) << "round " << round;
        EXPECT_FALSE(m->fstats.empty()) << "round " << round;
        EXPECT_LE(m->runs.size(), bitcask::oki::OkiState::kCompactRunLimit);
    }
}

// 墓碑所在文件被合并、旧 put 所在文件未参与：墓碑必须随输出保留。
TEST_F(OkiRegressionTest, MergeKeepsTombstoneForUnmergedOlderPut) {
    CaskOptions o;
    o.keydir_cache_entries = 128;
    o.max_file_size = 1;  // 每次写独立 data 文件
    std::string tomb_file;
    {
        auto c = open(dir_, o);
        ASSERT_TRUE(c);
        ASSERT_TRUE(c->put(bytes("K"), bytes("old")));  // file 1
        ASSERT_TRUE(c->put(bytes("X"), bytes("x")));    // file 2
        ASSERT_TRUE(c->remove(bytes("K")));             // file 3（墓碑）
        ASSERT_TRUE(c->put(bytes("Y"), bytes("y")));    // file 4（active）
        const auto files = data_files();
        ASSERT_GE(files.size(), 4u);
        tomb_file = files[2];
        auto m = c->merge({tomb_file});
        ASSERT_TRUE(m.has_value()) << m.error().detail;
        EXPECT_FALSE(c->get(bytes("K")).has_value());
        c->close();
    }
    ASSERT_FALSE(fs::exists(tomb_file));
    // 任何 data 重建（manifest 丢失 / 损坏）都不得让 K 复活。
    fs::remove(bitcask::oki::mk_manifest_filename(dir_.string()));
    auto c = open(dir_, o);
    ASSERT_TRUE(c);
    EXPECT_FALSE(c->get(bytes("K")).has_value()) << "已删除的 K 复活";
    EXPECT_TRUE(c->get(bytes("X")).has_value());
    c->close();
}

// TTL 过期记录被 merge 丢弃时，更旧的版本不得在 data 重建后复活。
TEST_F(OkiRegressionTest, MergeExpiryDoesNotResurrectOlderVersion) {
    CaskOptions o;
    o.keydir_cache_entries = 128;
    o.max_file_size = 1;
    std::string expired_file;
    {
        auto c = open(dir_, o);
        ASSERT_TRUE(c);
        ASSERT_TRUE(c->put(bytes("K"), bytes("v1"), 1000));        // file 1
        ASSERT_TRUE(c->put(bytes("K"), bytes("v2"), 1000, 2000));  // file 2，2000 过期
        ASSERT_TRUE(c->put(bytes("Y"), bytes("y"), 1000));         // file 3（active）
        const auto files = data_files();
        ASSERT_GE(files.size(), 3u);
        expired_file = files[1];
        auto m = c->merge({expired_file}, /*now_sec=*/5000);
        ASSERT_TRUE(m.has_value()) << m.error().detail;
        EXPECT_EQ(m->records_expired, 1u);
        c->close();
    }
    ASSERT_FALSE(fs::exists(expired_file));
    fs::remove(bitcask::oki::mk_manifest_filename(dir_.string()));
    auto c = open(dir_, o);
    ASSERT_TRUE(c);
    auto g = c->get(bytes("K"));
    EXPECT_FALSE(g.has_value()) << "过期删除后旧版本 v1 复活";
    c->close();
}

// merge 退役文件尚未删除时崩溃 + manifest 丢失 → data 重建；等 ord 的
// merge 副本必须胜出（退役文件稍后被删，指向它的索引会悬空）。
TEST_F(OkiRegressionTest, RebuildPrefersMergeCopyOnEqualOrd) {
    CaskOptions o;
    o.keydir_cache_entries = 128;
    o.max_file_size = 1;
    std::uint32_t old_fid = 0;
    {
        auto c = open(dir_, o);
        ASSERT_TRUE(c);
        ASSERT_TRUE(c->put(bytes("K"), bytes("v1")));  // file 1
        ASSERT_TRUE(c->put(bytes("Z"), bytes("z")));   // file 2（active）
        const auto files = data_files();
        old_fid = static_cast<std::uint32_t>(
            std::stoul(fs::path(files[0]).filename().string()));
        ASSERT_TRUE(c->merge({files[0]}).has_value());
        crash_copy(dir_, crash_);  // 退休队列未 drain：旧文件仍在
        c->close();
    }
    fs::remove(bitcask::oki::mk_manifest_filename(crash_.string()));
    {
        auto c = open(crash_, o);
        ASSERT_TRUE(c);
        ASSERT_TRUE(c->get(bytes("K")).has_value());
        c->close();
    }
    auto m = bitcask::oki::read_manifest(crash_.string());
    ASSERT_TRUE(m.has_value());
    ASSERT_FALSE(m->runs.empty());
    auto rd = bitcask::oki::OkiRunReader::open(
        bitcask::oki::mk_run_filename(crash_.string(), m->runs.back().gen));
    ASSERT_TRUE(rd.has_value());
    const std::string k = "K";
    auto e = rd->find(bytes(k));
    ASSERT_TRUE(e.has_value() && e->has_value());
    EXPECT_NE((*e)->loc.file_id, old_fid) << "重建选中了退役输入文件";
}

TEST_F(OkiRegressionTest, LevelACleanReopenDoesNotRebuild) {
    {
        auto c = open(dir_, CaskOptions{});
        ASSERT_TRUE(c);
        for (int i = 0; i < 100; ++i) {
            ASSERT_TRUE(c->put(bytes("k" + std::to_string(i)), bytes("v")));
        }
        c->close();
    }
    auto gen_of = [&] {
        auto m = bitcask::oki::read_manifest(dir_.string());
        return (m && !m->runs.empty()) ? m->runs.back().gen : 0;
    };
    const auto g0 = gen_of();
    auto c = open(dir_, CaskOptions{});
    ASSERT_TRUE(c);
    EXPECT_EQ(gen_of(), g0) << "open 不应重建";
    ASSERT_TRUE(c->get(bytes("k1")).has_value());
    EXPECT_EQ(gen_of(), g0) << "首个 get 不应触发全量重建";
    EXPECT_EQ(scan_count(*c), 100u);
    c->close();
}

// ---------------------------------------------------------------------------
// 并发：后台归并、无锁读视图、运行期重建
// ---------------------------------------------------------------------------

std::size_t live_count(const bitcask::oki::OkiState::ReadView& v) {
    auto mc = bitcask::oki::OkiMergeCursor::open(v);
    EXPECT_TRUE(mc.has_value());
    if (!mc) return 0;
    std::size_t n = 0;
    bitcask::oki::OkiState::DeltaRow row;
    while (true) {
        auto r = mc->next(row);
        EXPECT_TRUE(r.has_value());
        if (!r || !*r) break;
        ++n;
    }
    return n;
}

// 越阈值的 flush 把全归并交给后台；期间并发读视图不得缺行（读视图不再持
// flush_mu_，靠「先 delta 后 runs」的发布序保证完整）。
TEST_F(OkiRegressionTest, AsyncCompactionKeepsReadViewsComplete) {
    using bitcask::oki::OkiState;
    using bitcask::oki::RowLoc;
    OkiState st;
    ASSERT_TRUE(st.rebuild(dir_.string(), {}, 0));
    std::atomic<std::size_t> published{0};
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> violations{0};
    std::thread reader([&] {
        while (!stop.load()) {
            const auto floor = published.load();
            auto v = st.make_read_view();
            if (!v) { ++violations; continue; }
            if (live_count(*v) < floor) ++violations;
        }
    });
    std::uint64_t ord = 0;
    constexpr int kRounds = 40, kPer = 25;
    for (int r = 0; r < kRounds; ++r) {
        for (int i = 0; i < kPer; ++i) {
            const RowLoc loc{1, 10, ord * 10, 1000};
            st.append("k" + std::to_string(r) + "-" + std::to_string(i), ord++,
                      false, &loc);
        }
        published.store(static_cast<std::size_t>((r + 1) * kPer));
        ASSERT_TRUE(st.flush(dir_.string(), {}, std::nullopt,
                             /*async_compaction=*/true));
    }
    st.wait_compaction();
    stop.store(true);
    reader.join();
    EXPECT_EQ(violations.load(), 0u) << "读视图在归并/flush 窗口内缺行";
    auto v = st.make_read_view();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(live_count(*v), static_cast<std::size_t>(kRounds * kPer));
    EXPECT_LE(st.run_count(), OkiState::kCompactRunLimit + 1);
    // 归并产物落盘：重新 load 后内容一致。
    OkiState again;
    again.load(dir_.string());
    ASSERT_TRUE(again.loaded());
    auto v2 = again.make_read_view();
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(live_count(*v2), static_cast<std::size_t>(kRounds * kPer));
}

// rebuild 同时替换 runs 与 delta：读视图经 view_epoch_ 检出窗口重试，
// 任何时刻都看到完整的 N 行。
TEST_F(OkiRegressionTest, ReadViewNeverTornByRebuild) {
    using bitcask::oki::OkiState;
    constexpr std::size_t kN = 300;
    auto rows = [&] {
        std::vector<OkiState::DeltaRow> out;
        for (std::size_t i = 0; i < kN; ++i) {
            out.push_back(OkiState::DeltaRow{
                "r" + std::to_string(i), i, false, true,
                bitcask::oki::RowLoc{1, 10, i * 10, 1000}});
        }
        return out;
    };
    OkiState st;
    ASSERT_TRUE(st.rebuild(dir_.string(), rows(), kN));
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> bad{0};
    std::thread reader([&] {
        while (!stop.load()) {
            auto v = st.make_read_view();
            if (!v || live_count(*v) != kN) ++bad;
        }
    });
    for (int i = 0; i < 30; ++i) ASSERT_TRUE(st.rebuild(dir_.string(), rows(), kN));
    stop.store(true);
    reader.join();
    EXPECT_EQ(bad.load(), 0u);
}

// 运行期 rebuild_from_data 与并发写：重建全程持写序闸门，写入不再被
// 「结束时清空 memdelta / 整体替换账本」吞掉。
TEST_F(OkiRegressionTest, RuntimeRebuildDoesNotLoseConcurrentWrites) {
    CaskOptions o;
    o.keydir_cache_entries = 128;
    auto c = open(dir_, o);
    ASSERT_TRUE(c);
    constexpr int kKeys = 1500;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (int i = 0; i < kKeys; ++i) {
            EXPECT_TRUE(c->put(bytes("w" + std::to_string(i)), bytes("v")));
        }
        done.store(true);
    });
    int rebuilds = 0;
    while (!done.load() || rebuilds < 3) {
        EXPECT_TRUE(c->index_authority().rebuild_from_data(dir_.string()));
        c->index_authority().state().enable_point_query();
        ++rebuilds;
    }
    writer.join();
    int missing = 0;
    for (int i = 0; i < kKeys; ++i) {
        if (!c->get(bytes("w" + std::to_string(i)))) ++missing;
    }
    EXPECT_EQ(missing, 0);
    EXPECT_EQ(c->status().key_count, static_cast<std::uint64_t>(kKeys));
    EXPECT_EQ(scan_count(*c), static_cast<std::size_t>(kKeys));
    c->close();
    auto r = open(dir_, o);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status().key_count, static_cast<std::uint64_t>(kKeys));
    EXPECT_EQ(scan_count(*r), static_cast<std::size_t>(kKeys));
    r->close();
}

// merge 后首个写不应先落进旧 active 再被拒收重写。
TEST_F(OkiRegressionTest, NoWastedWriteAfterMerge) {
    CaskOptions o;
    o.keydir_cache_entries = 128;
    auto c = open(dir_, o);
    ASSERT_TRUE(c);
    for (int i = 0; i < 20; ++i) ASSERT_TRUE(c->put(bytes("m" + std::to_string(i)), bytes("v")));
    ASSERT_TRUE(c->close_write_file());
    for (int i = 0; i < 20; ++i) ASSERT_TRUE(c->put(bytes("m" + std::to_string(i)), bytes("v2")));
    const auto before = data_files();
    ASSERT_GE(before.size(), 2u);
    ASSERT_TRUE(c->merge({before.front()}).has_value());
    // merge 输出 id 越过 active；下一次写应先换文件而非写进旧 active。
    const auto active_before = before.back();
    const auto size_before = fs::file_size(active_before);
    ASSERT_TRUE(c->put(bytes("after"), bytes("x")));
    EXPECT_EQ(fs::file_size(active_before), size_before)
        << "首个写不应落进已被 merge 输出越过的旧 active 文件";
    EXPECT_TRUE(c->get(bytes("after")).has_value());
    c->close();
}

}  // namespace
