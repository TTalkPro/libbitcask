// Unit tests for bitcask::io::PosixFile.


#include <cerrno>
#include "support/test_paths.hpp"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bitcask/io.hpp"
#include "bitcask/detail/file_util.hpp"  // W5：adopt_stream / flush_and_sync

using bitcask::io::IoError;
using bitcask::io::OpenFlag;
using bitcask::io::PosixFile;
using bitcask::io::ReadEof;
using bitcask::io::ReadOk;

namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("bitcask_test_" + std::to_string(bitcask::test::test_pid()) + "_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path_);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path_, ec); }
    std::string file(const std::string& name) const {
        return (path_ / name).string();
    }
private:
    fs::path path_;
};

std::span<const std::byte> as_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

}  // namespace

TEST(PosixFile, OpenCreateAndWriteAndRead) {
    TempDir td;
    const auto path = td.file("a.dat");

    auto f = PosixFile::open(path, OpenFlag::kCreate);
    ASSERT_TRUE(f) << "open failed";
    ASSERT_TRUE(f->write(as_bytes("hello"))) << "write failed";

    auto g = PosixFile::open(path, OpenFlag::kReadOnly);
    ASSERT_TRUE(g);
    auto r = g->pread(0, 5);
    ASSERT_TRUE(r);
    ASSERT_TRUE(std::holds_alternative<ReadOk>(*r));
    auto& ok = std::get<ReadOk>(*r);
    EXPECT_EQ(ok.data.size(), 5u);
    EXPECT_EQ(0, std::memcmp(ok.data.data(), "hello", 5));
}

TEST(PosixFile, CreateExclusiveFailsIfExists) {
    TempDir td;
    const auto path = td.file("a.dat");
    auto f1 = PosixFile::open(path, OpenFlag::kCreate);
    ASSERT_TRUE(f1);
    auto f2 = PosixFile::open(path, OpenFlag::kCreate);
    ASSERT_FALSE(f2);
    EXPECT_EQ(f2.error().errnum, EEXIST);
}

TEST(PosixFile, OpenReadonlyMissingFile) {
    TempDir td;
    auto f = PosixFile::open(td.file("nope.dat"), OpenFlag::kReadOnly);
    ASSERT_FALSE(f);
    EXPECT_EQ(f.error().errnum, ENOENT);
}

TEST(PosixFile, PreadShortReturnsPartial) {
    TempDir td;
    auto f = PosixFile::open(td.file("p.dat"), OpenFlag::kCreate);
    ASSERT_TRUE(f);
    ASSERT_TRUE(f->pwrite(0, as_bytes("abc")));

    auto r = f->pread(0, 100);
    ASSERT_TRUE(r);
    auto& ok = std::get<ReadOk>(*r);
    EXPECT_EQ(ok.data.size(), 3u);
}

TEST(PosixFile, PreadAtEofReturnsEof) {
    TempDir td;
    auto f = PosixFile::open(td.file("e.dat"), OpenFlag::kCreate);
    ASSERT_TRUE(f);
    auto r = f->pread(0, 16);
    ASSERT_TRUE(r);
    EXPECT_TRUE(std::holds_alternative<ReadEof>(*r));
}

TEST(PosixFile, AppendModeWritesAtEnd) {
    // Default open flags include O_APPEND, so consecutive writes accumulate.
    TempDir td;
    const auto path = td.file("ap.dat");
    {
        auto f = PosixFile::open(path, OpenFlag::kCreate);
        ASSERT_TRUE(f);
        ASSERT_TRUE(f->write(as_bytes("AAA")));
    }
    {
        auto f = PosixFile::open(path, OpenFlag::kNone);
        ASSERT_TRUE(f);
        ASSERT_TRUE(f->write(as_bytes("BBB")));
    }
    auto g = PosixFile::open(path, OpenFlag::kReadOnly);
    ASSERT_TRUE(g);
    auto r = g->pread(0, 6);
    ASSERT_TRUE(r);
    auto& ok = std::get<ReadOk>(*r);
    EXPECT_EQ(0, std::memcmp(ok.data.data(), "AAABBB", 6));
}

