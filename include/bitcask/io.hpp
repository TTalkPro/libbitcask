// 文件 I/O 平台抽象层。纯 C++，不依赖 Erlang/OTP——这样可以直接喂给
// gtest，跟 NIF 层解耦。语义上跟 legacy bitcask_nifs.c 的 file_* 系列原语
// 一一对应，方便从 C 翻译过来不出现行为漂移。
//
// === S37-1：本头是「平台 seam」===
// 全库**唯一**允许直接调用宿主文件系统原语的地方是本头的实现文件
// （POSIX: src/io/posix_file.cpp；Windows: src/io/win32_file.cpp）。
// 其余第一方代码一律经由本头，使 Windows 移植 = 新增一个实现文件，
// 而不是改 20 个调用站点。
//
// 收编前的形态（2026-08-07 实测）：12 处裸 ::open、53 处 ::close、
// 8 处 ::fstat、8 处 ::fdatasync、4 处 ::ftruncate、4 处 std::rename
// 散在 20 个文件里——其中 std::rename **在 Windows 上目标存在即失败**
// （POSIX 下是原子覆盖），9 个原子写站点会全线挂。见 io::atomic_rename。
//
// === 线程模型 ===
// PosixFile 对象本身只持有一个 int fd_，没有内部互斥量。
//   - 同对象的「带 offset」方法（pread / pwrite）：OS 层 thread-safe，
//     多线程可并发调用同一个 PosixFile 对同一 fd 读写不同 offset。
//   - 同对象的「使用 fd 内部 offset」方法（read / write / seek /
//     truncate_here）：依赖 fd 当前 offset，多线程同时调会互相踩
//     ——caller 必须串行化（典型用法是写路径单线程持有该对象）。
//   - 跨对象：完全独立、线程安全。
//   - 构造 / 析构 / open / close_quiet：非线程安全，由对象所有者控制。
// 本模块本身不提供任何锁——并发需求由上层（cask / data_file）保证。

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bitcask::io {

// ---------------------------------------------------------------------------
// 平台原生文件句柄。
//
// POSIX 下是 int fd；Windows 下将是 void*（HANDLE）。第一方代码以本别名
// 为通货（而非裸 int），使 Windows 后端只换 typedef 而不改调用站点形态。
// 向量插件（hnsw / ivf_rq / diskann）仍以裸句柄为通货——它们的 build/append
// 路径把句柄穿过多层 helper，包成 RAII 对象的收益不抵改动风险，故本层
// 同时提供句柄级自由函数（见下）。
// ---------------------------------------------------------------------------
// S37-5：Windows 后端落地，typedef 按平台分叉。
//
// **哨兵值刻意选 nullptr 而非 INVALID_HANDLE_VALUE**：后者是
// `(HANDLE)(LONG_PTR)-1`，需要 reinterpret_cast，无法出现在 constexpr 里
// （本常量与 handle_valid 都是 constexpr，65 处调用站点依赖之）。
// win32_file.cpp 在边界处把 CreateFileW 的 INVALID_HANDLE_VALUE 归一成
// nullptr——「无效句柄只有一种表示」在库内是硬约定，比多一种哨兵更不易错。
#if defined(_WIN32)
using FileHandle = void*;  // Win32 HANDLE
inline constexpr FileHandle kInvalidHandle = nullptr;
[[nodiscard]] constexpr bool handle_valid(FileHandle h) noexcept {
    return h != nullptr;
}
#else
using FileHandle = int;  // POSIX fd
inline constexpr FileHandle kInvalidHandle = -1;
[[nodiscard]] constexpr bool handle_valid(FileHandle h) noexcept {
    return h >= 0;
}
#endif

// ---------------------------------------------------------------------------
// 文件身份——「这个路径还是不是我上次写的那个文件」。
//
// POSIX 用 (st_dev, st_ino)；Windows 用 (dwVolumeSerialNumber,
// nFileIndexHigh/Low)。本结构替代 hnsw.hpp 此前直接暴露的 dev_t/ino_t，
// 顺带把 <sys/types.h> 逐出公开头（原先污染所有下游用户）。
//
// 用途（S14-2）：.vec / .qc8 payload 追加前校验目标路径未被外部替换——
// 若身份不符则放弃追加、退回全量重写。
// ---------------------------------------------------------------------------
struct FileIdentity {
    std::uint64_t device  = 0;
    std::uint64_t file_id = 0;
    friend bool operator==(const FileIdentity&,
                           const FileIdentity&) noexcept = default;
};

// open() 接受的 flag 位掩码。bitcask 自己的语义层，不直接对应 POSIX flag。
enum class OpenFlag : unsigned {
    kNone     = 0,
    // 默认（无 flag）：O_RDWR | O_APPEND | O_CREAT
    // kCreate：           O_CREAT | O_EXCL | O_RDWR | O_APPEND（强制新建）
    kCreate   = 1u << 0,
    kReadOnly = 1u << 1,  // 改用 O_RDONLY，跟 kCreate 互斥（caller 自己保证）
    kOSync    = 1u << 2,  // 在原 flag 上 OR 一个 O_DSYNC（见 translate 注释）

    // --- S37-1 新增：收编裸 ::open 站点所需的形态 ---------------------------
    // 这四位的存在理由是「原先绕过本抽象的站点用了本抽象表达不了的 flag」，
    // 逐个补齐后裸调用才能归零。语义严格对齐各站点收编前的原样。
    kWriteOnly   = 1u << 3,  // O_WRONLY（hnsw payload 追加：只写不读）
    // O_RDWR | O_CREAT | O_TRUNC —— **不带 O_APPEND**（与 kCreate 的关键差别：
    // 向量段 build 走 pwrite 定位写，O_APPEND 会让每次写强制落到文件尾）
    kTruncate    = 1u << 4,
    kCloseOnExec = 1u << 5,  // OR 一个 O_CLOEXEC（Windows 句柄默认不继承）
    // OR 一个 O_SYNC（**非** O_DSYNC）。仅 bitcask.write.lock 用：stale-lock
    // 检查要求写进去的 pid 立刻对其它进程的读锁可见，元数据也需同步。
    kSyncAll     = 1u << 6,
    // 从基模式里**去掉** O_APPEND。仅 bitcask.write.lock 用。
    // 理由是 POSIX 的一条暗礁：**O_APPEND 下 pwrite 会忽略 offset 改为追加**。
    // 锁文件走 ftruncate(0) + pwrite(0, …) 覆盖写，若带上 O_APPEND，写入位置
    // 就不再由 offset 决定。当前 truncate 到 0 使二者结果碰巧一致，但语义已
    // 漂移——收编时原样保留「无 O_APPEND」，不依赖这个巧合。
    kNoAppend    = 1u << 7,
};
constexpr OpenFlag operator|(OpenFlag a, OpenFlag b) noexcept {
    return static_cast<OpenFlag>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
constexpr bool has_flag(OpenFlag set, OpenFlag bit) noexcept {
    return (static_cast<unsigned>(set) & static_cast<unsigned>(bit)) != 0u;
}

// 新建文件的权限位。POSIX 语义；Windows 后端忽略（由 ACL 继承决定）。
// 两档取值来自收编前各站点的原样：核心 KV 路径 0600、向量段 tmp 0644。
enum class FileMode : unsigned {
    kOwnerOnly     = 0,  // 0600
    kWorldReadable = 1,  // 0644
};

// IoError 只带 errno；NIF 层用 erl_errno_id() 翻成 atom，跟 legacy 行为
// 完全对齐（业务上拿到的 {error, enoent}/{error, eio} 等都不变）。
struct IoError {
    int errnum = 0;
};

// pread / read 的结果：要么读到了字节（可能短读），要么 EOF，要么报错。
// EOF 单独走 ReadEof 是因为 NIF 那边返回的是 atom 'eof'（不是 {ok, <<>>}），
// 必须在类型层面就把这两种情况区分开，避免下游误把 0 字节当成「合法空读」。
struct ReadOk {
    std::vector<std::byte> data;
};
struct ReadEof {};
using ReadResult = std::expected<std::variant<ReadOk, ReadEof>, IoError>;

// 句柄持有者：移动语义、析构 close、不可拷贝。
// 跟 std::unique_ptr 一样的所有权模型，但定制了 kInvalidHandle sentinel。
//
// S37-1：类名由 PosixFile 改为 File（Windows 后端下 "Posix" 是误称）；
// PosixFile 保留为别名，既有 65 处引用零改动。
class File {
public:
    File() noexcept = default;
    explicit File(FileHandle fd) noexcept : fd_(fd) {}
    ~File() noexcept { close_quiet(); }

    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept : fd_(other.fd_) { other.fd_ = kInvalidHandle; }
    File& operator=(File&& other) noexcept {
        if (this != &other) {
            close_quiet();
            fd_ = other.fd_;
            other.fd_ = kInvalidHandle;
        }
        return *this;
    }

    [[nodiscard]] bool is_open() const noexcept { return handle_valid(fd_); }
    [[nodiscard]] FileHandle fd() const noexcept { return fd_; }

    // 放弃所有权并交出裸句柄（析构不再 close）。给「先经 File 打开拿到
    // 正确 flag 翻译，再把句柄交给以裸句柄为通货的既有路径」的站点用。
    [[nodiscard]] FileHandle release() noexcept {
        const FileHandle h = fd_;
        fd_ = kInvalidHandle;
        return h;
    }

    // 按 bitcask 风格的 flag 打开文件。flag 推导规则跟 legacy
    // get_file_open_flags() 1:1 对齐（见 OpenFlag 注释）。
    // 线程安全: 是（每次调用产出一个新对象，不触碰任何共享状态）。
    [[nodiscard]] static std::expected<File, IoError>
    open(std::string_view path, OpenFlag flags,
         FileMode mode = FileMode::kOwnerOnly) noexcept;

    // 关 fd；幂等。错误吞掉——legacy 也是这个行为，反正 close 失败没救。
    // 线程安全: 否（修改 fd_）；caller 必须保证此刻无其它线程在用该对象。
    void close_quiet() noexcept;

    // fsync(fd_)。
    // 线程安全: 是（仅读 fd_，sys call 自身线程安全）；无需锁。
    [[nodiscard]] std::expected<void, IoError> sync() noexcept;

    // pread 循环：完整读到、短读 (>0)、EOF (0)、或 errno。短读不会被
    // 误报成 EOF——返回的 data.size() 直接告诉调用方读到多少。
    // 线程安全: 是（pread 不修改 fd offset；多线程可并发 pread 同一对象）。
    [[nodiscard]] ReadResult pread(std::uint64_t offset, std::size_t count) noexcept;

    // pread 的零分配版：读进 caller 提供的缓冲区。返回实际读到的字节数
    // （0 = EOF；可能短读，语义同 pread）。热路径（get/fold）用它配合
    // 复用缓冲，避免每次读都构造 vector。
    // 线程安全: 是（同 pread）。
    [[nodiscard]] std::expected<std::size_t, IoError>
    pread_into(std::uint64_t offset, std::span<std::byte> buf) noexcept;

    // pwrite 循环直到全部写完或出错。pwrite 部分写不退化成短写——会继续。
    // 线程安全: 是（pwrite 不动 fd offset）；但 caller 自己保证写区间不重叠。
    [[nodiscard]] std::expected<void, IoError>
    pwrite(std::uint64_t offset, std::span<const std::byte> data) noexcept;

    // 顺序读写（用当前 fd offset，不指定 offset）。
    // 线程安全: 否（依赖并修改 fd 内部 offset，多线程并发会乱序读/覆盖）。
    // caller 必须串行化对同一对象的 read/write/seek/truncate_here 调用。
    [[nodiscard]] ReadResult read(std::size_t count) noexcept;
    [[nodiscard]] std::expected<void, IoError>
    write(std::span<const std::byte> data) noexcept;

    // lseek。whence: SEEK_SET / SEEK_CUR / SEEK_END。
    // 线程安全: 否（改 fd offset）；同一对象 caller 串行化。
    [[nodiscard]] std::expected<std::uint64_t, IoError>
    seek(std::int64_t offset, int whence) noexcept;

    // 便捷：seek 到文件头。
    // 线程安全: 否（同 seek）。
    [[nodiscard]] std::expected<void, IoError> seek_bof() noexcept;

    // ftruncate 到当前 fd offset。跟 legacy file_truncate 一致：截掉
    // current offset 之后的所有内容（不是 truncate 到 0）。
    // 线程安全: 否（依赖 fd 当前 offset）；同一对象 caller 串行化。
    [[nodiscard]] std::expected<void, IoError> truncate_here() noexcept;

    // --- S37-1 新增 ---------------------------------------------------------

    // 截断到显式长度（不依赖 fd 当前 offset，故线程安全）。
    [[nodiscard]] std::expected<void, IoError>
    truncate(std::uint64_t length) noexcept;

    // 当前文件大小。收编 8 处 ::fstat（其中 7 处只为取 st_size）。
    // 线程安全: 是（不触碰 fd offset）。
    [[nodiscard]] std::expected<std::uint64_t, IoError> size() const noexcept;

    // 文件身份（见 FileIdentity 注释）。线程安全: 是。
    [[nodiscard]] std::expected<FileIdentity, IoError>
    identity() const noexcept;

private:
    FileHandle fd_ = kInvalidHandle;
};

// S37-1 之前的类名。既有站点（data_file / hint_file / oki_run / segment /
// chunked_reader 等 65 处）继续可用。
using PosixFile = File;

// ---------------------------------------------------------------------------
// S36 后续（backlog B3）：只读整文件 mmap 的 RAII 归并。
//
// 此前 data_file / segment_v2 / hnsw / diskann / ivf_rq 共 7 处各写各的
// map/unmap/madvise/移动语义——本身就是维护面（S33-B2「mmap 窗口外读」
// 那类 bug 的温床）。本类统一承载：
//   - 语义：PROT_READ + MAP_SHARED 整文件映射，[0, size)。
//   - **不接管 fd**：映射建立后 fd 可关可留（内核对映射页持引用，close
//     后映射仍有效）——各站点自行决定 fd 去留（data_file 留 fd 供 pread；
//     向量 payload 关 fd 省预算）。
//   - 析构 munmap；move-only（源置空，杜绝双 munmap）。
//   - POSIX unlink-while-mapped 语义下映射持续有效（B4 的延迟删除队列
//     落地前，各站点的 pin 语义不变）。
//
// === S37-6：Windows 上这条 pin 语义同样成立（实测，非推断）===
// 移植设计稿曾假设「被 section 映射持有的文件在 Windows 上删不掉」，
// 据此规划了一项「退休前先逐出映射、并与 epoch 回收协调」的架构改动。
// **实测（Windows 10.0.26200）推翻了该前提**——只要所有句柄都带
// FILE_SHARE_DELETE（本库的 CreateFileW 全部如此）：
//   - DeleteFile 一个正被映射的文件**成功**，且名字**立刻**从目录消失
//     （Win10 1709+ 的 POSIX 语义删除），同名可立即重建；
//   - MoveFileEx(..., REPLACE_EXISTING) 覆盖一个正被映射的文件**成功**；
//   - 上述两种情况下，**已建立的视图完整保持旧内容**（含覆盖后才首次
//     触碰的页）——与 POSIX unlink-while-mapped 逐条等价。
// 于是 merge 的「unlink 旧文件，在途读者继续读旧映射」在两平台同样成立，
// 那项架构改动不必做。
//
// ⚠️ **真正的 Windows 限制是另一件事：仍开着的文件句柄**。
// MoveFileEx(REPLACE_EXISTING) 覆盖一个**尚有文件句柄打开**的目标必然
// ERROR_ACCESS_DENIED（实测：任何访问模式都拦，连 GENERIC_READ 也不例外），
// 与共享模式和映射都无关。故凡是「写 tmp → rename 覆盖自己正在读的文件」
// 的站点，**必须先关掉那个句柄**——映射不用动。这正是
// IvfSegment/DiskannSegment::open 在建好映射后立刻 close_handle 的理由
// （它们本就只经映射读，fd 从未被用过）。
//
// 线程模型：映射建立后只读、无内部状态变更——多线程并发读 data() 安全；
// 构造/析构/移动由所有者单线程控制（同 PosixFile 约定）。
// ---------------------------------------------------------------------------
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();  // munmap（定义在 .cpp——头文件不引 <sys/mman.h>）

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& o) noexcept : base_(o.base_), len_(o.len_) {
        o.base_ = nullptr;
        o.len_ = 0;
    }
    MappedFile& operator=(MappedFile&& o) noexcept;

    // 映射 fd 的 [0, len) 只读区间。len == 0 或 mmap 失败 → 无效对象
    // （valid() == false，caller 走 pread 回退——与各站点既有降级一致）。
    // advise_random：建立后 madvise(MADV_RANDOM)（随机点查负载防内核
    // 预读浪费——data_file get 热路径 / 向量 payload 的既有纪律）。
    // S37-5：形参由裸 int 改 FileHandle（Windows 上 int 装不下 HANDLE）。
    // Linux 上 FileHandle 就是 int，签名逐字不变。
    [[nodiscard]] static MappedFile map_readonly(FileHandle fd, std::size_t len,
                                                 bool advise_random) noexcept;

    [[nodiscard]] bool valid() const noexcept { return base_ != nullptr; }
    [[nodiscard]] const std::byte* data() const noexcept { return base_; }
    [[nodiscard]] std::size_t size() const noexcept { return len_; }
    [[nodiscard]] std::span<const std::byte> view() const noexcept {
        return {base_, len_};
    }

    // 主动解除映射（幂等）。析构自动调用。
    void reset() noexcept;

private:
    const std::byte* base_ = nullptr;
    std::size_t len_ = 0;
};

