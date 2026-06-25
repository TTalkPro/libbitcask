#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/keydir_registry.hpp>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

// S6-P0-pre：open() 现强制非空 registry。测试/bench 共享一个进程内 registry——
// 各用例用唯一目录名，互不冲突；同用例内 open→close→reopen 经 refcount 归零
// 重新从盘加载，与旧 nullptr 行为等价。
inline bitcask::keydir::KeyDirRegistry& test_registry() {
    static bitcask::keydir::KeyDirRegistry reg;
    return reg;
}

using bitcask::Cask;
using bitcask::CaskOptions;

class CrashRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tmpdir_ = std::filesystem::temp_directory_path() /
                  (std::string("bitcask_crash_recovery_test_") + info->name());
        std::error_code ec;
        std::filesystem::remove_all(tmpdir_, ec);
        std::filesystem::create_directories(tmpdir_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(tmpdir_, ec);
    }

    static std::string key_for(int i) {
        char buf[16]{};
        std::snprintf(buf, sizeof(buf), "key%04d", i);
        return std::string(buf);
    }

    static std::string value_for(int i) {
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "val%04d_fixed", i);
        return std::string(buf);
    }

    static std::vector<std::byte> bytes(std::string_view s) {
        const auto* p = reinterpret_cast<const std::byte*>(s.data());
        return std::vector<std::byte>(p, p + s.size());
    }

    std::filesystem::path max_data_file() const {
        std::uint64_t max_id = 0;
        std::filesystem::path found;
        for (const auto& e : std::filesystem::directory_iterator(tmpdir_)) {
            if (!e.is_regular_file()) continue;
            const std::string name = e.path().filename().string();
            constexpr std::string_view kSuffix = ".bitcask.data";
            if (name.size() <= kSuffix.size()) continue;
            if (name.compare(name.size() - kSuffix.size(), kSuffix.size(),
                             kSuffix) != 0) {
                continue;
            }
            const std::string id_str =
                name.substr(0, name.size() - kSuffix.size());
            char* end = nullptr;
            const unsigned long id = std::strtoul(id_str.c_str(), &end, 10);
            if (end != id_str.c_str() + id_str.size()) continue;
            if (id >= max_id) {
                max_id = id;
                found = e.path();
            }
        }
        return found;
    }

    std::filesystem::path tmpdir_;
};

TEST_F(CrashRecoveryTest, MidPutRestartFoldsCorrectly) {
    constexpr int kN = 50;

    const pid_t child = fork();
    ASSERT_NE(child, -1) << "fork failed";

    if (child == 0) {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        if (!c) {
            std::fprintf(stderr, "child open failed: %s\n",
                         c.error().detail.c_str());
            _exit(1);
        }
        for (int i = 0; i < kN; ++i) {
            auto pr = (*c)->put(bytes(key_for(i)), bytes(value_for(i)),
                                static_cast<std::uint32_t>(1000 + i));
            if (!pr) {
                std::fprintf(stderr, "child put failed: %s\n",
                             pr.error().detail.c_str());
                _exit(1);
            }
            auto sr = (*c)->sync();
            if (!sr) {
                std::fprintf(stderr, "child sync failed: %s\n",
                             sr.error().detail.c_str());
                _exit(1);
            }
        }
        _exit(0);
    }

    int status = 0;
    const pid_t waited = waitpid(child, &status, 0);
    ASSERT_NE(waited, -1);
    ASSERT_TRUE(WIFEXITED(status) || WIFSIGNALED(status));

    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c) << "reopen after crash failed: " << c.error().detail;

    auto it = (*c)->make_iter();
    auto start = it->start();
    ASSERT_TRUE(start);

    std::map<std::string, std::string> recovered;
    while (true) {
        auto entry = it->next();
        ASSERT_TRUE(entry);
        if (!*entry) break;
        std::string key(reinterpret_cast<const char*>((*entry)->key.data()),
                        (*entry)->key.size());
        std::string value(
            reinterpret_cast<const char*>((*entry)->value.data()),
            (*entry)->value.size());
        recovered.emplace(std::move(key), std::move(value));
    }

    EXPECT_GE(recovered.size(), 1u);
    EXPECT_EQ(recovered.size(), static_cast<std::size_t>(kN))
        << "all sync'd puts should be recoverable";

    for (const auto& [k, v] : recovered) {
        ASSERT_GE(k.size(), 3u);
        const int idx = std::atoi(k.substr(3).c_str());
        EXPECT_EQ(v, value_for(idx))
            << "corruption detected for key " << k;
    }

    EXPECT_EQ(recovered.count(key_for(0)), 1u);

    (*c)->close();
}

