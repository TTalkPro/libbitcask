// S45-5：分片执行器下的崩溃恢复（designs/index-pipeline-unordered-design-zh.md §4.5/§4.6）。
//
// 子进程：search 模式，4 个写线程对同一组 key 并发 put_doc / 覆盖 / 删除，
// 主线程穿插数次 checkpoint（暂停期取 LSN 连续完成前缀为覆盖水位）；写线程
// 仍在跑时直接 crash_exit——在途索引任务、未落 ckpt 的应用、data 尾部全部
// 处于崩溃态。父进程重开：恢复重放（分片执行器并行、ord 判重）后，索引必须
// 与 KV 逐 key 一致（verify_index_consistency + 当前正文唯一词检索），再重开
// 一次仍一致（重放幂等）。

#include "support/crash_child.hpp"

using bitcask::test::crash_exit;

#include <atomic>
#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/codec.hpp>
#include <bitcask/data_file.hpp>
#include <bitcask/detail/committed_fold.hpp>
#include <bitcask/index_authority.hpp>
#include <bitcask/index.hpp>
#include <bitcask/index_registry.hpp>
#include <bitcask/vector_plugin.hpp>

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskOptions;

namespace {

bitcask::oki::IndexRegistry& test_registry() {
    static bitcask::oki::IndexRegistry reg;
    return reg;
}

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

constexpr std::size_t kKeys = 300;
constexpr std::uint16_t kDim = 8;

CaskOptions crash_opts() {
    CaskOptions o;
    o.read_write = true;
    o.enable_search = true;
    bitcask::search::SearchLayerConfig scfg;
    scfg.analyzer_config.type = bitcask::text::AnalyzerType::Whitespace;
    o.search_config = scfg;
    o.vector_dim = kDim;
    o.vector_metric = bitcask::meta::VectorMetric::kCosineNormalized;
    o.sync_every_n = 64;  // 组提交：崩溃时 data 尾部有未 fsync 的批（OS 仍持有）
    return o;
}

std::string key_of(std::size_t i) { return "ck" + std::to_string(i); }

}  // namespace