// ---------------------------------------------------------------------------
// 句柄级自由函数（S37-1）
//
// 供仍以裸句柄为通货的路径使用——主要是向量插件的 build / payload 追加，
// 它们把句柄穿过多层 helper（TmpFile、pwrite_all…），改成 RAII 对象的
// 收益不抵改动风险。这些函数是 File 之外**唯一**的宿主原语出口。
// ---------------------------------------------------------------------------

// 打开并交出裸句柄（flag 翻译与 File::open 完全同一条路径）。
[[nodiscard]] std::expected<FileHandle, IoError>
open_handle(std::string_view path, OpenFlag flags,
            FileMode mode = FileMode::kOwnerOnly) noexcept;

// 关句柄；对 kInvalidHandle 为 no-op。错误吞掉（close 失败无恢复路径）。
void close_handle(FileHandle h) noexcept;

// fdatasync（Windows: FlushFileBuffers——无 data/metadata 之分）。
[[nodiscard]] bool sync_data(FileHandle h) noexcept;

// ftruncate 到显式长度。
[[nodiscard]] bool truncate_handle(FileHandle h, std::uint64_t length) noexcept;

// 文件大小 / 身份；失败返回 nullopt。
[[nodiscard]] std::optional<std::uint64_t> handle_size(FileHandle h) noexcept;
[[nodiscard]] std::optional<FileIdentity> handle_identity(FileHandle h) noexcept;