TEST(PosixFile, SeekAndPosition) {
    TempDir td;
    auto f = PosixFile::open(td.file("s.dat"), OpenFlag::kCreate);
    ASSERT_TRUE(f);
    ASSERT_TRUE(f->pwrite(0, as_bytes("0123456789")));

    auto p = f->seek(4, SEEK_SET);
    ASSERT_TRUE(p);
    EXPECT_EQ(*p, 4u);
    auto r = f->read(2);
    ASSERT_TRUE(r);
    auto& ok = std::get<ReadOk>(*r);
    EXPECT_EQ(0, std::memcmp(ok.data.data(), "45", 2));

    ASSERT_TRUE(f->seek_bof());
    auto p2 = f->seek(0, SEEK_CUR);
    ASSERT_TRUE(p2);
    EXPECT_EQ(*p2, 0u);
}

TEST(PosixFile, TruncateHereCutsToCurrentOffset) {
    TempDir td;
    const auto path = td.file("t.dat");
    auto f = PosixFile::open(path, OpenFlag::kCreate);
    ASSERT_TRUE(f);
    ASSERT_TRUE(f->pwrite(0, as_bytes("abcdef")));

    ASSERT_TRUE(f->seek(3, SEEK_SET));
    ASSERT_TRUE(f->truncate_here());

    auto sz = fs::file_size(path);
    EXPECT_EQ(sz, 3u);
}

TEST(PosixFile, MoveCloses) {
    TempDir td;
    auto f = PosixFile::open(td.file("m.dat"), OpenFlag::kCreate);
    ASSERT_TRUE(f);
    // S37-5：不能写死 int——Windows 上 FileHandle 是 HANDLE(void*)。
    bitcask::io::FileHandle fd = f->fd();
    PosixFile g = std::move(*f);
    EXPECT_FALSE(f->is_open());
    EXPECT_EQ(g.fd(), fd);
}

TEST(PosixFile, SyncOnRegularFileIsOk) {
    TempDir td;
    auto f = PosixFile::open(td.file("y.dat"), OpenFlag::kCreate);
    ASSERT_TRUE(f);
    EXPECT_TRUE(f->sync());
}

// ---------------------------------------------------------------------------
// S37-5：进程实例令牌（stale-lock 判定，设计稿 C4 / 风险 #2）
//
// 这组用例守的是「PID 复用误判」这一整个移植里最容易造成生产事故的单点：
// 判错的后果是**拒绝回收有效的 stale lock → 库彻底打不开**，且只在
// 「新进程恰好复用了崩溃进程的 PID」这一时序下复现——没有单测就只能等线上。
//
// 关键是**否定用例**：令牌不符时必须判死。若 process_alive(pid, token) 忽略
// 了令牌参数（比如实现退化成只查 pid），全套 ctest 照样全绿，而守卫已然失效。
// ---------------------------------------------------------------------------
TEST(ProcessToken, SelfIsAliveWithMatchingToken) {
    const int me = bitcask::io::current_process_id();
    EXPECT_GT(me, 0);
    const std::uint64_t tok = bitcask::io::process_start_token(me);

    EXPECT_TRUE(bitcask::io::process_alive(me));
    EXPECT_TRUE(bitcask::io::process_alive(me, tok));
    // 令牌 0 = 「取不到 / 老锁文件」，须退化为纯 pid 判断（向后兼容）。
    EXPECT_TRUE(bitcask::io::process_alive(me, 0));
}

TEST(ProcessToken, MismatchedTokenMeansDifferentProcessInstance) {
    const int me = bitcask::io::current_process_id();
    const std::uint64_t tok = bitcask::io::process_start_token(me);

#if defined(_WIN32)
    // Windows 必须给出令牌——它是这里唯一的判别手段。
    ASSERT_NE(tok, 0u) << "Windows 上 process_start_token 必须可用，"
                          "否则 PID 复用防护完全失效";
    // 令牌不符 ⇒ 「同一 pid 的另一个进程实例」⇒ 原主人已死。
    EXPECT_FALSE(bitcask::io::process_alive(me, tok ^ 1u))
        << "令牌不符仍判活 ⇒ stale lock 永远回收不掉 ⇒ 库打不开";
#else
    // POSIX 侧刻意不提供令牌（见 io.hpp）：恒 0，且带令牌版与不带版等价，
    // 保证本届对 Linux 行为零改动。
    EXPECT_EQ(tok, 0u);
    EXPECT_TRUE(bitcask::io::process_alive(me, 12345u));
#endif
}

TEST(ProcessToken, DeadPidIsNotAlive) {
    // pid <= 0 是「解析不出 pid」的哨兵，两平台都必须判死。
    EXPECT_FALSE(bitcask::io::process_alive(0));
    EXPECT_FALSE(bitcask::io::process_alive(-1));
    EXPECT_FALSE(bitcask::io::process_alive(-1, 0));
    EXPECT_EQ(bitcask::io::process_start_token(-1), 0u);
}

