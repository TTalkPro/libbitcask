// io seam 的 Windows 后端（S37-5）。与 src/io/posix_file.cpp 二选一，由
// CMake 按平台挑源文件——**本文件与 posix_file.cpp 是全库仅有的两处允许直接
// 调用宿主文件系统原语的地方**（io.hpp 头注释的 seam 契约）。
//
// windows.h 只准出现在这里：全库 94 处 std::min/max 与它的同名宏冲突，
// NOMINMAX / WIN32_LEAN_AND_MEAN 由顶层 CMakeLists 全局定义兜底，但把
// windows.h 关在移植层 TU 内才是真正的防线。
//
// === 与 POSIX 后端的语义差异总表 ===
// 逐条都有理由，且都在下面对应函数处展开：
//   1. File::write() 恒为「原子追加到 EOF」，不走文件指针  —— 见该函数
//   2. 定位读写（pread/pwrite 家族）**会移动文件指针**       —— 见 make_ov
//   3. sync_directory 是 no-op，持久性改由 MOVEFILE_WRITE_THROUGH 承担
//   4. FileMode（0600/0644）被忽略——Windows 靠 ACL 继承
//   5. kCloseOnExec 是 no-op——Windows 句柄默认就不被子进程继承
//   6. max_open_files 返回 nullopt——句柄不受 fd 表约束
//   7. MappedFile 的 advise_random 被忽略                    —— 见该函数
//   8. kOSync(O_DSYNC) 与 kSyncAll(O_SYNC) 合并为 FILE_FLAG_WRITE_THROUGH

#include "bitcask/io.hpp"

#if !defined(_WIN32)
#  error "win32_file.cpp 是 io seam 的 Windows 后端；POSIX 构建应编 posix_file.cpp"
#endif

// PrefetchVirtualMemory 需要 Win8+ (0x0602)；这里按 Win10 声明。
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
#  undef _WIN32_WINNT
#  define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>


#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

namespace bitcask::io {

namespace {

// ---------------------------------------------------------------------------
// 句柄互转。io.hpp 把 kInvalidHandle 定为 nullptr（constexpr 需要），而
// CreateFileW 失败返回 INVALID_HANDLE_VALUE((HANDLE)-1)——**归一在此处完成**，
// 出了这个文件就只有 nullptr 一种无效表示。
// ---------------------------------------------------------------------------
inline HANDLE native(FileHandle h) noexcept { return static_cast<HANDLE>(h); }

inline FileHandle wrap(HANDLE h) noexcept {
    return (h == INVALID_HANDLE_VALUE) ? kInvalidHandle
                                       : static_cast<FileHandle>(h);
}

// ---------------------------------------------------------------------------
// Win32 错误码 → errno。
//
// 上层**唯一**依赖具体取值做控制流的是 EEXIST（cask.cpp 的写锁竞争判定，
// 4 处：拿不到锁时区分「已被别人持有」与真 I/O 错误）。其余取值只进
// 诊断字段（DataFileFault::errnum / MetaError），不参与分支。
//
// ⚠️ EEXIST 有两个来源且含义相反：
//   ERROR_FILE_EXISTS(80)    —— CREATE_NEW 撞已存在文件，**是**错误
//   ERROR_ALREADY_EXISTS(183) —— OPEN_ALWAYS 打开了已存在文件，
//                                CreateFileW **返回成功**，仅作提示
// 故只在 CreateFileW 失败后才读 GetLastError（见 open_handle）。
// ---------------------------------------------------------------------------
int errno_of(DWORD e) noexcept {
    switch (e) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_PATHNAME:            return ENOENT;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:          return EEXIST;
        case ERROR_ACCESS_DENIED:
        case ERROR_NETWORK_ACCESS_DENIED:   return EACCES;
        // 共享冲突：POSIX 无对应物（不做强制锁）。映射到 EBUSY 而非 EACCES，
        // 让诊断能区分「权限不够」与「别人开着这个文件」——S37-6 的删除/
        // 映射生命周期问题会大量以此形式出现。
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:          return EBUSY;
        case ERROR_INVALID_HANDLE:          return EBADF;
        case ERROR_HANDLE_DISK_FULL:
        case ERROR_DISK_FULL:               return ENOSPC;
        case ERROR_TOO_MANY_OPEN_FILES:     return EMFILE;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:             return ENOMEM;
        case ERROR_DIR_NOT_EMPTY:           return ENOTEMPTY;
        case ERROR_INVALID_PARAMETER:       return EINVAL;
        case ERROR_INVALID_NAME:
        case ERROR_FILENAME_EXCED_RANGE:    return ENAMETOOLONG;
        case ERROR_WRITE_PROTECT:           return EROFS;
        case ERROR_NEGATIVE_SEEK:           return EINVAL;
        case ERROR_SEEK_ON_DEVICE:          return ESPIPE;
        case ERROR_BROKEN_PIPE:             return EPIPE;
        case ERROR_OPERATION_ABORTED:       return EINTR;
        default:                            return EIO;
    }
}

inline int last_errno() noexcept { return errno_of(::GetLastError()); }

// ---------------------------------------------------------------------------
// 窄路径 → 宽路径。
//
// **窄路径一律按 UTF-8 解读**（设计稿 C8）。这是刻意的严格选择：MB_ERR_INVALID_CHARS
// 让非法序列直接失败，而不是退回 ANSI 代码页猜一次——「同一个 char* 可能是两种
// 编码」会让「打开了错误的文件」这种故障无法定位。
//
// ⚠️ 已知缺口：库内多处经 std::filesystem::path::string() 产出窄路径，
// 而它在 Windows 上走的是**系统 ANSI 代码页**，不是 UTF-8。纯 ASCII 路径
// 两者一致（现有全部测试与典型部署），非 ASCII 路径则会在此被判为非法 UTF-8
// 而报 EINVAL。彻底修复需要把那些站点换成 u8string()——见 TASK.md 的遗留项。
// ---------------------------------------------------------------------------
std::optional<std::wstring> widen(std::string_view s) noexcept {
    if (s.empty()) return std::wstring();
    if (s.size() > static_cast<std::size_t>(INT_MAX)) return std::nullopt;
    const int len = static_cast<int>(s.size());
    const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        s.data(), len, nullptr, 0);
    if (n <= 0) return std::nullopt;
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), len,
                              w.data(), n) != n) {
        return std::nullopt;
    }
    return w;
}

