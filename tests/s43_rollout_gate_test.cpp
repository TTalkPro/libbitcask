// S43-10 rollout gate：把 S43-0 定义的崩溃矩阵收敛为可重复执行的验收面。
//
// 这里不是替代既有细粒度 CRC/merge/并发测试，而是把发布前必须成立的
// 参考模型与 K1-K15 逐项注入集中起来。所有“崩溃”场景都通过 exec-self
// 子进程在真实进程终止点收尾；文件级窗口用上一提交点/孤儿文件显式复现。

#include "support/crash_child.hpp"

using bitcask::test::crash_exit;

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <ctime>
#include <vector>

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>
#include <bitcask/oki_run.hpp>

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskError;
using bitcask::CaskOptions;

namespace {

bitcask::oki::IndexRegistry& test_registry() {
    static bitcask::oki::IndexRegistry reg;
    return reg;
}

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string as_string(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

std::string key_for(int i) { return "k" + std::to_string(i); }
std::string value_for(int i) { return "v" + std::to_string(i); }

std::vector<char> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

void write_bytes(const fs::path& path, std::span<const char> data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    ASSERT_TRUE(out.good());
}

CaskOptions rollout_opts(std::size_t cache = 128) {
    CaskOptions o;
    o.read_write = true;
    o.keydir_cache_entries = cache;
    o.max_file_size = 16 * 1024;
    return o;
}

std::optional<std::string> get_optional(Cask& c, std::string_view key) {
    auto r = c.get_owned(bytes(key));
    if (r) return as_string(r->value);
    EXPECT_EQ(r.error().kind, CaskError::kNotFound)
        << key << ": " << r.error().detail;
    return std::nullopt;
}

std::map<std::string, std::string> range_model(Cask& c) {
    auto it = c.make_range_iter(bitcask::RangeOptions{});
    EXPECT_TRUE(it.has_value());
    std::map<std::string, std::string> out;
    if (!it.has_value()) return out;
    while (true) {
        auto e = (*it)->next();
        EXPECT_TRUE(e.has_value());
        if (!e.has_value() || !*e) break;
        out.emplace(as_string((*e)->key), as_string((*e)->value));
    }
    return out;
}

void expect_model(Cask& c,
                  const std::map<std::string, std::string>& model) {
    for (const auto& [key, value] : model) {
        EXPECT_EQ(get_optional(c, key), value) << key;
    }
    EXPECT_EQ(range_model(c), model);
}

class S43RolloutGateTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() /
               (std::string("bitcask_s43_rollout_") + info->name());
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    std::unique_ptr<Cask> open_rw() {
        auto opened = Cask::open(dir_.string(), rollout_opts(),
                                 &test_registry());
        EXPECT_TRUE(opened) << (opened ? "" : opened.error().detail);
        return opened ? *std::move(opened) : nullptr;
    }

    fs::path dir_;
};

// K1/K2：每条 put+sync 都已进入 data；OKI 只在进程内存 delta 中或根本
// 未提交。崩溃后不允许丢这些活跃 key，恢复按 LWW/幂等收敛到同一状态。
BITCASK_CRASH_SCENARIO(k1_k2_data_without_oki_commit) {
    auto c = Cask::open(dir, rollout_opts(), &test_registry());
    if (!c) crash_exit(1);
    for (int i = 0; i < 24; ++i) {
        const auto tstamp = static_cast<std::uint64_t>(1000 + i);
        if (!(*c)->put(bytes(key_for(i)), bytes(value_for(i)), tstamp) ||
            !(*c)->sync()) {
            crash_exit(1);
        }
    }
    crash_exit(0);
}

// K3/K4：pending data 已 fsync，但 run/manifest 发布在 directory 阻塞点
// 失败；随后进程崩溃。失败只留下孤儿 run，旧 commit 不回退 pending 数据。
BITCASK_CRASH_SCENARIO(k3_k4_run_before_manifest_commit) {
    auto c = Cask::open(dir, rollout_opts(), &test_registry());
    if (!c) crash_exit(1);
    if (!(*c)->put(bytes("committed"), bytes("old"), 1000) ||
        !(*c)->sync() || !(*c)->checkpoint()) {
        crash_exit(1);
    }
    const auto old_manifest = read_file(
        bitcask::oki::mk_manifest_filename(dir));
    if (old_manifest.empty()) crash_exit(2);

    if (!(*c)->put(bytes("pending"), bytes("new"), 2000) || !(*c)->sync()) {
        crash_exit(1);
    }
    std::error_code ec;
    fs::create_directory(dir + "/kv.oki.manifest.tmp", ec);
    if (ec || !(*c)->checkpoint()) crash_exit(3);
    const auto still_old = bitcask::oki::read_manifest(dir);
    if (!still_old.has_value() || still_old->metadata->active_keys != 1) {
        crash_exit(4);  // K4 注入失败：提交点越过了阻塞。
    }
    crash_exit(0);
}

// K10：sync_every_n=1 使组提交阈值在每个写边界触发；进程在若干提交后
// 立即终止，恢复 flush 不能跳过已完整记录。
BITCASK_CRASH_SCENARIO(k10_recovery_flush_boundary) {
    CaskOptions o = rollout_opts();
    o.sync_every_n = 1;
    auto c = Cask::open(dir, o, &test_registry());
    if (!c) crash_exit(1);
    for (int i = 0; i < 37; ++i) {
        if (!(*c)->put(bytes(key_for(i)), bytes(value_for(i)), 1000)) {
            crash_exit(1);
        }
    }
    crash_exit(0);
}

// K11/K12/K13：merge 输出已存在，但把 manifest 强制回退到 merge 前提交点，
// 并在子进程不 close（输入/退休文件保持在场）。新输出是未引用残件；
// 旧输入与旧 OKI 仍必须构成一致状态。
BITCASK_CRASH_SCENARIO(k11_k13_merge_output_not_committed) {
    auto c = Cask::open(dir, rollout_opts(), &test_registry());
    if (!c) crash_exit(1);
    // 16 KiB 文件上限必须真正滚出 sealed 输入；180 条不足以触发。
    // 500 条约 13 KiB，仍可能滞留同一 active；900 条必然滚出 sealed。
    for (int i = 0; i < 900; ++i) {
        if (!(*c)->put(bytes(key_for(i)), bytes(value_for(i)), 1000)) {
            crash_exit(1);
        }
    }
    if (!(*c)->sync() || !(*c)->checkpoint()) crash_exit(2);
    for (int i = 0; i < 450; ++i) {
        if (!(*c)->put(bytes(key_for(i)), bytes(value_for(i) + "!"), 1500)) {
            crash_exit(3);
        }
    }
    if (!(*c)->sync()) crash_exit(4);
    // 将完整负载封口；显式 merge 所有 sealed 输入，避免默认策略门槛让
    // 注入退化为 no-op。
    if (!(*c)->close_write_file()) crash_exit(5);
    const auto old_bytes = read_file(bitcask::oki::mk_manifest_filename(dir));
    if (old_bytes.empty()) crash_exit(5);

    std::set<std::string> before_names;
    for (const auto& e : fs::directory_iterator(dir)) {
        const auto name = e.path().filename().string();
        if (bitcask::fileops::parse_data_tstamp(name).has_value()) {
            before_names.insert(name);
        }
    }
    auto need = (*c)->needs_merge();
    if (need.files.empty()) {
        std::vector<std::string> files;
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            const auto name = e.path().filename().string();
            if (bitcask::fileops::parse_data_tstamp(name).has_value()) {
                files.push_back(e.path().string());
            }
        }
        std::sort(files.begin(), files.end());
        if (!files.empty()) files.pop_back();  // 排除新 active。
        need.needs = true;
        need.files = std::move(files);
    }
    if (need.files.empty()) crash_exit(6);
    auto ms = (*c)->merge(need.files);
    if (!ms) crash_exit(7);
    std::string output_name;
    for (const auto& e : fs::directory_iterator(dir)) {
        const auto name = e.path().filename().string();
        if (bitcask::fileops::parse_data_tstamp(name).has_value() &&
            !before_names.count(name)) {
            output_name = name;
        }
    }
    if (output_name.empty() || !fs::exists(dir + "/" + output_name)) {
        crash_exit(8);
    }
    const auto merged = bitcask::oki::read_manifest(dir);
    if (!merged.has_value() || merged->runs.empty()) crash_exit(9);
    for (const auto& run : merged->runs) {
        if (!fs::exists(dir + "/kv.oki.seg-" + std::to_string(run.gen))) {
            crash_exit(10);
        }
    }
    {
        std::ofstream out(bitcask::oki::mk_manifest_filename(dir),
                          std::ios::binary | std::ios::trunc);
        out.write(old_bytes.data(),
                  static_cast<std::streamsize>(old_bytes.size()));
        if (!out.good()) crash_exit(11);
    }
    crash_exit(0);
}

