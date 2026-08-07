// test_paths — 测试用的可移植路径 / 文件属性 helper（S37-2）。
//
// 收编测试里的三类 POSIX 依赖，使测试与库本体一样能在 Windows 上编：
//   ① `::getpid()` 造唯一临时目录名  → test_pid()（走 io seam）
//   ② 硬编码 "/tmp/..."              → unique_tmpdir() / nonexistent_path()
//   ③ `::stat` 断言 inode / 大小     → same_file() / file_size_of()
//
// ②的要点不只是可移植：Windows 的临时目录是 %TEMP%（通常在用户 profile 下），
// 硬编码 /tmp 会让测试在 Windows 上**运行时**失败而非编译期失败——更难查。

#ifndef BITCASK_TESTS_SUPPORT_TEST_PATHS_HPP
#define BITCASK_TESTS_SUPPORT_TEST_PATHS_HPP

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "bitcask/io.hpp"

namespace bitcask::test {

// 当前进程 id。经库的 io seam，Windows 后端已一并覆盖。
inline int test_pid() { return io::current_process_id(); }

// 系统临时目录下的唯一路径（不创建）。同一进程内同 prefix 只需靠调用方
// 追加区分量（通常是用例名）——pid 保证跨并发 ctest 进程不撞。
inline std::filesystem::path unique_tmpdir(std::string_view prefix) {
    return std::filesystem::temp_directory_path() /
           (std::string(prefix) + "_" + std::to_string(test_pid()));
}

// 一条保证不存在的路径（用于「打不开」类负面用例）。
inline std::filesystem::path nonexistent_path() {
    return std::filesystem::temp_directory_path() /
           ("bitcask_should_not_exist_" + std::to_string(test_pid())) /
           "nested" / "missing";
}

// 两条路径是否指向同一个物理文件（POSIX: 同 dev+ino；Windows: 同卷序列号 +
// 文件索引）。替代测试里直接比较 st_ino——「二次 save 是追加而非 tmp+rename」
// 这类断言的可移植写法。
inline bool same_file(const std::string& a, const std::string& b) {
    const auto ia = io::path_identity(a);
    const auto ib = io::path_identity(b);
    return ia && ib && *ia == *ib;
}

// 文件身份快照——用于「同一路径前后是否还是同一个文件」。
inline std::optional<io::FileIdentity> identity_of(const std::string& p) {
    return io::path_identity(p);
}

// 文件大小；取不到返回 nullopt。
inline std::optional<std::uintmax_t> file_size_of(const std::string& p) {
    std::error_code ec;
    const auto n = std::filesystem::file_size(p, ec);
    if (ec) return std::nullopt;
    return n;
}

}  // namespace bitcask::test

#endif  // BITCASK_TESTS_SUPPORT_TEST_PATHS_HPP
