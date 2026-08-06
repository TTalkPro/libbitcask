// POSIX 文件 I/O 包装。纯 C++，不依赖 Erlang/OTP——这样可以直接喂给
// gtest，跟 NIF 层解耦。语义上跟 legacy bitcask_nifs.c 的 file_* 系列原语
// 一一对应，方便从 C 翻译过来不出现行为漂移。
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
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bitcask::io {

// open() 接受的 flag 位掩码。bitcask 自己的语义层，不直接对应 POSIX flag。
enum class OpenFlag : unsigned {
    kNone     = 0,
    // 默认（无 flag）：O_RDWR | O_APPEND | O_CREAT
    // kCreate：           O_CREAT | O_EXCL | O_RDWR | O_APPEND（强制新建）
    kCreate   = 1u << 0,
    kReadOnly = 1u << 1,  // 改用 O_RDONLY，跟 kCreate 互斥（caller 自己保证）
    kOSync    = 1u << 2,  // 在原 flag 上 OR 一个 O_SYNC
};
constexpr OpenFlag operator|(OpenFlag a, OpenFlag b) noexcept {
    return static_cast<OpenFlag>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
constexpr bool has_flag(OpenFlag set, OpenFlag bit) noexcept {
    return (static_cast<unsigned>(set) & static_cast<unsigned>(bit)) != 0u;
}

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

// fd 持有者：移动语义、析构 close、不可拷贝。
// 跟 std::unique_ptr 一样的所有权模型，但定制了 fd_=-1 sentinel。
class PosixFile {
public:
    PosixFile() noexcept = default;
    explicit PosixFile(int fd) noexcept : fd_(fd) {}
    ~PosixFile() noexcept { close_quiet(); }

    PosixFile(const PosixFile&) = delete;
    PosixFile& operator=(const PosixFile&) = delete;
    PosixFile(PosixFile&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    PosixFile& operator=(PosixFile&& other) noexcept {
        if (this != &other) { close_quiet(); fd_ = other.fd_; other.fd_ = -1; }
        return *this;
    }

    [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int  fd()      const noexcept { return fd_; }

    // 按 bitcask 风格的 flag 打开文件。flag 推导规则跟 legacy
    // get_file_open_flags() 1:1 对齐（见 OpenFlag 注释）。mode = 0600。
    // 线程安全: 是（每次调用产出一个新对象，不触碰任何共享状态）。
    [[nodiscard]] static std::expected<PosixFile, IoError>
    open(std::string_view path, OpenFlag flags) noexcept;

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

private:
    int fd_ = -1;
};

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
    [[nodiscard]] static MappedFile map_readonly(int fd, std::size_t len,
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

}  // namespace bitcask::io
