#include "bitcask/io.hpp"

#include <signal.h>
#include <sys/resource.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace bitcask::io {

namespace {

// 把 bitcask 的 OpenFlag 翻成 POSIX open(2) 的 flag 位。
// 规则跟 legacy bitcask_nifs.c::get_file_open_flags 完全一致：
//   默认       = O_RDWR | O_APPEND | O_CREAT
//   kCreate    = O_CREAT | O_EXCL | O_RDWR | O_APPEND   （覆盖默认）
//   kTruncate  = O_RDWR | O_CREAT | O_TRUNC             （覆盖默认，无 APPEND）
//   kReadOnly  = O_RDONLY                               （覆盖默认）
//   kWriteOnly = O_WRONLY                               （覆盖默认）
//   kOSync     = 在以上基础上 OR 一个 O_DSYNC
//   kSyncAll   = 在以上基础上 OR 一个 O_SYNC
//   kCloseOnExec = 在以上基础上 OR 一个 O_CLOEXEC
//
// 基模式四选一，按 kTruncate → kCreate → kReadOnly → kWriteOnly 顺序覆盖；
// 调用方保证不同时给互斥的基模式位（与收编前各站点一一对应，无混用形态）。
int translate_open_flags(OpenFlag in) noexcept {
    int flags = O_RDWR | O_APPEND | O_CREAT;
    if (has_flag(in, OpenFlag::kCreate)) {
        flags = O_CREAT | O_EXCL | O_RDWR | O_APPEND;
    }
    if (has_flag(in, OpenFlag::kTruncate)) {
        // **不带 O_APPEND**：向量段 build 走 pwrite 定位写，O_APPEND 会让
        // 每次写强制落到文件尾，破坏「回头补文件头」的写法。
        flags = O_RDWR | O_CREAT | O_TRUNC;
    }
    if (has_flag(in, OpenFlag::kReadOnly)) {
        flags = O_RDONLY;
    }
    if (has_flag(in, OpenFlag::kWriteOnly)) {
        flags = O_WRONLY;
    }
    if (has_flag(in, OpenFlag::kOSync)) {
        // S13-P2：O_DSYNC 而非 O_SYNC——追加写场景下 fdatasync 语义已保证
        // 数据 + 文件大小（检索数据所需的全部元数据）落盘，省去每次写的
        // mtime 等 journal 元数据提交。durability 语义不变。
        flags |= O_DSYNC;
    }
    if (has_flag(in, OpenFlag::kSyncAll)) {
        flags |= O_SYNC;  // 全同步（含元数据）——仅 write.lock 用
    }
    if (has_flag(in, OpenFlag::kCloseOnExec)) {
        flags |= O_CLOEXEC;
    }
    if (has_flag(in, OpenFlag::kNoAppend)) {
        flags &= ~O_APPEND;  // 见 OpenFlag::kNoAppend 注释（pwrite × O_APPEND）
    }
    return flags;
}

constexpr mode_t translate_mode(FileMode m) noexcept {
    return m == FileMode::kWorldReadable
               ? static_cast<mode_t>(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)
               : static_cast<mode_t>(S_IRUSR | S_IWUSR);
}

}  // namespace

std::expected<FileHandle, IoError>
open_handle(std::string_view path, OpenFlag flags, FileMode mode) noexcept {
    // open(2) 需要 NUL-terminated 字符串；caller 给的 string_view 可能不带
    // 末尾 NUL（NIF 那边先 copy 进固定缓冲区再传过来），所以这里再 copy 一份。
    std::string p(path);
    const int fd =
        ::open(p.c_str(), translate_open_flags(flags), translate_mode(mode));
    if (fd < 0) return std::unexpected(IoError{errno});
    return fd;
}

std::expected<File, IoError>
File::open(std::string_view path, OpenFlag flags, FileMode mode) noexcept {
    auto h = open_handle(path, flags, mode);
    if (!h) return std::unexpected(h.error());
    return File{*h};
}

void close_handle(FileHandle h) noexcept {
    if (handle_valid(h)) ::close(h);
}

// 关 fd；幂等（多次调用安全）。close 错误吞掉——legacy 行为，反正
// close 失败没有合理的恢复路径。
void File::close_quiet() noexcept {
    if (handle_valid(fd_)) {
        ::close(fd_);
        fd_ = kInvalidHandle;
    }
}