// 按路径取身份（不需要已打开的句柄）。收编 hnsw 的 2 处 ::stat。
[[nodiscard]] std::optional<FileIdentity>
path_identity(const std::string& path) noexcept;

// 定位读写循环，含 EINTR 重试。短读/短写视作失败（调用方读写已知长度）。
// 原 vec::diskint::pwrite_all / pread_all，S37-1 收编至此。
[[nodiscard]] bool pwrite_all(FileHandle h, const void* buf, std::size_t len,
                              std::uint64_t off) noexcept;
[[nodiscard]] bool pread_all(FileHandle h, void* buf, std::size_t len,
                             std::uint64_t off) noexcept;

// 单次定位读/写——**不循环、不重试**，返回实际传输字节数（nullopt = 错误）。
// 与上面的 *_all 是刻意并存的两套语义：file_lock 的 legacy 契约是「部分写
// 也算成功，不重试」（见 file_lock.cpp write_data 注释），套 *_all 会把该
// 契约悄悄改掉。锁文件 < 一页，短读写实际不会发生，但语义须原样保留。
[[nodiscard]] std::optional<std::size_t>
pread_once(FileHandle h, void* buf, std::size_t len,
           std::uint64_t off) noexcept;
[[nodiscard]] std::optional<std::size_t>
pwrite_once(FileHandle h, const void* buf, std::size_t len,
            std::uint64_t off) noexcept;

