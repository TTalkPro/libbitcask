// path_utf8_test — 窄路径的编码约定（P0 / 遗留项 W4）。
//
// 守的是一条库级约定：**窄路径一律是 UTF-8**。它此前只写在 io 后端的注释里，
// 而库内 74 处窄↔`fs::path` 转换走的是 `fs::path(窄)` / `.string()`，两者在
// Windows 上都按**系统 ANSI 代码页**编解码。实测（简中 Windows，GetACP()=936）
// 有三种失败形态，全部由本文件的用例覆盖：
//
//   1. UTF-8 字节碰巧也是合法 GBK → 静默解成别的宽字符。往返能抵消，所以
//      「窄→path→窄→seam」看着是好的，但这段 path 交给 fs::exists /
//      ifstream 时指向的是**另一个名字**。 → NonAsciiDirIsUsableEndToEnd
//   2. UTF-8 字节不是合法 GBK → **fs::path 构造直接抛 std::system_error**。
//      而 detail::fsync_parent_dir 是 noexcept，且是全库 9 个原子写站点的
//      公共收尾 ⇒ std::terminate。 → AtomicWriteInNonAsciiDirDoesNotTerminate
//   3. 路径含真·宽来源（directory_iterator 的 entry）→ .string() 编出 GBK，
//      喂给 seam 被 MB_ERR_INVALID_CHARS 判非法 → EINVAL，且 remove_file
//      返回 false 而文件仍在。 → ScannerYieldsSeamUsablePaths
//
// Linux 侧 string()/u8string() 等价，这些用例在两个平台上都该绿——它们守的
// 是约定本身，不是某个平台的怪癖。
//
// **唯一一条两平台结果不同、也不该相同的**是「非法 UTF-8 输入」：Windows 上
// 转换真会失败（收敛成空 path），POSIX 上路径就是字节串、逐字节透传，因为
// 非 UTF-8 文件名在 Linux 上完全合法、P0 之前能打开、之后也必须能打开。
// 见 InvalidUtf8ConvergesWithoutThrowing 与 PosixNonUtf8FilenameStillUsable。
//
// 注：本文件的中文字面量依赖 CMakeLists.txt:53 的 `/utf-8`
// （含 /execution-charset:utf-8），故运行期字节就是 UTF-8。

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bitcask/detail/file_util.hpp"
#include "bitcask/detail/path_utf8.hpp"
#include "bitcask/detail/scanner.hpp"
#include "bitcask/io.hpp"
#include "support/test_paths.hpp"

namespace fs = std::filesystem;

using bitcask::detail::from_utf8;
using bitcask::detail::to_utf8;

namespace {

// 「测试库」——刻意选这三个字：它们的 UTF-8 字节**不是**合法 GBK，
// 正是形态 2 的触发数据（只用「测试」会落进形态 1，测不出构造抛异常）。
constexpr const char* kCjkDir  = "测试库";
constexpr const char* kCjkFile = "数据文件.dat";

// 带中文名的临时目录。**用宽路径创建**（即 from_utf8），模拟「用户在磁盘上
// 本来就有这么一个目录」，而不是我们自己用错编码造出来的。
class CjkTempDir {
public:
    CjkTempDir() {
        path_ = fs::temp_directory_path() /
                from_utf8(std::string("bitcask_utf8_") +
                          std::to_string(bitcask::test::test_pid()) + "_" + kCjkDir);
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }
    ~CjkTempDir() { std::error_code ec; fs::remove_all(path_, ec); }

    const fs::path& path() const { return path_; }
    // 库拿到的形态：UTF-8 窄串
    std::string narrow() const { return to_utf8(path_); }
    std::string narrow_file(const char* name) const {
        return to_utf8(path_ / from_utf8(name));
    }

private:
    fs::path path_;
};

std::span<const std::byte> as_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

}  // namespace

// --- 转换本身 ---------------------------------------------------------------

TEST(PathUtf8, AsciiRoundTripIsByteIdentical) {
    // ASCII 上必须与旧写法逐字等价——否则这次收编就是一次行为变更。
    const std::string a = "some/dir/0001.bitcask.data";
    EXPECT_EQ(to_utf8(from_utf8(a)), a);
    EXPECT_EQ(from_utf8(a), fs::path(a));
    EXPECT_EQ(to_utf8(fs::path(a)), fs::path(a).string());
}

