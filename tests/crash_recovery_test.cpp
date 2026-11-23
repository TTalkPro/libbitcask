#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>
#include <bitcask/data_file.hpp>
#include <bitcask/format.hpp>

#include "support/crash_child.hpp"
#include "support/stale_writer_lock.hpp"

using bitcask::test::crash_exit;

#include <algorithm>
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
inline bitcask::oki::IndexRegistry& test_registry() {
    static bitcask::oki::IndexRegistry reg;
    return reg;
}

using bitcask::Cask;
using bitcask::CaskOptions;

// S37-2：原为 CrashRecoveryTest 的 protected static——崩溃场景函数在命名空间
// 作用域（非 fixture 成员），够不着。三者都是无状态纯函数，外提无副作用。
std::string key_for(int i) {
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "key%04d", i);
    return std::string(buf);
}

std::string value_for(int i) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "val%04d_fixed", i);
    return std::string(buf);
}

std::vector<std::byte> bytes(std::string_view s) {
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    return std::vector<std::byte>(p, p + s.size());
}

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

constexpr int kMidPutN = 50;

BITCASK_CRASH_SCENARIO(mid_put) {
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(dir, opts, &test_registry());
    if (!c) {
        std::fprintf(stderr, "child open failed: %s\n",
                     c.error().detail.c_str());
        crash_exit(1);
    }
    for (int i = 0; i < kMidPutN; ++i) {
        auto pr = (*c)->put(bytes(key_for(i)), bytes(value_for(i)),
                            static_cast<std::uint32_t>(1000 + i));
        if (!pr) {
            std::fprintf(stderr, "child put failed: %s\n",
                         pr.error().detail.c_str());
            crash_exit(1);
        }
        auto sr = (*c)->sync();
        if (!sr) {
            std::fprintf(stderr, "child sync failed: %s\n",
                         sr.error().detail.c_str());
            crash_exit(1);
        }
    }
    crash_exit(0);  // 不 close：写锁/句柄随进程消失（模拟崩溃）
}

