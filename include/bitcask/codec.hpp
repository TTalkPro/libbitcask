// bitcask data 文件 / hint 文件 record 的编解码。
//
// 纯函数：只在 caller 提供的 byte buffer 上做事，不做 I/O，不分配除
// caller 容器之外的内存。磁盘格式定义见 format.hpp。
//
// 错误用 std::expected 返回（kBufferTooShort / kBadCrc / 字段越界），
// 不抛异常——所有上层调用方需要在错误路径下做出选择（截断 / 拒绝整文件 /
// 跳过当前 record 等）。
//
// === 线程模型 ===
// 本模块所有函数均为纯函数：
//   - 可重入 / 线程安全：是。多线程可在不同 buffer 上并发调用。
//   - 锁要求：无。caller 自行保证「同一 buffer 在不同线程被并发改写」
//     不会发生（标准 const-correct 约定即可）。
//   - 不抛异常、不分配额外堆内存（除 encode_* 往 caller 的 vector 里 push）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "bitcask/format.hpp"  // RecordType + kDoc value 打包常量

namespace bitcask::codec {

enum class DecodeError {
    kBufferTooShort,      // 输入不够长，连 header 都读不全
    kBadCrc,              // CRC32 校验失败（数据损坏 / 写入到一半被 kill）
    kKeySizeOverflow,     // KeySz/ValueSz 字段读出来后跟 buffer 实际长度不符
    kUnsupportedVersion,  // kDoc value 的 Ver 字段不被支持
};

// 解码后的 data record 视图。key/value 是 zero-copy span，生命周期跟着
// 输入 buf 走——caller 持有 buf 的时候才能用 view。
struct DataRecordView {
    std::uint32_t      crc;
    format::RecordType type;
    std::uint64_t      tstamp;
    std::uint64_t      ord;
    std::span<const std::byte> key;
    std::span<const std::byte> value;
    std::size_t total_size;  // kHeaderSize + key.size() + value.size()
};

// 解码后的 hint record。consumed 是消耗的字节数，caller 用它推进 buf
// 指针读下一条。
struct HintRecord {
    std::uint64_t tstamp;   // 盘上 u64（v4 起）
    std::uint32_t total_sz;
    std::uint64_t offset;
    std::uint64_t ord = 0;  // S33 v5 起盘上有（vbyte delta）；见 format.hpp
    bool tombstone;
    std::span<const std::byte> key;
    std::size_t consumed;
};

// ---------------------------------------------------------------------------
// data 文件 record
// ---------------------------------------------------------------------------

// 编码一条 data record，append 到 out 末尾。返回写入字节数
// （== kHeaderSize + key.size() + value.size()）。
// CRC 在内部算好填到前 4 字节（覆盖 Type..Value）。
// 线程安全: 是（纯函数，但写入 out 由 caller 串行保证）；不需任何锁。
std::size_t encode_data_record(std::vector<std::byte>& out,
                               format::RecordType type,
                               std::uint64_t tstamp,
                               std::uint64_t ord,
                               std::span<const std::byte> key,
                               std::span<const std::byte> value);

// S29-7 铺垫：把已编码 record 的 Ord 字段改写为 ord，并重算 CRC。
// 用途：写路径把 O(V) 的 record 编码（memcpy key/value + header）移出
// write_mu_——锁外用占位 ord 预编码，锁内 alloc_ord 后仅 patch 8 字节 +
// 一次 CRC 扫描（文件序 == ord 序的恢复不变量要求 ord 必须锁内分配）。
// record 必须是一条完整的 encode_data_record 产物（长度 ≥ kHeaderSize）。
// 线程安全: 是（只写 caller 的 record）；不需任何锁。
void patch_data_record_ord(std::span<std::byte> record, std::uint64_t ord);

// 从 buf 头部读一条 data record。CRC 会校验；不通过返回 kBadCrc。
// 不修改 buf；caller 用 result.total_size 自己 advance。
// 线程安全: 是（纯函数，只读 buf）；不需任何锁。
[[nodiscard]] std::expected<DataRecordView, DecodeError>
decode_data_record(std::span<const std::byte> buf);

// ---------------------------------------------------------------------------
// kDoc value 打包/解包（§2.4）。仅用于 type==kDoc 的 record 的 VALUE 段。
// ---------------------------------------------------------------------------

// 命名字段（S8.6 多字段）。#1：磁盘上存 field id（schema interning），不再内联
// 字段名。encode 输入 / decode 输出都用 id；name ↔ id 由 Cask 的 field.schema 维护。
// value 是 zero-copy span（指进原 buffer）。
struct DocField {
    std::uint32_t              id = 0;
    std::span<const std::byte> value;
};

// encode 输入：三段皆可选（nullopt = 该段缺省，不写 flag）。vector 是 f32
// 向量（V1 不量化）。fields 非空时写 fields 段（DocField.id 已由 caller 经 schema 解析）。
struct DocValueParts {
    std::optional<std::span<const float>>      vector;
    std::optional<std::span<const std::byte>>  text;
    std::optional<std::span<const std::byte>>  meta;
    std::vector<DocField>                      fields;  // 空 = 不写 fields 段
    bool                                      vec_quantized = false;  // V6.4.1 stub
    // S13-D5：per-key 过期时刻（绝对 unix 秒；0 = 永不过期，不写段）。v4 起 u64。
    std::uint64_t                             expiry_at = 0;
};

// 解码后的 kDoc value 视图。各段是 zero-copy span，生命周期跟着输入 buf。
// vector_raw 是原始字节（f32 小端，未量化时长度 == dim*4）；caller 在 LE 主机
// 上可直接 memcpy 成 float[]。
struct DocValueView {
    std::uint8_t ver;
    bool has_vector = false;
    bool has_text   = false;
    bool has_meta   = false;
    bool has_fields = false;                // S8.6
    bool vec_quantized = false;
    std::uint32_t dim = 0;                  // 向量元素数（has_vector 或 vec_quantized 有效）
    // vector_raw 语义随 vec_quantized 而变：
    //   未量化：f32 小端字节，长度 == dim*4，LE 主机可直接 memcpy 成 float[]。
    //   量化：  int8 codes 字节，长度 == dim；配合 vec_scale 用 doc_vector_f32() 还原。
    std::span<const std::byte> vector_raw;
    float vec_scale = 0.0f;                 // 仅 vec_quantized 有效（重建标度）
    std::span<const std::byte> text;
    std::span<const std::byte> meta;
    std::vector<DocField>      fields;      // 解出的字段（id + zero-copy value span）
    std::uint64_t expiry_at = 0;            // S13-D5：0 = 无 per-key TTL（v4 起 u64）
};

// 把 {vector,text,meta} 打包成 kDoc value，append 到 out。返回写入字节数。
// 线程安全: 是（纯函数）；不需任何锁。
std::size_t encode_doc_value(std::vector<std::byte>& out, const DocValueParts& parts);

// 解包 kDoc value。Ver 不支持返回 kUnsupportedVersion；截断返回 kBufferTooShort。
// 线程安全: 是（纯函数，只读 buf）；不需任何锁。
[[nodiscard]] std::expected<DocValueView, DecodeError>
decode_doc_value(std::span<const std::byte> buf);

// 把 DocValueView 的向量段还原成 f32：未量化直接 memcpy，量化则 dequant
// （v̂ = code*scale/127）。无向量段 → 返回空。给 get / 非 int8 路径用；HNSW
// int8 路径直接吃 vector_raw(codes)+vec_scale，不走这里。纯函数。
[[nodiscard]] std::vector<float> doc_vector_f32(const DocValueView& v);

// ---------------------------------------------------------------------------
// hint 文件 record
// ---------------------------------------------------------------------------

// ---- hint v5（S33 flag-day：v4 变长编码 + vbyte ord_delta；文件级布局见
// format.hpp）。v2 定宽 18B 与 v4 编解码已随 flag-day 整体退役——库内不再
// 有任何旧格式 hint 读端（BCH4 在 HintFile 层按校验失败退 fold(data)）。----
// 编码一条 v5 记录（append 进 out）。prev_end/prev_ord 是差分串联状态，
// 传入并在返回时更新（prev_end ← offset+total_sz，prev_ord ← ord）。
// 线程安全: 是（纯函数，串联状态由 caller 持有）；不需任何锁。
void encode_hint_record_v5(std::vector<std::byte>& out,
                           std::uint64_t tstamp,
                           std::uint32_t total_sz,
                           std::uint64_t offset, bool tombstone,
                           std::span<const std::byte> key,
                           std::uint64_t ord,
                           std::uint64_t& prev_end,
                           std::uint64_t& prev_ord);
// 从 buf 头部解一条 v5 记录；prev_end/prev_ord 传入并在成功时更新。字节
// 不足返回 kBufferTooShort（流式 caller 据此 refill 重试）。
[[nodiscard]] std::expected<HintRecord, DecodeError>
decode_hint_record_v5(std::span<const std::byte> buf,
                      std::uint64_t& prev_end, std::uint64_t& prev_ord);

// ---------------------------------------------------------------------------
// CRC32 (zlib / IEEE 802.3 多项式，跟 erlang:crc32/1 一致)
// 全部为纯函数，线程安全、可重入，不需任何锁。
// ---------------------------------------------------------------------------

// 一次性算一段。
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> data) noexcept;

// 流式：seed 是上一次的 CRC，data 是新增的字节。hint 文件 running CRC 用。
[[nodiscard]] std::uint32_t crc32_update(std::uint32_t seed,
                                         std::span<const std::byte> data) noexcept;

}  // namespace bitcask::codec