TEST(PathUtf8, NonAsciiRoundTripIsByteIdentical) {
    const std::string a = std::string(kCjkDir) + "/" + kCjkFile;
    EXPECT_EQ(to_utf8(from_utf8(a)), a);
}

TEST(PathUtf8, EmptyStaysEmpty) {
    EXPECT_TRUE(from_utf8("").empty());
    EXPECT_TRUE(to_utf8(fs::path{}).empty());
}

// 两个函数都是 noexcept，而标准转换**在 Windows 上**对非法输入会抛。若这条守
// 不住，fsync_parent_dir（noexcept）就会 std::terminate——见文件头形态 2。
//
// 「不抛」是两平台共同的不变量，但**「非法输入」的结果两平台并不相同**，
// 而且不该相同（本用例初版把 Windows 的结果写成了通用断言，Linux 首次复验
// 即炸；实现是对的，断言过窄）：
//
//   Windows：`fs::path` 内部是 UTF-16，构造要真做一次 UTF-8 解码，非法序列
//            无处安放 → 转换失败 → 收敛成空 path（下游 open/remove 照常返错）。
//   POSIX  ：路径就是字节串，`char8_t` 与 `char` 之间没有解码这一步，
//            libstdc++ **逐字节透传、不校验**。而这正是要的：**非 UTF-8 的
//            文件名在 Linux 上完全合法**（Latin-1 名字、从别的 locale 拷来的
//            目录），P0 之前的 `fs::path(窄串)` 能打开它们，P0 之后必须照旧。
//            把「非法 UTF-8 → 空 path」强加到 POSIX 上，等于让库突然打不开
//            一批本来能打开的文件——那才是真回归。
//
// 所以两边各断言各自的不变量，共同的那条（不抛、不 terminate）由「函数返回了」
// 这件事本身证明。
TEST(PathUtf8, InvalidUtf8ConvergesWithoutThrowing) {
    const std::string cases[] = {
        std::string("\x80"),                  // 孤立续字节
        std::string("\xFF\xFE"),              // 非法首字节
        std::string("\xE6\xB5"),              // 截断的三字节序列
        std::string("\xC0\x80"),              // 超长编码
        std::string("\xED\xA0\x80"),          // 代理区
        std::string("ok\xFF.dat"),            // 合法 + 非法混合
    };
    for (const auto& bad : cases) {
        const fs::path p = from_utf8(bad);  // 走到这行就说明没抛（noexcept 下抛 = terminate）
#if defined(_WIN32)
        EXPECT_TRUE(p.empty()) << "输入字节数 " << bad.size();
#else
        // POSIX：与 P0 之前的写法逐字等价，一个字节都不能变。
        EXPECT_EQ(p, fs::path(bad)) << "输入字节数 " << bad.size();
        EXPECT_EQ(to_utf8(p), bad) << "输入字节数 " << bad.size();
#endif
    }
}