// fdatasync(2)：确保 fd 的脏数据页 + 检索所需元数据（文件大小）落盘。
// bitcask 的 sync_strategy 控制何时调用。S13-P2：fdatasync 替代 fsync——
// 追加写下持久性等价，每次省一笔 mtime 等元数据 journal 提交（ext4/xfs 可观）。
std::expected<void, IoError> File::sync() noexcept {
    if (::fdatasync(fd_) == -1) return std::unexpected(IoError{errno});
    return {};
}

// pread：保留「短读不当 EOF」的语义。返回值三种：
//   ReadOk{data}  — 读到 data.size() 字节（可能 < count，是合法短读）
//   ReadEof       — 0 字节（真 EOF）
//   IoError       — errno（不重试 EINTR——caller 需要的话自己重试）
ReadResult File::pread(std::uint64_t offset, std::size_t count) noexcept {
    std::vector<std::byte> buf(count);
    const ssize_t n = ::pread(fd_, buf.data(), count, static_cast<off_t>(offset));
    if (n > 0) {
        if (static_cast<std::size_t>(n) < count) buf.resize(static_cast<std::size_t>(n));
        return ReadOk{std::move(buf)};
    }
    if (n == 0) return ReadEof{};
    return std::unexpected(IoError{errno});
}

// pread 零分配版：读进 caller 缓冲区，返回读到的字节数（0 = EOF）。
std::expected<std::size_t, IoError>
File::pread_into(std::uint64_t offset, std::span<std::byte> buf) noexcept {
    const ssize_t n =
        ::pread(fd_, buf.data(), buf.size(), static_cast<off_t>(offset));
    if (n < 0) return std::unexpected(IoError{errno});
    return static_cast<std::size_t>(n);
}

// pwrite：循环写直到全部完成或出错。部分写不会被当作成功返回。
// 注意 w==0 也算错误（极少见，通常意味着 EAGAIN 在 non-blocking fd 上）。
std::expected<void, IoError>
File::pwrite(std::uint64_t offset, std::span<const std::byte> data) noexcept {
    const std::byte* buf = data.data();
    std::size_t remaining = data.size();
    off_t off = static_cast<off_t>(offset);
    while (remaining > 0) {
        const ssize_t w = ::pwrite(fd_, buf, remaining, off);
        if (w <= 0) return std::unexpected(IoError{errno});
        buf += w;
        off += w;
        remaining -= static_cast<std::size_t>(w);
    }
    return {};
}

// 顺序 read：用当前 fd 偏移。返回语义跟 pread 一致。
ReadResult File::read(std::size_t count) noexcept {
    std::vector<std::byte> buf(count);
    const ssize_t n = ::read(fd_, buf.data(), count);
    if (n > 0) {
        if (static_cast<std::size_t>(n) < count) buf.resize(static_cast<std::size_t>(n));
        return ReadOk{std::move(buf)};
    }
    if (n == 0) return ReadEof{};
    return std::unexpected(IoError{errno});
}

// 顺序 write：循环到全部写完。跟 pwrite 一致，部分写继续。
std::expected<void, IoError>
File::write(std::span<const std::byte> data) noexcept {
    const std::byte* buf = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const ssize_t w = ::write(fd_, buf, remaining);
        if (w <= 0) return std::unexpected(IoError{errno});
        buf += w;
        remaining -= static_cast<std::size_t>(w);
    }
    return {};
}

// lseek 包装。返回新偏移。whence: SEEK_SET/CUR/END。
std::expected<std::uint64_t, IoError>
File::seek(std::int64_t offset, int whence) noexcept {
    const off_t r = ::lseek(fd_, static_cast<off_t>(offset), whence);
    if (r == static_cast<off_t>(-1)) return std::unexpected(IoError{errno});
    return static_cast<std::uint64_t>(r);
}

// 便捷：seek 到文件头。
std::expected<void, IoError> File::seek_bof() noexcept {
    const off_t r = ::lseek(fd_, 0, SEEK_SET);
    if (r == static_cast<off_t>(-1)) return std::unexpected(IoError{errno});
    return {};
}