TEST_F(CrashRecoveryTest, MidPutRestartFoldsCorrectly) {
    constexpr int kN = kMidPutN;

    // 原 fork 版接受 WIFEXITED || WIFSIGNALED（子进程被信号杀死也算「崩溃
    // 发生了」）。exec-self 下场景走完必然正常退出 0，异常终止由
    // spawn_crash_child 归一为 -2——两种都不该出现，故直接断言 0。
    ASSERT_EQ(bitcask::test::spawn_crash_child("mid_put", tmpdir_.string()), 0);

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
    // S67：尾巴只在「上一个写者没有正常关闭」时才截——把那把陈旧的写锁也留下。
    bitcask::test::plant_stale_writer_lock(tmpdir_, data_path);

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
    constexpr std::size_t kThreads = 8;  // 用作 vector 尺寸/下标
    constexpr int kPuts = 200;

    // value 长度随 i 大幅变化（~10..310），强制缓冲反复 clear/扩缩。
    auto value_for_tp = [](std::size_t tid, int i) {
        std::string v = "T" + std::to_string(tid) + "_V" + std::to_string(i) + "_";
        v.append(static_cast<std::size_t>(10 + (i * 7) % 300),
                 static_cast<char>('a' + static_cast<int>(tid % 26)));
        return v;
    };
    auto key_for_tp = [](std::size_t tid, int i) {
        return "t" + std::to_string(tid) + "_k" + std::to_string(i);
    };

    std::vector<fs::path> dirs(kThreads);
    for (std::size_t t = 0; t < kThreads; ++t) {
        dirs[t] = tmpdir_ / ("thread_" + std::to_string(t));
        std::error_code ec;
        fs::create_directories(dirs[t], ec);
    }

    std::atomic<bool> ok{true};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (std::size_t t = 0; t < kThreads; ++t) {
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
    for (std::size_t t = 0; t < kThreads; ++t) {
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

    // 数据面：get/put/remove/sync 均返回 kClosed（S12-5：与参数非法区分），不崩。
    auto g = (*c)->get(bytes("k0"));
    ASSERT_FALSE(g);
    EXPECT_EQ(g.error().kind, bitcask::CaskError::kClosed);

    auto p = (*c)->put(bytes("k1"), bytes("v1"));
    ASSERT_FALSE(p);
    EXPECT_EQ(p.error().kind, bitcask::CaskError::kClosed);

    auto rm = (*c)->remove(bytes("k0"));
    ASSERT_FALSE(rm);
    EXPECT_EQ(rm.error().kind, bitcask::CaskError::kClosed);

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

// S11-W4：并行全表扫描——所有 live key 恰好被访问一次、value 正确、并发 fn 安全。
TEST_F(CrashRecoveryTest, ParallelScanVisitsAllKeysOnce) {
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    constexpr int kN = 2000;
    for (int i = 0; i < kN; ++i) {
        ASSERT_TRUE((*c)->put(bytes(key_for(i)), bytes(value_for(i))));
    }
    // 删一部分 → 不应出现在扫描中。
    for (int i = 0; i < kN; i += 10) {
        ASSERT_TRUE((*c)->remove(bytes(key_for(i))));
    }

    std::mutex mu;
    std::map<std::string, std::string> seen;
    auto collect = [&](std::span<const std::byte> k, const bitcask::GetResultView& v) {
        std::string ks(reinterpret_cast<const char*>(k.data()), k.size());
        std::string vs(reinterpret_cast<const char*>(v.value.data()), v.value.size());
        std::lock_guard<std::mutex> lk(mu);
        auto [it, fresh] = seen.emplace(std::move(ks), std::move(vs));
        EXPECT_TRUE(fresh) << "key 被访问多次: " << it->first;  // 恰好一次
    };

    auto n = (*c)->parallel_scan(4, collect);
    ASSERT_TRUE(n);
    const std::size_t expected = static_cast<std::size_t>(kN - (kN + 9) / 10);
    EXPECT_EQ(*n, expected);
    EXPECT_EQ(seen.size(), expected);
    // 内容正确 + 被删的不在。
    for (int i = 0; i < kN; ++i) {
        auto it = seen.find(key_for(i));
        if (i % 10 == 0) {
            EXPECT_EQ(it, seen.end()) << "已删 key 出现在扫描: " << key_for(i);
        } else {
            ASSERT_NE(it, seen.end()) << "缺 key: " << key_for(i);
            EXPECT_EQ(it->second, value_for(i));
        }
    }

    // n_threads=0 → hardware_concurrency；结果一致。
    std::atomic<std::size_t> cnt{0};
    auto count_only = [&](std::span<const std::byte>, const bitcask::GetResultView&) {
        cnt.fetch_add(1, std::memory_order_relaxed);
    };
    auto n2 = (*c)->parallel_scan(0, count_only);
    ASSERT_TRUE(n2);
    EXPECT_EQ(*n2, expected);
    EXPECT_EQ(cnt.load(), expected);

    (*c)->close();
    // close 后 fail-fast（W3）。
    auto after = (*c)->parallel_scan(4, count_only);
    ASSERT_FALSE(after);
    EXPECT_EQ(after.error().kind, bitcask::CaskError::kClosed);
}


// =============================================================================
// S67（feedbacks/2026-11-23-recovery-truncates-synced-data-on-bad-record-header.md）
//
// 恢复此前把 fold 停下的地方一律当作「没写完的尾巴」截掉——哪怕那是已封口文件
// 中间的一处损坏，后面全是 fsync 过的完好记录。现在：
//   * 只有「上一个写者没有正常关闭」时它正在写的文件才可能有尾巴，才截断；
//   * 其余文件（含正常关闭后的全部文件）里的坏区跳过去接着读，文件一个字节不动；
//   * 不完整的原子批仍然整批不可见（跳过坏区不会把它的成员单独放出来）。
// =============================================================================

namespace s67 {

struct Rec {
    std::uint64_t offset;
    std::uint32_t size;
};

// data 文件里各记录的 (偏移, 长度)，按文件序。
std::vector<Rec> records_of(const std::filesystem::path& data) {
    std::vector<Rec> out;
    auto df = bitcask::fileops::DataFile::open(
        data.string(), bitcask::fileops::DataFile::Mode::kRead, false, false);
    EXPECT_TRUE(df.has_value());
    if (!df) return out;
    auto r = df->fold([&](const bitcask::codec::DataRecordView&, std::uint64_t off,
                          std::uint32_t total) { out.push_back({off, total}); });
    EXPECT_TRUE(r.has_value());
    return out;
}

void overwrite(const std::filesystem::path& data, std::uint64_t offset,
               const std::string& bytes_) {
    std::fstream f(data, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(f.is_open());
    f.seekp(static_cast<std::streamoff>(offset));
    f.write(bytes_.data(), static_cast<std::streamsize>(bytes_.size()));
    ASSERT_TRUE(f.good());
}

void drop_index(const std::filesystem::path& dir) {
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("kv.oki.", 0) == 0 || e.path().extension() == ".hint") {
            std::error_code ec;
            std::filesystem::remove(e.path(), ec);
        }
    }
}

std::vector<std::filesystem::path> data_files(const std::filesystem::path& dir) {
    std::vector<std::pair<unsigned long, std::filesystem::path>> v;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const std::string name = e.path().filename().string();
        constexpr std::string_view kSuffix = ".bitcask.data";
        if (name.size() <= kSuffix.size() ||
            name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
            continue;
        }
        v.emplace_back(std::strtoul(name.c_str(), nullptr, 10), e.path());
    }
    std::sort(v.begin(), v.end());
    std::vector<std::filesystem::path> out;
    for (auto& [id, path] : v) out.push_back(std::move(path));
    return out;
}

}  // namespace s67

// 反馈里的复现：2000 条 sync 过的记录，正常关闭；第 1000 条的记录头被写坏；索引
// 文件被删（下次打开从 data 全量重建）。改前：文件从损坏处被截断，后 1000 条消失。
TEST_F(CrashRecoveryTest, CorruptHeaderInSealedFileKeepsLaterRecords) {
    constexpr int kN = 2000;
    {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < kN; ++i) {
            ASSERT_TRUE((*c)->put(bytes(key_for(i)), bytes(value_for(i))));
        }
        ASSERT_TRUE((*c)->sync());
        (*c)->close();
    }
    const auto data = max_data_file();
    const auto recs = s67::records_of(data);
    ASSERT_EQ(recs.size(), static_cast<std::size_t>(kN));
    const auto size_before = std::filesystem::file_size(data);

    for (const char* what : {"header-ff", "value-size-huge", "zero-4k"}) {
        SCOPED_TRACE(what);
        // 每一轮在不同的位置再弄坏一处（累积）。
        std::vector<int> lost;
        if (std::string(what) == "header-ff") {
            s67::overwrite(data, recs[999].offset, std::string(27, '\xff'));
            lost = {999};
        } else if (std::string(what) == "value-size-huge") {
            // 只把 value_sz 改成一个大数：头里的长度越过文件尾。
            s67::overwrite(data, recs[400].offset + 23, std::string("\xff\xff\xff\x7f", 4));
            lost = {999, 400};
        } else {
            // 一段 4 KiB 的零（文件系统故障后常见的形态），跨好几条记录。
            s67::overwrite(data, recs[1500].offset + 5, std::string(4096, '\0'));
            lost = {999, 400};
            for (int i = 1500; i < kN; ++i) {
                if (recs[static_cast<std::size_t>(i)].offset < recs[1500].offset + 5 + 4096) {
                    lost.push_back(i);
                }
            }
            ASSERT_GT(lost.size(), 10u);
        }
        s67::drop_index(tmpdir_);

        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c) << c.error().detail;
        EXPECT_EQ(std::filesystem::file_size(data), size_before) << "sealed file was truncated";
        int present = 0;
        for (int i = 0; i < kN; ++i) {
            auto gr = (*c)->get_owned(bytes(key_for(i)));
            const bool expect_lost =
                std::find(lost.begin(), lost.end(), i) != lost.end();
            if (expect_lost) {
                EXPECT_FALSE(gr) << "corrupted record " << i << " came back";
            } else {
                ASSERT_TRUE(gr) << "record " << i << " lost: " << gr.error().detail;
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(gr->value.data()),
                                           gr->value.size()),
                          value_for(i));
                ++present;
            }
        }
        EXPECT_EQ(present, kN - static_cast<int>(lost.size()));
        // 还能接着写；再开一次结果不变。
        ASSERT_TRUE((*c)->put(bytes("after-" + std::string(what)), bytes("ok")));
        (*c)->close();
    }
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    EXPECT_TRUE((*c)->get_owned(bytes("after-header-ff")));
    EXPECT_TRUE((*c)->get_owned(bytes("after-zero-4k")));
    EXPECT_TRUE((*c)->get_owned(bytes(key_for(1999))));
    EXPECT_EQ(std::filesystem::file_size(data), size_before);
    (*c)->close();
}