// ---------------------------------------------------------------------------
// 路径级自由函数（S37-1）
// ---------------------------------------------------------------------------

// 原子改名，**目标已存在则覆盖**。
//
// ⚠️ 本函数是 Windows 移植的头号风险点（设计稿 C1）。POSIX 的 rename(2)
// 在目标存在时原子覆盖，而 **Windows CRT 的 std::rename 目标存在即失败**。
// 全库 9 个原子写站点（keydir snapshot / index manifest / field.schema /
// hnsw ×3 / docmap ckpt / search ckpt / oki run）都经由本函数落地——
// 若各站点继续直接调 std::rename，Windows 上第二次写入即全线失败。
// Windows 实现须用 MoveFileExW(MOVEFILE_REPLACE_EXISTING |
// MOVEFILE_WRITE_THROUGH)。
[[nodiscard]] bool atomic_rename(const std::string& from,
                                 const std::string& to) noexcept;

// fsync 路径所在目录，使其中的 rename 持久化。尽力而为（打不开即跳过）。
// Windows 无对应物 → 后端将降为 no-op，rename 的目录项持久性改由
// MOVEFILE_WRITE_THROUGH 承担（持久性契约不变，机制不同）。
void sync_directory(const std::string& path) noexcept;

// 已打开的 FILE* 流：fflush + fdatasync。两个返回值都检查——disk-full 下
// fflush 失败而 fdatasync 对已落盘部分成功，就会 rename 出半截文件。
[[nodiscard]] bool flush_and_sync_stream(std::FILE* f) noexcept;