// MAX_PATH(260) 突破：长路径加 \\?\ 前缀。
//
// \\?\ 会**关闭 Win32 的路径规范化**（相对路径、. 与 ..、正斜杠都不再被处理），
// 所以必须先用 GetFullPathNameW 自己规范化再加前缀，否则短路径能开、长路径
// 静默开不了。仅对超长路径做这一步——常规路径保持原样，免得平白改变行为
// （例如 \\?\ 下尾随空格/点不再被剥离）。
std::optional<std::wstring> native_path(std::string_view utf8) noexcept {
    auto w = widen(utf8);
    if (!w) return std::nullopt;
    // 留 12 字符余量：CreateFileW 对目录路径要求 MAX_PATH-12（8.3 文件名空间）。
    if (w->size() < MAX_PATH - 12) return w;
    if (w->rfind(LR"(\\?\)", 0) == 0) return w;  // 已带前缀

    const DWORD need = ::GetFullPathNameW(w->c_str(), 0, nullptr, nullptr);
    if (need == 0) return w;  // 规范化失败——原样交给 CreateFileW 去报错
    std::wstring full(need, L'\0');
    const DWORD got = ::GetFullPathNameW(w->c_str(), need, full.data(), nullptr);
    if (got == 0 || got >= need) return w;
    full.resize(got);
    if (full.rfind(LR"(\\)", 0) == 0) {
        // UNC \\server\share → \\?\UNC\server\share
        return std::wstring(LR"(\\?\UNC\)") + full.substr(2);
    }
    return std::wstring(LR"(\\?\)") + full;
}

// ---------------------------------------------------------------------------
// OpenFlag → CreateFileW 参数。与 posix_file.cpp 的 translate_open_flags
// 逐条对应（基模式四选一，按 kTruncate → kCreate → kReadOnly → kWriteOnly
// 顺序覆盖，与 POSIX 后端同序）。
//
// **共享模式恒为 READ|WRITE|DELETE**：POSIX 不做强制锁，任何更严的共享模式
// 都会凭空引入 Linux 上不存在的失败。FILE_SHARE_DELETE 尤其是硬要求——
// 少了它，「删除仍被打开的文件」直接 ERROR_SHARING_VIOLATION，而 bitcask 的
// merge/退休路径依赖 POSIX 的 unlink-while-open 语义（设计稿 C2 / S37-6）。
// ---------------------------------------------------------------------------
struct OpenSpec {
    DWORD access      = 0;
    DWORD disposition = 0;
    DWORD attrs       = FILE_ATTRIBUTE_NORMAL;
    // O_APPEND 的近似：开档后把文件指针置于 EOF。见 File::write 的长注释——
    // 真正的追加原子性由那里的 OVERLAPPED 惯用法保证，这里只是让「开档后
    // 立刻读指针/seek」的站点看到与 Linux 一致的初值。
    bool  at_end      = false;
};