TEST_F(CrashRecoveryTest, TornWriteTruncated) {
    constexpr int kN = 10;
    constexpr std::string_view kGarbage =
        "GARBAGE_DATA_NOT_A_VALID_RECORD_HEADER_THIS_IS_LONGER_THAN_100_BYTES_"
        "AND_WILL_NEVER_LOOK_LIKE_A_REAL_BITCASK_RECORD";
    constexpr std::size_t kGarbageBytes = 100;
    static_assert(kGarbage.size() >= kGarbageBytes);

    {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < kN; ++i) {
            ASSERT_TRUE((*c)->put(bytes(key_for(i)), bytes(value_for(i)),
                                  static_cast<std::uint32_t>(2000 + i)));
        }
        ASSERT_TRUE((*c)->sync());
        (*c)->close();
    }

    const std::filesystem::path data_path = max_data_file();
    ASSERT_FALSE(data_path.empty()) << "no data file found after clean close";

    const std::filesystem::path hint_path =
        data_path.parent_path() /
        (data_path.stem().string() + ".hint");

    std::error_code ec;
    std::filesystem::remove(hint_path, ec);

    const std::uint64_t valid_size =
        std::filesystem::file_size(data_path, ec);
    ASSERT_FALSE(ec) << "failed to stat data file";

    {
        std::ofstream out(data_path, std::ios::binary | std::ios::app);
        ASSERT_TRUE(out.is_open());
        out.write(kGarbage.data(), static_cast<std::streamsize>(kGarbageBytes));
        ASSERT_TRUE(out.good());
    }

    const std::uint64_t corrupted_size =
        std::filesystem::file_size(data_path, ec);
    ASSERT_FALSE(ec);
    EXPECT_EQ(corrupted_size, valid_size + kGarbageBytes);

    {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c) << "reopen with torn tail failed: " << c.error().detail;

        const std::uint64_t repaired_size =
            std::filesystem::file_size(data_path, ec);
        ASSERT_FALSE(ec);
        EXPECT_EQ(repaired_size, valid_size)
            << "torn tail was not truncated to last_valid_end";

        for (int i = 0; i < kN; ++i) {
            auto gr = (*c)->get_owned(bytes(key_for(i)));
            ASSERT_TRUE(gr) << "key " << key_for(i) << " missing after reopen";
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(
                                           gr->value.data()),
                                       gr->value.size()),
                      value_for(i));
        }

        (*c)->close();
    }
}