#if !defined(_WIN32)
// POSIX 专用：非 UTF-8 的文件名在磁盘上是合法的，收编后必须还能建/开/删。
// 上一个用例守的是转换的字节等价，这个守的是「等价之后真的还能用」——
// 前者过了后者仍可能挂（比如 seam 哪天在窄路径上加了 UTF-8 校验）。
TEST(PathUtf8, PosixNonUtf8FilenameStillUsable) {
    const fs::path dir = fs::temp_directory_path() /
                         ("bitcask_nonutf8_" + std::to_string(bitcask::test::test_pid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(fs::create_directories(dir, ec));

    // Latin-1 的「café.dat」——合法 POSIX 文件名，非法 UTF-8。
    const std::string p = to_utf8(dir) + "/caf\xE9.dat";

    auto h = bitcask::io::open_handle(p, bitcask::io::OpenFlag::kCreate,
                                      bitcask::io::FileMode::kOwnerOnly);
    ASSERT_TRUE(h) << "非 UTF-8 文件名 open_handle 失败 errno=" << (h ? 0 : h.error().errnum);
    bitcask::io::close_handle(*h);

    EXPECT_TRUE(fs::exists(from_utf8(p)));
    EXPECT_TRUE(bitcask::io::remove_file(p));
    EXPECT_FALSE(fs::exists(from_utf8(p)));

    fs::remove_all(dir, ec);
}
#endif

// --- seam ------------------------------------------------------------------

TEST(PathUtf8, SeamOpensAndRemovesNonAsciiPath) {
    CjkTempDir td;
    const std::string p = td.narrow_file(kCjkFile);

    auto h = bitcask::io::open_handle(p, bitcask::io::OpenFlag::kCreate,
                                      bitcask::io::FileMode::kOwnerOnly);
    ASSERT_TRUE(h) << "非 ASCII 路径 open_handle 失败 errno=" << (h ? 0 : h.error().errnum);
    bitcask::io::close_handle(*h);

    EXPECT_TRUE(fs::exists(from_utf8(p)));
    EXPECT_TRUE(bitcask::io::remove_file(p)) << "remove_file 对非 ASCII 路径必须生效";
    EXPECT_FALSE(fs::exists(from_utf8(p)));
}

// --- 原子写（形态 2：曾经 std::terminate 的那条路）--------------------------

TEST(PathUtf8, AtomicWriteInNonAsciiDirDoesNotTerminate) {
    CjkTempDir td;
    const std::string p = td.narrow_file(kCjkFile);

    // fsync_dir=true 才会走到 fsync_parent_dir——即原先抛穿 noexcept 的那处。
    ASSERT_TRUE(bitcask::detail::atomic_write_bytes(p, as_bytes("hello"),
                                                    /*fsync_dir=*/true));
    auto back = bitcask::detail::read_file_bytes<>(p);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->size(), 5u);

    // fsync_parent_dir 直接拿非法 UTF-8 也不能炸（它 noexcept）。
    bitcask::detail::fsync_parent_dir(std::string("\xFF\xFE/x"));
}

TEST(PathUtf8, AtomicFileWriterWorksInNonAsciiDir) {
    CjkTempDir td;
    const std::string p = td.narrow_file("流式.dat");
    {
        bitcask::detail::AtomicFileWriter w(p);
        ASSERT_TRUE(w) << "tmp 开不出来（非 ASCII 路径）";
        ASSERT_EQ(std::fwrite("abc", 1, 3, w.get()), 3u);
        ASSERT_TRUE(w.commit());
    }
    auto back = bitcask::detail::read_file_bytes<>(p);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->size(), 3u);
}

// --- 目录扫描（形态 3：真·宽来源）------------------------------------------

TEST(PathUtf8, ScannerYieldsSeamUsablePaths) {
    CjkTempDir td;
    // 在中文目录下造两个合法 data 文件
    for (std::uint64_t ts : {1u, 2u}) {
        const std::string f = td.narrow_file((std::to_string(ts) + ".bitcask.data").c_str());
        ASSERT_TRUE(bitcask::detail::atomic_write_bytes(f, as_bytes("x")));
    }

    auto scanned = bitcask::fileops::scan_dir(td.narrow());
    ASSERT_TRUE(scanned) << "非 ASCII 目录扫不动";
    ASSERT_EQ(scanned->size(), 2u);

    // 关键断言：扫出来的窄路径必须能**原样喂回 seam**。形态 3 下这里拿到的
    // 是 GBK 字节，open_handle 会以 EINVAL 失败。
    for (const auto& e : *scanned) {
        auto h = bitcask::io::open_handle(e.data_path, bitcask::io::OpenFlag::kReadOnly,
                                          bitcask::io::FileMode::kOwnerOnly);
        EXPECT_TRUE(h) << "扫描产出的路径 seam 收不下: " << e.data_path;
        if (h) bitcask::io::close_handle(*h);
    }
}

// --- 端到端（形态 1：往返抵消掩盖掉的那类）----------------------------------

TEST(PathUtf8, NonAsciiDirIsUsableEndToEnd) {
    CjkTempDir td;
    const std::string dir = td.narrow();

    // 形态 1 的要害不在往返，而在「窄串转成 path 后直接喂 fs::」——
    // 那条链上没有第二次错误来抵消第一次。
    EXPECT_TRUE(fs::exists(from_utf8(dir)));
    EXPECT_TRUE(fs::is_directory(from_utf8(dir)));

    const std::string meta = td.narrow_file("bitcask.meta");
    ASSERT_TRUE(bitcask::detail::atomic_write_bytes(meta, as_bytes("m")));
    EXPECT_TRUE(fs::exists(from_utf8(meta)));

    // 窄 → path → 窄 之后仍指向同一个文件
    EXPECT_EQ(to_utf8(from_utf8(meta)), meta);
}