// 索引还在时：水位之内那条坏记录在读的时候报 kBadCrc（此前并进 kIo）。
TEST_F(CrashRecoveryTest, CorruptRecordReadReportsBadCrc) {
    {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 50; ++i) {
            ASSERT_TRUE((*c)->put(bytes(key_for(i)), bytes(value_for(i))));
        }
        (*c)->close();
    }
    const auto data = max_data_file();
    const auto recs = s67::records_of(data);
    s67::overwrite(data, recs[20].offset, std::string(27, '\xff'));

    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    auto bad = (*c)->get_owned(bytes(key_for(20)));
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().kind, bitcask::CaskError::kBadCrc);
    EXPECT_TRUE((*c)->get_owned(bytes(key_for(19))));
    EXPECT_TRUE((*c)->get_owned(bytes(key_for(21))));
    (*c)->close();
}

// 正常关闭之后文件末尾多出来的垃圾 / 被掐掉一截：不是崩溃尾巴，不截断；
// 不完整的原子批仍然整批不可见。
TEST_F(CrashRecoveryTest, CleanCloseThenDamagedTailIsNotTruncated) {
    std::uint64_t size_before_batch = 0;
    {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 20; ++i) {
            ASSERT_TRUE((*c)->put(bytes(key_for(i)), bytes(value_for(i))));
        }
        ASSERT_TRUE((*c)->sync());
        size_before_batch = std::filesystem::file_size(max_data_file());
        const auto k1 = bytes("b1"), k2 = bytes("b2"), k3 = bytes("b3"), v = bytes("v");
        const std::vector<Cask::BatchOp> ops = {
            {.type = Cask::BatchOp::Type::kPut, .key = k1, .value = v},
            {.type = Cask::BatchOp::Type::kPut, .key = k2, .value = v},
            {.type = Cask::BatchOp::Type::kPut, .key = k3, .value = v}};
        ASSERT_TRUE((*c)->put_batch_atomic(ops));
        (*c)->close();
    }
    const auto data = max_data_file();
    std::error_code ec;
    const auto full = std::filesystem::file_size(data);
    std::filesystem::resize_file(data, full - 1, ec);   // 批的最后一个成员缺 1 字节
    ASSERT_FALSE(ec);
    s67::drop_index(tmpdir_);
    {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c) << c.error().detail;
        for (int i = 0; i < 20; ++i) EXPECT_TRUE((*c)->get_owned(bytes(key_for(i))));
        for (const char* k : {"b1", "b2", "b3"}) {
            auto gr = (*c)->get_owned(bytes(k));
            ASSERT_FALSE(gr) << "member of an incomplete batch became visible: " << k;
            EXPECT_EQ(gr.error().kind, bitcask::CaskError::kNotFound);
        }
        EXPECT_EQ(std::filesystem::file_size(data), full - 1) << "no crash evidence: must not truncate";
        ASSERT_TRUE((*c)->put(bytes("later"), bytes("ok")));
        (*c)->close();
    }
    // 同一个目录再开：结论不变（不会因为那个文件不再是最新的而把批成员放出来）。
    s67::drop_index(tmpdir_);
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    EXPECT_FALSE((*c)->get_owned(bytes("b1")));
    EXPECT_FALSE((*c)->get_owned(bytes("b3")));
    EXPECT_TRUE((*c)->get_owned(bytes("later")));
    EXPECT_TRUE((*c)->get_owned(bytes(key_for(19))));
    EXPECT_GT(size_before_batch, 0u);
    (*c)->close();
}

