// S36-2：locate() 统一点查原语 + 影子对拍集成测试。
// 设计：doc/keydir-disk-resident-design-zh.md §5.1/§10（S36-2 行）。
// 覆盖：
//   - 组合视图点查（memdelta 辅助哈希 → v2 run bloom/seek）与哈希权威
//     全量对拍（delta 态 / run 态 / 重开后 / 墓碑 / 缺席 key）；
//   - merge 搬迁行入 delta（Level A「零交互」反转，设计 §D1）——搬迁后
//     locate 给新位置，flush 固化后仍新（(ord, gen) 等 ord 高 gen 胜）；
//   - TTL conditional_remove 的组合视图墓碑（受害者 ord 记账）；
//   - 全归并携带位置字段（等 ord 平局取高 gen 行）；
//   - v1 run 使点查降级 kUnavailable（不是错答），影子对拍跳过；
//   - 影子对拍统计：checks > 0 且 drifts == 0（NDEBUG 下 assert 的镜像）。

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/data_file.hpp>  // parse_data_tstamp（TTL 用例收集 sealed）
#include <bitcask/keydir_registry.hpp>
#include <bitcask/oki_run.hpp>
#include <bitcask/oki_state.hpp>

namespace fs = std::filesystem;
using bitcask::Cask;
using bitcask::CaskOptions;
using bitcask::oki::OkiState;