// R3:多数据文件纯 KV 冷启动并行 fold。小 max_file_size 强制滚动出多个
// data 文件，触发 load_keydir_from_disk 的并行路径（search_layer==null）。
// 含跨文件覆盖（同 key 多版本）以校验并行下 (file_id,tstamp,offset) LWW
// 冲突解析与到达序无关——最后写入的值必胜。
TEST_F(CrashRecoveryTest, MultiFileParallelFoldRecovers) {
    constexpr int kN = 400;
    // ~100B 值，4 KiB 文件 → 每文件 ~30 record，共 ~13 个 data 文件。
    auto big_value = [](int i) {
        std::string v = value_for(i);
        v.resize(100, 'x');
        return v;
    };

    {
        CaskOptions opts;
        opts.read_write = true;
        opts.max_file_size = 4096;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c) << c.error().detail;

        std::uint32_t ts = 1000;
        // 第一轮：全部写入旧值（故意写错的 value，稍后被覆盖）。
        for (int i = 0; i < kN; ++i) {
            auto pr = (*c)->put(bytes(key_for(i)), bytes("STALE_VERSION"), ts++);
            ASSERT_TRUE(pr) << pr.error().detail;
        }
        // 第二轮：用更高 tstamp 覆盖成正确值，跨越多个后续文件。
        for (int i = 0; i < kN; ++i) {
            auto pr = (*c)->put(bytes(key_for(i)), bytes(big_value(i)), ts++);
            ASSERT_TRUE(pr) << pr.error().detail;
        }
        (*c)->close();
    }

    // 应当滚出多个 data 文件，否则没测到并行路径。
    int data_files = 0;
    for (const auto& e : std::filesystem::directory_iterator(tmpdir_)) {
        const std::string name = e.path().filename().string();
        if (name.size() > 13 &&
            name.compare(name.size() - 13, 13, ".bitcask.data") == 0) {
            ++data_files;
        }
    }
    ASSERT_GT(data_files, 1) << "expected multiple data files to exercise "
                                "parallel fold";

    {
        CaskOptions opts;
        opts.read_write = true;
        opts.max_file_size = 4096;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c) << "parallel reopen failed: " << c.error().detail;

        for (int i = 0; i < kN; ++i) {
            auto gr = (*c)->get_owned(bytes(key_for(i)));
            ASSERT_TRUE(gr) << "key " << key_for(i) << " missing after reopen";
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(
                                           gr->value.data()),
                                       gr->value.size()),
                      big_value(i))
                << "LWW resolution wrong under parallel fold for " << key_for(i);
        }
        (*c)->close();
    }
}

// X1:迭代器存活期间调用 close() 不得 UAF。close() 会 reset keydir_（经
// registry 递减引用计数释放 KeyDir），而 CaskIter 内部的 IterHandle 持
// KeyDir* 裸指针；迭代器在 close() 后析构（release()→BarrierGuard 锁
// KeyDir mutex）若 KeyDir 已释放即 heap-use-after-free。CaskIter 现 pin
// 一份 KeyDir shared_ptr 兜住其生命周期。本测试在 TSan 下守护该契约。
TEST_F(CrashRecoveryTest, IteratorAliveAcrossCloseNoUaf) {
    constexpr int kN = 20;
    {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c) << c.error().detail;
        for (int i = 0; i < kN; ++i) {
            ASSERT_TRUE((*c)->put(bytes(key_for(i)), bytes(value_for(i)),
                                  static_cast<std::uint32_t>(3000 + i)));
        }
        (*c)->close();
    }

    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c) << c.error().detail;

    auto it = (*c)->make_iter();
    auto start = it->start();
    ASSERT_TRUE(start);
    // 部分迭代后即 close()——迭代器仍存活，随后离开作用域析构。
    auto first = it->next();
    ASSERT_TRUE(first);

    (*c)->close();  // reset keydir_ 于迭代器存活期间

    // it 在此后析构 → release() 锁 KeyDir mutex。pin 生效时不应 UAF/崩溃。
    it.reset();
    SUCCEED();
}

// T7:X1 显式 release 路径回归。IteratorAliveAcrossCloseNoUaf 走隐式析构
// （it.reset()→~CaskIter→release）；本例在 close() 后**显式** it->release()，
// 验证显式释放序（iter_->release → iter_.reset → keydir_pin_.reset）下 KeyDir
// 仍存活，且 release() 幂等（二次调用不崩）。
TEST_F(CrashRecoveryTest, IteratorExplicitReleaseAfterCloseNoUaf) {
    {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c) << c.error().detail;
        for (int i = 0; i < 20; ++i) {
            ASSERT_TRUE((*c)->put(bytes(key_for(i)), bytes(value_for(i)),
                                  static_cast<std::uint32_t>(3000 + i)));
        }
        (*c)->close();
    }

    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c) << c.error().detail;

    auto it = (*c)->make_iter();
    ASSERT_TRUE(it->start());
    ASSERT_TRUE(it->next());

    (*c)->close();    // KeyDir 经 pin 存活
    it->release();    // 显式 release：锁已释放出 registry 的 KeyDir mutex
    it->release();    // 幂等：二次 release 不得崩
    it.reset();       // ~CaskIter 再走一次 release（已空，no-op）
    SUCCEED();
}

