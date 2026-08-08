// M3.2 unit tests for the bitcask directory scanner.


#include <cerrno>
#include <cstdio>
#include "support/test_paths.hpp"
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bitcask/detail/path_utf8.hpp"
#include "bitcask/detail/scanner.hpp"

using bitcask::fileops::scan_dir;
using bitcask::fileops::DataFileEntry;
using bitcask::fileops::ScanError;

namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("bitcask_scan_" + std::to_string(bitcask::test::test_pid()) + "_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path_);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path_, ec); }
    std::string path() const { return path_.string(); }
    void touch(const std::string& name) {
        std::FILE* fp = std::fopen((path_ / name).string().c_str(), "wb");
        if (fp) std::fclose(fp);
    }
    void mkdir(const std::string& name) {
        fs::create_directory(path_ / name);
    }
private:
    fs::path path_;
};

}  // namespace

TEST(Scanner, EmptyDirectory) {
    TempDir td;
    auto r = scan_dir(td.path());
    ASSERT_TRUE(r);
    EXPECT_TRUE(r->empty());
}

TEST(Scanner, MissingDirectoryReturnsError) {
    auto r = scan_dir(bitcask::detail::to_utf8(bitcask::test::nonexistent_path()));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, ScanError::kCannotOpenDir);

    // P4：断言**具体的 errno**，不是「非零」。
    //
    // 原先只断 errnum != 0，而 Windows 上这里装的是 Win32 码，非零同样成立——
    // 于是这条链一路错到 C API 都没人发现：ScanFault.errnum → cask_recovery 的
    // io_fault → CaskFault → c_api 的 errnum，而那个字段的公开契约写的是
    // 「errno 值」（c_api/bitcask_kv.h）。MSVC 下 std::filesystem 的 error_code
    // 是 system_category，目录不存在给 3 = ERROR_PATH_NOT_FOUND，按 errno 读
    // 就是 ESRCH「没有这个进程」。Linux 上同样场景给 ENOENT(2)。
    //
    // 这条断言在两个平台上都必须是 ENOENT——那正是 io::errno_of_native 的职责。
    EXPECT_EQ(r.error().errnum, ENOENT)
        << "errnum 契约是 errno；拿到 " << r.error().errnum
        << " 多半是 Win32 码直接漏了出来";
}

TEST(Scanner, FindsDataFilesAndSortsByTstamp) {
    TempDir td;
    td.touch("3.bitcask.data");
    td.touch("1.bitcask.data");
    td.touch("2.bitcask.data");
    auto r = scan_dir(td.path());
    ASSERT_TRUE(r);
    ASSERT_EQ(r->size(), 3u);
    EXPECT_EQ((*r)[0].tstamp, 1u);
    EXPECT_EQ((*r)[1].tstamp, 2u);
    EXPECT_EQ((*r)[2].tstamp, 3u);
}

TEST(Scanner, ReportsHintFilePresence) {
    TempDir td;
    td.touch("1.bitcask.data");
    td.touch("2.bitcask.data");
    td.touch("2.bitcask.hint");        // matches 2
    td.touch("9.bitcask.hint");        // hint without matching data — ignored

    auto r = scan_dir(td.path());
    ASSERT_TRUE(r);
    ASSERT_EQ(r->size(), 2u);

    EXPECT_EQ((*r)[0].tstamp, 1u);
    EXPECT_FALSE((*r)[0].has_hint);
    EXPECT_TRUE((*r)[0].hint_path.ends_with("1.bitcask.hint"));

    EXPECT_EQ((*r)[1].tstamp, 2u);
    EXPECT_TRUE((*r)[1].has_hint);
    EXPECT_TRUE((*r)[1].hint_path.ends_with("2.bitcask.hint"));
}

TEST(Scanner, IgnoresGarbageFilenames) {
    TempDir td;
    td.touch("README");
    td.touch("bitcask.write.lock");
    td.touch("bitcask.merge.lock");
    td.touch("12345.bitcask.data");
    td.touch("abc.bitcask.data");      // non-numeric tstamp — skipped
    td.touch("123.bitcask.txt");        // wrong suffix — skipped

    auto r = scan_dir(td.path());
    ASSERT_TRUE(r);
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].tstamp, 12345u);
}

TEST(Scanner, IgnoresSubdirectories) {
    TempDir td;
    td.touch("1.bitcask.data");
    td.mkdir("subdir");
    td.mkdir("2.bitcask.data");  // would-be data file, but it's a directory

    auto r = scan_dir(td.path());
    ASSERT_TRUE(r);
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].tstamp, 1u);
}

TEST(Scanner, LargeTstampValuesNotOverflowed) {
    TempDir td;
    td.touch("18446744073709551615.bitcask.data");   // uint64 max
    td.touch("999999999999.bitcask.data");

    auto r = scan_dir(td.path());
    ASSERT_TRUE(r);
    ASSERT_EQ(r->size(), 2u);
    EXPECT_EQ((*r)[0].tstamp, 999999999999u);
    EXPECT_EQ((*r)[1].tstamp, 18446744073709551615ull);
}

TEST(Scanner, FullPathInDataFileEntry) {
    TempDir td;
    td.touch("42.bitcask.data");
    auto r = scan_dir(td.path());
    ASSERT_TRUE(r);
    ASSERT_EQ(r->size(), 1u);
    // S37-5：scanner 交出的是 fs::directory_entry::path().string()，
    // 分隔符为平台原生（Windows 上是 '\'）。期望值按同样方式构造。
    EXPECT_EQ((*r)[0].data_path, (fs::path(td.path()) / "42.bitcask.data").string());
    EXPECT_EQ((*r)[0].hint_path, (fs::path(td.path()) / "42.bitcask.hint").string());
}
