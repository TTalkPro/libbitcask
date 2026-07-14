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
// 后续可在此扩展 read_file_bytes / atomic_write_bytes 等公共文件样板（RED-2
// 余下 ~6 份整读样板 + ~7 份原子写样板的归并）。

#pragma once

#include <cstdio>
#include <memory>

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

}  // namespace bitcask::detail