// ---------------------------------------------------------------------------
// S37-6：文件生命周期语义——merge 退休 / 段 rebase 全都建立在这三条上。
//
// 这些性质在 POSIX 上是常识（unlink-while-mapped），在 Windows 上则是**实测
// 结论而非文档承诺**（见 io.hpp MappedFile 头注释）。做成测试是因为：一旦
// 哪条不成立，表现是「merge 后文件删不掉、退休队列无限增长」或「rebase 静默
// 失败」，两者都不会在别处炸出来。
// ---------------------------------------------------------------------------

TEST(FileLifecycle, DeleteWhileMappedKeepsViewContent) {
    TempDir td;
    const auto path = td.file("mapped.dat");
    {
        auto w = PosixFile::open(path, OpenFlag::kCreate);
        ASSERT_TRUE(w);
        ASSERT_TRUE(w->pwrite(0, as_bytes("OLDDATA")));
    }
    auto r = PosixFile::open(path, OpenFlag::kReadOnly);
    ASSERT_TRUE(r);
    auto m = bitcask::io::MappedFile::map_readonly(r->fd(), 7,
                                                   /*advise_random=*/false);
    ASSERT_TRUE(m.valid());
    r->close_quiet();  // 只留映射（Windows 上「仍开着的句柄」才是限制项）

    // 删除一个正被映射的文件必须成功。
    ASSERT_TRUE(bitcask::io::remove_file(path))
        << "被映射的文件删不掉 ⇒ merge 退休队列会无限增长";
    EXPECT_FALSE(fs::exists(path)) << "删除后名字应立刻从目录消失";

    // 旧视图仍读到旧内容——在途读者的正确性基础。
    EXPECT_EQ(0, std::memcmp(m.data(), "OLDDATA", 7));

    // 同名可立即重建，且不影响旧视图。
    {
        auto n = PosixFile::open(path, OpenFlag::kCreate);
        ASSERT_TRUE(n) << "删除后同名新建应立刻成功";
        ASSERT_TRUE(n->pwrite(0, as_bytes("NEWDATA")));
    }
    EXPECT_EQ(0, std::memcmp(m.data(), "OLDDATA", 7))
        << "旧视图串到了新文件内容 ⇒ 在途读者会读到撕裂数据";
}

TEST(FileLifecycle, AtomicRenameOverMappedFileKeepsViewContent) {
    // 段 rebase 的形态：写 tmp → atomic_rename 覆盖自己正在映射的旧段。
    TempDir td;
    const auto target = td.file("seg.dat");
    const auto tmp    = td.file("seg.dat.tmp");
    {
        auto w = PosixFile::open(target, OpenFlag::kCreate);
        ASSERT_TRUE(w);
        ASSERT_TRUE(w->pwrite(0, as_bytes("OLDSEG")));
    }
    auto r = PosixFile::open(target, OpenFlag::kReadOnly);
    ASSERT_TRUE(r);
    auto m = bitcask::io::MappedFile::map_readonly(r->fd(), 6, false);
    ASSERT_TRUE(m.valid());
    // ⚠️ 必须先关句柄：Windows 上 rename 覆盖一个仍开着句柄的目标必然失败
    // （任何访问模式都拦）。IvfSegment/DiskannSegment::open 因此在建好映射
    // 后立刻 close_handle——本行就是那条纪律的测试面。
    r->close_quiet();
    {
        auto w = PosixFile::open(tmp, OpenFlag::kCreate);
        ASSERT_TRUE(w);
        ASSERT_TRUE(w->pwrite(0, as_bytes("NEWSEG")));
    }
    ASSERT_TRUE(bitcask::io::atomic_rename(tmp, target))
        << "rename 覆盖被映射的目标失败 ⇒ 段 rebase 无法落地";
    EXPECT_EQ(0, std::memcmp(m.data(), "OLDSEG", 6))
        << "旧视图必须保持旧内容（等价 POSIX unlink-while-mapped）";
}