OpenSpec translate(OpenFlag in) noexcept {
    OpenSpec s;
    // 默认 = O_RDWR | O_APPEND | O_CREAT
    s.access      = GENERIC_READ | GENERIC_WRITE;
    s.disposition = OPEN_ALWAYS;
    s.at_end      = true;

    if (has_flag(in, OpenFlag::kCreate)) {      // O_CREAT|O_EXCL|O_RDWR|O_APPEND
        s.access      = GENERIC_READ | GENERIC_WRITE;
        s.disposition = CREATE_NEW;             // 已存在 → ERROR_FILE_EXISTS
        s.at_end      = true;
    }
    if (has_flag(in, OpenFlag::kTruncate)) {    // O_RDWR|O_CREAT|O_TRUNC（无 APPEND）
        s.access      = GENERIC_READ | GENERIC_WRITE;
        s.disposition = CREATE_ALWAYS;
        s.at_end      = false;
    }
    if (has_flag(in, OpenFlag::kReadOnly)) {    // O_RDONLY（不含 O_CREAT）
        s.access      = GENERIC_READ;
        s.disposition = OPEN_EXISTING;
        s.at_end      = false;
    }
    if (has_flag(in, OpenFlag::kWriteOnly)) {   // O_WRONLY（不含 O_CREAT）
        s.access      = GENERIC_WRITE;
        s.disposition = OPEN_EXISTING;
        s.at_end      = false;
    }

    // O_DSYNC 与 O_SYNC 在 Windows 上没有区分——都是 FILE_FLAG_WRITE_THROUGH。
    // 差异记在 io.hpp 的 kOSync/kSyncAll 注释里（S13-P2 的 dsync 优化在此
    // 平台不成立，写锁需要的元数据同步则天然满足）。
    if (has_flag(in, OpenFlag::kOSync) || has_flag(in, OpenFlag::kSyncAll)) {
        s.attrs |= FILE_FLAG_WRITE_THROUGH;
    }
    // kCloseOnExec：Windows 句柄默认不可继承（SECURITY_ATTRIBUTES 为空即
    // bInheritHandle=FALSE），无需动作。
    if (has_flag(in, OpenFlag::kNoAppend)) s.at_end = false;
    return s;
}

// 定位读写用的 OVERLAPPED。
//
// ⚠️ **同步句柄上带 OVERLAPPED 的 ReadFile/WriteFile 会更新文件指针**
// （POSIX 的 pread/pwrite 不会）。库内唯一交错使用「顺序写 + 定位读」的是
// HintFile（write(pending_) 与 pread 系交替），若顺序写依赖文件指针，
// 一次 pread 就会把追加变成覆盖——静默数据损坏。File::write 因此不走
// 文件指针（见该函数）。其余站点的 seek() 全部是显式定位或纯尺寸查询，
// 不受指针漂移影响（已逐站点核对）。
//
// ⚠️ 并发：同步句柄上的 I/O 会被内核在文件对象上串行化，即多线程并发
// pread 同一句柄不会真正并行（POSIX 下会）。这是设计稿 C3 点名的
// 「测试全绿、只有 bench 掉」风险，解法是每线程句柄池，与 read-handle LRU
// 预算合并考虑——不在本次落地范围，见 TASK.md S37-5.3。
OVERLAPPED make_ov(std::uint64_t off) noexcept {
    OVERLAPPED ov{};
    ov.Offset     = static_cast<DWORD>(off & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    return ov;
}

// 单次 I/O 的字节上限。ReadFile/WriteFile 的长度是 DWORD；这里再压到 1 GiB，
// 使超大请求走调用方已有的循环（*_all）而不是一次巨量 I/O。
constexpr std::size_t kIoChunk = 1u << 30;

inline DWORD chunk_of(std::size_t len) noexcept {
    return static_cast<DWORD>(len < kIoChunk ? len : kIoChunk);
}

// 一次定位读。返回读到的字节数；EOF 记 0。失败返回 nullopt。
std::optional<std::size_t> read_at(HANDLE h, void* buf, std::size_t len,
                                   std::uint64_t off) noexcept {
    if (len == 0) return std::size_t{0};
    OVERLAPPED ov = make_ov(off);
    DWORD got = 0;
    if (!::ReadFile(h, buf, chunk_of(len), &got, &ov)) {
        // 越过 EOF 的定位读在同步句柄上报 ERROR_HANDLE_EOF 而非返回 0 字节。
        if (::GetLastError() == ERROR_HANDLE_EOF) return std::size_t{0};
        return std::nullopt;
    }
    return static_cast<std::size_t>(got);
}

// 一次定位写。返回写出的字节数；失败返回 nullopt。
std::optional<std::size_t> write_at(HANDLE h, const void* buf, std::size_t len,
                                    std::uint64_t off) noexcept {
    if (len == 0) return std::size_t{0};
    OVERLAPPED ov = make_ov(off);
    DWORD put = 0;
    if (!::WriteFile(h, buf, chunk_of(len), &put, &ov)) return std::nullopt;
    return static_cast<std::size_t>(put);
}

// 不移动文件指针地设置文件长度。
//
// 用 SetFileInformationByHandle(FileEndOfFileInfo) 而非
// SetFilePointerEx + SetEndOfFile：后者以文件指针为截断点，会把
// truncate 变成「依赖并修改 fd 偏移」的操作，而 io.hpp 明写
// File::truncate(len) / truncate_handle 「不依赖 fd 当前 offset，故线程安全」。
bool set_length(HANDLE h, std::uint64_t length) noexcept {
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(length);
    return ::SetFileInformationByHandle(h, FileEndOfFileInfo, &info,
                                        sizeof(info)) != FALSE;
}

std::optional<FileIdentity> identity_of(HANDLE h) noexcept {
    BY_HANDLE_FILE_INFORMATION bi{};
    if (!::GetFileInformationByHandle(h, &bi)) return std::nullopt;
    // (卷序列号, 128 位文件 ID 的低 64 位) —— NTFS 上与 (st_dev, st_ino) 等效。
    // ⚠️ ReFS 的文件 ID 是 128 位，此处只取低 64 位，理论上可能碰撞；
    // 需要时改用 GetFileInformationByHandleEx(FileIdInfo)。当前用途
    // （.vec/.qc8 追加前校验目标未被换掉）对碰撞不敏感。
    const std::uint64_t id =
        (static_cast<std::uint64_t>(bi.nFileIndexHigh) << 32) |
        static_cast<std::uint64_t>(bi.nFileIndexLow);
    return FileIdentity{static_cast<std::uint64_t>(bi.dwVolumeSerialNumber), id};
}

}  // namespace

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------