// K14/K15：merge 完成 relocation 并发布新 commit，但进程在 close/checkpoint
// 前终止。重开后任何 OKI loc 都必须落到仍可恢复的 data，不允许悬空。
BITCASK_CRASH_SCENARIO(k14_k15_crash_after_merge_commit) {
    auto c = Cask::open(dir, rollout_opts(), &test_registry());
    if (!c) crash_exit(1);
    for (int i = 0; i < 900; ++i) {
        if (!(*c)->put(bytes(key_for(i)), bytes(value_for(i)), 1000)) {
            crash_exit(1);
        }
    }
    if (!(*c)->sync() || !(*c)->checkpoint()) crash_exit(2);
    for (int i = 0; i < 250; ++i) {
        if (!(*c)->put(bytes(key_for(i)), bytes(value_for(i) + "!"), 2000)) {
            crash_exit(3);
        }
    }
    if (!(*c)->merge()) crash_exit(4);
    crash_exit(0);
}

}  // namespace

TEST_F(S43RolloutGateTest, RandomReferenceModelMatchesAllViews) {
    std::map<std::string, std::string> model;
    std::mt19937 rng(0x4343);
    const std::uint64_t now = static_cast<std::uint64_t>(::time(nullptr));
    std::uint64_t last_ord = 0;
    std::uint32_t last_file_id = 0;

    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        for (int step = 0; step < 480; ++step) {
            const int id = static_cast<int>(rng() % 64);
            const std::string key = key_for(id);
            const unsigned op = static_cast<unsigned>(rng() % 100);

            if (op < 46) {  // put / overwrite / TTL reinsert
                const std::string value = value_for(step);
                const bool expired = (rng() % 12) == 0;
                const auto tstamp =
                    static_cast<std::uint64_t>(1000 + step);
                ASSERT_TRUE(c->put(bytes(key), bytes(value),
                                   tstamp,
                                   expired ? now - 1 : 0));
                if (!expired) model[key] = value;
                else model.erase(key);
            } else if (op < 64) {  // remove / conditional reinsert tomb
                ASSERT_TRUE(c->remove(bytes(key),
                                      static_cast<std::uint64_t>(
                                          2000 + step)));
                model.erase(key);
            } else if (op < 78) {
                const std::string victim = key_for(
                    static_cast<int>(rng() % 64));
                const std::string fresh = key_for(64 + (rng() % 16));
                const std::string batch_value =
                    "batch-" + std::to_string(step);
                const std::vector<Cask::BatchOp> ops = {
                    {.type = Cask::BatchOp::Type::kRemove,
                     .key = bytes(victim)},
                    {.type = Cask::BatchOp::Type::kPut,
                     .key = bytes(fresh),
                     .value = bytes(batch_value)},
                };
                ASSERT_TRUE(c->put_batch_atomic(
                    ops, static_cast<std::uint64_t>(3000 + step)));
                model.erase(victim);
                model[fresh] = batch_value;
            } else if (op < 88) {
                ASSERT_TRUE(c->sync());
            } else if (op < 96) {
                ASSERT_TRUE(c->checkpoint());
            } else {
                ASSERT_TRUE(c->sync());
                auto ms = c->merge();
                ASSERT_TRUE(ms) << ms.error().detail;
            }

            const std::uint64_t ord = c->index_authority().peek_next_ord();
            const std::uint32_t fid = c->index_authority().biggest_file_id();
            EXPECT_GE(ord, last_ord);
            EXPECT_GE(fid, last_file_id);
            last_ord = ord;
            last_file_id = fid;
            if (step % 97 == 96) expect_model(*c, model);
        }
        ASSERT_TRUE(c->sync());
        ASSERT_TRUE(c->checkpoint());
        expect_model(*c, model);
    }

    auto reopened = open_rw();
    ASSERT_TRUE(reopened);
    expect_model(*reopened, model);
    EXPECT_GE(reopened->index_authority().peek_next_ord(), last_ord);
    EXPECT_GE(reopened->index_authority().biggest_file_id(), last_file_id);
}