// ---------------------------------------------------------------------------
// 打开一个 std::FILE* 流（S37-6）。语义同 std::fopen，**唯一差别是持有期间
// 该文件仍可被删除**。失败返回 nullptr。mode 支持 "r"/"w"/"a" + 可选 "+"/"b"。
//
// 为什么需要它：**MSVC 的 CRT 用 `_SH_DENYNO` 开文件，共享位只有
// FILE_SHARE_READ|FILE_SHARE_WRITE，没有 FILE_SHARE_DELETE，且 CRT 不提供
// 任何开关能加上它**。于是任何被 std::fopen 长期持有的文件在 Windows 上都
// 删不掉（实测 DeleteFileW 报 ERROR_SHARING_VIOLATION），连带整个目录都删不掉。
// POSIX 侧本函数就是 std::fopen，零差别。
//
// 只有**长期持有**的流需要用它。用完即关的读写（file_util 的 FilePtr、
// AtomicFileWriter 的 tmp）继续用 std::fopen 即可——它们的持有窗口内不会
// 有人来删该文件。
[[nodiscard]] std::FILE* open_stream(const std::string& path,
                                     const char* mode) noexcept;

// 建议宿主预取 [addr, addr+len) 的页（POSIX: madvise(MADV_WILLNEED)；
// Windows: PrefetchVirtualMemory）。尽力而为，失败无碍（页仍会按需缺页装入）。
//
// 取地址区间而非「映射 + 偏移」：唯一调用方（HNSW 精排前的候选页预取）
// 自己按页掩码合并了候选地址区间，地址式接口免去一层偏移换算；
// Windows 的 PrefetchVirtualMemory 同样以地址区间为单位，语义直接对应。
void prefetch_range(const void* addr, std::size_t len) noexcept;