std::expected<FileHandle, IoError>
open_handle(std::string_view path, OpenFlag flags, FileMode mode) noexcept {
    (void)mode;  // Windows 无 POSIX 权限位——新文件的 ACL 由父目录继承。

    auto wp = native_path(path);
    if (!wp) return std::unexpected(IoError{EINVAL});  // 非法 UTF-8（见 widen）

    const OpenSpec s = translate(flags);
    const HANDLE h = ::CreateFileW(
        wp->c_str(), s.access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, s.disposition, s.attrs, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::unexpected(IoError{last_errno()});

    if (s.at_end) {
        LARGE_INTEGER zero{};
        if (!::SetFilePointerEx(h, zero, nullptr, FILE_END)) {
            const int e = last_errno();
            ::CloseHandle(h);
            return std::unexpected(IoError{e});
        }
    }
    return wrap(h);
}

std::expected<File, IoError>
File::open(std::string_view path, OpenFlag flags, FileMode mode) noexcept {
    auto h = open_handle(path, flags, mode);
    if (!h) return std::unexpected(h.error());
    return File{*h};
}

void close_handle(FileHandle h) noexcept {
    if (handle_valid(h)) ::CloseHandle(native(h));
}

void File::close_quiet() noexcept {
    if (handle_valid(fd_)) {
        ::CloseHandle(native(fd_));
        fd_ = kInvalidHandle;
    }
}

// ---------------------------------------------------------------------------
// File 成员
// ---------------------------------------------------------------------------

// FlushFileBuffers 是 fsync 与 fdatasync 的共同对应物——Windows 不区分
// 「数据」与「检索所需元数据」。故 S13-P2 的 fdatasync 省元数据提交那笔
// 优化在本平台上不存在；checkpoint 路径的开销需另行 bench（设计稿 C5）。
std::expected<void, IoError> File::sync() noexcept {
    if (!::FlushFileBuffers(native(fd_))) {
        return std::unexpected(IoError{last_errno()});
    }
    return {};
}

ReadResult File::pread(std::uint64_t offset, std::size_t count) noexcept {
    std::vector<std::byte> buf(count);
    const auto n = read_at(native(fd_), buf.data(), count, offset);
    if (!n) return std::unexpected(IoError{last_errno()});
    if (*n == 0) return ReadEof{};
    if (*n < count) buf.resize(*n);  // 短读是合法结果，不当 EOF（同 POSIX 后端）
    return ReadOk{std::move(buf)};
}

std::expected<std::size_t, IoError>
File::pread_into(std::uint64_t offset, std::span<std::byte> buf) noexcept {
    const auto n = read_at(native(fd_), buf.data(), buf.size(), offset);
    if (!n) return std::unexpected(IoError{last_errno()});
    return *n;
}

std::expected<void, IoError>
File::pwrite(std::uint64_t offset, std::span<const std::byte> data) noexcept {
    const std::byte* p = data.data();
    std::size_t remaining = data.size();
    std::uint64_t off = offset;
    while (remaining > 0) {
        const auto w = write_at(native(fd_), p, remaining, off);
        if (!w) return std::unexpected(IoError{last_errno()});
        if (*w == 0) return std::unexpected(IoError{EIO});  // 无进展，防死循环
        p         += *w;
        off       += *w;
        remaining -= *w;
    }
    return {};
}

// 顺序读：走文件指针。**当前全库零调用点**（S37-5 核查）——保留是为了
// 与 POSIX 后端保持同一接口。
ReadResult File::read(std::size_t count) noexcept {
    std::vector<std::byte> buf(count);
    DWORD got = 0;
    if (!::ReadFile(native(fd_), buf.data(), chunk_of(count), &got, nullptr)) {
        return std::unexpected(IoError{last_errno()});
    }
    if (got == 0) return ReadEof{};
    if (got < count) buf.resize(got);
    return ReadOk{std::move(buf)};
}

// 顺序写 —— **本后端上恒为「原子追加到 EOF」，不使用也不依赖文件指针**。
//
// 为什么不能直译成普通 WriteFile：
//   HintFile 是本函数唯一的调用方（hint_file.cpp:50），它把顺序 write() 与
//   定位 pread/pread_into 交错用在同一个 File 上（112/129/179/189/206/230 行）。
//   同步句柄上带 OVERLAPPED 的定位读**会移动文件指针**，于是随后的普通
//   WriteFile 不再落在 EOF，而是从上次读到的位置**覆盖**已有内容。
//   编译通过、单条写入也「成功」，只有 hint 文件内容被悄悄写坏——
//   典型的静默损坏。
//
// 用 OVERLAPPED 的 Offset/OffsetHigh 全 1（0xFFFFFFFF）即 Win32 文档的
// 原子追加写法：写入点由内核取 EOF，与 POSIX 的 O_APPEND 语义逐条对应
// （含并发追加的原子性），且完全不受文件指针状态影响。
//
// ⚠️ 由此产生的平台差异：POSIX 后端上，若句柄**没有** O_APPEND，write()
// 从当前偏移写；本后端恒追加。当前全部调用点都以追加模式开档（kNone /
// kCreate 均含 O_APPEND），故行为一致。将来若新增「顺序定位写」的需求，
// 应当用 pwrite()，不要指望本函数在两平台上一致。
std::expected<void, IoError>
File::write(std::span<const std::byte> data) noexcept {
    const std::byte* p = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        OVERLAPPED ov{};
        ov.Offset     = 0xFFFFFFFFu;  // 这一对全 1 = 「写到文件末尾」
        ov.OffsetHigh = 0xFFFFFFFFu;
        DWORD put = 0;
        if (!::WriteFile(native(fd_), p, chunk_of(remaining), &put, &ov)) {
            return std::unexpected(IoError{last_errno()});
        }
        if (put == 0) return std::unexpected(IoError{EIO});
        p         += put;
        remaining -= put;
    }
    return {};
}

