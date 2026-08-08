// file_util — 文件句柄 RAII 公共归宿（RED-2/MEM-LOW-1）。
//
// 历史：FileCloser struct 在 9 处重复定义（field_schema / index_manifest /
// search_checkpoint / segment_v2 / inverted / keydir / hnsw ×3 / migrate 引用），
// 行为逐字相同但分散导致：① 漂移温床；② 命名空间 workaround
// （index_manifest 的 manifest_io 命名空间专门为避免与 field_schema 撞名而存在）。
// 本头作为单一真相源，全部归 bitcask::detail::FileCloser / FilePtr。
//
// MEM-LOW-1：裸 FILE* 在 bad_alloc 路径跳过 fclose → fd 泄漏。本头让所有
// 调用站点以零成本（默认 unique_ptr 析构 noexcept）获得异常安全。
//
// T21（P6-RED-1/2）：整读样板 ×6 与原子写样板 ×9 已归并至本头
// （read_file_bytes / atomic_write_bytes / AtomicFileWriter）。归并前九个
// 原子写站点跑出**四套** fsync 纪律——hnsw ×3 完全不 sync（P6-DUR-1，
// 断电后 rename 已覆盖旧文件却只留半截）、index_manifest 不检查 fdatasync
// 返回值、field_schema 用 fsync 而非 fdatasync、其余用 fflush+fdatasync。
// 样板不归并 ⇒ 纪律靠人肉复制 ⇒ 必然漂移。此处为单一真相源。

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "bitcask/io.hpp"  // S37-1：宿主原语一律经 io seam（原 <fcntl.h>/<unistd.h>）
#include "bitcask/detail/path_utf8.hpp"  // P0：窄路径 = UTF-8，不走 ANSI 代码页

// W5：**唯一**需要在头里出现平台头的地方——`stream_handle` 必须 inline
// （理由见该函数）。两边都只是 CRT 的小头，不引 windows.h、不带 min/max 宏：
//   Windows：<io.h> 给 _get_osfhandle（_fileno 在 <cstdio> 里）
//   POSIX  ：fileno 由 <cstdio> 提供，无需额外头
#if defined(_WIN32)
#  include <fcntl.h>   // _O_BINARY / _O_RDONLY / _O_APPEND（adopt_stream）
#  include <io.h>      // _get_osfhandle / _open_osfhandle / _close
#endif

