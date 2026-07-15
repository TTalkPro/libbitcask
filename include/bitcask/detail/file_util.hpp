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

#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>   // T21: fsync_parent_dir 的 O_DIRECTORY
#include <unistd.h>  // T21: fdatasync / fsync

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
template <class Byte = std::byte>
[[nodiscard]] std::optional<std::vector<Byte>>
read_file_bytes(const std::string& path) {
    static_assert(sizeof(Byte) == 1, "read_file_bytes: Byte 须为单字节类型");
    FilePtr f(std::fopen(path.c_str(), "rb"));
    if (!f) return std::nullopt;
    if (std::fseek(f.get(), 0, SEEK_END) != 0) return std::nullopt;
    const long sz = std::ftell(f.get());
    if (sz < 0) return std::nullopt;
    if (std::fseek(f.get(), 0, SEEK_SET) != 0) return std::nullopt;
    // sz 来自可能损坏的文件（可为巨值）：vector 分配可抛 bad_alloc，
    // FilePtr 保证 fd 不泄漏（MEM-LOW-1 同源）。
    std::vector<Byte> buf(static_cast<std::size_t>(sz));
    if (!buf.empty() &&
        std::fread(buf.data(), 1, buf.size(), f.get()) != buf.size()) {
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
inline void fsync_parent_dir(const std::string& path) noexcept {
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) parent = ".";
    const int dfd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
}

// 已开的 tmp 文件：flush + fdatasync。两个返回值都检查（见上）。
inline bool flush_and_sync(std::FILE* f) noexcept {
    return std::fflush(f) == 0 && ::fdatasync(::fileno(f)) == 0;
}

// 缓冲区一次性原子落盘。失败即 remove(tmp) 并返回 false——最终路径始终
// 保持原样（要么旧内容，要么新内容，不会是半截）。
[[nodiscard]] inline bool atomic_write_bytes(const std::string& path,
                                             std::span<const std::byte> bytes,
                                             bool fsync_dir = false) {
    const std::string tmp = path + ".tmp";
    {
        FilePtr f(std::fopen(tmp.c_str(), "wb"));
        if (!f) return false;
        const bool ok =
            (bytes.empty() ||
             std::fwrite(bytes.data(), 1, bytes.size(), f.get()) ==
                 bytes.size()) &&
            flush_and_sync(f.get());
        f.reset();  // 必须先 close 再 rename
        if (!ok) {
            std::remove(tmp.c_str());
            return false;
        }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
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
          f_(std::fopen(tmp_path_.c_str(), "wb")) {}

    ~AtomicFileWriter() {
        if (committed_) return;
        f_.reset();  // 先 close 再 remove
        if (!tmp_path_.empty()) std::remove(tmp_path_.c_str());
    }

    AtomicFileWriter(const AtomicFileWriter&) = delete;
    AtomicFileWriter& operator=(const AtomicFileWriter&) = delete;

    explicit operator bool() const noexcept { return f_ != nullptr; }
    [[nodiscard]] std::FILE* get() const noexcept { return f_.get(); }

    // flush + fdatasync + rename。成功后析构不再清理 tmp。
    [[nodiscard]] bool commit(bool fsync_dir = false) {
        if (!f_) return false;
        const bool synced = flush_and_sync(f_.get());
        f_.reset();  // 必须先 close 再 rename
        if (!synced) {
            std::remove(tmp_path_.c_str());
            return false;
        }
        if (std::rename(tmp_path_.c_str(), final_path_.c_str()) != 0) {
            std::remove(tmp_path_.c_str());
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