// ftruncate 到当前 offset：截断 offset 之后的所有内容。注意是「截到这里」
// 而不是「截到 0」——给 torn-write 修复用。
std::expected<void, IoError> File::truncate_here() noexcept {
    const off_t cur = ::lseek(fd_, 0, SEEK_CUR);
    if (cur == static_cast<off_t>(-1)) return std::unexpected(IoError{errno});
    if (::ftruncate(fd_, cur) == -1) return std::unexpected(IoError{errno});
    return {};
}

// --- S37-1 新增成员 ---------------------------------------------------------

std::expected<void, IoError> File::truncate(std::uint64_t length) noexcept {
    if (::ftruncate(fd_, static_cast<off_t>(length)) == -1) {
        return std::unexpected(IoError{errno});
    }
    return {};
}

std::expected<std::uint64_t, IoError> File::size() const noexcept {
    struct stat st{};
    if (::fstat(fd_, &st) != 0) return std::unexpected(IoError{errno});
    return static_cast<std::uint64_t>(st.st_size);
}

std::expected<FileIdentity, IoError> File::identity() const noexcept {
    struct stat st{};
    if (::fstat(fd_, &st) != 0) return std::unexpected(IoError{errno});
    return FileIdentity{static_cast<std::uint64_t>(st.st_dev),
                        static_cast<std::uint64_t>(st.st_ino)};
}


// ---------------------------------------------------------------------------
// S36 后续（backlog B3）：MappedFile——只读整文件 mmap 的 RAII（语义与
// 线程模型见 io.hpp 类注释）。
// ---------------------------------------------------------------------------

MappedFile::~MappedFile() { reset(); }

MappedFile& MappedFile::operator=(MappedFile&& o) noexcept {
    if (this != &o) {
        reset();
        base_ = o.base_;
        len_ = o.len_;
        o.base_ = nullptr;
        o.len_ = 0;
    }
    return *this;
}

MappedFile MappedFile::map_readonly(FileHandle fd, std::size_t len,
                                    bool advise_random) noexcept {
    MappedFile m;
    if (!handle_valid(fd) || len == 0) return m;
    void* base = ::mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) return m;  // 无效对象——caller 走 pread 回退
    if (advise_random) {
        ::madvise(base, len, MADV_RANDOM);  // best-effort
    }
    m.base_ = static_cast<const std::byte*>(base);
    m.len_ = len;
    return m;
}

void MappedFile::reset() noexcept {
    if (base_ != nullptr) {
        ::munmap(const_cast<std::byte*>(base_), len_);
        base_ = nullptr;
        len_ = 0;
    }
}

// ---------------------------------------------------------------------------
// S37-1：句柄级 / 路径级自由函数（POSIX 后端）
// ---------------------------------------------------------------------------

bool sync_data(FileHandle h) noexcept {
    return ::fdatasync(h) == 0;
}

bool truncate_handle(FileHandle h, std::uint64_t length) noexcept {
    return ::ftruncate(h, static_cast<off_t>(length)) == 0;
}

std::optional<std::uint64_t> handle_size(FileHandle h) noexcept {
    struct stat st{};
    if (::fstat(h, &st) != 0) return std::nullopt;
    return static_cast<std::uint64_t>(st.st_size);
}

std::optional<FileIdentity> handle_identity(FileHandle h) noexcept {
    struct stat st{};
    if (::fstat(h, &st) != 0) return std::nullopt;
    return FileIdentity{static_cast<std::uint64_t>(st.st_dev),
                        static_cast<std::uint64_t>(st.st_ino)};
}

std::optional<FileIdentity> path_identity(const std::string& path) noexcept {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return std::nullopt;
    return FileIdentity{static_cast<std::uint64_t>(st.st_dev),
                        static_cast<std::uint64_t>(st.st_ino)};
}

// EINTR 重试（审计修复 2026-07-13：此前 w<0 一律判失败——极端信号压力下
// build 假失败）。w==0 仍判失败（磁盘满等无进展场景防死循环）。
bool pwrite_all(FileHandle h, const void* buf, std::size_t len,
                std::uint64_t off) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(buf);
    while (len > 0) {
        const ssize_t w = ::pwrite(h, p, len, static_cast<off_t>(off));
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p   += w;
        off += static_cast<std::uint64_t>(w);
        len -= static_cast<std::size_t>(w);
    }
    return true;
}