BITCASK_CRASH_SCENARIO(index_crash_mid_writes) {
    auto oc = Cask::open(dir, crash_opts(), &test_registry());
    if (!oc) crash_exit(1);
    auto& c = *oc;
    // 写量有界（每线程 kPerThread 次）：HNSW 软删节点不收缩，放任写线程空转
    // 会把图撑到百万节点、每次 checkpoint 全量序列化数秒——那测的是 HNSW 规模
    // 而非恢复正确性。主线程在写量穿过 25%/50%/75% 时各 checkpoint 一次，写到
    // ~90% 时（写线程仍在跑）崩溃。
    constexpr std::uint64_t kPerThread = 4000;
    constexpr std::uint64_t kTotal = 4 * kPerThread;
    std::atomic<std::uint64_t> ops{0};
    std::vector<std::thread> ws;
    for (int t = 0; t < 4; ++t) {
        ws.emplace_back([&, t] {
            std::mt19937_64 rng(static_cast<std::uint64_t>(t) + 7);
            std::uniform_real_distribution<float> uf(-1.0f, 1.0f);
            for (std::uint64_t n = 0; n < kPerThread; ++n) {
                const std::string key = key_of(rng() % kKeys);
                if (rng() % 100 < 75) {
                    const std::string text =
                        "tok_" + key + "_" + std::to_string(t) + "_" +
                        std::to_string(n) + " common";
                    std::vector<float> v(kDim);
                    for (auto& x : v) x = uf(rng);
                    bitcask::DocInput d;
                    d.text = bytes(text);
                    d.vector = v;
                    (void)c->put_doc(bytes(key), d);
                } else {
                    (void)c->remove(bytes(key));
                }
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::uint64_t mark : {kTotal / 4, kTotal / 2, kTotal * 3 / 4}) {
        while (ops.load() < mark) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!c->checkpoint()) crash_exit(2);
    }
    while (ops.load() < kTotal * 9 / 10) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    crash_exit(0);  // 写线程仍在跑：不 close、不 join
}


// S45-A 后续：OKI 崩溃 tail 补行的墓碑 LSN。key 先删除并经 checkpoint 持久化
// 进 OKI 视图（墓碑 T1），之后再写入、再删除（墓碑 T2）后崩溃——T2 只在
// data 的 tail 里。修复前 tail 补行见视图已是墓碑就跳过，locate 报 T1。
BITCASK_CRASH_SCENARIO(oki_tomb_tail) {
    CaskOptions o = crash_opts();
    o.enable_search = false;
    o.vector_dim = 0;
    auto oc = Cask::open(dir, o, &test_registry());
    if (!oc) crash_exit(1);
    auto& c = *oc;
    for (int i = 0; i < 50; ++i) {  // 让 run 非空（零活 key 不落 run）
        const std::string k = "pad" + std::to_string(i);
        if (!c->put(bytes(k), bytes("v"))) crash_exit(2);
    }
    if (!c->put(bytes("victim"), bytes("v1"))) crash_exit(2);
    if (!c->remove(bytes("victim"))) crash_exit(2);   // T1
    if (!c->checkpoint()) crash_exit(3);
    if (!c->put(bytes("victim"), bytes("v2"))) crash_exit(2);
    if (!c->put(bytes("victim"), bytes("v3"))) crash_exit(2);
    if (!c->remove(bytes("victim"))) crash_exit(2);   // T2
    if (!c->sync()) crash_exit(4);
    crash_exit(0);
}


// 打开期兜底：manifest 高估 data 覆盖（旧版本提交切面缺陷写出）。子进程把
// 一个写者停在「data 已追加、尚未应用到 OKI」的窗口，模拟旧版本删掉该文件的
// 已应用落点登记后提交 OKI 切面——manifest 声明覆盖到文件末尾、run 却没有
// 这条记录——然后崩溃。
BITCASK_CRASH_SCENARIO(oki_overclaimed_manifest) {
    CaskOptions o = crash_opts();
    o.enable_search = false;
    o.vector_dim = 0;
    auto oc = Cask::open(dir, o, &test_registry());
    if (!oc) crash_exit(1);
    auto& c = *oc;
    for (int i = 0; i < 10; ++i) {
        if (!c->put(bytes("pre" + std::to_string(i)), bytes("v"))) crash_exit(2);
    }
    if (!c->checkpoint()) crash_exit(3);
    auto& auth = c->index_authority();
    std::atomic<bool> in_window{false};
    auth.set_pre_apply_hook_for_test([&] {
        in_window.store(true);
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));  // 停到崩溃
    });
    std::thread w([&] { (void)c->put(bytes("victim"), bytes("lost-before-fix")); });
    w.detach();
    while (!in_window.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // 旧版本行为：victim 所在文件没有已应用落点登记 → 切面取文件当前大小。
    std::uint32_t fid = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        const auto n = e.path().filename().string();
        if (n.size() > 13 && n.substr(n.size() - 13) == ".bitcask.data") {
            fid = std::max(fid, static_cast<std::uint32_t>(std::stoul(n)));
        }
    }
    auth.drop_applied_end_for_test(fid);
    auto cut = auth.capture_commit_cut(
        [&]() -> std::optional<bitcask::oki::IndexAuthority::DataWms> {
            bitcask::oki::IndexAuthority::DataWms wms;
            for (const auto& e : fs::directory_iterator(dir)) {
                const auto n = e.path().filename().string();
                if (n.size() > 13 && n.substr(n.size() - 13) == ".bitcask.data") {
                    wms.emplace_back(static_cast<std::uint32_t>(std::stoul(n)),
                                     e.file_size());
                }
            }
            return wms;
        });
    if (!cut) crash_exit(4);
    if (!auth.state().flush(dir, cut->second, std::move(cut->first))) crash_exit(5);
    crash_exit(0);
}