// 系统页大小。
// ⚠️ Windows 下页大小（dwPageSize，4 KiB）与**映射分配粒度**
// （dwAllocationGranularity，64 KiB）不同；本函数返回前者（用于预取区间
// 对齐）。映射视图偏移的对齐须用后者——当前全部是整文件映射（offset=0），
// 尚无站点需要，故暂不暴露。
[[nodiscard]] std::size_t page_size() noexcept;

// 删除一个文件。成功返回 true；目标不存在或被占用返回 false。
//
// ⚠️ POSIX 下删除仍被打开的文件是合法的（inode 延迟回收）；**Windows 下
// 除非所有句柄都以 FILE_SHARE_DELETE 打开，否则报 ERROR_SHARING_VIOLATION，
// 且被 section 映射持有的文件即便如此也删不掉**（设计稿 C2）。调用方须
// 容忍失败——Cask::drain_retired_files 的重试队列即为此存在。
[[nodiscard]] bool remove_file(const std::string& path) noexcept;

// ---------------------------------------------------------------------------
// 进程原语（S37-1 一并收编——它们与写锁的 stale 回收强耦合，留在外面会让
// S37-5 的「Windows 后端 = 新增一个实现文件」不成立）
// ---------------------------------------------------------------------------

// 当前进程 id。写进 bitcask.write.lock 供 stale-lock 检查。
[[nodiscard]] int current_process_id() noexcept;