// T8:X1 多 iterator 交错 release → 仅最后一个（keyfolders_ fetch_sub 返回 1）
// 触发 pending 应用 + MultiEntry 折叠；全程 KeyDir 经多 pin 在 close() 后存活。
// 真正要守护的是 KeyDir 内部协调（折叠时机 + 活 iterator 一致快照），非
// shared_ptr 计数本身。需 TSan 验证折叠期无 race。
TEST_F(CrashRecoveryTest, MultiIteratorInterleavedReleaseAfterClose) {
    auto str = [](const std::vector<std::byte>& v) {
        return std::string(reinterpret_cast<const char*>(v.data()), v.size());
    };

    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c) << c.error().detail;
    auto& cask = **c;

    constexpr int kN = 5;
    for (int i = 0; i < kN; ++i) {
        ASSERT_TRUE(cask.put(bytes(key_for(i)), bytes(value_for(i)),
                             static_cast<std::uint32_t>(4000 + i)));
    }

    // it1 先 start；之后在 fold 态 overwrite k0 → SingleEntry 升级成 MultiEntry
    // （旧 revision 留给 it1，新 revision 给后启 iterator）。
    auto it1 = cask.make_iter();
    ASSERT_TRUE(it1->start());
    const std::string k0_new = "k0_NEW_REVISION";
    ASSERT_TRUE(cask.put(bytes(key_for(0)), bytes(k0_new), 4100));

    auto it2 = cask.make_iter();
    ASSERT_TRUE(it2->start());
    auto it3 = cask.make_iter();
    ASSERT_TRUE(it3->start());

    cask.close();  // keyfolders_==3，snapshot 跳过；KeyDir 经三 pin 存活

    // 交错 release：it1、it3 均非最后一个 → 不折叠。
    it1->release();
    it3->release();

    // it2 仍活（keyfolders_==1）：drain 全部 entry，应见 k0..k4 且 k0=新 revision。
    std::map<std::string, std::string> got;
    while (true) {
        auto e = it2->next();
        ASSERT_TRUE(e) << "post-close next() 失败：" << e.error().detail;
        if (!e->has_value()) break;
        got[str((*e)->key)] = str((*e)->value);
    }
    EXPECT_EQ(got.size(), static_cast<std::size_t>(kN));
    EXPECT_EQ(got[key_for(0)], k0_new)
        << "后启 iterator 应看到 MultiEntry 链头的新 revision";

    it2->release();  // keyfolders_ 1→0 → 最后一个 → 触发折叠（pinned KeyDir 上）
    SUCCEED();
}