// 写者崩溃（留下陈旧写锁）+ 多个文件：崩溃时正在写的那个文件的尾巴照旧截断；
// 更早的已封口文件中间的坏区跳过、不截断。
TEST_F(CrashRecoveryTest, CrashedWriterTailTruncatedSealedFilesHealed) {
    {
        CaskOptions opts;
        opts.read_write = true;
        opts.max_file_size = 8 * 1024;  // 逼出多个文件
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 1000; ++i) {
            ASSERT_TRUE((*c)->put(bytes(key_for(i)), bytes(value_for(i))));
        }
        ASSERT_TRUE((*c)->sync());
        (*c)->close();
    }
    const auto files = s67::data_files(tmpdir_);
    ASSERT_GE(files.size(), 3u);
    const auto& sealed = files.front();
    const auto& active = files.back();
    const auto sealed_recs = s67::records_of(sealed);
    ASSERT_GE(sealed_recs.size(), 10u);
    const auto sealed_size = std::filesystem::file_size(sealed);
    const auto active_valid = std::filesystem::file_size(active);
    s67::overwrite(sealed, sealed_recs[3].offset, std::string(27, '\xff'));
    {
        std::ofstream out(active, std::ios::binary | std::ios::app);
        out << std::string(100, 'G');
    }
    s67::drop_index(tmpdir_);
    bitcask::test::plant_stale_writer_lock(tmpdir_, active);

    CaskOptions opts;
    opts.read_write = true;
    opts.max_file_size = 8 * 1024;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c) << c.error().detail;
    EXPECT_EQ(std::filesystem::file_size(sealed), sealed_size) << "sealed file truncated";
    EXPECT_EQ(std::filesystem::file_size(active), active_valid) << "crashed writer's tail kept";
    int missing = 0;
    for (int i = 0; i < 1000; ++i) {
        if (!(*c)->get_owned(bytes(key_for(i)))) ++missing;
    }
    EXPECT_EQ(missing, 1);  // 只丢被写坏头的那一条
    (*c)->close();
}