namespace {

bitcask::keydir::KeyDirRegistry& test_registry() {
    static bitcask::keydir::KeyDirRegistry reg;
    return reg;
}

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

// 对单个 key：locate（缓存→组合视图）与 get（哈希权威）逐字段相等。
void expect_locate_matches_get(bitcask::keydir::KeyDir& kd,
                               const std::string& key,
                               const char* ctx) {
    auto g = kd.get(key);
    auto l = kd.locate(key);
    ASSERT_EQ(g.has_value(), l.has_value()) << ctx << " key=" << key;
    if (!g) return;
    EXPECT_EQ(l->file_id, g->file_id) << ctx << " key=" << key;
    EXPECT_EQ(l->total_sz, g->total_sz) << ctx << " key=" << key;
    EXPECT_EQ(l->offset, g->offset) << ctx << " key=" << key;
    EXPECT_EQ(l->tstamp, g->tstamp) << ctx << " key=" << key;
    EXPECT_EQ(l->ord, g->ord) << ctx << " key=" << key;
}

// 对单个 key：OKI 冷侧（绕开哈希缓存）必须独立给出与哈希一致的答案——
// 这是 S36-4 开逐出后 get 冷路径正确性的直接前提。
void expect_cold_view_matches(bitcask::keydir::KeyDir& kd,
                              const std::string& key, const char* ctx) {
    auto g = kd.get(key);
    const auto c = kd.oki().locate(key);
    ASSERT_NE(c.status, OkiState::LocateStatus::kUnavailable)
        << ctx << " key=" << key;
    if (g.has_value()) {
        ASSERT_EQ(c.status, OkiState::LocateStatus::kHit)
            << ctx << " key=" << key;
        EXPECT_FALSE(c.tomb) << ctx << " key=" << key;
        ASSERT_TRUE(c.has_loc) << ctx << " key=" << key;
        EXPECT_EQ(c.loc.file_id, g->file_id) << ctx << " key=" << key;
        EXPECT_EQ(c.loc.total_sz, g->total_sz) << ctx << " key=" << key;
        EXPECT_EQ(c.loc.offset, g->offset) << ctx << " key=" << key;
        EXPECT_EQ(c.loc.tstamp, g->tstamp) << ctx << " key=" << key;
        EXPECT_EQ(c.ord, g->ord) << ctx << " key=" << key;
    } else {
        EXPECT_TRUE(c.status == OkiState::LocateStatus::kMiss ||
                    (c.status == OkiState::LocateStatus::kHit && c.tomb))
            << ctx << " key=" << key;
    }
}

class OkiLocateTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() /
               (std::string("bitcask_oki_locate_") + info->name());
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

// 点查基线：delta 态 / flush 后 run 态 / 重开后（快照 + run 载入）三形态
// 下 locate == get；墓碑与缺席 key 给 miss；影子对拍全程零漂移。
TEST_F(OkiLocateTest, LocateMatchesHashAcrossDeltaRunAndReopen) {
    CaskOptions o;
    o.read_write = true;
    {
        auto c = Cask::open(dir_.string(), o, &test_registry());
        ASSERT_TRUE(c);
        auto& kd = (*c)->keydir();
        kd.set_oki_shadow_check(true);  // 不依赖 NDEBUG 自动开启

        for (int i = 0; i < 40; ++i) {
            ASSERT_TRUE((*c)->put(bytes("k" + std::to_string(i)),
                                  bytes("v" + std::to_string(i)), 1000));
        }
        // 覆盖写 + 删除，行还在 memdelta。
        for (int i = 0; i < 10; ++i) {
            ASSERT_TRUE((*c)->put(bytes("k" + std::to_string(i)),
                                  bytes("V!" + std::to_string(i)), 1500));
        }
        ASSERT_TRUE((*c)->remove(bytes("k5"), 2000));
        ASSERT_TRUE((*c)->remove(bytes("k17"), 2000));
        for (int i = 0; i < 40; ++i) {
            expect_locate_matches_get(kd, "k" + std::to_string(i), "delta");
            expect_cold_view_matches(kd, "k" + std::to_string(i), "delta");
        }
        expect_cold_view_matches(kd, "absent-key", "delta");

        // flush（checkpoint 搭车）→ 行固化进 v2 run。
        ASSERT_TRUE((*c)->checkpoint());
        for (int i = 0; i < 40; ++i) {
            expect_locate_matches_get(kd, "k" + std::to_string(i), "run");
            expect_cold_view_matches(kd, "k" + std::to_string(i), "run");
        }
        // bloom 挡缺席 key（不能误报成 hit）。
        expect_cold_view_matches(kd, "no-such-key", "run");

        const auto st = kd.shadow_stats();
        EXPECT_GT(st.checks, 0u) << "影子对拍必须真的跑过";
        EXPECT_EQ(st.drifts, 0u);
        (*c)->close();
    }
    // 重开：快照 + run 载入后同样成立。
    {
        auto c = Cask::open(dir_.string(), o, &test_registry());
        ASSERT_TRUE(c);
        auto& kd = (*c)->keydir();
        kd.set_oki_shadow_check(true);
        for (int i = 0; i < 40; ++i) {
            expect_locate_matches_get(kd, "k" + std::to_string(i), "reopen");
            expect_cold_view_matches(kd, "k" + std::to_string(i), "reopen");
        }
        EXPECT_EQ(kd.shadow_stats().drifts, 0u);
        (*c)->close();
    }
}

// merge 搬迁行入 delta（设计 §D1 反转）：搬迁后组合视图立刻给新位置；
// flush 固化后（搬迁行与被搬迁行同 ord、不同 gen）仍给新位置。
TEST_F(OkiLocateTest, MergeRelocationRowsKeepColdViewFresh) {
    CaskOptions o;
    o.read_write = true;
    o.max_file_size = 512;  // 逼出多个 sealed 文件
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    auto& kd = (*c)->keydir();
    kd.set_oki_shadow_check(true);

    const std::string pad(64, 'x');
    for (int i = 0; i < 30; ++i) {
        ASSERT_TRUE((*c)->put(bytes("r" + std::to_string(i)),
                              bytes(pad), 1000));
    }
    // 覆盖一半 → 旧文件死字节，merge 有活可干且有活 key 需搬迁。
    for (int i = 0; i < 15; ++i) {
        ASSERT_TRUE((*c)->put(bytes("r" + std::to_string(i)),
                              bytes(pad + "!"), 1500));
    }
    // 先固化一版（含旧位置的行进 run），让搬迁行走「与 run 行同 ord、
    // 更高 gen」的胜出路径。
    ASSERT_TRUE((*c)->checkpoint());

    auto ms = (*c)->merge();
    ASSERT_TRUE(ms);
    ASSERT_GT(ms->records_kept, 0u) << "本测试前提：确有活记录被搬迁";

    // 搬迁行在 delta：组合视图冷侧必须已知新位置。
    for (int i = 0; i < 30; ++i) {
        expect_locate_matches_get(kd, "r" + std::to_string(i), "post-merge");
        expect_cold_view_matches(kd, "r" + std::to_string(i), "post-merge");
    }
    // flush 固化搬迁行 → run 间等 ord 平局（gen 大者胜）。
    ASSERT_TRUE((*c)->checkpoint());
    for (int i = 0; i < 30; ++i) {
        expect_cold_view_matches(kd, "r" + std::to_string(i), "post-flush");
    }
    const auto st = kd.shadow_stats();
    EXPECT_GT(st.checks, 0u);
    EXPECT_EQ(st.drifts, 0u);
    (*c)->close();
}

// TTL：merge 的 conditional_remove（keydir-only 删除，无数据记录背书）
// 必须在组合视图留下墓碑（受害者 ord 记账），否则逐出态的 get 会从 run
// 里捞回已删 key。
TEST_F(OkiLocateTest, TtlConditionalRemoveTombstonesColdView) {
    CaskOptions o;
    o.read_write = true;
    o.max_file_size = 256;  // 让过期记录尽快离开 active 文件
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    auto& kd = (*c)->keydir();
    kd.set_oki_shadow_check(true);

    // expiry_at = 100（绝对秒，已过期）；filler 把它挤出 active 文件。
    ASSERT_TRUE((*c)->put(bytes("ttl-key"), bytes("doomed"), 50,
                          /*expiry_at=*/100));
    const std::string pad(64, 'f');
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE((*c)->put(bytes("filler" + std::to_string(i)),
                              bytes(pad), 50));
    }
    // 先固化：ttl-key 的活行进 run（之后的墓碑必须能抵消它）。
    ASSERT_TRUE((*c)->checkpoint());
    ASSERT_TRUE(kd.get("ttl-key").has_value()) << "merge 前还活着";

