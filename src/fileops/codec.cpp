#include "bitcask/codec.hpp"

#include <bit>
#include <cassert>
#include <cstring>

#include "bitcask/byte_order.hpp"  // LE load/store（P0-d 提取共享）
#include "bitcask/detail/int8_kernels.hpp"  // P3a：向量落盘 int8 对称量化
#include "bitcask/format.hpp"
#include "bitcask/hw_crc32.hpp"
#include "bitcask/vbyte.hpp"  // S9-P1-b：vbyte_encode 模板（消除本地 vbyte_append）

namespace bitcask::codec {

// 一次性算 CRC——薄包装 hw::crc32_update(0, ...)。
std::uint32_t crc32(std::span<const std::byte> data) noexcept {
    return crc32_update(0, data);
}

// 流式 CRC：seed 是上一段的结果，可以多段累计。hint 文件的 trailer CRC
// 就靠这个边写边累加。CRC32 有 incremental 性质——seed 起始值是 0，append
// 一段后的 CRC 跟「整段一次性算」结果一致。
//
// 实际计算交给 bitcask::hw::crc32_update（PCLMULQDQ 硬件加速 + zlib 兜底），
// 输出与 zlib::crc32() bit-identical，确保现有 data file / hint file 的
// on-disk CRC 与历史数据兼容。
std::uint32_t crc32_update(std::uint32_t seed,
                           std::span<const std::byte> data) noexcept {
    return bitcask::hw::crc32_update(seed, data);
}

// ---------------------------------------------------------------------------
// data record 编解码
// ---------------------------------------------------------------------------

// 把一条 data record append 到 out 末尾。返回写入字节数。
// caller 用 assert 兜底大小限制——超出 uint16/uint32 字段范围是 caller bug。
std::size_t encode_data_record(std::vector<std::byte>& out,
                               format::RecordType type,
                               std::uint64_t tstamp,
                               std::uint64_t ord,
                               std::span<const std::byte> key,
                               std::span<const std::byte> value) {
    assert(key.size() <= format::kMaxKeySize);
    assert(value.size() <= format::kMaxValueSize);

    // resize 一次性预留空间：避免 push_back 多次 realloc。
    const std::size_t total = format::kHeaderSize + key.size() + value.size();
    const std::size_t base = out.size();
    out.resize(base + total);
    std::byte* p = out.data() + base;

    // 布局: [CRC|Type|Tstamp|Ord|KeySz|ValueSz|Key|Value]。
    // CRC 覆盖它自身之后的全部字节（即 Type..Value）。
    p[format::kTypeOffset] = static_cast<std::byte>(type);
    le_store_u64(p + format::kTstampOffset, tstamp);
    le_store_u64(p + format::kOrdOffset, ord);
    le_store_u16(p + format::kKeySzOffset, static_cast<std::uint16_t>(key.size()));
    le_store_u32(p + format::kValueSzOffset, static_cast<std::uint32_t>(value.size()));
    if (!key.empty()) std::memcpy(p + format::kHeaderSize, key.data(), key.size());
    if (!value.empty()) std::memcpy(p + format::kHeaderSize + key.size(),
                                    value.data(), value.size());

    const std::span<const std::byte> covered{p + format::kTypeOffset,
                                              total - format::kTypeOffset};
    le_store_u32(p + format::kCrcOffset, crc32(covered));
    return total;
}

// S29-7 铺垫：改写已编码 record 的 Ord 字段并重算 CRC（布局同上）。
void patch_data_record_ord(std::span<std::byte> record, std::uint64_t ord) {
    assert(record.size() >= format::kHeaderSize);
    std::byte* p = record.data();
    le_store_u64(p + format::kOrdOffset, ord);
    const std::span<const std::byte> covered{p + format::kTypeOffset,
                                              record.size() - format::kTypeOffset};
    le_store_u32(p + format::kCrcOffset, crc32(covered));
}

// 从 buf 头部解一条 data record，校验 CRC 后返回 view（zero-copy）。
// 不修改 buf；caller 负责用 result.total_size 推进自己的指针。
std::expected<DataRecordView, DecodeError>
decode_data_record(std::span<const std::byte> buf) {
    // 第一道关：连固定 header 都读不全 → kBufferTooShort
    if (buf.size() < format::kHeaderSize) {
        return std::unexpected(DecodeError::kBufferTooShort);
    }
    const std::uint32_t crc      = le_load_u32(buf.data() + format::kCrcOffset);
    const auto          type     = static_cast<format::RecordType>(
                                       buf.data()[format::kTypeOffset]);
    const std::uint64_t tstamp   = le_load_u64(buf.data() + format::kTstampOffset);
    const std::uint64_t ord      = le_load_u64(buf.data() + format::kOrdOffset);
    const std::uint16_t key_sz   = le_load_u16(buf.data() + format::kKeySzOffset);
    const std::uint32_t value_sz = le_load_u32(buf.data() + format::kValueSzOffset);

    // 第二道关：header 说有 N 字节 body 但 buf 不够长 → kBufferTooShort
    const std::size_t total = format::kHeaderSize + key_sz + value_sz;
    if (buf.size() < total) {
        return std::unexpected(DecodeError::kBufferTooShort);
    }

    const std::span<const std::byte> covered{
        buf.data() + format::kTypeOffset, total - format::kTypeOffset};
    if (crc32(covered) != crc) {
        return std::unexpected(DecodeError::kBadCrc);
    }

    return DataRecordView{
        .crc = crc,
        .type = type,
        .tstamp = tstamp,
        .ord = ord,
        .key = buf.subspan(format::kHeaderSize, key_sz),
        .value = buf.subspan(format::kHeaderSize + key_sz, value_sz),
        .total_size = total,
    };
}

// ---------------------------------------------------------------------------
// kDoc value 打包/解包（§2.4）
// ---------------------------------------------------------------------------

// 向量 f32 数组固定小端存储。V1 只目标 LE 主机（x86/ARM64），此处直接 memcpy；
// BE 主机会在这里编译失败而非静默写出错误字节序。
static_assert(std::endian::native == std::endian::little,
              "kDoc value 的 f32 向量按小端存储，当前仅支持小端主机");

std::size_t encode_doc_value(std::vector<std::byte>& out, const DocValueParts& parts) {
    const std::size_t base = out.size();

    const bool has_fields = !parts.fields.empty();

    std::uint8_t flags = 0;
    if (parts.vec_quantized && parts.vector) {
        flags |= format::kFlagVecQuantized;
    } else if (parts.vector) {
        flags |= format::kFlagHasVector;
    }
    if (parts.text)   flags |= format::kFlagHasText;
    if (parts.meta)   flags |= format::kFlagHasMeta;
    if (has_fields)   flags |= format::kFlagHasFields;
    if (parts.expiry_at != 0) flags |= format::kFlagHasExpiry;  // S13-D5

    out.push_back(static_cast<std::byte>(format::kDocValueVersion));  // v3：统一版本
    out.push_back(static_cast<std::byte>(flags));

    // 追加「varint 长度前缀 + 原始字节」的段（text/meta/字段值用，#2）。
    auto append_bytes = [&out](std::span<const std::byte> s) {
        vbyte_encode(s.size(), out);
        const std::size_t at = out.size();
        out.resize(at + s.size());
        if (!s.empty()) std::memcpy(out.data() + at, s.data(), s.size());
    };

    if (parts.vec_quantized && parts.vector) {
        // P3a：per-vector 对称 int8。[Dim:varint][SchemeVer:u8][scale:f32 LE][int8×Dim]
        const auto& v = *parts.vector;
        const auto qv = vec::int8::quantize(v.data(), v.size());
        vbyte_encode(v.size(), out);  // Dim（元素数）
        out.push_back(static_cast<std::byte>(format::kQuantizedVersion));
        out.resize(out.size() + sizeof(float));  // scale f32 LE
        std::memcpy(out.data() + out.size() - sizeof(float), &qv.scale, sizeof(float));
        const std::size_t at = out.size();       // int8 codes
        out.resize(at + qv.codes.size());
        if (!qv.codes.empty()) {
            std::memcpy(out.data() + at, qv.codes.data(), qv.codes.size());
        }
    } else if (parts.vector) {
        const auto& v = *parts.vector;
        vbyte_encode(v.size(), out);  // Dim（元素个数）
        const std::size_t at = out.size();
        const std::size_t bytes = v.size() * sizeof(float);
        out.resize(at + bytes);
        if (bytes) std::memcpy(out.data() + at, v.data(), bytes);  // f32 LE
    }
    if (parts.text) append_bytes(*parts.text);
    if (parts.meta) append_bytes(*parts.meta);
    // fields 段（#1）：[FieldCount:varint] × { [FieldId:varint][ValLen:varint][value] }
    if (has_fields) {
        vbyte_encode(parts.fields.size(), out);
        for (const auto& f : parts.fields) {
            vbyte_encode(f.id, out);
            append_bytes(f.value);
        }
    }
    // S13-D5：expiry 段固定追加在最后（旧读端按位忽略尾部字节）。v4：u64 LE。
    if (parts.expiry_at != 0) {
        const std::size_t at = out.size();
        out.resize(at + sizeof(std::uint64_t));
        std::memcpy(out.data() + at, &parts.expiry_at, sizeof(std::uint64_t));
    }
    return out.size() - base;
}

std::expected<DocValueView, DecodeError>
decode_doc_value(std::span<const std::byte> buf) {
    if (buf.size() < format::kDocValueHeaderSize) {
        return std::unexpected(DecodeError::kBufferTooShort);
    }
    const std::uint8_t ver   = static_cast<std::uint8_t>(buf[0]);
    const std::uint8_t flags = static_cast<std::uint8_t>(buf[1]);
    // v3 统一格式；不考虑向后兼容，只接受当前版本。
    if (ver != format::kDocValueVersion) {
        return std::unexpected(DecodeError::kUnsupportedVersion);
    }

    DocValueView v{};
    v.ver           = ver;
    v.has_vector    = (flags & format::kFlagHasVector) != 0;
    v.has_text      = (flags & format::kFlagHasText) != 0;
    v.has_meta      = (flags & format::kFlagHasMeta) != 0;
    v.has_fields    = (flags & format::kFlagHasFields) != 0;
    v.vec_quantized = (flags & format::kFlagVecQuantized) != 0;

    std::size_t pos = format::kDocValueHeaderSize;

    // 读一个 [varint 字节长度][payload] 段（text/meta/字段值用），推进 pos。
    auto read_bytes_section =
        [&buf, &pos](std::span<const std::byte>& out_span) -> bool {
        auto vr = vbyte_read_checked(buf, pos);
        if (!vr) return false;
        const auto len = vr->first;
        pos = vr->second;
        if (len > buf.size() - pos) return false;  // S25-M3:减法避溢出
        out_span = buf.subspan(pos, len);
        pos += len;
        return true;
    };

    if (v.vec_quantized) {
        // P3a：[Dim:varint][SchemeVer:u8][scale:f32 LE][int8×Dim]。vector_raw =
        // int8 codes（零拷贝），配 vec_scale 用 doc_vector_f32() 还原。
        auto dim_vr = vbyte_read_checked(buf, pos);
        if (!dim_vr) {
            return std::unexpected(DecodeError::kBufferTooShort);
        }
        pos = dim_vr->second;
        const auto dim = dim_vr->first;
        // S25-M3:need = 1 + sizeof(float) + dim。dim 来自不可信 vbyte，
        // 可能大到令 need 溢出。改用减法：dim + 5 必须 ≤ 剩余字节。
        constexpr std::size_t kQuantOverhead = 1 + sizeof(float);
        if (dim > buf.size() - pos || kQuantOverhead > buf.size() - pos - dim) {
            return std::unexpected(DecodeError::kBufferTooShort);
        }
        const std::uint8_t scheme = static_cast<std::uint8_t>(buf[pos]);
        pos += 1;
        if (scheme != format::kQuantizedVersion) {
            return std::unexpected(DecodeError::kUnsupportedVersion);
        }
        std::memcpy(&v.vec_scale, buf.data() + pos, sizeof(float));
        pos += sizeof(float);
        v.dim = static_cast<std::uint32_t>(dim);
        v.vector_raw = buf.subspan(pos, static_cast<std::size_t>(dim));  // int8 codes
        pos += static_cast<std::size_t>(dim);
    }
    if (v.has_vector) {
        // vector 段：[Dim:varint 元素个数][f32×Dim 小端]。Dim 是元素数、非字节数。
        auto dim_vr = vbyte_read_checked(buf, pos);
        if (!dim_vr) {
            return std::unexpected(DecodeError::kBufferTooShort);
        }
        pos = dim_vr->second;
        const auto dim = dim_vr->first;
        // S25-M3:dim * sizeof(float) 可乘法溢出。改用除法：dim 不得超过
        // 剩余字节 / sizeof(float)。
        if (dim > (buf.size() - pos) / sizeof(float)) {
            return std::unexpected(DecodeError::kBufferTooShort);
        }
        const std::size_t bytes = static_cast<std::size_t>(dim) * sizeof(float);
        if (buf.size() < pos + bytes) {
            return std::unexpected(DecodeError::kBufferTooShort);
        }
        v.dim = static_cast<std::uint32_t>(dim);
        v.vector_raw = buf.subspan(pos, bytes);
        pos += bytes;
    }
    if (v.has_text && !read_bytes_section(v.text)) {
        return std::unexpected(DecodeError::kBufferTooShort);
    }
    if (v.has_meta && !read_bytes_section(v.meta)) {
        return std::unexpected(DecodeError::kBufferTooShort);
    }
    // fields 段（#1）：[FieldCount:varint] × { [FieldId:varint][ValLen:varint][value] }
    if (v.has_fields) {
        auto fc_vr = vbyte_read_checked(buf, pos);
        if (!fc_vr) {
            return std::unexpected(DecodeError::kBufferTooShort);
        }
        pos = fc_vr->second;
        const auto fc = fc_vr->first;
        v.fields.reserve(static_cast<std::size_t>(fc));
        for (std::uint64_t i = 0; i < fc; ++i) {
            auto id_vr = vbyte_read_checked(buf, pos);
            if (!id_vr) {
                return std::unexpected(DecodeError::kBufferTooShort);
            }
            pos = id_vr->second;
            DocField f;
            f.id = static_cast<std::uint32_t>(id_vr->first);
            if (!read_bytes_section(f.value)) {
                return std::unexpected(DecodeError::kBufferTooShort);
            }
            v.fields.push_back(f);
        }
    }
    // S13-D5：expiry 段（flag 0x20，末尾 u64 LE 绝对 unix 秒；v4 起 u64）。
    if ((flags & format::kFlagHasExpiry) != 0) {
        if (pos + sizeof(std::uint64_t) > buf.size()) {
            return std::unexpected(DecodeError::kBufferTooShort);
        }
        std::memcpy(&v.expiry_at, buf.data() + pos, sizeof(std::uint64_t));
        pos += sizeof(std::uint64_t);
    }
    return v;
}

std::vector<float> doc_vector_f32(const DocValueView& v) {
    std::vector<float> out;
    if (v.vec_quantized) {
        // dequant：v̂[i] = code[i] * scale / 127。
        out.resize(v.dim);
        const auto* codes =
            reinterpret_cast<const std::int8_t*>(v.vector_raw.data());
        const float s = v.vec_scale / 127.0f;
        for (std::uint32_t i = 0; i < v.dim; ++i) {
            out[i] = static_cast<float>(codes[i]) * s;
        }
    } else if (v.has_vector) {
        out.resize(v.dim);
        if (v.dim) {
            std::memcpy(out.data(), v.vector_raw.data(),
                        static_cast<std::size_t>(v.dim) * sizeof(float));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// hint record 编解码
// ---------------------------------------------------------------------------

// 编码一条 hint record。offset 必须 <= 2^63-1（最高位是 tombstone bit）。
// 这两个 assert 失败都是 caller bug——data file 的 offset 不可能超过 8 EiB，
// 但还是兜底校验，以防上层算 offset 时溢出。
std::size_t encode_hint_record(std::vector<std::byte>& out,
                               std::uint32_t tstamp,
                               std::uint32_t total_sz,
                               std::uint64_t offset,
                               bool tombstone,
                               std::span<const std::byte> key) {
    assert(offset <= format::kMaxOffsetV2);
    assert(key.size() <= format::kMaxKeySize);

    const std::size_t total = format::kHintRecordSize + key.size();
    const std::size_t base = out.size();
    out.resize(base + total);
    std::byte* p = out.data() + base;

    le_store_u32(p + 0, tstamp);
    le_store_u16(p + 4, static_cast<std::uint16_t>(key.size()));
    le_store_u32(p + 6, total_sz);

    // 把 tombstone 标志压到 offset 的最高位——节省 1 字节，跟 legacy 完全
    // 一致的 wire format。读取时反向 mask。
    const std::uint64_t packed =
        (tombstone ? format::kTombMaskV2 : 0ull) | offset;
    le_store_u64(p + 10, packed);

    if (!key.empty()) std::memcpy(p + format::kHintRecordSize, key.data(), key.size());
    return total;
}

std::size_t encode_hint_eof(std::vector<std::byte>& out, std::uint32_t running_crc) {
    // 布局跟普通 hint record 一致，但语义被特殊化：
    //   Tstamp=0, KeySz=0, TotalSz 借用来放整文件 running CRC,
    //   Tomb=0, Offset=kMaxOffsetV2, 无 key payload。
    // 解码方靠 (KeySz==0 && Offset==kMaxOffsetV2) 识别 sentinel。
    return encode_hint_record(out,
                              /*tstamp*/ 0,
                              /*total_sz*/ running_crc,
                              /*offset*/ format::kMaxOffsetV2,
                              /*tombstone*/ false,
                              {});
}

// 从 buf 头部解一条 hint record；CRC 不在这里校验（hint 文件用 trailer
// CRC 一次性兜底，不是逐条 CRC）。EOF sentinel 也作为 HintRecord 返回——
// caller 用 is_hint_eof() 判断。
std::expected<HintRecord, DecodeError>
decode_hint_record(std::span<const std::byte> buf) {
    if (buf.size() < format::kHintRecordSize) {
        return std::unexpected(DecodeError::kBufferTooShort);
    }
    const std::uint32_t tstamp   = le_load_u32(buf.data() + 0);
    const std::uint16_t key_sz   = le_load_u16(buf.data() + 4);
    const std::uint32_t total_sz = le_load_u32(buf.data() + 6);
    const std::uint64_t packed   = le_load_u64(buf.data() + 10);

    // 反向解 packed：最高位 bit = tombstone，剩余 63 位 = offset。
    const bool tomb = (packed & format::kTombMaskV2) != 0;
    const std::uint64_t offset = packed & format::kMaxOffsetV2;

    if (buf.size() < format::kHintRecordSize + key_sz) {
        return std::unexpected(DecodeError::kBufferTooShort);
    }
    return HintRecord{
        .tstamp = tstamp,
        .total_sz = total_sz,
        .offset = offset,
        .tombstone = tomb,
        .key = buf.subspan(format::kHintRecordSize, key_sz),
        .consumed = format::kHintRecordSize + key_sz,
    };
}

bool is_hint_eof(const HintRecord& r) noexcept {
    return r.tstamp == 0 && r.key.empty() && r.offset == format::kMaxOffsetV2;
}

// ---- hint v3（S23-A1）----

std::uint64_t encode_hint_record_v4(std::vector<std::byte>& out,
                                    std::uint64_t tstamp,
                                    std::uint32_t total_sz,
                                    std::uint64_t offset, bool tombstone,
                                    std::span<const std::byte> key,
                                    std::uint64_t prev_end) {
    assert(key.size() <= format::kMaxKeySize);
    // gap 经 u64 二补数回绕：decode 侧 prev_end + gap ≡ offset (mod 2^64)，
    // 正确性不依赖 offset ≥ prev_end；正常连续追加 gap==0 → 1 字节。
    vbyte_encode(offset - prev_end, out);
    vbyte_encode(total_sz, out);
    vbyte_encode((static_cast<std::uint64_t>(key.size()) << 1) |
                     (tombstone ? 1u : 0u),
                 out);
    const std::size_t base = out.size();
    out.resize(base + 8);
    le_store_u64(out.data() + base, tstamp);  // v4：tstamp u64 LE
    if (!key.empty()) {
        out.insert(out.end(), key.begin(), key.end());
    }
    return offset + total_sz;
}

std::expected<HintRecord, DecodeError>
decode_hint_record_v4(std::span<const std::byte> buf, std::uint64_t& prev_end) {
    // 边界安全 vbyte（buf 可能只含半条记录，短读返回 kBufferTooShort）。
    std::size_t pos = 0;
    auto vb = [&](std::uint64_t& v) -> bool {
        v = 0;
        std::uint64_t shift = 0;
        while (true) {
            if (pos >= buf.size() || shift > 63) return false;
            const auto byte = static_cast<std::uint8_t>(buf[pos++]);
            v |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if (byte & 0x80) return true;
            shift += 7;
        }
    };
    std::uint64_t gap = 0, tsz = 0, kt = 0;
    if (!vb(gap) || !vb(tsz) || !vb(kt)) {
        return std::unexpected(DecodeError::kBufferTooShort);
    }
    const std::uint64_t key_sz = kt >> 1;
    if (tsz > 0xFFFFFFFFull || key_sz > format::kMaxKeySize) {
        return std::unexpected(DecodeError::kKeySizeOverflow);
    }
    if (buf.size() < pos + 8 + key_sz) {
        return std::unexpected(DecodeError::kBufferTooShort);
    }
    const std::uint64_t tstamp = le_load_u64(buf.data() + pos);  // v4：u64 LE
    pos += 8;
    const std::uint64_t offset = prev_end + gap;  // 回绕还原
    HintRecord rec{
        .tstamp = tstamp,
        .total_sz = static_cast<std::uint32_t>(tsz),
        .offset = offset,
        .tombstone = (kt & 1) != 0,
        .key = buf.subspan(pos, static_cast<std::size_t>(key_sz)),
        .consumed = pos + static_cast<std::size_t>(key_sz),
    };
    prev_end = offset + rec.total_sz;
    return rec;
}

}  // namespace bitcask::codec