// DataFile::find_next_record / batch_end_at 的契约。
TEST_F(CrashRecoveryTest, FindNextRecordResyncRules) {
    using bitcask::fileops::DataFile;
    using bitcask::format::RecordType;
    const auto path = (tmpdir_ / "1.bitcask.data").string();
    std::vector<s67::Rec> recs;
    std::uint64_t embedded_at = 0;
    {
        auto f = DataFile::open(path, DataFile::Mode::kCreate);
        ASSERT_TRUE(f);
        auto add = [&](const std::string& k, const std::string& v) {
            auto w = f->write(RecordType::kDoc, 1, recs.size(), bytes(k), bytes(v));
            ASSERT_TRUE(w);
            recs.push_back({w->offset, w->total_size});
        };
        add("a", std::string(40, 'x'));
        add("b", std::string(40, 'y'));
        // 一条 value 里嵌着「一条完整记录的字节」的记录：把 a 的整条记录映像放进
        // value 中间。嵌着的那条后面不是另一条完好记录，不能当成重新同步点。
        std::string img(recs[0].size, '\0');
        {
            std::ifstream in(path, std::ios::binary);
            in.read(img.data(), static_cast<std::streamsize>(img.size()));
        }
        add("c", std::string(10, 'p') + img + std::string(10, 'q'));
        embedded_at = recs[2].offset + 27 + 1 + 10;
        add("d", std::string(40, 'z'));
        add("e", std::string(40, 'w'));
    }
    auto f = DataFile::open(path, DataFile::Mode::kRead, false, false);
    ASSERT_TRUE(f);
    // 从任意一条的中间出发，找到的是下一条的起点。
    auto n0 = f->find_next_record(recs[0].offset + 1);
    ASSERT_TRUE(n0.has_value() && n0->has_value());
    EXPECT_EQ(**n0, recs[1].offset);
    // 从 c 的头后面出发：越过嵌在 value 里的那条映像，落在 d。
    auto n2 = f->find_next_record(recs[2].offset + 1);
    ASSERT_TRUE(n2.has_value() && n2->has_value());
    EXPECT_NE(**n2, embedded_at);
    EXPECT_EQ(**n2, recs[3].offset);
    // 最后一条：恰好结束在文件尾，算完好。
    auto n4 = f->find_next_record(recs[3].offset + 1);
    ASSERT_TRUE(n4.has_value() && n4->has_value());
    EXPECT_EQ(**n4, recs[4].offset);
    // 之后没有了。
    auto n5 = f->find_next_record(recs[4].offset + 1);
    ASSERT_TRUE(n5.has_value());
    EXPECT_FALSE(n5->has_value());
    // 普通记录不是批头。
    auto be = f->batch_end_at(recs[0].offset);
    ASSERT_TRUE(be.has_value());
    EXPECT_FALSE(be->has_value());
}

}  // namespace