// T6:put 路径的 `thread_local encoded` 缓冲（tier-3 ⑩）跨线程无干扰。
// 每线程独立 Cask（单写者模型），并发 put 变长 value——若缓冲是 static 而非
// thread_local，并发会数据竞争 / 串台；若跨 put 未正确 clear，变长 value 会读到
// 残留字节。重开逐值校验 + TSan 守护。
TEST_F(CrashRecoveryTest, ThreadLocalEncodedBufferNoCrossThreadInterference) {
    namespace fs = std::filesystem;
    constexpr int kThreads = 8;
    constexpr int kPuts = 200;

    // value 长度随 i 大幅变化（~10..310），强制缓冲反复 clear/扩缩。
    auto value_for_tp = [](int tid, int i) {
        std::string v = "T" + std::to_string(tid) + "_V" + std::to_string(i) + "_";
        v.append(static_cast<std::size_t>(10 + (i * 7) % 300),
                 static_cast<char>('a' + (tid % 26)));
        return v;
    };
    auto key_for_tp = [](int tid, int i) {
        return "t" + std::to_string(tid) + "_k" + std::to_string(i);
    };

    std::vector<fs::path> dirs(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        dirs[t] = tmpdir_ / ("thread_" + std::to_string(t));
        std::error_code ec;
        fs::create_directories(dirs[t], ec);
    }

    std::atomic<bool> ok{true};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            CaskOptions opts;
            opts.read_write = true;
            auto c = Cask::open(dirs[t].string(), opts, &test_registry());
            if (!c) { ok = false; return; }
            for (int i = 0; i < kPuts; ++i) {
                if (!(*c)->put(bytes(key_for_tp(t, i)), bytes(value_for_tp(t, i)),
                               static_cast<std::uint32_t>(1000 + i))) {
                    ok = false;
                    return;
                }
            }
            (*c)->close();
        });
    }
    for (auto& w : workers) w.join();
    ASSERT_TRUE(ok.load()) << "并发 put 失败";

    // 逐 Cask 重开，校验每个 value 完整无串台。
    for (int t = 0; t < kThreads; ++t) {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(dirs[t].string(), opts, &test_registry());
        ASSERT_TRUE(c) << "reopen thread_" << t << " failed";
        for (int i = 0; i < kPuts; ++i) {
            auto gr = (*c)->get_owned(bytes(key_for_tp(t, i)));
            ASSERT_TRUE(gr) << "missing tid=" << t << " i=" << i;
            std::string got(reinterpret_cast<const char*>(gr->value.data()),
                            gr->value.size());
            EXPECT_EQ(got, value_for_tp(t, i))
                << "value 串台/残留 tid=" << t << " i=" << i;
        }
        (*c)->close();
    }
}