std::expected<std::uint64_t, IoError>
File::seek(std::int64_t offset, int whence) noexcept {
    DWORD method = FILE_BEGIN;
    switch (whence) {
        case SEEK_SET: method = FILE_BEGIN;   break;
        case SEEK_CUR: method = FILE_CURRENT; break;
        case SEEK_END: method = FILE_END;     break;
        default: return std::unexpected(IoError{EINVAL});
    }
    LARGE_INTEGER dist{};
    dist.QuadPart = offset;
    LARGE_INTEGER out{};
    if (!::SetFilePointerEx(native(fd_), dist, &out, method)) {
        return std::unexpected(IoError{last_errno()});
    }
    return static_cast<std::uint64_t>(out.QuadPart);
}

std::expected<void, IoError> File::seek_bof() noexcept {
    auto r = seek(0, SEEK_SET);
    if (!r) return std::unexpected(r.error());
    return {};
}

// 截到当前文件指针。这一个**刻意**用 SetEndOfFile（以文件指针为截断点），
// 因为语义就是「截到这里」——与 io.hpp 「依赖 fd 当前 offset，非线程安全」
// 的声明一致。显式长度版见下面的 truncate()。
std::expected<void, IoError> File::truncate_here() noexcept {
    if (!::SetEndOfFile(native(fd_))) {
        return std::unexpected(IoError{last_errno()});
    }
    return {};
}