    // 全 live 不触发 merge 策略——手动收集 sealed 文件（同
    // cask_docvalue_test 的 TTL 用例惯例），排除 active。
    std::vector<std::string> files;
    for (const auto& de : fs::directory_iterator(dir_)) {
        const auto name = de.path().filename().string();
        if (bitcask::fileops::parse_data_tstamp(name).has_value()) {
            files.push_back(de.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    ASSERT_GT(files.size(), 1u);
    files.pop_back();  // active
    auto ms = (*c)->merge(files, /*now_sec=*/200);
    ASSERT_TRUE(ms);
    ASSERT_GT(ms->records_expired, 0u) << "本测试前提：TTL 过期确被触发";

    EXPECT_FALSE(kd.get("ttl-key").has_value());
    const auto cold = kd.oki().locate("ttl-key");
    ASSERT_EQ(cold.status, OkiState::LocateStatus::kHit)
        << "组合视图必须有墓碑行（而不是 run 里的陈旧活行胜出）";
    EXPECT_TRUE(cold.tomb);
    EXPECT_FALSE(kd.locate("ttl-key").has_value());

    // flush 固化墓碑（与活行同 ord、更高 gen）→ 冷侧仍是墓碑。
    ASSERT_TRUE((*c)->checkpoint());
    const auto cold2 = kd.oki().locate("ttl-key");
    ASSERT_EQ(cold2.status, OkiState::LocateStatus::kHit);
    EXPECT_TRUE(cold2.tomb);

    // 删除后重写必须可见（墓碑不误杀更高 ord 的新行）。
    ASSERT_TRUE((*c)->put(bytes("ttl-key"), bytes("reborn"), 300));
    expect_locate_matches_get(kd, "ttl-key", "reborn");
    expect_cold_view_matches(kd, "ttl-key", "reborn");

    EXPECT_EQ(kd.shadow_stats().drifts, 0u);
    (*c)->close();
}

// 全归并（run 数 > kCompactRunLimit）：位置字段随行归并；等 ord 平局取
// 高 gen（搬迁后的新位置不被旧行覆没）。
TEST_F(OkiLocateTest, CompactionCarriesLocAndPrefersHigherGenOnOrdTie) {
    CaskOptions o;
    o.read_write = true;
    o.max_file_size = 512;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    auto& kd = (*c)->keydir();
    kd.set_oki_shadow_check(true);

    const std::string pad(64, 'x');
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE((*c)->put(bytes("c" + std::to_string(i)),
                              bytes(pad), 1000));
    }
    for (int i = 0; i < 10; ++i) {  // 制造死字节，给 merge 干活
        ASSERT_TRUE((*c)->put(bytes("c" + std::to_string(i)),
                              bytes(pad + "!"), 1500));
    }
    ASSERT_TRUE((*c)->checkpoint());       // run1：旧位置行
    auto ms = (*c)->merge();               // 搬迁 → delta（同 ord 新位置）
    ASSERT_TRUE(ms);
    ASSERT_GT(ms->records_kept, 0u);
    ASSERT_TRUE((*c)->checkpoint());       // run2：搬迁行固化

    // 灌 checkpoint 直到触发全归并（run 数回落到 1）。
    for (int round = 0; round < 12 && kd.oki().run_count() > 1; ++round) {
        ASSERT_TRUE((*c)->put(bytes("pad" + std::to_string(round)),
                              bytes("v"), 2000));
        ASSERT_TRUE((*c)->checkpoint());
    }
    ASSERT_EQ(kd.oki().run_count(), 1u) << "全归并必须已发生";

    // 归并后：单 run 里的行必须是「新位置」（等 ord 高 gen 胜出的结果）。
    for (int i = 0; i < 20; ++i) {
        expect_cold_view_matches(kd, "c" + std::to_string(i), "compacted");
    }
    EXPECT_EQ(kd.shadow_stats().drifts, 0u);
    (*c)->close();
}

// v1 run（Level A 老格式，无位置字段/bloom）：点查必须降级 kUnavailable
// ——绝不能答 kMiss（答 miss = 谎报「权威不存在」）。影子对拍照跑但跳过。
TEST_F(OkiLocateTest, V1RunDegradesLocateToUnavailable) {
    namespace ok = bitcask::oki;
    // 手工铺一个 v1 run + v1 manifest（老纪元目录的样子）。
    {
        auto w = ok::OkiRunWriter::create(
            ok::mk_run_filename(dir_.string(), 1),
            ok::kDefaultBlockBytes, ok::kRunVersion);
        ASSERT_TRUE(w.has_value());
        ASSERT_TRUE(w->add(bytes("legacy-key"), 0, false).has_value());
        ASSERT_TRUE(w->finish(true).has_value());
        ok::OkiManifest m;
        m.runs.push_back({1, 1, /*format_ver=*/1});
        // wm=0：本目录没有数据文件，新 keydir 的 ord 从 0 起——wm 若 >0
        // 会把新写行挡在水位门外（真实老目录的 wm 恒 ≤ 数据里的 next_ord，
        // 不会出现这种倒挂；这里手工对齐）。
        m.wm = 0;
        ASSERT_TRUE(ok::write_manifest(dir_.string(), m));
    }

    CaskOptions o;
    o.read_write = true;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    auto& kd = (*c)->keydir();
    kd.set_oki_shadow_check(true);

    // delta miss → 走到 v1 run → kUnavailable（不是 kMiss）。
    EXPECT_EQ(kd.oki().locate("legacy-key").status,
              OkiState::LocateStatus::kUnavailable);
    EXPECT_EQ(kd.oki().locate("whatever").status,
              OkiState::LocateStatus::kUnavailable);

    // 影子对拍：get 走一遍不崩、判为 skip、零漂移。
    (void)kd.get("legacy-key");
    (void)kd.get("whatever");
    const auto st = kd.shadow_stats();
    EXPECT_GT(st.skips, 0u);
    EXPECT_EQ(st.drifts, 0u);

    // 新写仍可点查（delta 命中不经过 v1 run）。
    ASSERT_TRUE((*c)->put(bytes("fresh"), bytes("v"), 1000));
    expect_cold_view_matches(kd, "fresh", "v1-mixed");
    (*c)->close();
}

// 属性式对拍：随机 put/覆盖/删/merge/checkpoint/重开交错，全程影子对拍
// 开启（每次 get 自动双查），并对影子 map 的每个 key 显式跑 locate 对拍。
TEST_F(OkiLocateTest, RandomizedWorkloadZeroDrift) {
    CaskOptions o;
    o.read_write = true;
    o.max_file_size = 1024;
    auto open_cask = [&] {
        auto c = Cask::open(dir_.string(), o, &test_registry());
        EXPECT_TRUE(c);
        (*c)->keydir().set_oki_shadow_check(true);
        return std::move(*c);
    };
    auto c = open_cask();

    std::mt19937_64 rng(0x536236);  // 确定性种子
    std::map<std::string, std::string> shadow;
    auto rand_key = [&] {
        return "p" + std::to_string(rng() % 200);
    };

    for (int round = 0; round < 8; ++round) {
        for (int i = 0; i < 150; ++i) {
            const auto k = rand_key();
            switch (rng() % 5) {
                case 0:
                case 1:
                case 2: {
                    const std::string v = "v" + std::to_string(rng() % 1000);
                    ASSERT_TRUE(c->put(bytes(k), bytes(v), 1000));
                    shadow[k] = v;
                    break;
                }
                case 3: {
                    (void)c->remove(bytes(k), 2000);
                    shadow.erase(k);
                    break;
                }
                case 4: {
                    if (rng() % 8 == 0) {
                        ASSERT_TRUE(c->merge());
                    } else if (rng() % 8 == 1) {
                        ASSERT_TRUE(c->checkpoint());
                    } else if (rng() % 8 == 2) {
                        c->close();
                        c = open_cask();
                    }
                    break;
                }
            }
        }
        auto& kd = c->keydir();
        for (const auto& [k, v] : shadow) {
            expect_locate_matches_get(kd, k, "randomized");
            expect_cold_view_matches(kd, k, "randomized");
        }
        // 已删 key 也抽查（组合视图不得复活）。
        for (int i = 0; i < 200; ++i) {
            const std::string k = "p" + std::to_string(i);
            if (shadow.count(k) != 0) continue;
            EXPECT_FALSE(kd.get(k).has_value());
            EXPECT_FALSE(kd.locate(k).has_value()) << k;
        }
        EXPECT_EQ(kd.shadow_stats().drifts, 0u) << "round " << round;
    }
    c->close();
}

// ============================================================================
// S36-3：get 冷路径 + 逐出 + 读升温 + 块 LRU
// ============================================================================

// 逐出后 Cask::get 经组合视图照常出货；连续两次冷命中（频度门）后回填
// 升温——第三次起哈希直接命中。
TEST_F(OkiLocateTest, ColdGetServesEvictedKeyAndWarmFillPromotes) {
    CaskOptions o;
    o.read_write = true;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    auto& kd = (*c)->keydir();
    kd.set_oki_shadow_check(true);

    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE((*c)->put(bytes("w" + std::to_string(i)),
                              bytes("val" + std::to_string(i)), 1000));
    }
    ASSERT_TRUE((*c)->checkpoint());  // 行固化进 v2 run

    ASSERT_TRUE(kd.evict("w7"));
    EXPECT_FALSE(kd.evict("w7")) << "已逐出，二次逐出应 false";
    EXPECT_FALSE(kd.get("w7").has_value()) << "哈希缓存确实没了";

    // 第一次冷 get：值正确（组合视图 → run 行 → pread），频度门记一票。
    auto g1 = (*c)->get_owned(bytes("w7"));
    ASSERT_TRUE(g1.has_value());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(g1->value.data()),
                          g1->value.size()),
              "val7");
    EXPECT_FALSE(kd.get("w7").has_value()) << "首次冷命中不回填（二次门）";

    // 第二次冷 get：门放行 → 回填 → 哈希命中恢复。
    auto g2 = (*c)->get_owned(bytes("w7"));
    ASSERT_TRUE(g2.has_value());
    auto promoted = kd.get("w7");
    ASSERT_TRUE(promoted.has_value()) << "二次命中后应已回填升温";
    // 回填行与组合视图行逐字段一致（ord/loc 都是权威值）。
    expect_cold_view_matches(kd, "w7", "promoted");

    // 逐出不动逻辑计数（D4）：回填/逐出往返后 key_count 不漂移。
    ASSERT_TRUE(kd.evict("w0"));
    const auto info = kd.info();
    EXPECT_EQ(info.key_count, 20u) << "逐出/回填不得改 key_count";

    EXPECT_EQ(kd.shadow_stats().drifts, 0u);
    (*c)->close();
}