namespace {

class IndexCrashRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("bitcask_index_crash_" +
                std::string(::testing::UnitTest::GetInstance()
                                ->current_test_info()
                                ->name()));
        fs::remove_all(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    fs::path dir_;
};

// 索引 vs KV 逐 key 对拍：KV 当前正文唯一词恰好 1 命中且归属该 key；
// 已删 key 的 docmap 无行。返回违例数。
std::size_t check_against_kv(Cask& c) {
    std::size_t bad = 0;
    for (std::size_t i = 0; i < kKeys; ++i) {
        const std::string key = key_of(i);
        auto got = c.get(bytes(key));
        const bool in_docmap = c.docmap()->get(key).has_value();
        if (!got) {
            if (in_docmap) {
                ADD_FAILURE() << key << ": deleted in KV but present in index";
                ++bad;
            }
            continue;
        }
        const std::string_view txt(reinterpret_cast<const char*>(got->value.data()),
                                   got->value.size());
        const std::string tok(txt.substr(0, txt.find(' ')));
        auto r = c.search_text(tok, 5);
        if (!r || r->hits.size() != 1 || r->hits[0].key != key || !in_docmap) {
            ADD_FAILURE() << key << ": current version not indexed exactly once ("
                          << (r ? r->hits.size() : 999) << " hits)";
            ++bad;
        }
    }
    return bad;
}

TEST_F(IndexCrashRecoveryTest, CrashMidConcurrentWritesRecoversConsistentIndex) {
    ASSERT_EQ(bitcask::test::spawn_crash_child("index_crash_mid_writes",
                                               dir_.string()),
              0);
    std::size_t vec_nodes_round0 = 0;
    for (int round = 0; round < 2; ++round) {
        SCOPED_TRACE("reopen round " + std::to_string(round));
        auto oc = Cask::open(dir_.string(), crash_opts(), &test_registry());
        ASSERT_TRUE(oc) << oc.error().detail;
        auto& c = *oc;
        auto rep = c->verify_index_consistency();
        ASSERT_TRUE(rep) << rep.error().detail;
        EXPECT_TRUE(rep->consistent())
            << "mismatch=" << rep->version_mismatch << " ghost=" << rep->ghost_in_index
            << " missing=" << rep->missing_in_index
            << (rep->samples.empty() ? "" : " e.g. " + rep->samples.front());
        EXPECT_GT(rep->matched, 0u);
        EXPECT_EQ(check_against_kv(*c), 0u);
        // 向量侧：每个存活 key 的当前版本都已登记进向量 key+LSN 表（= 当前
        // 向量在图里），且 LSN 与 docmap 一致。
        {
            const auto* hp =
                dynamic_cast<const bitcask::vec::VectorPlugin*>(c->vector_plugin());
            ASSERT_NE(hp, nullptr);
            std::size_t vec_mismatch = 0;
            for (std::size_t i = 0; i < kKeys; ++i) {
                const auto h = c->docmap()->get(key_of(i));
                if (h && hp->key_ord_for_test(key_of(i)) != h->ord) ++vec_mismatch;
            }
            EXPECT_EQ(vec_mismatch, 0u);
        }
        // 第二次重开从第一次 close 的 checkpoint 起：向量节点数不得再长
        // （重放经 key+LSN 裁决，不重复插入）。
        if (round == 0) {
            vec_nodes_round0 = c->vector_plugin()->size();
        } else {
            EXPECT_EQ(c->vector_plugin()->size(), vec_nodes_round0);
        }
        c->close();
    }
}


// data 中某 key 的全部记录 (ord, 是否墓碑)。
std::vector<std::pair<std::uint64_t, bool>> records_of(const fs::path& dir,
                                                       std::string_view key) {
    std::vector<std::pair<std::uint64_t, bool>> out;
    for (const auto& e : fs::directory_iterator(dir)) {
        const auto n = e.path().filename().string();
        if (n.size() < 13 || n.substr(n.size() - 13) != ".bitcask.data") continue;
        auto df = bitcask::fileops::DataFile::open(
            e.path().string(), bitcask::fileops::DataFile::Mode::kRead, false, false);
        if (!df) continue;
        (void)bitcask::fileops::fold_committed(
            *df,
            [&](const bitcask::codec::DataRecordView& v, std::uint64_t, std::uint32_t) {
                if (std::string_view(reinterpret_cast<const char*>(v.key.data()),
                                     v.key.size()) == key) {
                    out.emplace_back(v.ord,
                                     v.type == bitcask::format::RecordType::kTombstone);
                }
            });
    }
    return out;
}

TEST_F(IndexCrashRecoveryTest, OkiTailTombstoneOverOlderTombstoneKeepsLatestLsn) {
    ASSERT_EQ(bitcask::test::spawn_crash_child("oki_tomb_tail", dir_.string()), 0);
    const auto recs = records_of(dir_, "victim");
    ASSERT_EQ(recs.size(), 5u);
    ASSERT_TRUE(recs.back().second) << "最后一条应为墓碑 T2";
    const std::uint64_t t2 = recs.back().first;

    CaskOptions o = crash_opts();
    o.enable_search = false;
    o.vector_dim = 0;
    auto oc = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(oc) << oc.error().detail;
    auto& c = *oc;
    EXPECT_FALSE(c->get(bytes("victim")));
    const auto r = c->index_authority().state().locate("victim");
    if (r.status == bitcask::oki::OkiState::LocateStatus::kHit) {
        EXPECT_TRUE(r.tomb);
        EXPECT_EQ(r.ord, t2) << "OKI 墓碑 LSN 必须是 data 里最新的墓碑";
    } else {
        // 缺席同样是正确的删除语义（无 LSN 可偏旧）。
        EXPECT_EQ(r.status, bitcask::oki::OkiState::LocateStatus::kMiss);
    }
    c->close();
}


// OKI 一致切面的 data 水位钳制（AutoCheckpointOnRoll 偶发丢写的根因）：
// 写路径「追加 data → apply_put」之间，capture_commit_cut 只持权威写序
// 闸门，能看到已追加未应用的记录。各文件水位须钳到真正应用过的末尾。
//   ① 刚分配的新文件（roll 后首条记录尚未应用）：此前无登记项、不钳。
//   ② 运行期受控重建之后：此前登记表被清空，active 文件不再钳。
TEST(OkiCommitCut, DataWatermarkClampedToAppliedRecords) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_oki_cut_clamp";
    fs::remove_all(dir);
    CaskOptions o;
    o.read_write = true;
    auto oc = Cask::open(dir.string(), o, &test_registry());
    ASSERT_TRUE(oc) << oc.error().detail;
    auto& c = *oc;
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(c->put(bytes("k" + std::to_string(i)), bytes("value")));
    }
    auto& auth = c->index_authority();
    // 当前 active 文件：最大 file id、真实大小。
    std::uint32_t active = 0;
    std::uint64_t active_size = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        const auto n = e.path().filename().string();
        if (n.size() > 13 && n.substr(n.size() - 13) == ".bitcask.data") {
            const auto id = static_cast<std::uint32_t>(std::stoul(n));
            if (id >= active) {
                active = id;
                active_size = e.file_size();
            }
        }
    }
    ASSERT_GT(active, 0u);
    ASSERT_GT(active_size, 0u);

    auto cut_wm = [&](std::uint32_t fid, std::uint64_t claimed) {
        auto cut = auth.capture_commit_cut(
            [&]() -> std::optional<bitcask::oki::IndexAuthority::DataWms> {
                return bitcask::oki::IndexAuthority::DataWms{{fid, claimed}};
            });
        EXPECT_TRUE(cut);
        return cut ? cut->second.at(0).second : ~0ull;
    };

    // 已应用文件：钳到最后一条已应用记录末尾（= 当前大小）。
    EXPECT_EQ(cut_wm(active, active_size + 4096), active_size);
    // ① 新分配文件，首批记录「已追加未应用」：钳到 0。
    const std::uint32_t fresh = auth.increment_file_id();
    EXPECT_EQ(cut_wm(fresh, 4096), 0u);
    // ② 运行期重建后：active 文件钳到扫描末尾，不是放行任意水位。
    ASSERT_TRUE(auth.rebuild_from_data(dir.string()));
    auth.state().enable_point_query();
    EXPECT_EQ(cut_wm(active, active_size + 4096), active_size);
    c->close();
    fs::remove_all(dir);
}