namespace bitcask::detail {

struct FileCloser {
    void operator()(std::FILE* f) const noexcept { if (f) std::fclose(f); }
};

// FILE* 的 RAII 句柄。构造传 fopen 返回值（可为 nullptr，析构安全）。
//   bitcask::detail::FilePtr f{std::fopen(path, "rb")};
//   if (!f) { /* fopen 失败 */ }
//   // … 使用 f.get() 取裸 FILE*
//   // 作用域结束自动 fclose；抛出路径也覆盖
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

// ---------------------------------------------------------------------------
// std::fopen 的 UTF-8 版（P0）。
//
// **Windows 上 `std::fopen` 按 CRT 的 ANSI 代码页解释窄路径**，而库内窄路径
// 一律是 UTF-8（见 path_utf8.hpp）。简中 Windows 实测 `GetACP()==936`，于是
// 非 ASCII 路径要么开错文件、要么开不出来。`_wfopen` 收宽串，把编码这一层
// 交给 from_utf8 而不是代码页。POSIX 下就是 std::fopen，零差别。
//
// 注意本函数只解决**编码**。共享位（Windows CRT 不带 FILE_SHARE_DELETE）与
// CRT 边界是另一条线，见 io.hpp 的 open_handle / adopt_stream。
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::FILE* fopen_utf8(const std::string& path,
                                           const char* mode) noexcept {
#if defined(_WIN32)
    const std::filesystem::path wp = from_utf8(path);
    // mode 全库只有 "rb"/"wb"/"ab" 这类纯 ASCII 短串，逐字符加宽即可。
    wchar_t wmode[8] = {};
    std::size_t i = 0;
    for (; mode[i] != '\0' && i + 1 < std::size(wmode); ++i) {
        wmode[i] = static_cast<wchar_t>(mode[i]);
    }
    return ::_wfopen(wp.c_str(), wmode);
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// std::remove 的 UTF-8 版（P0）。同上：CRT 的 remove 也按 ANSI 解释窄路径。
// 走 io::remove_file（Windows 后端是 DeleteFileW + widen，POSIX 是 ::remove）。
inline void remove_utf8(const std::string& path) noexcept {
    (void)io::remove_file(path);
}

// ---------------------------------------------------------------------------
// 64 位偏移的 fseek（P2）。
//
// `std::fseek` 的偏移类型是 `long`，**MSVC x64 上只有 4 字节**（Linux 上 8 字节，
// 所以这条分歧只在 Windows 出现，CI 与全部现有测试都照不到）。实测：
//   static_cast<long>(3 GiB + 4 KiB) == -1073737728   —— 静默截断成负数
//   2.54 GiB 文件上 ftell() == -1
// 于是「offset 超过 2 GiB」不会报错，而是 seek 到一个负数位置。
//
// 只读流用不上它——那条路已整体改走 seam 的定位读（pread_all 收 uint64）。
// 本函数留给仍以 FILE* 顺序写、只在小偏移上回头补头的写路径（hnsw 的分页
// CRC + 回头补头），把那个 static_cast 去掉，免得日后格式一变就静默截断。
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool fseek64(std::FILE* f, std::int64_t off, int origin) noexcept {
#if defined(_WIN32)
    return ::_fseeki64(f, off, origin) == 0;
#else
    return ::fseeko(f, static_cast<off_t>(off), origin) == 0;
#endif
}

// ---- 整文件读 -------------------------------------------------------------

// 整文件读入内存。空文件 → 空向量（成功）；fopen/seek/ftell 失败或短读
// → nullopt。
//
// **尺寸谓词由调用方查 .size() 自负**——各站点门槛互不相同（>=16、精确
// 16+bits_len+4、>=kHeaderLen+kTrailerLen、允许空…），塞进本函数只会长出
// 一堆参数。本函数的契约仅「要么完整读出来，要么 nullopt」。
//
// Byte 模板化而非统一为 std::byte：调用方两种元素类型并存
// （keydir/hnsw 的 deserialize 吃 uint8_t，其余吃 byte），而仓库策略禁
// reinterpret_cast 逃逸，统一类型会逼出更多 cast。
// P2：由 stdio 改走 io seam。**动机是 32 位的 long，不是通货统一。**
//
// 原实现用 `fseek(SEEK_END)` + `ftell` 量大小，而这两者的偏移类型是 `long`，
// 在 **MSVC x64 上是 4 字节**（Linux 上是 8 字节，所以 CI 与全部现有测试都照
// 不到）。实测 2.54 GiB 的文件：`ftell()` 返回 **-1**，于是本函数把一个完全
// 正常的文件当成读不出来，返回 nullopt。调用方包括 keydir 快照、HNSW payload、
// BM25 段、migrate 的数据文件——都是大部署下能过 2 GiB 的东西。
//
// seam 的 `handle_size` / `pread_all` 收 `std::uint64_t`，没有这个上限；
// 顺带也不再需要「seek 到尾、量、再 seek 回来」这三步。
//
// 注意本函数把整个文件读进内存，所以 2 GiB 级的文件本就应当走 mmap 而非这里；
// 但「Linux 能读、Windows 读不了」是移植引入的行为分歧，与设计上限是两回事。
template <class Byte = std::byte>
[[nodiscard]] std::optional<std::vector<Byte>>
read_file_bytes(const std::string& path) {
    static_assert(sizeof(Byte) == 1, "read_file_bytes: Byte 须为单字节类型");
    auto f = io::File::open(path, io::OpenFlag::kReadOnly);
    if (!f) return std::nullopt;
    const auto sz = io::handle_size(f->fd());
    if (!sz) return std::nullopt;
    if (*sz > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return std::nullopt;  // 32 位宿主上装不下
    }
    // 大小来自可能损坏的文件（可为巨值）：vector 分配可抛 bad_alloc，
    // io::File 的析构保证句柄不泄漏（MEM-LOW-1 同源）。
    std::vector<Byte> buf(static_cast<std::size_t>(*sz));
    if (!buf.empty() && !io::pread_all(f->fd(), buf.data(), buf.size(), 0)) {
        return std::nullopt;
    }
    return buf;
}

// ---- 原子写 ---------------------------------------------------------------
//
// 全库约定（原文见 keydir.cpp write_snapshot 的 S21-2 A4 注释）：
// 写 tmp → fflush → fdatasync → rename。**「可重建」不是免 sync 的理由**：
// rename 已覆盖旧文件，断电丢页 = 最终路径下留半截文件，比「没写」更糟。
//
// fflush 与 fdatasync 的返回值**都要检查**：归并前 index_manifest 只做
// `if (wrote) ::fdatasync(...)` 丢弃返回值、其余站点丢弃 fflush 返回值——
// disk-full 下 fflush 失败而 fdatasync 对已落盘部分成功，就会 rename 出
// 半截文件。此处统一检查（T21 相对各原型的加固）。
//
// 目录 fsync（fsync_dir）：POSIX 下 rename 本身的持久性需要 fsync 父目录。
// 当前仅 manifest（唯一 commit 点）这样做，其余站点沿用归并前行为——默认
// 关，保持 T21 为纯重构。是否全面铺开属 Phase 7「目录 fsync 专项」。

// fsync 路径所在目录，使其中的 rename 持久化。尽力而为（打不开即跳过）。
//
// S37-1：目录打开与 fsync 下沉到 io::sync_directory（Windows 无对应物，
// 后端将降为 no-op，持久性改由 MOVEFILE_WRITE_THROUGH 承担）。
// 注意 parent 须转成 std::string 再传——std::filesystem::path::c_str() 在
// Windows 上返回 const wchar_t*，直接喂给窄字符 API 会编译失败。
//
// P0：这里原先是 `fs::path(path)` + `parent.string()`，两头都走 ANSI 代码页。
// **本函数是 noexcept，而 `fs::path(窄串)` 在 CP936 下遇到非 GBK 字节会抛
// `std::system_error`** —— 穿过 noexcept 就是 `std::terminate`。而本函数正是
// atomic_write_bytes 与 AtomicFileWriter::commit 的收尾步骤，即全库 9 个原子
// 写站点的公共出口：库目录名带中文 → 写任何东西 → 进程直接挂。实测见
// path_utf8.hpp 的形态 2。
inline void fsync_parent_dir(const std::string& path) noexcept {
    std::filesystem::path parent = from_utf8(path).parent_path();
    if (parent.empty()) parent = ".";
    io::sync_directory(to_utf8(parent));
}

// ---------------------------------------------------------------------------
// std::FILE* → 内核句柄（W5）
//
// **必须 inline，这是本函数存在的全部理由。** `fflush` 与「取出 fd / 句柄」
// 只在**创建该 FILE\* 的那份 CRT** 里有意义：`FILE` 的布局和 fd 表都是 CRT
// 私有的。放在头里 ⇒ 在调用方模块内展开 ⇒ 跨库边界的只剩一个**内核句柄**
// （内核对象，与 CRT 无关）。
//
// 此前这段逻辑在 `io::flush_and_sync_stream`（编进 bitcask_io），于是调用方
// 建的 `FILE*` 要穿过模块边界进到库里的 `_fileno`。`/MD`（本项目默认）下只有
// 一份 CRT 故无事；`/MT` 下每模块各一张 fd 表，拿到的是别人的号，触发
// invalid-parameter handler → `__fastfail`：**进程无声消失、退出码 0xC0000409、
// terminate/abort 都不触发**。见 `doc/api-c.md` §2.1。
// ---------------------------------------------------------------------------
inline io::FileHandle stream_handle(std::FILE* f) noexcept {
    if (f == nullptr) return io::kInvalidHandle;
#if defined(_WIN32)
    const int fd = ::_fileno(f);
    if (fd < 0) return io::kInvalidHandle;
    const std::intptr_t h = ::_get_osfhandle(fd);
    // -1 = fd 非法；-2 = 该 fd 未关联到打开的文件（两者都是文档明载的返回值）。
    if (h == -1 || h == -2) return io::kInvalidHandle;
    return reinterpret_cast<io::FileHandle>(h);
#else
    const int fd = ::fileno(f);          // POSIX：fd 本身就是 io::FileHandle
    return fd < 0 ? io::kInvalidHandle : fd;
#endif
}

// ---------------------------------------------------------------------------
// 内核句柄 → std::FILE*（W5），`stream_handle` 的反向。
//
// **同样必须 inline**：包出来的 FILE* 属于**执行这行代码的那份 CRT**，只有让
// 它在调用方模块内展开，FILE* 的分配与随后的 fclose 才在同一份 CRT 里。
// 于是跨库边界的只有 `io::open_handle` 交出来的**内核句柄**。
//
// 成功后句柄所有权转移给返回的 FILE*——fclose 会一路关到底，调用方按
// std::fopen 的习惯用即可。**失败返回 nullptr 且句柄已被关闭**（与 fopen
// 一致：拿到 nullptr 就没有任何东西需要清理）。
//
// mode 直接透传给 fdopen/_fdopen，须与打开句柄时的 flag 相容
// （典型：`io::OpenFlag::kNone` 配 "ab"）。
// ---------------------------------------------------------------------------
inline std::FILE* adopt_stream(io::FileHandle h, const char* mode) noexcept {
    if (!io::handle_valid(h) || mode == nullptr || mode[0] == '\0') {
        if (io::handle_valid(h)) io::close_handle(h);
        return nullptr;
    }
#if defined(_WIN32)
    int flags = _O_BINARY;
    if (mode[0] == 'a') flags |= _O_APPEND;
    bool plus = false;
    for (const char* p = mode + 1; *p != '\0'; ++p) {
        if (*p == '+') plus = true;
    }
    if (mode[0] == 'r' && !plus) flags |= _O_RDONLY;
    const int fd = ::_open_osfhandle(reinterpret_cast<std::intptr_t>(h), flags);
    if (fd == -1) {
        io::close_handle(h);   // 尚未被 CRT 接管，得自己收
        return nullptr;
    }
    std::FILE* f = ::_fdopen(fd, mode);
    if (f == nullptr) {
        ::_close(fd);          // 已被接管：关 fd 即连带关掉那个句柄
        return nullptr;
    }
    return f;
#else
    std::FILE* f = ::fdopen(h, mode);
    if (f == nullptr) io::close_handle(h);
    return f;
#endif
}

// 已开的 tmp 文件：flush + fdatasync。两个返回值都检查（见上）。
inline bool flush_and_sync(std::FILE* f) noexcept {
    if (std::fflush(f) != 0) return false;
    const io::FileHandle h = stream_handle(f);
    if (!io::handle_valid(h)) return false;
    return io::sync_data(h);  // 只有内核句柄跨进库里
}

// 缓冲区一次性原子落盘。失败即 remove(tmp) 并返回 false——最终路径始终
// 保持原样（要么旧内容，要么新内容，不会是半截）。
[[nodiscard]] inline bool atomic_write_bytes(const std::string& path,
                                             std::span<const std::byte> bytes,
                                             bool fsync_dir = false) {
    const std::string tmp = path + ".tmp";
    {
        FilePtr f(fopen_utf8(tmp, "wb"));
        if (!f) return false;
        const bool ok =
            (bytes.empty() ||
             std::fwrite(bytes.data(), 1, bytes.size(), f.get()) ==
                 bytes.size()) &&
            flush_and_sync(f.get());
        f.reset();  // 必须先 close 再 rename
        if (!ok) {
            remove_utf8(tmp);
            return false;
        }
    }
    // S37-1：经 io::atomic_rename 而非 std::rename——后者在 Windows 上
    // 目标已存在即失败，本站点是 9 个原子写站点的公共出口（见 io.hpp）。
    if (!io::atomic_rename(tmp, path)) {
        remove_utf8(tmp);
        return false;
    }
    if (fsync_dir) fsync_parent_dir(path);
    return true;
}

// 流式原子写：内容须逐段 fwrite/fseek 生成（分页 CRC、回头补头等）而无法
// 先攒进一个缓冲区的站点用。
//
//   AtomicFileWriter w(path);
//   if (!w) return false;                 // tmp 开不出来
//   if (std::fwrite(..., w.get()) != n) return false;   // 析构自动清 tmp
//   return w.commit();                    // flush+fdatasync+rename
//
// 未 commit 即析构（含异常路径）→ 自动 remove(tmp)，不留垃圾。
class AtomicFileWriter {
public:
    // tmp_suffix 可定制：残留文件名带诊断信息（如 ".upgrade.tmp" 一眼看出是
    // schema 升级路径崩的，而非普通写）。
    explicit AtomicFileWriter(std::string final_path,
                              const char* tmp_suffix = ".tmp")
        : final_path_(std::move(final_path)),
          tmp_path_(final_path_ + tmp_suffix),
          f_(fopen_utf8(tmp_path_, "wb")) {}

    ~AtomicFileWriter() {
        if (committed_) return;
        f_.reset();  // 先 close 再 remove
        if (!tmp_path_.empty()) remove_utf8(tmp_path_);
    }

    AtomicFileWriter(const AtomicFileWriter&) = delete;
    AtomicFileWriter& operator=(const AtomicFileWriter&) = delete;

    // 可移动（S33-3：OkiRunWriter 按值持有）。源移出后标记 committed_，
    // 其析构不再清理 tmp——所有权完整转移。
    AtomicFileWriter(AtomicFileWriter&& o) noexcept
        : final_path_(std::move(o.final_path_)),
          tmp_path_(std::move(o.tmp_path_)),
          f_(std::move(o.f_)),
          committed_(o.committed_) {
        o.committed_ = true;
    }
    AtomicFileWriter& operator=(AtomicFileWriter&& o) noexcept {
        if (this != &o) {
            if (!committed_) {
                f_.reset();
                if (!tmp_path_.empty()) remove_utf8(tmp_path_);
            }
            final_path_ = std::move(o.final_path_);
            tmp_path_   = std::move(o.tmp_path_);
            f_          = std::move(o.f_);
            committed_  = o.committed_;
            o.committed_ = true;
        }
        return *this;
    }

    explicit operator bool() const noexcept { return f_ != nullptr; }
    [[nodiscard]] std::FILE* get() const noexcept { return f_.get(); }

    // flush + fdatasync + rename。成功后析构不再清理 tmp。
    [[nodiscard]] bool commit(bool fsync_dir = false) {
        if (!f_) return false;
        const bool synced = flush_and_sync(f_.get());
        f_.reset();  // 必须先 close 再 rename
        if (!synced) {
            remove_utf8(tmp_path_);
            return false;
        }
        if (!io::atomic_rename(tmp_path_, final_path_)) {  // S37-1，见上
            remove_utf8(tmp_path_);
            return false;
        }
        committed_ = true;  // 析构不再清理
        if (fsync_dir) fsync_parent_dir(final_path_);
        return true;
    }

private:
    std::string final_path_;
    std::string tmp_path_;
    FilePtr     f_;
    bool        committed_ = false;
};

}  // namespace bitcask::detail
