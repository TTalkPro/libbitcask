// bitcask 磁盘格式常量（向量库 typed record）。
//
// 这里的所有数字、字段顺序都是「磁盘契约」的一部分，改一处就是
// binary-incompatible 变更，必须同步更新黄金测试
// （cpp/tests/codec_test.cpp、data_file_test.cpp 里有跟二进制 fixture 的
// 字节级比对）。设计见 doc/vector-db-design-zh.md §2。
//
// === 线程模型 ===
// 全部为 inline constexpr 常量 + enum。
//   - 可重入 / 线程安全：是（无可变状态）。
//   - 锁要求：无。

#pragma once

#include <cstddef>
#include <cstdint>

namespace bitcask::format {

// ---------------------------------------------------------------------------
// 数据文件 record 布局（向量库 typed record，V2：Tstamp 扩为 u64）：
//   [0..3]   CRC32       (覆盖 Type..Value 区段，即 [4..] 全部)
//   [4]      Type        u8   (RecordType：kDoc / kTombstone)
//   [5..12]  Tstamp      u64 小端 (绝对 unix 秒；u32 时代上限 2106，flag-day 扩宽)
//   [13..20] Ord         u64 小端 (引擎单调分配的写入序号，per-write，永不复用)
//   [21..22] KeySz       u16 小端 (key == ext_id)
//   [23..26] ValueSz     u32 小端 (kDoc 时是打包 value；kTombstone 时通常为 0)
//   [27..]   Key | Value
// 总长 = kHeaderSize + KeySz + ValueSz
//
// 设计依据见 doc/vector-db-design-zh.md §2.2。CRC 覆盖范围从 Type 开始
// （含 ord），而非 legacy 的 Tstamp 起。
// 字节序：P 起全盘统一小端(LE-only 主机，原生零转换 + mmap 零拷贝)。
// flag-day 切换先例：大端旧文件不可读(需重建)；Tstamp u32→u64 同为
// flag-day（meta v4 门禁，旧库干净拒开），见 doc/format-zh.md。
// ---------------------------------------------------------------------------
inline constexpr std::size_t kHeaderSize = 27;  // 4 + 1 + 8 + 8 + 2 + 4
inline constexpr std::size_t kCrcOffset = 0;
inline constexpr std::size_t kTypeOffset = 4;
inline constexpr std::size_t kTstampOffset = 5;
inline constexpr std::size_t kOrdOffset = 13;
inline constexpr std::size_t kKeySzOffset = 21;
inline constexpr std::size_t kValueSzOffset = 23;

inline constexpr std::uint16_t kMaxKeySize = 0xFFFF;          // 16-bit 字段上限
inline constexpr std::uint32_t kMaxValueSize = 0xFFFF'FFFFu;  // 32-bit 字段上限

// record 类型（Type 字段，u8）。墓碑不再靠 value 魔法串识别，而是一等 record 类型。
enum class RecordType : std::uint8_t {
    kDoc       = 0,  // 一条文档：value 是 §2.4 打包的 {vector,text,meta}
    kTombstone = 1,  // 删除标记：value 通常为空，target 由 Key=ext_id + Ord 确定
    // S35：原子批批头（doc/atomic-batch-design-zh.md）。key 为空，value =
    // 批头布局（见下方 kBatchHeader* 常量）声明「其后 count 条、共
    // span_bytes 字节」为一个原子批；成员是普通 kDoc/kTombstone 记录。
    // 声明区间完整且逐条 CRC 有效 ⟺ 批已提交；否则 fold 的
    // last_valid_end 停在批头起点（整批不可见，恢复截断）。
    // 批头永不进入 keydir/hint。含此类型的目录 meta ≥ v6（懒升级门禁，
    // 旧读端对未知 type 盲转会误读，必须拒开）。
    kBatchHeader = 2,
};

// ---------------------------------------------------------------------------
// S35 批头 value 布局（写在 kBatchHeader record 的 VALUE 段，全小端）：
//   [0]      Ver         u8   (= kBatchHeaderVersion)
//   [1..4]   Count       u32  (成员条数，≥1)
//   [5..12]  SpanBytes   u64  (成员区间总字节：批头 record 之后紧邻的
//                              count 条完整 record 的 total_size 之和)
// ---------------------------------------------------------------------------
inline constexpr std::uint8_t kBatchHeaderVersion   = 1;
inline constexpr std::size_t  kBatchHeaderValueSize = 13;  // 1 + 4 + 8

// ---------------------------------------------------------------------------
// hint 文件 v5 布局（S33 flag-day：v4 变长格式 + 记录内嵌 ord）：
//   [0..3]           magic "BCH5"
//   记录流（变长）    [vbyte gap][vbyte total_sz][vbyte keysz<<1|tomb]
//                    [vbyte ord_delta][tstamp u64 小端][key]
//   [size-8..size-1] trailer: magic "BCHE" u32 + running_crc u32
//                    （CRC 覆盖 [0, size-8)，含文件头与全部记录字节）
// gap = offset − prev_end（prev_end = 上条 offset+total_sz，首条为 0）；
// ord_delta = ord − prev_ord（prev_ord = 上条 ord，首条为 0）。二者均经
// u64 二补数回绕无损还原，正确性**不依赖**单调性/连续性假设；正常 append
// 序 gap==0（1 字节）、ord 递增（ord_delta 1-2 字节），典型记录 ~13-15B。
// v5 使 hint 快路径恢复与 fold(data) 完全等价（ord 不再丢失，v4 时代 hint
// 路径 ord 恒 0 的怪癖随之消除），也是 OKI（S33）tail 重放的前提。
// 兼容：写端恒 v5；读端仅 v5——BCH4 及更早视作校验失败退 fold(data)
// 重建（hint 是派生缓存），纪元硬门禁在 bitcask.meta v5（meta_file.cpp）。
// 设计见 doc/ordered-key-index-design-zh.md §3.4。
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kHintMagicV5     = 0x35484342;  // "BCH5" LE
inline constexpr std::uint32_t kHintMagicV4     = 0x34484342;  // "BCH4" LE（仅旧纪元识别/拒收）
inline constexpr std::uint32_t kHintTrailerMagic = 0x45484342;  // "BCHE" LE
inline constexpr std::size_t   kHintHeader       = 4;
inline constexpr std::size_t   kHintTrailer      = 8;

// ---------------------------------------------------------------------------
// kDoc value 打包布局（写在 kDoc record 的 VALUE 段）。设计见 §2.4。
//
// DocValue 格式为本项目自定义格式（无公开规范），但设计灵感来源于：
//   - Apache Lucene 的 stored fields 格式（字段值紧凑打包）
//   - Tantivy 的 field value 编码（varint 长度前缀 + 字段值）
//   核心思路：按 Flags 分段、varint 压缩长度、向量段靠前便于 HNSW 重建切片。
//   [0]      Ver         u8   (布局版本号，当前 = kDocValueVersion = 4)
//   [1]      Flags       u8   (见下方 kFlag* 位)
//   [可选] vector 段：  [Dim:varint][ f32×Dim 小端  或  量化码字 ]
//   [可选] text   段：  [Len:varint][ utf8 字节 ]
//   [可选] meta   段：  [Len:varint][ 序列化字节(msgpack/CBOR) ]
//   [可选] fields 段：  [FieldCount:varint] × { [FieldId:varint][ValLen:varint][value] }
// 各段按 vector→text→meta→fields 定序出现，由 Flags 决定是否存在（向量段放
// 最前，便于 HNSW 重建按 Dim O(1) 切片）。
//
// 长度/计数全部用 VByte 变长（#2，省小字段的固定 4B 前缀）；向量 f32 数组固定
// 小端（x86/ARM64 原生零转换，见 §2.4）。
//
// fields 段存 FieldId（u32 的 varint）而非字段名（#1，schema interning）：
// 字段名 ↔ id 映射由 Cask 的 append-only field.schema 注册表维护，避免每条
// record 重复内联字段名。decode 是纯函数、只还原 id，由上层用 schema 译回名字。
//
// 版本：v3 统一格式（不再有 v1/v2 的 fields 区分，fields 仅由 Flags 标记）；
// v4 = v3 + ExpiryAt 段 u32→u64（tstamp 64 位 flag-day）。
// 项目不考虑向后兼容；decode 只接受 Ver==4。
// ---------------------------------------------------------------------------
inline constexpr std::uint8_t kDocValueVersion    = 4;  // v4：ExpiryAt u64
inline constexpr std::size_t  kDocValueHeaderSize = 2;  // Ver + Flags

inline constexpr std::uint8_t kFlagHasVector    = 0x01;
inline constexpr std::uint8_t kFlagHasText      = 0x02;
inline constexpr std::uint8_t kFlagHasMeta      = 0x04;
inline constexpr std::uint8_t kFlagVecQuantized = 0x08;
inline constexpr std::uint8_t kFlagHasFields    = 0x10;  // fields 段存在（S8.6）
// S13-D5：per-key TTL。置位时 value 末尾追加 [ExpiryAt:u64 LE]（绝对 unix 秒，
// 恒非 0；v4 起 u64）。段追加在既有全部段之后 ⟹ 旧读端（不识别本位）按位
// 忽略、跳过尾部字节——旧库读带 TTL 的记录 = 永不过期（静默降级，非拒绝）。
inline constexpr std::uint8_t kFlagHasExpiry    = 0x20;

// P3a 量化向量码字（kFlagVecQuantized 段，per-vector 对称 int8）。布局：
//   [Dim:varint 元素数][SchemeVer:u8][scale:f32 小端][int8 × Dim]
// 重建 v̂[i] = codes[i] * scale / 127（见 detail/int8_kernels.hpp）。大小
// = varint(Dim) + 1 + 4 + Dim，≈ f32 的 1/4（Dim 大时）。SchemeVer=1=对称 int8；
// 未来 affine 等新方案 bump 此版本（读端按版本分发，旧端见未知版本拒绝）。
inline constexpr std::uint8_t kQuantizedVersion = 1;

// ---------------------------------------------------------------------------
// hint 文件的 CRC chunk 大小（解析时做合理性边界检查）。
// hint 末尾 EOF sentinel 的 TotalSz 字段实际放的是 running CRC，参见
// codec.cpp::encode_hint_eof / decode_hint_record。
// ---------------------------------------------------------------------------
inline constexpr std::size_t kChunkSize = 65535;
inline constexpr std::size_t kMinChunkSize = 1024;
inline constexpr std::size_t kMaxChunkSize = 134217728;

}  // namespace bitcask::format