TEST_F(IndexCrashRecoveryTest, OverclaimedOkiManifestFallsBackToRebuild) {
    ASSERT_EQ(bitcask::test::spawn_crash_child("oki_overclaimed_manifest",
                                               dir_.string()), 0);
    ASSERT_EQ(records_of(dir_, "victim").size(), 1u) << "victim 必须已在 data 里";
    CaskOptions o = crash_opts();
    o.enable_search = false;
    o.vector_dim = 0;
    {
        auto oc = Cask::open(dir_.string(), o, &test_registry());
        ASSERT_TRUE(oc) << oc.error().detail;
        auto& c = *oc;
        EXPECT_FALSE(c->index_authority().last_ledger_view_consistent())
            << "抽检必须识别出高估覆盖的 manifest";
        auto got = c->get(bytes("victim"));
        ASSERT_TRUE(got) << "兜底重建后 victim 不得丢失";
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(got->value.data()),
                                   got->value.size()),
                  "lost-before-fix");
        for (int i = 0; i < 10; ++i) {
            EXPECT_TRUE(c->get(bytes("pre" + std::to_string(i))));
        }
        EXPECT_EQ(c->status().key_count, 11u);
        c->close();
    }
    {  // 健康目录：抽检通过、不误报（不触发重建）。
        auto oc = Cask::open(dir_.string(), o, &test_registry());
        ASSERT_TRUE(oc) << oc.error().detail;
        auto& c = *oc;
        EXPECT_TRUE(c->index_authority().last_ledger_view_consistent());
        EXPECT_GE(c->index_authority().last_ledger_view_probes(), 1u)
            << "抽检必须实际执行";
        EXPECT_TRUE(c->get(bytes("victim")));
        c->close();
    }
}

}  // namespace