// P2：2 GiB 以上的文件。守的是「大小与定位偏移全程 64 位」。
//
// 原实现用 std::fseek(SEEK_END)+std::ftell 量大小，而它们的偏移类型是 long——
// **MSVC x64 上只有 4 字节**。实测 2.54 GiB 的文件 ftell() 返 -1，于是
// read_file_bytes 把好文件当成读不出来、search_checkpoint 把好 ckpt 判成结构
// 损坏；而 static_cast<long>(off) 那几处更会静默截断成负数。Linux 上 long 是
// 8 字节，所以这条分歧只在 Windows 出现，CI 与其余用例全都照不到——正因为
// 照不到，才需要这一条显式守着。
TEST(LargeFile, SizeAndPositionedIoBeyond2GiB) {
    constexpr std::uint64_t kSize = 2600ull << 20;  // 2.54 GiB，跨过 2^31
    TempDir td;

    // 稀疏与否取决于文件系统；空间不够就跳过，不要在 CI runner 上把盘写满。
    std::error_code sec;
    const auto sp = fs::space(fs::temp_directory_path(), sec);
    if (sec || sp.available < kSize + (4ull << 30)) {
        GTEST_SKIP() << "可用空间不足，跳过大文件用例";
    }

    const auto path = td.file("big.bin");
    // kTruncate 而非 kCreate：后者带 O_APPEND，POSIX 下会让 pwrite 忽略 offset。
    auto f = bitcask::io::File::open(path, OpenFlag::kTruncate);
    ASSERT_TRUE(f);
    if (!bitcask::io::truncate_handle(f->fd(), kSize)) {
        GTEST_SKIP() << "文件系统不支持撑到该大小";
    }

    const char mark[8] = {'M', 'A', 'R', 'K', 'E', 'R', '!', '!'};
    ASSERT_TRUE(bitcask::io::pwrite_all(f->fd(), mark, sizeof mark, kSize - 8))
        << "2 GiB 以上的定位写失败";

    const auto sz = bitcask::io::handle_size(f->fd());
    ASSERT_TRUE(sz);
    EXPECT_EQ(*sz, kSize) << "大小被截断（std::ftell 在这里会给 -1）";

    char back[8] = {};
    ASSERT_TRUE(bitcask::io::pread_all(f->fd(), back, sizeof back, kSize - 8))
        << "2 GiB 以上的定位读失败";
    EXPECT_EQ(0, std::memcmp(back, mark, sizeof mark));
}

TEST(FileLifecycle, LongHeldWriteHandleAllowsDeleteAndTracksOffset) {
    // field_schema 是全库唯一长期持有写句柄的地方（field.schema，生命周期 =
    // FieldSchema 对象）。本用例守它依赖的两条性质：
    //
    // 1) 句柄必须由 io::open_handle 打开——它恒带 FILE_SHARE_DELETE。退回
    //    std::fopen 的话，Windows 上整个库目录都会因这一个句柄删不掉
    //    （ERROR_SHARING_VIOLATION，S37-6）。
    // 2) 追加必须**自己记偏移**（kNoAppend + pwrite），不能靠 O_APPEND：
    //    POSIX 下 O_APPEND 会让 pwrite 忽略 offset，而 Windows 根本没有
    //    O_APPEND 的等价物，seam 的定位写走 OVERLAPPED 偏移。靠 O_APPEND
    //    两边行为就会分叉（P3）。
    //
    // W5 时这里包了一层 detail::adopt_stream 把句柄变成 FILE*；P3 后
    // field_schema 直接用句柄，adopt_stream 已删除。
    TempDir td;
    const auto path = td.file("held.dat");
    auto h = bitcask::io::open_handle(path, OpenFlag::kNoAppend,
                                      bitcask::io::FileMode::kUmaskDefault);
    ASSERT_TRUE(h);
    bitcask::io::File wf{*h};

    // 自记偏移的追加：连写两段，第二段必须落在第一段之后。
    std::uint64_t off = 0;
    ASSERT_TRUE(bitcask::io::pwrite_all(wf.fd(), "AB", 2, off));
    off += 2;
    ASSERT_TRUE(bitcask::io::pwrite_all(wf.fd(), "CD", 2, off));
    off += 2;
    const auto sz = bitcask::io::handle_size(wf.fd());
    ASSERT_TRUE(sz);
    EXPECT_EQ(*sz, off) << "偏移推进与文件大小不符（O_APPEND 语义分叉的征兆）";
    char back[4] = {};
    ASSERT_TRUE(bitcask::io::pread_all(wf.fd(), back, 4, 0));
    EXPECT_EQ(0, std::memcmp(back, "ABCD", 4));

    // 句柄仍开着，文件必须仍可删除。
    EXPECT_TRUE(bitcask::io::remove_file(path))
        << "长期持有的写句柄不得阻止删除"
           "（Windows 上 std::fopen 不带 FILE_SHARE_DELETE）";
}