bool pread_all(FileHandle h, void* buf, std::size_t len,
               std::uint64_t off) noexcept {
    auto* p = static_cast<std::uint8_t*>(buf);
    while (len > 0) {
        const ssize_t r = ::pread(h, p, len, static_cast<off_t>(off));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;  // EOF 短读 = 失败（调用方读已知长度）
        p   += r;
        off += static_cast<std::uint64_t>(r);
        len -= static_cast<std::size_t>(r);
    }
    return true;
}

std::optional<std::size_t> pread_once(FileHandle h, void* buf, std::size_t len,
                                      std::uint64_t off) noexcept {
    const ssize_t n = ::pread(h, buf, len, static_cast<off_t>(off));
    if (n < 0) return std::nullopt;
    return static_cast<std::size_t>(n);
}

std::optional<std::size_t> pwrite_once(FileHandle h, const void* buf,
                                       std::size_t len,
                                       std::uint64_t off) noexcept {
    const ssize_t n = ::pwrite(h, buf, len, static_cast<off_t>(off));
    if (n < 0) return std::nullopt;
    return static_cast<std::size_t>(n);
}

// POSIX 的 rename(2) 目标存在时原子覆盖——正是各原子写站点依赖的语义。
// Windows 后端须改 MoveFileExW(MOVEFILE_REPLACE_EXISTING|
// MOVEFILE_WRITE_THROUGH)，因为 CRT 的 rename 目标存在即失败（见 io.hpp）。
bool atomic_rename(const std::string& from, const std::string& to) noexcept {
    return std::rename(from.c_str(), to.c_str()) == 0;
}

void sync_directory(const std::string& path) noexcept {
    const int dfd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
}

bool flush_and_sync_stream(std::FILE* f) noexcept {
    return std::fflush(f) == 0 && ::fdatasync(::fileno(f)) == 0;
}

// S37-6：POSIX 下就是 std::fopen——unlink 一个已打开的文件本就合法，
// 无须任何额外共享位。差异全在 Windows 侧（见 io.hpp）。
std::FILE* open_stream(const std::string& path, const char* mode) noexcept {
    return std::fopen(path.c_str(), mode);
}

std::size_t page_size() noexcept {
    const long ps = ::sysconf(_SC_PAGESIZE);
    return ps > 0 ? static_cast<std::size_t>(ps) : 4096u;
}

void prefetch_range(const void* addr, std::size_t len) noexcept {
    if (addr == nullptr || len == 0) return;
    // best-effort：返回值故意丢弃（失败时页仍会在缺页时按需装入）。
    ::madvise(const_cast<void*>(addr), len, MADV_WILLNEED);
}

bool remove_file(const std::string& path) noexcept {
    return ::unlink(path.c_str()) == 0;
}

// --- 进程原语 ---------------------------------------------------------------

int current_process_id() noexcept {
    return static_cast<int>(::getpid());
}

// 对应 legacy bitcask_lockops:os_pid_exists/1 的 `kill -0 <pid>` 做法：
//   返回 0     — 信号能投递，进程在
//   -1 + ESRCH — 进程已死
//   -1 + EPERM — 进程在但我们无权 signal（保守地视为「活着」，避免误删
//                别人的 lock）
bool process_alive(int pid) noexcept {
    if (pid <= 0) return false;
    if (::kill(pid, 0) == 0) return true;
    return errno != ESRCH;
}

// S37-5：POSIX 侧不提供进程实例令牌，恒返回 0 ——理由见 io.hpp 的长注释
// （Linux pid 顺序分配、绕 pid_max 才回卷，仅按 pid 判断是本库长期既有行为；
// 引入 /proc/<pid>/stat 的 starttime 属本届不涉及的行为变更）。
// 于是 process_alive(pid, token) 在 POSIX 上恒等于 process_alive(pid)，
// Linux 行为逐字不变。
std::uint64_t process_start_token(int /*pid*/) noexcept { return 0; }

bool process_alive(int pid, std::uint64_t /*expect_token*/) noexcept {
    return process_alive(pid);
}

std::optional<std::uint64_t> max_open_files() noexcept {
    struct ::rlimit rl{};
    if (::getrlimit(RLIMIT_NOFILE, &rl) != 0) return std::nullopt;
    if (rl.rlim_cur == RLIM_INFINITY) return std::nullopt;
    return static_cast<std::uint64_t>(rl.rlim_cur);
}

}  // namespace bitcask::io