TEST_F(S43RolloutGateTest, MigrationAndCorruptionFormsRecoverExplicitly) {
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        ASSERT_TRUE(c->put(bytes("legacy"), bytes("with-hint")));
        ASSERT_TRUE(c->put(bytes("gone"), bytes("old")));
        ASSERT_TRUE(c->remove(bytes("gone")));
        ASSERT_TRUE(c->sync());
        ASSERT_TRUE(c->checkpoint());
        c->close();
    }
    const auto committed = bitcask::oki::read_manifest(dir_.string());
    ASSERT_TRUE(committed.has_value());

    // 带 hint 旧库：hint 是可弃缓存；v4 + data 主路径必须清理它。
    {
        std::ofstream hint(dir_.string() + "/1.bitcask.data.hint");
        ASSERT_TRUE(hint.is_open());
        hint << "stale-legacy-hint";
    }
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        EXPECT_EQ(get_optional(*c, "legacy"), "with-hint");
        EXPECT_EQ(get_optional(*c, "gone"), std::nullopt);
        EXPECT_FALSE(fs::exists(dir_.string() + "/1.bitcask.data.hint"));
        c->close();
    }

    // K6/K7a：中断的 manifest 原子写残件不能影响 last-good 提交点。
    const std::string broken_tmp = "BCOM?";
    write_bytes(dir_ / "kv.oki.manifest.tmp", broken_tmp);
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        EXPECT_EQ(get_optional(*c, "legacy"), "with-hint");
        c->close();
    }

    // K7b/K8：CRC 损坏 manifest 不可读；空 run + 非空 data 显式触发重建。
    auto broken = *committed;
    broken.runs.clear();
    ASSERT_TRUE(bitcask::oki::write_manifest(dir_.string(), broken));
    {
        std::ofstream out(bitcask::oki::mk_manifest_filename(dir_.string()),
                          std::ios::binary | std::ios::app);
        ASSERT_TRUE(out.is_open());
        out << "tail-crc-corruption";
    }
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        EXPECT_EQ(get_optional(*c, "legacy"), "with-hint");
        EXPECT_EQ(get_optional(*c, "gone"), std::nullopt);
        const auto recovered = bitcask::oki::read_manifest(dir_.string());
        ASSERT_TRUE(recovered.has_value());
        ASSERT_TRUE(recovered->metadata.has_value());
        EXPECT_EQ(recovered->metadata->active_keys, 1u);
        c->close();
    }

    // K7c：健康 manifest 引用的 run 位翻转必须受控重建，不能静默错值。
    const auto healthy = bitcask::oki::read_manifest(dir_.string());
    ASSERT_TRUE(healthy.has_value());
    ASSERT_FALSE(healthy->runs.empty());
    const fs::path run = dir_ /
        ("kv.oki.seg-" + std::to_string(healthy->runs.front().gen));
    auto raw = read_file(run);
    ASSERT_GT(raw.size(), 64u);
    raw[4] = '\x63';  // BCOK version 99：直接复现旧读端拒收的 run 损坏。
    write_bytes(run, raw);
    EXPECT_FALSE(bitcask::oki::OkiRunReader::open(run.string()).has_value());
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        EXPECT_EQ(get_optional(*c, "legacy"), "with-hint");
        const auto rebuilt = bitcask::oki::read_manifest(dir_.string());
        ASSERT_TRUE(rebuilt.has_value());
        EXPECT_EQ(get_optional(*c, "legacy"), "with-hint");
        c->close();
    }

    // K9：torn append 只能丢弃不完整尾，不得跳过前面的活跃记录。
    const fs::path data = dir_ / "1.bitcask.data";
    if (fs::exists(data)) {
        std::ofstream out(data, std::ios::binary | std::ios::app);
        ASSERT_TRUE(out.is_open());
        out << "GARBAGE_DATA_NOT_A_VALID_RECORD_HEADER_THIS_IS_LONGER_THAN_"
               "ONE_HUNDRED_BYTES_AND_CANNOT_BE_A_RECORD";
        auto c = open_rw();
        ASSERT_TRUE(c);
        EXPECT_EQ(get_optional(*c, "legacy"), "with-hint");
        c->close();
    }
}