std::expected<void, IoError> File::truncate(std::uint64_t length) noexcept {
    if (!set_length(native(fd_), length)) {
        return std::unexpected(IoError{last_errno()});
    }
    return {};
}

std::expected<std::uint64_t, IoError> File::size() const noexcept {
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(native(fd_), &sz)) {
        return std::unexpected(IoError{last_errno()});
    }
    return static_cast<std::uint64_t>(sz.QuadPart);
}

std::expected<FileIdentity, IoError> File::identity() const noexcept {
    auto id = identity_of(native(fd_));
    if (!id) return std::unexpected(IoError{last_errno()});
    return *id;
}

// ---------------------------------------------------------------------------
// MappedFile
// ---------------------------------------------------------------------------

MappedFile::~MappedFile() { reset(); }

MappedFile& MappedFile::operator=(MappedFile&& o) noexcept {
    if (this != &o) {
        reset();
        base_ = o.base_;
        len_  = o.len_;
        o.base_ = nullptr;
        o.len_  = 0;
    }
    return *this;
}

// **section 句柄用完即关，不进成员**：MapViewOfFile 产生的视图自身持有对
// section 对象的引用，关掉 section 句柄后视图依然有效（Win32 文档明载）。
// 因此 MappedFile 不需要像设计稿预估的那样多存一个成员，公开头形状不变。
//
// ⚠️ 但这不改变 S37-6 的问题：**只要视图还在，文件就删不掉**——section
// 对象对文件的引用与句柄的共享模式无关。退休一个文件前必须先 reset() 掉
// 它的映射，这正是 S37-6 要新增的不变量。
//
// advise_random 被忽略：Windows 的随机访问提示是 FILE_FLAG_RANDOM_ACCESS，
// 只能在**开文件时**给，此处已经拿到的是别人开好的句柄。设计稿 C7 建议把
// 时机前移到 open，但实测 6 个调用点里 3 个传 false（segment_v2 / diskann /
// ivf_rq 是顺序访问）、3 个传 true，一律加反而会关掉顺序路径的预读。
// 该提示只影响预读启发、不涉正确性，故本次不做；若 bench 显示有收益，
// 应把它做成 OpenFlag 位由调用方指定，而不是在 kReadOnly 上一刀切。
MappedFile MappedFile::map_readonly(FileHandle fd, std::size_t len,
                                    bool advise_random) noexcept {
    (void)advise_random;
    MappedFile m;
    if (!handle_valid(fd) || len == 0) return m;

    const HANDLE section = ::CreateFileMappingW(
        native(fd), nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (section == nullptr) return m;  // 无效对象——caller 走 pread 回退

    void* base = ::MapViewOfFile(section, FILE_MAP_READ, 0, 0, len);
    ::CloseHandle(section);            // 视图自持引用，此处即可释放
    if (base == nullptr) return m;

    m.base_ = static_cast<const std::byte*>(base);
    m.len_  = len;
    return m;
}

void MappedFile::reset() noexcept {
    if (base_ != nullptr) {
        // UnmapViewOfFile 只要基址，不需要长度（与 munmap 不同）。
        ::UnmapViewOfFile(const_cast<std::byte*>(base_));
        base_ = nullptr;
        len_  = 0;
    }
}

// ---------------------------------------------------------------------------
// 句柄级自由函数
// ---------------------------------------------------------------------------

bool sync_data(FileHandle h) noexcept {
    return ::FlushFileBuffers(native(h)) != FALSE;
}

bool truncate_handle(FileHandle h, std::uint64_t length) noexcept {
    return set_length(native(h), length);
}

std::optional<std::uint64_t> handle_size(FileHandle h) noexcept {
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(native(h), &sz)) return std::nullopt;
    return static_cast<std::uint64_t>(sz.QuadPart);
}

std::optional<FileIdentity> handle_identity(FileHandle h) noexcept {
    return identity_of(native(h));
}

std::optional<FileIdentity> path_identity(const std::string& path) noexcept {
    auto wp = native_path(path);
    if (!wp) return std::nullopt;
    // FILE_READ_ATTRIBUTES 足够取身份，且对被独占写的文件也能打开；
    // FILE_FLAG_BACKUP_SEMANTICS 让目录也能开（POSIX 的 stat 对目录同样有效）。
    const HANDLE h = ::CreateFileW(
        wp->c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    auto id = identity_of(h);
    ::CloseHandle(h);
    return id;
}

bool pwrite_all(FileHandle h, const void* buf, std::size_t len,
                std::uint64_t off) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(buf);
    while (len > 0) {
        const auto w = write_at(native(h), p, len, off);
        if (!w || *w == 0) return false;
        p   += *w;
        off += *w;
        len -= *w;
    }
    return true;
}