// 冷路径的三类 miss：墓碑 sentinel 被逐、真缺席、TTL 过期记录被逐——
// 都必须是干净的 kNotFound（不是错值也不是 IO 错）。
TEST_F(OkiLocateTest, ColdGetTombstoneAbsentAndTtlExpired) {
    CaskOptions o;
    o.read_write = true;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    auto& kd = (*c)->keydir();
    kd.set_oki_shadow_check(true);

    ASSERT_TRUE((*c)->put(bytes("dead"), bytes("v"), 1000));
    ASSERT_TRUE((*c)->remove(bytes("dead"), 1500));
    // TTL：expiry_at=100 已过（now 取实时钟，恒 > 100）。
    ASSERT_TRUE((*c)->put(bytes("ttl"), bytes("v"), 50, /*expiry_at=*/100));
    ASSERT_TRUE((*c)->checkpoint());

    // 墓碑 sentinel 也可逐（哈希里是 sentinel 形态）。
    (void)kd.evict("dead");  // sweep 可能已清，逐出与否都该 kNotFound
    auto gd = (*c)->get_owned(bytes("dead"));
    ASSERT_FALSE(gd.has_value());
    EXPECT_EQ(gd.error().kind, bitcask::CaskError::kNotFound);

    auto ga = (*c)->get_owned(bytes("never-existed"));
    ASSERT_FALSE(ga.has_value());
    EXPECT_EQ(ga.error().kind, bitcask::CaskError::kNotFound);

    ASSERT_TRUE(kd.evict("ttl"));
    auto gt = (*c)->get_owned(bytes("ttl"));
    ASSERT_FALSE(gt.has_value()) << "冷路径拿到 loc，但记录级 TTL 过滤兜底";
    EXPECT_EQ(gt.error().kind, bitcask::CaskError::kNotFound);

    EXPECT_EQ(kd.shadow_stats().drifts, 0u);
    (*c)->close();
}