TEST_F(S43RolloutGateTest, K1K2SyncedDataSurvivesWithoutOkiCommit) {
    ASSERT_EQ(bitcask::test::spawn_crash_child(
                  "k1_k2_data_without_oki_commit", dir_.string()), 0);
    auto c = open_rw();
    ASSERT_TRUE(c);
    for (int i = 0; i < 24; ++i) {
        EXPECT_EQ(get_optional(*c, key_for(i)), value_for(i)) << i;
    }
    EXPECT_EQ(c->index_authority().info().key_count, 24u);
}

TEST_F(S43RolloutGateTest, K3K4FailedRunCommitKeepsPendingData) {
    ASSERT_EQ(bitcask::test::spawn_crash_child(
                  "k3_k4_run_before_manifest_commit", dir_.string()), 0);
    const auto old = bitcask::oki::read_manifest(dir_.string());
    ASSERT_TRUE(old.has_value());
    ASSERT_TRUE(old->metadata.has_value());
    EXPECT_EQ(old->metadata->active_keys, 1u);

    auto c = open_rw();
    ASSERT_TRUE(c);
    EXPECT_EQ(get_optional(*c, "committed"), "old");
    EXPECT_EQ(get_optional(*c, "pending"), "new");
    EXPECT_EQ(c->index_authority().info().key_count, 2u);
    const auto opened_manifest = bitcask::oki::read_manifest(dir_.string());
    ASSERT_TRUE(opened_manifest.has_value());
    ASSERT_TRUE(opened_manifest->metadata.has_value());
    std::error_code remove_ec;
    fs::remove(dir_.string() + "/kv.oki.manifest.tmp", remove_ec);
    ASSERT_TRUE(c->checkpoint());
    const auto next = bitcask::oki::read_manifest(dir_.string());
    ASSERT_TRUE(next.has_value());
    ASSERT_TRUE(next->metadata.has_value());
    EXPECT_EQ(next->metadata->active_keys, 2u);
    EXPECT_GE(next->runs.size(), old->runs.size());
}