bool pread_all(FileHandle h, void* buf, std::size_t len,
               std::uint64_t off) noexcept {
    auto* p = static_cast<std::uint8_t*>(buf);
    while (len > 0) {
        const auto r = read_at(native(h), p, len, off);
        if (!r || *r == 0) return false;  // EOF 短读 = 失败（调用方读已知长度）
        p   += *r;
        off += *r;
        len -= *r;
    }
    return true;
}

std::optional<std::size_t> pread_once(FileHandle h, void* buf, std::size_t len,
                                      std::uint64_t off) noexcept {
    return read_at(native(h), buf, len, off);
}

std::optional<std::size_t> pwrite_once(FileHandle h, const void* buf,
                                       std::size_t len,
                                       std::uint64_t off) noexcept {
    return write_at(native(h), buf, len, off);
}

// ---------------------------------------------------------------------------
// 路径级自由函数
// ---------------------------------------------------------------------------

// 设计稿 C1 的落点，也是整个移植里「若做错必然立刻全线暴露」的那一项：
// CRT 的 std::rename 在目标已存在时**失败**，而全库 9 个原子写站点都依赖
// POSIX rename 的原子覆盖语义。
//
// MOVEFILE_REPLACE_EXISTING —— 恢复覆盖语义。
// MOVEFILE_WRITE_THROUGH    —— 函数返回前把目录项变更刷到盘。POSIX 侧
//   这件事由调用方的 sync_directory(fsync 父目录) 承担，而 Windows 没有
//   「打开目录再 fsync」这一手段（见下面的 sync_directory）；持久性契约
//   不变，承担者从「调用方显式 fsync 目录」换成「rename 自身 write-through」。
bool atomic_rename(const std::string& from, const std::string& to) noexcept {
    auto wf = native_path(from);
    auto wt = native_path(to);
    if (!wf || !wt) return false;
    return ::MoveFileExW(wf->c_str(), wt->c_str(),
                         MOVEFILE_REPLACE_EXISTING |
                         MOVEFILE_WRITE_THROUGH) != FALSE;
}

// no-op：Windows 打不开目录做 fsync（CreateFileW 即便带
// FILE_FLAG_BACKUP_SEMANTICS 拿到目录句柄，FlushFileBuffers 对它也不刷
// 目录项）。rename 的目录项持久性改由 atomic_rename 的
// MOVEFILE_WRITE_THROUGH 承担——见上。
void sync_directory(const std::string& path) noexcept {
    (void)path;
}

// P4：把 std::filesystem 的 Win32 error_code 翻成 errno。见 io.hpp 的声明注释。
int errno_of_native(int native_error) noexcept {
    if (native_error == 0) return 0;
    return errno_of(static_cast<DWORD>(native_error));
}

std::size_t page_size() noexcept {
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    // 刻意返回 dwPageSize（4 KiB）而非 dwAllocationGranularity（64 KiB）——
    // 见 io.hpp：本函数用于预取区间对齐；映射视图偏移的对齐要用后者，
    // 当前全部是整文件映射（offset=0），尚无站点需要。
    return si.dwPageSize != 0 ? static_cast<std::size_t>(si.dwPageSize)
                              : std::size_t{4096};
}

void prefetch_range(const void* addr, std::size_t len) noexcept {
    if (addr == nullptr || len == 0) return;
    WIN32_MEMORY_RANGE_ENTRY entry{};
    entry.VirtualAddress = const_cast<void*>(addr);
    entry.NumberOfBytes  = len;
    // best-effort：返回值故意丢弃（失败时页仍会在缺页时按需装入）。
    (void)::PrefetchVirtualMemory(::GetCurrentProcess(), 1, &entry, 0);
}

bool remove_file(const std::string& path) noexcept {
    auto wp = native_path(path);
    if (!wp) return false;
    // ⚠️ 仍被打开的文件：只有当**所有**持有者都以 FILE_SHARE_DELETE 开档
    // 时才能删（本后端的 CreateFileW 全部如此），且**被 section 映射持有的
    // 文件即便如此也删不掉**（设计稿 C2）。调用方须容忍失败——
    // Cask::drain_retired_files 的重试队列即为此存在；把映射逐出后再删
    // 是 S37-6 要新增的不变量。
    return ::DeleteFileW(wp->c_str()) != FALSE;
}

// ---------------------------------------------------------------------------
// 进程原语
// ---------------------------------------------------------------------------

