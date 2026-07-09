// LSN / DocId 概念别名（S27-1，分段索引地基）。
//
// 见 doc/segment-index-design-zh.md §2.1、§3.4；doc/ord-recycling-design-zh.md §4。
//
// 当前 `ord` 一个 std::uint64_t 兼两个角色。分段索引要把二者拆开——本头先在
// **接口层**把角色显式化（弱别名，二者仍同型、数值相等、零行为变更），为后续
// 「DocId 段内本地化 + merge 重编码」把耦合点标出来，避免届时盲扫 ord 不变量。

#pragma once

#include <cstdint>

namespace bitcask {

// LSN（Log Sequence Number）：全局单调递增的写入序列号（`alloc_ord()` 产出）。
// 承担——恢复重放定序、MVCC「后写胜」、幂等水位（`max_indexed_ord_`）、
// （未来）复制 seq_no。**永不回收、跨 merge 不变**。
using Lsn = std::uint64_t;

// DocId：搜索索引的文档序号——posting list 的存储值、docmap/HNSW 的数组下标。
// **当前 == Lsn**（docid==lsn==ord，单一全局索引下二者数值恒等）。
// 分段化后将变为「段内本地、dense、merge 可重编码」——那时才与 Lsn 真正发散。
// 现阶段本别名只在接口上标注角色，不改任何行为或存储。
using DocId = std::uint64_t;

}  // namespace bitcask