// S11-W1：多线程并发写**同一** Cask handle 的正确性护栏（通用 C++ 库定位）。
// 区别于上一个 T6 测试（每线程独立 Cask）——这里 N 个写线程**共享一个 handle**，
// 正是 W1（write_mu_ 写路径互斥）针对的场景。无 write_mu_ 时并发
// active_data_->write() 竞争 current_offset_/write_buf_/batch_buf_ + writes_since_sync_
// → 数据损坏/串台/offset 错位（TSan 插桩下直接报 data race）。
// 各线程写**互不相交**的 key 段 → 无跨线程 LWW，期望态确定可断言。
TEST_F(CrashRecoveryTest, ConcurrentWritersSharedCaskNoCorruption) {
    CaskOptions opts;
    opts.read_write = true;
    opts.sync_every_n = 4;  // 走 maybe_group_commit 组提交（writes_since_sync_ 亦受 write_mu_ 护）
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    constexpr int kThreads = 8;
    constexpr int kOps = 300;
    auto key_tp = [](int t, int i) {
        char b[32]{}; std::snprintf(b, sizeof b, "k_%02d_%05d", t, i);
        return std::string(b);
    };
    // 变长 value（8..24B，跨 SSO 边界）→ 若 write_buf_ 并发串台则读回残留/错位可检。
    auto val_tp = [](int t, int i) {
        std::string v(static_cast<std::size_t>(8 + (i % 17)),
                      static_cast<char>('A' + t));
        v += "_" + std::to_string(t) + "_" + std::to_string(i);
        return v;
    };

    std::atomic<bool> ok{true};

    // 阶段 1：N 线程并发 put（互不相交 key 段）。
    {
        std::vector<std::thread> ws;
        ws.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            ws.emplace_back([&, t] {
                for (int i = 0; i < kOps; ++i) {
                    if (!(*c)->put(bytes(key_tp(t, i)), bytes(val_tp(t, i)),
                                   static_cast<std::uint32_t>(1000 + i))) {
                        ok = false; return;
                    }
                }
            });
        }
        for (auto& w : ws) w.join();
    }
    ASSERT_TRUE(ok.load()) << "并发 put 失败";

    auto verify_all_present = [&](Cask& cask) {
        for (int t = 0; t < kThreads; ++t) {
            for (int i = 0; i < kOps; ++i) {
                auto gr = cask.get_owned(bytes(key_tp(t, i)));
                ASSERT_TRUE(gr) << "missing t=" << t << " i=" << i;
                std::string got(reinterpret_cast<const char*>(gr->value.data()),
                                gr->value.size());
                EXPECT_EQ(got, val_tp(t, i)) << "value 串台/损坏 t=" << t << " i=" << i;
            }
        }
    };
    verify_all_present(**c);

    // 阶段 2：N 线程并发 remove（各删自己偶数 i 的 key）+ 并发校验 put/remove 混合写路径。
    {
        std::vector<std::thread> ws;
        ws.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            ws.emplace_back([&, t] {
                for (int i = 0; i < kOps; i += 2) {
                    if (!(*c)->remove(bytes(key_tp(t, i)),
                                      static_cast<std::uint32_t>(5000 + i))) {
                        ok = false; return;
                    }
                }
            });
        }
        for (auto& w : ws) w.join();
    }
    ASSERT_TRUE(ok.load()) << "并发 remove 失败";

    auto verify_after_remove = [&](Cask& cask) {
        for (int t = 0; t < kThreads; ++t) {
            for (int i = 0; i < kOps; ++i) {
                auto gr = cask.get_owned(bytes(key_tp(t, i)));
                if (i % 2 == 0) {
                    EXPECT_FALSE(gr) << "应已删除 t=" << t << " i=" << i;
                } else {
                    ASSERT_TRUE(gr) << "missing t=" << t << " i=" << i;
                    std::string got(reinterpret_cast<const char*>(gr->value.data()),
                                    gr->value.size());
                    EXPECT_EQ(got, val_tp(t, i)) << "value 损坏 t=" << t << " i=" << i;
                }
            }
        }
    };
    verify_after_remove(**c);
    (*c)->close();

    // 重开：校验落盘字节无损坏（offset/CRC 正确）。
    auto c2 = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c2);
    verify_after_remove(**c2);
    (*c2)->close();
}

// S11-W3：生命周期 fail-fast——close() 后调用公共方法返回错误码而非 UB（通用
// C++ 库的防误用契约）。同时验证 close() 幂等（二次 close 安全）。
TEST_F(CrashRecoveryTest, OperationsAfterCloseReturnErrorNotUb) {
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    ASSERT_TRUE((*c)->put(bytes("k0"), bytes("v0")));

    // close 前 make_iter（合法）——但在 close 后才 start，验证 start fail-fast。
    auto it_after = (*c)->make_iter();

    (*c)->close();

    // 数据面：get/put/remove/sync 均返回 kInvalidOption，不崩。
    auto g = (*c)->get(bytes("k0"));
    ASSERT_FALSE(g);
    EXPECT_EQ(g.error().kind, bitcask::CaskError::kInvalidOption);

    auto p = (*c)->put(bytes("k1"), bytes("v1"));
    ASSERT_FALSE(p);
    EXPECT_EQ(p.error().kind, bitcask::CaskError::kInvalidOption);

    auto rm = (*c)->remove(bytes("k0"));
    ASSERT_FALSE(rm);
    EXPECT_EQ(rm.error().kind, bitcask::CaskError::kInvalidOption);

    EXPECT_FALSE((*c)->sync());
    EXPECT_FALSE((*c)->merge());

    // 内省：安全默认值，不解引用已释放的 keydir_。
    EXPECT_TRUE((*c)->is_empty_estimate());
    EXPECT_EQ((*c)->read_handle_count(), 0u);

    // close 后 start 迭代器 → fail-fast。
    auto sr = it_after->start(0, 0, false);
    EXPECT_FALSE(sr);

    // close 幂等：二次 close 不崩。
    (*c)->close();
}

}  // namespace