// merge 搬迁 + 全归并之后的冷 get：组合视图给的是新位置（S36-2 的
// (ord,gen) 胜出格），被逐 key 从归并后的单 run 里也能正确出货。
TEST_F(OkiLocateTest, ColdGetAfterRelocationReadsNewLocation) {
    CaskOptions o;
    o.read_write = true;
    o.max_file_size = 512;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    auto& kd = (*c)->keydir();
    kd.set_oki_shadow_check(true);

    const std::string pad(64, 'x');
    for (int i = 0; i < 30; ++i) {
        ASSERT_TRUE((*c)->put(bytes("m" + std::to_string(i)),
                              bytes(pad + std::to_string(i)), 1000));
    }
    for (int i = 0; i < 15; ++i) {
        ASSERT_TRUE((*c)->put(bytes("m" + std::to_string(i)),
                              bytes(pad + "!" + std::to_string(i)), 1500));
    }
    ASSERT_TRUE((*c)->checkpoint());
    auto ms = (*c)->merge();
    ASSERT_TRUE(ms);
    ASSERT_GT(ms->records_kept, 0u);
    ASSERT_TRUE((*c)->checkpoint());  // 搬迁行固化

    for (int i = 0; i < 30; ++i) {
        const std::string k = "m" + std::to_string(i);
        ASSERT_TRUE(kd.evict(k)) << k;
        auto g = (*c)->get_owned(bytes(k));
        ASSERT_TRUE(g.has_value()) << k;
        const std::string want =
            (i < 15) ? pad + "!" + std::to_string(i) : pad + std::to_string(i);
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(g->value.data()),
                              g->value.size()),
                  want)
            << k;
    }
    EXPECT_EQ(kd.shadow_stats().drifts, 0u);
    (*c)->close();
}