// 探测 pid 对应的进程是否还活着。**保守偏向「活着」**——无权查询等不确定
// 情形一律返回 true，避免误删他人持有的有效锁。
[[nodiscard]] bool process_alive(int pid) noexcept;

// ---------------------------------------------------------------------------
// 进程实例令牌（S37-5，设计稿 C4 / 风险 #2）
//
// **问题**：仅凭 pid 判存活，在 PID 被复用后会把「另一个进程」误认成「锁的
// 原主人还活着」，于是拒绝回收 stale lock —— 库彻底打不开，且只在特定时序
// 复现。Windows 上这不是理论风险：内核主动复用小号 PID，一个崩溃后立刻重启
// 的服务极易撞上自己上一次的 PID。
//
// **解法**：锁文件除 pid 外再记一个「进程实例令牌」，回收前双重比对。
// 令牌只需满足「同一 pid 的不同进程实例，取值几乎必然不同」。
//   Windows：GetProcessTimes 的 ftCreationTime（100ns 精度的创建时刻）。
//   POSIX  ：返回 0（无令牌）——见下。
//
// 令牌为 0 表示「取不到」，此时 process_alive(pid, 0) 退化为 process_alive(pid)，
// 与本函数引入前的行为逐字一致。POSIX 后端恒返回 0 是**刻意**的：Linux 的
// pid 是顺序分配、绕 pid_max 才回卷，复用间隔以万计，仅按 pid 判断是本库
// 长期以来的既有行为；改成读 /proc/<pid>/stat 的 starttime 会把一条
// 「本届不涉及、且无法在 Windows 上验证」的行为变更塞进移植里。
// ---------------------------------------------------------------------------
[[nodiscard]] std::uint64_t process_start_token(int pid) noexcept;

// 带令牌的存活探测：pid 存活 **且** 令牌吻合才算「原主人还活着」。
// expect_token == 0（老锁文件 / 取不到令牌）→ 退化为 process_alive(pid)。
[[nodiscard]] bool process_alive(int pid, std::uint64_t expect_token) noexcept;

// 进程可同时打开的文件数上限（POSIX: RLIMIT_NOFILE 软上限）。
// 取不到（含 RLIM_INFINITY）时返回 nullopt，由调用方给保守兜底。
// Windows 下句柄不受 fd 表约束，后端将返回 nullopt——read-handle LRU 预算
// 须另行标定（设计稿 C9）。
[[nodiscard]] std::optional<std::uint64_t> max_open_files() noexcept;

}  // namespace bitcask::io
