#include "bitcask/file_lock.hpp"

#include <cerrno>

#include <utility>
#include "bitcask/detail/file_util.hpp"

namespace bitcask::lock {

std::expected<FileLock, io::IoError>
FileLock::acquire(std::string_view filename, bool is_write_lock) noexcept {
    io::OpenFlag flags = io::OpenFlag::kReadOnly;
    if (is_write_lock) {
        // kSyncAll（O_SYNC，**非** O_DSYNC）：保证 write_data 写进去的内容
        // （pid / active file 路径等）立刻被其它进程的读锁看到——bitcask 用
        // 这个机制做 stale-lock 检查。S37-1 收编时原样保留该区别：库内其余
        // 写路径用的 kOSync 是 O_DSYNC，只有本处需要元数据也同步。
        flags = io::OpenFlag::kCreate | io::OpenFlag::kSyncAll |
                io::OpenFlag::kNoAppend;  // 见 kNoAppend 注释
    }
    std::string path(filename);
    auto fh = io::open_handle(path, flags);  // mode 0600 = kOwnerOnly（默认）
    if (!fh) return std::unexpected(io::IoError{fh.error()});
    return FileLock(*fh, is_write_lock, std::move(path));
}

void FileLock::release_quiet() noexcept {
    // S37-5：原为 `fd_ >= 0`——把 FileHandle 当成 int 的硬编码。Windows 下
    // 该别名是 void*，此式不再可编译。统一走 io::handle_valid。
    if (io::handle_valid(fd_)) {
        // 必须先 unlink 后 close：让仍持有 fd 的 reader 还能从老 inode 读到
        // 一致的内容；如果反过来先 close 后 unlink，新建同名锁文件的进程
        // 可能会被旧 reader 读出 garbage。这是 legacy lock_release 里的
        // 既定顺序，照搬。
        if (is_write_lock_ && !filename_.empty()) {
            bitcask::detail::remove_utf8(filename_);
        }
        io::close_handle(fd_);
        fd_ = io::kInvalidHandle;
    }
}

// 把锁文件全部读到内存。两步：fstat 拿大小、pread 一次读完。
// 错误分三种是为了让上层精确判断（fstat 失败可能是 fd 已坏；alloc 失败
// 是 OOM；pread 失败是真正的 I/O 错误）。
std::expected<std::vector<std::byte>, FileLock::ReadError>
FileLock::read_data() noexcept {
    const auto sz = io::handle_size(fd_);
    if (!sz) {
        return std::unexpected(ReadError{ReadErrorKind::kFstat, errno});
    }
    std::vector<std::byte> buf;
    try {
        buf.resize(static_cast<std::size_t>(*sz));
    } catch (...) {
        return std::unexpected(ReadError{ReadErrorKind::kAlloc, 0});
    }
    if (*sz > 0) {
        // S37-1：单次定位读。不套 io::pread_all——后者把短读判为失败，
        // 会改变下面注释所述的 legacy 行为。
        const auto n = io::pread_once(fd_, buf.data(), buf.size(), 0);
        if (!n) {
            return std::unexpected(ReadError{ReadErrorKind::kPread, errno});
        }
        // legacy 这里不检测短读——锁文件本来就 < 一页，pread 不太可能短读，
        // 出现的话就当合法的「内容比 fstat 看到的小」处理（resize 截断）。
        if (*n < buf.size()) buf.resize(*n);
    }
    return buf;
}

// truncate 到 0 然后 pwrite(0, data)：等价于「覆盖整个文件」。
// 仅写锁可用——读锁返回 kNotWritable。给 cask 把「pid + active file 路径」
// 写到 write.lock 里用。
std::expected<void, FileLock::WriteError>
FileLock::write_data(std::span<const std::byte> data) noexcept {
    if (!is_write_lock_) {
        return std::unexpected(WriteError{WriteErrorKind::kNotWritable, 0});
    }
    if (!io::truncate_handle(fd_, 0)) {
        return std::unexpected(WriteError{WriteErrorKind::kTruncate, errno});
    }
    // legacy 是单次 pwrite——不循环。这里照搬，以便错误语义完全一致：
    // 部分写在 legacy 里也算成功，不重试。锁文件容量极小，这种简化没
    // 实际风险。
    if (!data.empty()) {
        // legacy 是单次 pwrite——不循环，故不用 io::pwrite_all（那会重试补齐，
        // 改变「部分写也算成功」的既有语义）。
        if (!io::pwrite_once(fd_, data.data(), data.size(), 0)) {
            return std::unexpected(WriteError{WriteErrorKind::kPwrite, errno});
        }
    }
    return {};
}

}  // namespace bitcask::lock