// range 迭代（get_owned 回查）在逐出态照常出全量货。
TEST_F(OkiLocateTest, RangeIterServesEvictedKeys) {
    CaskOptions o;
    o.read_write = true;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    auto& kd = (*c)->keydir();
    kd.set_oki_shadow_check(true);

    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE((*c)->put(bytes("r" + std::to_string(i)),
                              bytes("v" + std::to_string(i)), 1000));
    }
    ASSERT_TRUE((*c)->checkpoint());
    for (int i = 0; i < 20; i += 2) {
        ASSERT_TRUE(kd.evict("r" + std::to_string(i)));
    }

    auto it = (*c)->make_range_iter(bitcask::RangeOptions{});
    ASSERT_TRUE(it.has_value());
    int n = 0;
    while (true) {
        auto e = (*it)->next();
        ASSERT_TRUE(e.has_value());
        if (!e->has_value()) break;
        ++n;
    }
    EXPECT_EQ(n, 20) << "被逐 key 不得从 range 输出里消失";
    EXPECT_EQ(kd.shadow_stats().drifts, 0u);
    (*c)->close();
}

// 块 LRU：相邻 key 的冷点查命中同一 4KiB 块——首次 miss 装载后其余全部
// 缓存命中；缓存关（容量 0）后照常正确（纯 pread）。
TEST_F(OkiLocateTest, BlockCacheServesAdjacentColdLookups) {
    CaskOptions o;
    o.read_write = true;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    auto& kd = (*c)->keydir();
    kd.set_oki_shadow_check(true);

    for (int i = 0; i < 50; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "b%03d", i);
        ASSERT_TRUE((*c)->put(bytes(buf), bytes("v"), 1000));
    }
    ASSERT_TRUE((*c)->checkpoint());

    const auto s0 = kd.oki().block_cache_stats();
    for (int i = 0; i < 50; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "b%03d", i);
        const auto r = kd.oki().locate(buf);
        ASSERT_EQ(r.status, OkiState::LocateStatus::kHit) << buf;
    }
    const auto s1 = kd.oki().block_cache_stats();
    EXPECT_GT(s1.hits, s0.hits) << "50 个相邻 key 应大量命中同块缓存";
    EXPECT_GT(s1.blocks, 0u);

    // 容量 0 = 关缓存：清空 + 后续不驻留，点查仍正确。
    kd.oki().reset_block_cache_capacity(0);
    for (int i = 0; i < 50; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "b%03d", i);
        const auto r = kd.oki().locate(buf);
        ASSERT_EQ(r.status, OkiState::LocateStatus::kHit) << buf;
    }
    EXPECT_EQ(kd.oki().block_cache_stats().blocks, 0u);
    kd.oki().reset_block_cache_capacity(256u << 20);  // 还原共享 OkiState

    EXPECT_EQ(kd.shadow_stats().drifts, 0u);
    (*c)->close();
}