TEST_F(S43RolloutGateTest, K10RecoveryFlushKeepsCompleteRecords) {
    ASSERT_EQ(bitcask::test::spawn_crash_child(
                  "k10_recovery_flush_boundary", dir_.string()), 0);
    auto c = open_rw();
    ASSERT_TRUE(c);
    for (int i = 0; i < 37; ++i) {
        EXPECT_EQ(get_optional(*c, key_for(i)), value_for(i)) << i;
    }
}

TEST_F(S43RolloutGateTest, K11K13UnreferencedMergeOutputIsSafe) {
    ASSERT_EQ(bitcask::test::spawn_crash_child(
                  "k11_k13_merge_output_not_committed", dir_.string()), 0);
    auto c = open_rw();
    ASSERT_TRUE(c);
    // 子进程在 merge 完整 fsync 后才回退 manifest，因此这是 K12/K13
    // 窗口：输出虽未被新 manifest 引用，但它是完整合法的更代数据。
    // 恢复按 ord/LWW 收敛到更新值；关键门是既不悬空、也不丢 900 key。
    for (int i = 0; i < 900; ++i) {
        const std::string want =
            i < 450 ? value_for(i) + "!" : value_for(i);
        EXPECT_EQ(get_optional(*c, key_for(i)), want) << i;
    }
    EXPECT_EQ(c->index_authority().info().key_count, 900u);

    // 推进一个新提交点后，旧输出残件要么被 sweep，要么不影响后续语义。
    ASSERT_TRUE(c->put(bytes("after-old-commit"), bytes("live")));
    ASSERT_TRUE(c->sync());
    ASSERT_TRUE(c->checkpoint());
    EXPECT_EQ(get_optional(*c, "after-old-commit"), "live");
}

TEST_F(S43RolloutGateTest, K14K15CrashAfterMergeCommitHasNoDanglingLoc) {
    ASSERT_EQ(bitcask::test::spawn_crash_child(
                  "k14_k15_crash_after_merge_commit", dir_.string()), 0);
    auto c = open_rw();
    ASSERT_TRUE(c);
    for (int i = 0; i < 900; ++i) {
        const std::string want =
            i < 250 ? value_for(i) + "!" : value_for(i);
        EXPECT_EQ(get_optional(*c, key_for(i)), want) << i;
    }
    EXPECT_EQ(c->index_authority().info().key_count, 900u);
    auto it = c->make_range_iter(bitcask::RangeOptions{});
    ASSERT_TRUE(it.has_value());
    std::size_t seen = 0;
    while (true) {
        auto e = (*it)->next();
        ASSERT_TRUE(e.has_value());
        if (!*e) break;
        ++seen;
    }
    EXPECT_EQ(seen, 900u);
}