int current_process_id() noexcept {
    return static_cast<int>(::GetCurrentProcessId());
}

// 与 POSIX 后端的 kill(pid, 0) 对应，且同样**保守偏向「活着」**——
// 不确定一律返回 true，避免误删他人持有的有效锁。
//
// ⚠️ 本函数单独使用在 Windows 上不足以判定 stale lock：**Windows 的 PID
// 复用远快于 Linux**，「新进程恰好复用了崩溃进程的 PID」会被判成锁仍有效，
// 于是拒绝回收 → 库彻底打不开。真正的判定需要配合进程创建时间
// （GetProcessTimes 的 ftCreationTime）——见 process_start_token 与
// TASK.md S37-5.4。本函数保留原语义，供只有 pid 可用的旧锁文件回退使用。
bool process_alive(int pid) noexcept {
    if (pid <= 0) return false;
    // ⚠️ **SYNCHRONIZE 必须显式申请**。PROCESS_QUERY_LIMITED_INFORMATION
    // 不包含它，而 WaitForSingleObject 要求该权限——少了它，等待不是返回
    // WAIT_TIMEOUT 而是 WAIT_FAILED，于是「活着的进程」被判成死的。
    // 初版正是漏了这一位：本进程查自己都返回 false ⇒ **每一把写锁都会被
    // 当成 stale 而删掉** ⇒ 两个 writer 能同时持锁。是 ProcessToken
    // 那组单测当场揪出来的（tests/posix_file_test.cpp）。
    const HANDLE p = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                   FALSE, static_cast<DWORD>(pid));
    if (p == nullptr) {
        // ERROR_INVALID_PARAMETER = 没有这个 pid（进程已完全消失）。
        // 其余（典型是 ERROR_ACCESS_DENIED：跨会话/更高完整性级别的进程）
        // 说明进程存在但我们无权查询 —— 判活。
        return ::GetLastError() != ERROR_INVALID_PARAMETER;
    }
    // 不用 GetExitCodeProcess：它的 STILL_ACTIVE(259) 与「进程以 259 退出」
    // 无法区分。等待对象是否已 signaled 没有这个歧义。
    const DWORD w = ::WaitForSingleObject(p, 0);
    ::CloseHandle(p);
    // 只有**明确 signaled** 才判死。WAIT_FAILED 一类的意外一律判活——
    // 与 POSIX 后端把 EPERM 视为「活着」同一取向：宁可不回收 stale lock
    // （表现为库打不开，吵闹且可诊断），也不能误删他人的有效锁
    // （表现为两个 writer 同时写，静默损坏）。
    return w != WAIT_OBJECT_0;
}

// 进程实例令牌 = 进程创建时刻（FILETIME，100ns 精度）。
// 同一 pid 的两次进程实例要撞上同一个创建时刻，需要它们在同一个 100ns 内
// 被创建——而复用一个 pid 至少要等前一个进程完全退出，故实际不可能。
std::uint64_t process_start_token(int pid) noexcept {
    if (pid <= 0) return 0;
    const HANDLE p = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                   static_cast<DWORD>(pid));
    if (p == nullptr) return 0;  // 进程不在，或无权查询 → 无令牌
    FILETIME created{}, exited{}, kernel{}, user{};
    const BOOL ok = ::GetProcessTimes(p, &created, &exited, &kernel, &user);
    ::CloseHandle(p);
    if (!ok) return 0;
    const std::uint64_t t =
        (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) |
        static_cast<std::uint64_t>(created.dwLowDateTime);
    // 0 是「无令牌」的哨兵，真实创建时刻不可能是 0；万一是，抬成 1。
    return t == 0 ? 1 : t;
}

bool process_alive(int pid, std::uint64_t expect_token) noexcept {
    if (!process_alive(pid)) return false;
    if (expect_token == 0) return true;  // 老锁文件没记令牌 → 只能按 pid 判
    const std::uint64_t now = process_start_token(pid);
    // 取不到当前令牌（典型是无权查询该进程）→ **保守判活**，与
    // process_alive 的既定取向一致：宁可不回收，也不误删他人的有效锁。
    if (now == 0) return true;
    return now == expect_token;  // 不等 ⇒ 是复用了同一 pid 的另一个进程
}

// Windows 的句柄不受 fd 表约束（也没有 RLIMIT_NOFILE 这样的软上限），
// 故按 io.hpp 的契约返回 nullopt，由调用方给保守兜底
// （cask.cpp 取 1024 后再过 resolve_read_handle_cap）。
std::optional<std::uint64_t> max_open_files() noexcept {
    return std::nullopt;
}

}  // namespace bitcask::io