// 并发：写者 + 冷读者 + 逐出者交错（无 merge——活性判定切 locate 排
// S36-5，见 KeyDir::evict 注释）。读者只接受「正确值或 kNotFound」。
TEST_F(OkiLocateTest, ConcurrentColdReadsWithWriterAndEvictor) {
    CaskOptions o;
    o.read_write = true;
    auto c = Cask::open(dir_.string(), o, &test_registry());
    ASSERT_TRUE(c);
    auto& kd = (*c)->keydir();
    kd.set_oki_shadow_check(true);
#if defined(__SANITIZE_THREAD__)          // GCC
#  define BITCASK_TSAN_ACTIVE 1
#elif defined(__has_feature)              // clang
#  if __has_feature(thread_sanitizer)
#    define BITCASK_TSAN_ACTIVE 1
#  endif
#endif
#ifdef BITCASK_TSAN_ACTIVE
    // TSan 下关闭 keydir 乐观读快路径（S29-6 回退开关）：seqlock 误报与
    // CI 既知豁免同根因（oki_range_test 同款处理）。本测试的对象是 S36-3
    // 冷路径/逐出/回填的并发，不是乐观读——关闭后这些路径仍全程受检，
    // 不新增豁免条目。
    kd.set_optimistic_reads(false);
#endif

    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE((*c)->put(bytes("s" + std::to_string(i)),
                              bytes("v"), 1000));
    }
    ASSERT_TRUE((*c)->checkpoint());

    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::thread writer([&] {
        std::mt19937_64 rng(1);
        for (int i = 0; i < 2000; ++i) {
            const std::string k = "s" + std::to_string(rng() % 100);
            if (rng() % 4 == 0) {
                (void)(*c)->remove(bytes(k), 2000);
            } else if (!(*c)->put(bytes(k), bytes("v"), 1000)) {
                errors.fetch_add(1);
            }
        }
        stop.store(true);
    });
    std::thread evictor([&] {
        std::mt19937_64 rng(2);
        while (!stop.load()) {
            (void)kd.evict("s" + std::to_string(rng() % 100));
        }
    });
    std::vector<std::thread> readers;
    for (int t = 0; t < 2; ++t) {
        readers.emplace_back([&, t] {
            std::mt19937_64 rng(100 + static_cast<std::uint64_t>(t));
            while (!stop.load()) {
                const std::string k = "s" + std::to_string(rng() % 100);
                auto g = (*c)->get_owned(bytes(k));
                if (!g && g.error().kind != bitcask::CaskError::kNotFound) {
                    errors.fetch_add(1);
                }
            }
        });
    }
    writer.join();
    evictor.join();
    for (auto& t : readers) t.join();

    EXPECT_EQ(errors.load(), 0);
    EXPECT_EQ(kd.shadow_stats().drifts, 0u);
    (*c)->close();
}
